/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * MT7921/MT7902 Connac2 MCU command framing and firmware download over the
 * native PCI WFDMA rings. The packet layouts are ported from mt76's Connac2
 * MCU implementation; all transport, DMA ownership, and synchronization use
 * FreeBSD APIs.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include "if_mt7902_native.h"

#define MCU_TXD_SIZE	64
#define MCU_UNI_TXD_SIZE	48
#define MCU_HW_TXD_SIZE	32
#define MCU_TXD_PKT_TYPE	0xa0
#define MCU_TXD_SET	1
#define MCU_TXD_NA	3
#define MCU_S2D_HOST_WM	0
#define MCU_S2D_HOST_WA	2
#define MCU_TXQ_MCU	0x20
#define MCU_TXQ_FWDL	0x3e
#define MCU_PKT_FMT_CMD	2
#define MCU_HDR_FMT_CMD	1
#define MCU_FW_FEATURE_OVERRIDE	(1U << 5)
#define MCU_FW_FEATURE_NON_DL	(1U << 6)
#define MCU_FW_FEATURE_ENCRYPT	(1U << 0)
#define MCU_FW_FEATURE_KEY_IDX	(3U << 1)
#define MCU_FW_FEATURE_ENCRY_MODE	(1U << 4)
#define MCU_FW_START_OVERRIDE	(1U << 0)
#define MCU_FW_START_WA	(1U << 2)
#define MCU_FW_DL_ENCRYPT	(1U << 0)
#define MCU_FW_DL_RESET_SEC_IV	(1U << 3)
#define MCU_FW_DL_ENCRY_MODE	(1U << 6)
#define MCU_PATCH_SEM_GET	1
#define MCU_PATCH_SEM_RELEASE	0
#define MCU_PATCH_NOT_DL_SEM_FAIL	0
#define MCU_PATCH_IS_DL		1
#define MCU_PATCH_NOT_DL_SEM_SUCCESS	2
#define MCU_PATCH_REL_SEM_SUCCESS	3
#define MCU_PATCH_TYPE_MASK	0x0000ffffU
#define MCU_PATCH_TYPE_INFO	0x2
#define MCU_PATCH_SEC_ENC_MASK	0xff000000U
#define MCU_PATCH_SEC_ENC_PLAIN	0x00000000U
#define MCU_PATCH_SEC_ENC_AES	0x01000000U
#define MCU_PATCH_SEC_ENC_SCRAMBLE	0x02000000U
#define MCU_FW_CMD_MAX	4096

/* MT_TOP_MISC2_FW_STATE: power-on and N9-running bits. */
#define MCU_FW_STATE_PWR_ON	(1U << 0)
#define MCU_FW_STATE_N9_ON	(1U << 1)
#define MCU_FW_STATE_N9_READY	(MCU_FW_STATE_PWR_ON | MCU_FW_STATE_N9_ON)

struct mt7902_mcu_tx_header {
	uint32_t txd[8];
	uint16_t len;
	uint16_t pq_id;
	uint8_t cid;
	uint8_t pkt_type;
	uint8_t set_query;
	uint8_t seq;
	uint8_t uc_d2b0_rev;
	uint8_t ext_cid;
	uint8_t s2d_index;
	uint8_t ext_cid_ack;
	uint32_t reserved[5];
} __packed __aligned(4);

struct mt7902_mcu_uni_header {
	uint32_t txd[8];
	uint16_t len;
	uint16_t cid;
	uint8_t reserved;
	uint8_t pkt_type;
	uint8_t frag_n;
	uint8_t seq;
	uint16_t checksum;
	uint8_t s2d_index;
	uint8_t option;
	uint8_t reserved2[4];
} __packed __aligned(4);

struct mt7902_fw_dl_request {
	uint32_t addr;
	uint32_t len;
	uint32_t mode;
} __packed;

struct mt7902_fw_start_request {
	uint32_t option;
	uint32_t addr;
} __packed;

struct mt7902_patch_sem_request {
	uint32_t operation;
} __packed;

struct mt7902_patch_finish_request {
	uint8_t check_crc;
	uint8_t reserved[3];
} __packed;

struct mt7902_eeprom_request {
	uint32_t addr;
} __packed;

struct mt7902_eeprom_response {
	uint32_t addr;
	uint32_t valid;
	uint8_t data[16];
} __packed;

static int
mt7902_tx_submit(struct mt7902_softc *sc, struct mt7902_ring *ring,
	const void *data, size_t len)
{
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	int idx, next, retry;
	uint32_t control;

	if (len == 0 || len > ring->buf[0].len || len > MT7902_DMA_LEN_MASK)
		return (EINVAL);
	mtx_lock(&sc->mtx);
	if (sc->dying || ring->queued >= ring->ndesc - 1) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	idx = ring->head;
	next = (idx + 1) % ring->ndesc;
	desc = &ring->desc[idx];
	buf = &ring->buf[idx];
	control = le32toh(desc->control);
	if ((control & MT7902_DMA_DESC_DONE) == 0) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	memcpy(buf->vaddr, data, len);
	bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_PREWRITE);
	desc->buf0 = htole32((uint32_t)buf->paddr);
	desc->buf1 = 0;
	desc->info = 0;
	desc->control = htole32(((uint32_t)len << MT7902_DMA_LEN_SHIFT) |
	    MT7902_DMA_DESC_LAST);
	bus_dmamap_sync(ring->desc_tag, ring->desc_map, BUS_DMASYNC_PREWRITE);
	ring->head = next;
	ring->queued++;
	mt7902_write(sc, ring->reg_base + 0x8, next);
	mtx_unlock(&sc->mtx);

	for (retry = 0; retry < 10000; retry++) {
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if (control & MT7902_DMA_DESC_DONE)
			break;
		DELAY(10);
	}
	if (retry == 10000) {
		device_printf(sc->dev,
		    "WFDMA TX ring %u timeout: desc[%d]=0x%08x dma=%u tx=%u ring=%08x/%08x/%08x/%08x\n",
		    ring->hw_qid, idx, le32toh(desc->control),
		    mt7902_read(sc, MT7902_REG_WFDMA_GLO_CFG),
		    mt7902_read(sc, MT7902_REG_WFDMA_INT_STA),
		    mt7902_read(sc, ring->reg_base),
		    mt7902_read(sc, ring->reg_base + 4),
		    mt7902_read(sc, ring->reg_base + 8),
		    mt7902_read(sc, ring->reg_base + 12));
		return (ETIMEDOUT);
	}

	bus_dmamap_sync(ring->buf_tag, buf->map,
	    BUS_DMASYNC_POSTWRITE | BUS_DMASYNC_PREREAD);
	mtx_lock(&sc->mtx);
	desc->control = htole32(MT7902_DMA_DESC_DONE);
	bus_dmamap_sync(ring->desc_tag, ring->desc_map, BUS_DMASYNC_PREWRITE);
	ring->tail = (idx + 1) % ring->ndesc;
	if (ring->queued > 0)
		ring->queued--;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
mt7902_mcu_send_core(struct mt7902_softc *sc, uint32_t command,
	const void *payload, size_t len, bool wait_response, void *response,
	size_t *response_len)
{
	struct mt7902_mcu_tx_header header;
	struct mt7902_ring *ring;
	uint8_t *packet;
	size_t packet_len, header_len, copy_len;
	uint8_t seq;
	uint32_t *txd;
	uint8_t cmd, ext, set_query, s2d;
	int error, deadline, remaining;
	bool uni;

	if (sc == NULL || (len != 0 && payload == NULL) || len > MCU_FW_CMD_MAX)
		return (EINVAL);
	cmd = command & MT7902_MCU_ID_MASK;
	ext = (command & MT7902_MCU_EXT_MASK) >> 8;
	uni = (command & MT7902_MCU_UNI_BIT) != 0;
	if (!uni && cmd == MT7902_MCU_CMD_FW_SCATTER) {
		ring = &sc->tx_fwdl;
		return (mt7902_tx_submit(sc, ring, payload, len));
	}

	header_len = uni ? sizeof(struct mt7902_mcu_uni_header) : sizeof(header);
	packet_len = header_len + len;
	if (packet_len > sc->tx_mcu.buf[0].len)
		return (EMSGSIZE);
	packet = malloc(packet_len, M_DEVBUF, M_WAITOK | M_ZERO);
	txd = (uint32_t *)packet;
	mtx_lock(&sc->mtx);
	seq = (++sc->mcu_seq) & 0x0f;
	if (seq == 0)
		seq = (++sc->mcu_seq) & 0x0f;
	if (wait_response) {
		sc->mcu_wait_seq = seq;
		sc->mcu_wait_cmd = cmd;
		sc->mcu_waiting = 1;
		sc->mcu_error = EINPROGRESS;
		sc->mcu_response_len = 0;
	}
	mtx_unlock(&sc->mtx);

	txd[0] = htole32((uint32_t)packet_len |
	    ((uint32_t)MCU_PKT_FMT_CMD << 23) |
	    ((uint32_t)MCU_TXQ_MCU << 25));
	txd[1] = htole32((1U << 31) | ((uint32_t)MCU_HDR_FMT_CMD << 16));
	s2d = (command & (1U << 19)) != 0 ? MCU_S2D_HOST_WA : MCU_S2D_HOST_WM;
	if (uni) {
		struct mt7902_mcu_uni_header *uh;

		uh = (struct mt7902_mcu_uni_header *)packet;
		uh->len = htole16((uint16_t)(packet_len - MCU_HW_TXD_SIZE));
		uh->cid = htole16(cmd);
		uh->pkt_type = MCU_TXD_PKT_TYPE;
		uh->seq = seq;
		uh->s2d_index = s2d;
		uh->option = wait_response ? 0x07 : 0x02;
	} else {
		bzero(&header, sizeof(header));
		header.txd[0] = txd[0];
		header.txd[1] = txd[1];
		header.len = htole16((uint16_t)(packet_len - MCU_HW_TXD_SIZE));
		header.pq_id = htole16((uint16_t)((1U << 15) | (MCU_TXQ_MCU << 10)));
		header.cid = cmd;
		header.pkt_type = MCU_TXD_PKT_TYPE;
		if (ext != 0 || (command & MT7902_MCU_CE_BIT) != 0) {
			set_query = (command & MT7902_MCU_QUERY_BIT) != 0 ? 0 : 1;
			header.set_query = set_query;
			header.ext_cid = ext;
			header.ext_cid_ack = ext != 0;
		} else {
			header.set_query = MCU_TXD_NA;
		}
		header.seq = seq;
		header.s2d_index = s2d;
		memcpy(packet, &header, sizeof(header));
	}
	if (len != 0)
		memcpy(packet + header_len, payload, len);
	ring = &sc->tx_mcu;
	error = mt7902_tx_submit(sc, ring, packet, packet_len);
	free(packet, M_DEVBUF);
	if (error != 0) {
		mtx_lock(&sc->mtx);
		sc->mcu_waiting = 0;
		mtx_unlock(&sc->mtx);
		return (error);
	}
	if (!wait_response)
		return (0);

	mtx_lock(&sc->mtx);
	deadline = ticks + 10 * hz;
	while (sc->mcu_waiting) {
		remaining = deadline - ticks;
		if (remaining <= 0)
			break;
		error = cv_timedwait(&sc->mcu_cv, &sc->mtx, remaining);
		if (error == EWOULDBLOCK)
			break;
		if (error != 0)
			break;
	}
	if (sc->mcu_waiting) {
		device_printf(sc->dev,
		    "MCU cmd 0x%02x seq %u timeout: irq=%08x seen=%08x/%ju rxev=%ju rx0=%08x/%08x/%08x/%08x rx0desc=%08x tx15=%08x/%08x/%08x/%08x int_en=%08x misc=%08x\n",
		    cmd, seq, mt7902_read(sc, MT7902_REG_WFDMA_INT_STA),
		    sc->irq_last_status, (uintmax_t)sc->irq_count,
		    (uintmax_t)sc->rx_event_count,
		    mt7902_read(sc, sc->rx_event.reg_base),
		    mt7902_read(sc, sc->rx_event.reg_base + 4),
		    mt7902_read(sc, sc->rx_event.reg_base + 0x8),
		    mt7902_read(sc, sc->rx_event.reg_base + 0xc),
		    le32toh(sc->rx_event.desc[sc->rx_event.tail].control),
		    mt7902_read(sc, sc->tx_mcu.reg_base),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 4),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 8),
		    mt7902_read(sc, sc->tx_mcu.reg_base + 0xc),
		    mt7902_read(sc, MT7902_REG_WFDMA_INT_EN),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
		sc->mcu_waiting = 0;
		error = ETIMEDOUT;
	} else {
		error = sc->mcu_error;
	}
	if (error == 0 && response_len != NULL) {
		copy_len = MIN(*response_len, sc->mcu_response_len);
		if (response != NULL && copy_len != 0)
			memcpy(response, sc->mcu_response, copy_len);
		*response_len = sc->mcu_response_len;
	}
	if (error == 0 && uni && sc->mcu_response_len >= 8 &&
	    (sc->mcu_response[0] != cmd ||
	    le32dec(sc->mcu_response + 4) != 0))
		error = EIO;
	mtx_unlock(&sc->mtx);
	return (error);
}

int
mt7902_mcu_send(struct mt7902_softc *sc, uint8_t cmd, uint8_t ext,
	const void *payload, size_t len, bool wait_response)
{
	uint32_t command;

	command = cmd | ((uint32_t)ext << 8);
	return (mt7902_mcu_send_core(sc, command, payload, len,
	    wait_response, NULL, NULL));
}

int
mt7902_mcu_send_cmd(struct mt7902_softc *sc, uint32_t command,
	const void *payload, size_t len, bool wait_response)
{
	return (mt7902_mcu_send_core(sc, command, payload, len,
	    wait_response, NULL, NULL));
}

int
mt7902_mcu_send_response(struct mt7902_softc *sc, uint32_t command,
	const void *payload, size_t len, void *response, size_t *response_len)
{
	if (response_len == NULL)
		return (EINVAL);
	return (mt7902_mcu_send_core(sc, command, payload, len, true,
	    response, response_len));
}

static int
mt7902_mcu_download_init(struct mt7902_softc *sc, uint32_t addr,
	uint32_t len, uint32_t mode)
{
	struct mt7902_fw_dl_request req;
	uint8_t cmd;

	req.addr = htole32(addr);
	req.len = htole32(len);
	req.mode = htole32(mode | (1U << 31)); /* DL_MODE_NEED_RSP */
	cmd = addr == 0x200000 || addr == 0x900000 ?
	    MT7902_MCU_CMD_PATCH_START : MT7902_MCU_CMD_TARGET_ADDR;
	return (mt7902_mcu_send(sc, cmd, 0, &req, sizeof(req), true));
}

static int
mt7902_mcu_start_fw(struct mt7902_softc *sc, uint32_t addr, uint32_t option)
{
	struct mt7902_fw_start_request req;

	req.option = htole32(option);
	req.addr = htole32(addr);
	return (mt7902_mcu_send(sc, MT7902_MCU_CMD_FW_START, 0, &req,
	    sizeof(req), true));
}

static int
mt7902_mcu_send_scatter(struct mt7902_softc *sc, const uint8_t *data,
	size_t len)
{
	size_t chunk;
	int error;

	while (len != 0) {
		chunk = MIN(len, (size_t)MCU_FW_CMD_MAX);
		error = mt7902_mcu_send(sc, MT7902_MCU_CMD_FW_SCATTER, 0,
		    data, chunk, false);
		if (error != 0)
			return (error);
		data += chunk;
		len -= chunk;
	}
	return (0);
}

static int
mt7902_mcu_load_patch(struct mt7902_softc *sc)
{
	struct mt7902_patch_sem_request sem;
	struct mt7902_patch_finish_request finish;
	struct mt7902_fw_image *image;
	struct mt7902_fw_region *region;
	uint32_t mode, enc, i;
	int error, sem_status;

	image = &sc->fw_patch;
	sem.operation = htole32(MCU_PATCH_SEM_GET);
	sem_status = mt7902_mcu_send(sc, MT7902_MCU_CMD_PATCH_SEM, 0,
	    &sem, sizeof(sem), true);
	switch (sem_status) {
	case MCU_PATCH_IS_DL:
		device_printf(sc->dev, "patch semaphore reports firmware already loaded\n");
		return (0);
	case MCU_PATCH_NOT_DL_SEM_SUCCESS:
		break;
	case MCU_PATCH_NOT_DL_SEM_FAIL:
	default:
		device_printf(sc->dev,
		    "patch semaphore acquisition failed (status=%d)\n",
		    sem_status);
		return (sem_status == 0 ? EAGAIN : sem_status);
	}
	error = 0;
	for (i = 0; i < image->nregion; i++) {
		region = &image->region[i];
		enc = region->sec & MCU_PATCH_SEC_ENC_MASK;
		mode = 1U << 31;
		switch (enc) {
		case MCU_PATCH_SEC_ENC_PLAIN:
			break;
		case MCU_PATCH_SEC_ENC_AES:
			mode |= (1U << 0) | (1U << 3) |
			    (((region->sec >> 24) & 0xff) << 1);
			break;
		case MCU_PATCH_SEC_ENC_SCRAMBLE:
			mode |= (1U << 0) | (1U << 3) | (1U << 6);
			break;
		default:
			error = EINVAL;
			goto release_sem;
		}
		if ((region->feature & MCU_PATCH_TYPE_MASK) != MCU_PATCH_TYPE_INFO) {
			error = EINVAL;
			goto release_sem;
		}
		error = mt7902_mcu_download_init(sc, region->addr,
		    (uint32_t)region->len, mode);
		if (error != 0)
			goto release_sem;
		error = mt7902_mcu_send_scatter(sc, region->data, region->len);
		if (error != 0)
			goto release_sem;
	}
	bzero(&finish, sizeof(finish));
	error = mt7902_mcu_send(sc, MT7902_MCU_CMD_PATCH_FINISH, 0,
	    &finish, sizeof(finish), true);
release_sem:
	sem.operation = htole32(MCU_PATCH_SEM_RELEASE);
	sem_status = mt7902_mcu_send(sc, MT7902_MCU_CMD_PATCH_SEM, 0,
	    &sem, sizeof(sem), true);
	if (sem_status != MCU_PATCH_REL_SEM_SUCCESS && error == 0) {
		device_printf(sc->dev,
		    "patch semaphore release failed (status=%d)\n", sem_status);
		error = sem_status == 0 ? EAGAIN : sem_status;
	}
	return (error);
}

static int
mt7902_mcu_load_wm(struct mt7902_softc *sc)
{
	struct mt7902_fw_image *image;
	struct mt7902_fw_region *region;
	uint32_t i, offset, option, mode;
	int error;

	image = &sc->fw_wm;
	offset = 0;
	option = 0;
	for (i = 0; i < image->nregion; i++) {
		region = &image->region[i];
		if ((region->feature & MCU_FW_FEATURE_NON_DL) == 0) {
			mode = region->feature & MCU_FW_FEATURE_KEY_IDX;
			if (region->feature & MCU_FW_FEATURE_ENCRYPT)
				mode |= MCU_FW_DL_ENCRYPT |
				    MCU_FW_DL_RESET_SEC_IV;
			if (region->feature & MCU_FW_FEATURE_ENCRY_MODE)
				mode |= MCU_FW_DL_ENCRY_MODE;
			error = mt7902_mcu_download_init(sc, region->addr,
			    region->len, mode);
			if (error != 0)
				return (error);
			error = mt7902_mcu_send_scatter(sc, region->data, region->len);
			if (error != 0)
				return (error);
		}
		if (region->feature & MCU_FW_FEATURE_OVERRIDE)
			option |= MCU_FW_START_OVERRIDE;
		offset += region->len;
	}
	(void)offset;
	error = mt7902_mcu_start_fw(sc, image->override_addr, option);
	if (error != 0)
		return (error);
	error = mt7902_wait(sc, MT7902_REG_CONN_ON_MISC,
	    MCU_FW_STATE_N9_READY, MCU_FW_STATE_N9_READY, 1500000);
	if (error != 0)
		device_printf(sc->dev,
		    "firmware N9 ready timeout (misc=%08x expected=%08x)\n",
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC),
		    MCU_FW_STATE_N9_READY);
	return (error);
}

static int
mt7902_pci_power_on(struct mt7902_softc *sc)
{
	int cap, error;
	uint16_t linkctl, linksta;

	/* Connac2 PCIe ownership handshake from mt7921e_mcu_fw_pmctrl() and
	 * mt7921e_driver_own(). The firmware/driver ownership request bits are
	 * write-one commands; bit 2 reports the synchronized owner.
	 */
	if (pci_find_cap(sc->dev, PCIY_EXPRESS, &cap) == 0) {
		linkctl = pci_read_config(sc->dev, cap + PCIER_LINK_CTL, 2);
		linksta = pci_read_config(sc->dev, cap + PCIER_LINK_STA, 2);
		device_printf(sc->dev,
		    "PCIe link ctl=%04x status=%04x ASPM=%u speed=%u width=x%u\n",
		    linkctl, linksta, linkctl & PCIEM_LINK_CTL_ASPMC,
		    linksta & PCIEM_LINK_STA_SPEED,
		    (linksta & PCIEM_LINK_STA_WIDTH) >> 4);
	}
	mt7902_write(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 0);
	error = mt7902_wait(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 2,
	    1U << 2, 50000);
	if (error != 0) {
		device_printf(sc->dev,
		    "FW ownership request failed (LPCTL=%08x MISC=%08x)\n",
		    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
		return (error);
	}
	device_printf(sc->dev, "FW ownership synchronized (LPCTL=%08x)\n",
	    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL));
	mt7902_write(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 1);
	error = mt7902_wait(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 2, 0, 50000);
	if (error != 0) {
		device_printf(sc->dev,
		    "driver ownership request failed (LPCTL=%08x MISC=%08x)\n",
		    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
		return (error);
	}
	device_printf(sc->dev, "driver ownership synchronized (LPCTL=%08x)\n",
	    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL));
	/* mt792xe_mcu_drv_pmctrl() allows the ASPM transition to settle before
	 * touching WFSYS.  The host has L0s and L1 enabled on this endpoint.
	 */
	if (pci_find_cap(sc->dev, PCIY_EXPRESS, &cap) == 0 &&
	    (pci_read_config(sc->dev, cap + PCIER_LINK_CTL, 2) &
	    PCIEM_LINK_CTL_ASPMC) != 0) {
		DELAY(2500);
		device_printf(sc->dev, "ASPM ownership settle delay applied\n");
	}

	/* PCI attach performs this WFSYS reset before DMA and MCU firmware. */
	device_printf(sc->dev, "WFSYS reset begin control=%08x misc=%08x\n",
	    mt7902_read(sc, MT7902_REG_WFSYS_RESET),
	    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
	mt7902_write(sc, MT7902_REG_WFSYS_RESET,
	    mt7902_read(sc, MT7902_REG_WFSYS_RESET) & ~MT7902_WFSYS_RESET_B);
	DELAY(50000);
	mt7902_write(sc, MT7902_REG_WFSYS_RESET,
	    mt7902_read(sc, MT7902_REG_WFSYS_RESET) | MT7902_WFSYS_RESET_B);
	error = mt7902_wait(sc, MT7902_REG_WFSYS_RESET,
	    MT7902_WFSYS_INIT_DONE, MT7902_WFSYS_INIT_DONE, 500000);
	if (error != 0)
		device_printf(sc->dev,
		    "WFSYS reset timed out (control=%08x misc=%08x)\n",
		    mt7902_read(sc, MT7902_REG_WFSYS_RESET),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
	else
		device_printf(sc->dev,
		    "WFSYS reset complete control=%08x LPCTL=%08x misc=%08x\n",
		    mt7902_read(sc, MT7902_REG_WFSYS_RESET),
		    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
	return (error);
}

int
mt7902_mcu_init(struct mt7902_softc *sc)
{
	int cap, error;
	uint16_t linkctl;

	/* mt7921e_mcu_init() reacquires driver ownership after the probe-time
	 * WFSYS reset.  The reset can restore firmware ownership.
	 */
	mt7902_write(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 1);
	error = mt7902_wait(sc, MT7902_REG_CONN_ON_LPCTL, 1U << 2, 0,
	    50000);
	if (error != 0) {
		device_printf(sc->dev,
		    "post-reset driver ownership failed LPCTL=%08x misc=%08x\n",
		    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
		return (error);
	}
	device_printf(sc->dev,
	    "post-reset driver ownership synchronized LPCTL=%08x\n",
	    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL));
	if (pci_find_cap(sc->dev, PCIY_EXPRESS, &cap) == 0) {
		linkctl = pci_read_config(sc->dev, cap + PCIER_LINK_CTL, 2);
		if (linkctl & PCIEM_LINK_CTL_ASPMC)
			DELAY(2500);
	}

	/* Match mt7921 __mt7921_init_hardware(): force normal mode first. */
	mt7902_write(sc, MT7902_REG_SWDEF_MODE, 0);
	mt7902_write(sc, MT7902_REG_PCIE_MAC_INT_ENABLE, 0xff);
	mt7902_write(sc, MT7902_REG_PCIE_PM,
	    mt7902_read(sc, MT7902_REG_PCIE_PM) | MT7902_PCIE_PM_L0S_DIS);
	/* mt792x_load_firmware() restarts the ROM MCU before testing its state. */
	{
		struct {
			uint8_t power_mode;
			uint8_t reserved[3];
		} __packed restart;

		bzero(&restart, sizeof(restart));
		restart.power_mode = 1;
		error = mt7902_mcu_send(sc, MT7902_MCU_CMD_NIC_POWER, 0,
		    &restart, sizeof(restart), false);
		if (error != 0)
			return (error);
	}
	error = mt7902_wait(sc, MT7902_REG_CONN_ON_MISC,
	    MCU_FW_STATE_PWR_ON, MCU_FW_STATE_PWR_ON, 1000000);
	if (error != 0)
		device_printf(sc->dev,
		    "firmware power-on state not ready before patch (misc 0x%08x, %d)\n",
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC), error);
	error = mt7902_mcu_load_patch(sc);
	if (error != 0) {
		device_printf(sc->dev, "MCU patch download failed: %d\n", error);
		return (error);
	}
	error = mt7902_mcu_load_wm(sc);
	if (error != 0) {
		device_printf(sc->dev, "WM firmware download failed: %d\n", error);
		return (error);
	}
	device_printf(sc->dev, "MT7902 WM firmware entered operational state\n");
	return (0);
}

/*
 * Configure the Connac2 receive filter before exposing the net80211 VAP.
 * This is the same SET_RX_FILTER command used by mt7921_configure_filter():
 * mode 1 replaces the filter flags, while bit_op/bit_map are reserved for
 * incremental RFCR bitmap changes.
 */
int
mt7902_mcu_set_rxfilter(struct mt7902_softc *sc, uint32_t flags)
{
	struct {
		uint8_t reserved[4];
		uint8_t mode;
		uint8_t reserved2[3];
		uint32_t fif;
		uint32_t bit_map;
		uint8_t bit_op;
		uint8_t pad[51];
	} __packed req;

	bzero(&req, sizeof(req));
	req.mode = flags != 0 ? 1 : 2;
	req.fif = htole32(flags);
	return (mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_CE_CMD(MT7902_MCU_CE_SET_RX_FILTER),
	    &req, sizeof(req), false));
}

int
mt7902_mcu_read_mac(struct mt7902_softc *sc)
{
	struct mt7902_eeprom_request req;
	struct mt7902_eeprom_response resp;
	size_t resp_len;
	int i, error;
	uint8_t mac_or;

	for (i = 0; i < IEEE80211_ADDR_LEN; i += sizeof(resp.data)) {
		req.addr = htole32(i & ~0x0fU);
		resp_len = sizeof(resp);
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_EXT_QUERY(MT7902_MCU_EXT_EFUSE_ACCESS),
		    &req, sizeof(req), &resp, &resp_len);
		if (error != 0)
			return (error);
		if (resp_len < sizeof(resp) || le32toh(resp.valid) == 0)
			return (ENOENT);
		memcpy(sc->macaddr, resp.data + (i & 0x0f),
		    MIN((size_t)sizeof(resp.data) - (i & 0x0f),
		    (size_t)IEEE80211_ADDR_LEN - i));
	}
	mac_or = 0;
	for (i = 0; i < IEEE80211_ADDR_LEN; i++)
		mac_or |= sc->macaddr[i];
	if (mac_or == 0 || ETHER_IS_MULTICAST(sc->macaddr))
		return (EINVAL);
	return (0);
}

int
mt7902_fw_boot(struct mt7902_softc *sc)
{
	uint32_t rx_filter;
	int error, reset_error;

	error = mt7902_fw_get(sc, MT7902_FW_PATCH, &sc->fw_patch);
	if (error != 0)
		goto fail;
	error = mt7902_fw_validate_patch(&sc->fw_patch);
	if (error != 0) {
		device_printf(sc->dev, "invalid firmware patch container: %d\n", error);
		goto fail;
	}
	error = mt7902_fw_get(sc, MT7902_FW_WM, &sc->fw_wm);
	if (error != 0)
		goto fail;
	error = mt7902_fw_validate_wm(&sc->fw_wm);
	if (error != 0) {
		device_printf(sc->dev, "invalid WM firmware container: %d\n", error);
		goto fail;
	}
	error = mt7902_pci_power_on(sc);
	if (error != 0)
		goto fail;
	error = mt7902_dma_init(sc);
	if (error != 0)
		goto fail;
	error = mt7902_irq_setup(sc);
	if (error != 0)
		goto fail;
	error = mt7902_mcu_init(sc);
	if (error != 0)
		goto fail;
	/*
	 * Re-apply the host receive filter on every firmware start, including
	 * hw_start retries after a stop/reset. A monitor VAP additionally needs
	 * control frames and failed-FCS frames for packet capture.
	 */
	rx_filter = MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS;
	mtx_lock(&sc->mtx);
	if (sc->monitor_active)
		rx_filter |= MT7902_RX_FILTER_CONTROL | MT7902_RX_FILTER_FCSFAIL;
	mtx_unlock(&sc->mtx);
	error = mt7902_mcu_set_rxfilter(sc, rx_filter);
	if (error != 0)
		device_printf(sc->dev, "could not program MCU RX filter: %d\\n",
		    error);
	error = mt7902_mcu_read_mac(sc);
	if (error != 0) {
		device_printf(sc->dev, "could not read a valid MAC address: %d\n",
		    error);
		goto fail;
	}
	error = mt7902_net80211_attach(sc);
	if (error != 0)
		goto fail;
	mt7902_fw_put(&sc->fw_patch);
	mt7902_fw_put(&sc->fw_wm);
	return (0);

fail:
	device_printf(sc->dev, "firmware boot failed: %d\n", error);
	mt7902_irq_teardown(sc);
	mt7902_dma_stop(sc);
	/* A failed patch or FW_START can leave the ROM/WM MCU holding firmware
	 * ownership.  Linux's forced WPDMA reset also resets WFSYS; do that here
	 * before permitting a later hw_start retry.
	 */
	if (sc->mem != NULL) {
		uint32_t reset;

		reset = mt7902_read(sc, MT7902_REG_WFSYS_RESET);
		mt7902_write(sc, MT7902_REG_WFSYS_RESET,
		    reset & ~MT7902_WFSYS_RESET_B);
		DELAY(50000);
		reset = mt7902_read(sc, MT7902_REG_WFSYS_RESET) |
		    MT7902_WFSYS_RESET_B;
		mt7902_write(sc, MT7902_REG_WFSYS_RESET, reset);
		reset_error = mt7902_wait(sc, MT7902_REG_WFSYS_RESET,
		    MT7902_WFSYS_INIT_DONE, MT7902_WFSYS_INIT_DONE, 500000);
		device_printf(sc->dev,
		    "failed-boot WFSYS recovery %s control=%08x LPCTL=%08x misc=%08x\n",
		    reset_error == 0 ? "complete" : "timed out",
		    mt7902_read(sc, MT7902_REG_WFSYS_RESET),
		    mt7902_read(sc, MT7902_REG_CONN_ON_LPCTL),
		    mt7902_read(sc, MT7902_REG_CONN_ON_MISC));
	}
	mt7902_fw_put(&sc->fw_patch);
	mt7902_fw_put(&sc->fw_wm);
	return (error);
}
