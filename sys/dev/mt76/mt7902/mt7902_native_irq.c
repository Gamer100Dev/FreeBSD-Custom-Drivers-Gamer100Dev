/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native PCI interrupt and MCU event-ring handling.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>

#include "if_mt7902_native.h"

#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_proto.h>

#define MT7902_RXD_SIZE	36
#define MT7902_RXD_EID_OFFSET	28
#define MT7902_RXD_SEQ_OFFSET	29
#define MT7902_RXD_EVENT	0x06
#define MT7902_RXD_LEN_OFFSET	24
#define MT7902_RX_LEN_SHIFT	16
#define MT7902_RX_LEN_MASK	0x3fffU
#define MT7902_RXD0_LEN_MASK	0xffffU
#define MT7902_RXD0_TYPE_SHIFT	27
#define MT7902_RXD1_GROUP1	(1U << 11)
#define MT7902_RXD1_GROUP2	(1U << 12)
#define MT7902_RXD1_GROUP3	(1U << 13)
#define MT7902_RXD1_GROUP4	(1U << 14)
#define MT7902_RXD1_GROUP5	(1U << 15)
#define MT7902_RXD2_PAD_MASK	(3U << 14)
#define MT7902_RXD2_PAD_SHIFT	14

/*
 * Return the reserved producer slot to WFDMA.  RX buffers stay paired with
 * their descriptors for their entire lifetime: swapping buffer structs here
 * leaves the descriptor-to-buffer mapping inconsistent after the ring wraps.
 */
/*
 * Refill the reserved producer slot using the completed slot's buffer.
 * mt76's dma dequeue/fill path does the same ownership rotation: the
 * consumed buffer is recycled at head, while the formerly reserved buffer
 * remains associated with the consumed descriptor until the ring wraps.
 */
static int
mt7902_rx_rearm(struct mt7902_softc *sc, struct mt7902_ring *ring, int idx)
{
	struct mt7902_dma_buffer tmp, *buf;
	struct mt7902_dma_desc *desc;
	int refill_idx;

	refill_idx = ring->head;
	if (refill_idx == idx || ring->queued <= 0 ||
	    ring->queued >= ring->ndesc)
		return (EBUSY);

	/* Remove the completed tail and rotate its buffer to the producer. */
	ring->queued--;
	tmp = ring->buf[idx];
	ring->buf[idx] = ring->buf[refill_idx];
	ring->buf[refill_idx] = tmp;
	buf = &ring->buf[refill_idx];
	desc = &ring->desc[refill_idx];

	bus_dmamap_sync(ring->buf_tag, buf->map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	bus_dmamap_sync(ring->buf_tag, buf->map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	desc->buf0 = htole32((uint32_t)buf->paddr);
	desc->buf1 = 0;
	desc->info = 0;
	desc->control = htole32(((uint32_t)buf->len &
	    MT7902_DMA_LEN_MASK) << MT7902_DMA_LEN_SHIFT);
	bus_dmamap_sync(ring->desc_tag, ring->desc_map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	ring->tail = (idx + 1) % ring->ndesc;
	ring->head = (refill_idx + 1) % ring->ndesc;
	ring->queued++;
	mt7902_write(sc, ring->reg_base + 0x8, ring->head);
	return (0);
}

static void
mt7902_rx_drop(struct mt7902_softc *sc)
{
	mtx_lock(&sc->mtx);
	sc->rx_drop_count++;
	mtx_unlock(&sc->mtx);
}

static size_t
mt7902_rx_80211_hdrlen(uint16_t fc)
{
	size_t len;
	uint16_t type, subtype;

	type = fc & 0x000c;
	subtype = (fc >> 4) & 0x0f;
	switch (type) {
	case 0x0000:             /* management */
		len = sizeof(struct ieee80211_frame);
		if (fc & 0x8000)
			len += 4;
		break;
	case 0x0004:             /* control */
		len = (subtype == 0x0c || subtype == 0x0d) ?
		    sizeof(struct ieee80211_frame_cts) :
		    sizeof(struct ieee80211_frame_min);
		break;
	case 0x0008:             /* data */
		len = sizeof(struct ieee80211_frame);
		if ((fc & 0x0300) == 0x0300)
			len += IEEE80211_ADDR_LEN;
		if (subtype & 0x08)
			len += 2;
		if (fc & 0x8000)
			len += 4;
		break;
	default:
		len = 0;
		break;
	}
	return (len);
}

/*
 * Connac2 normal RXD parser.  The six-word base RXD is followed by optional
 * groups in the order used by mt7921_mac_fill_rx(): GROUP_4, GROUP_1,
 * GROUP_2, GROUP_3 and (inside GROUP_3) GROUP_5.  Header-translated frames
 * are deliberately rejected for now because net80211 expects native 802.11
 * headers; the management frames needed for scanning are not translated.
 */
static void
mt7902_rx_data_frame(struct mt7902_softc *sc, const uint8_t *data,
    size_t dma_len)
{
	struct ieee80211_rx_stats rxs;
	const struct ieee80211_frame_min *wh;
	struct mbuf *m;
	uint32_t rxd0, rxd1, rxd2, rxd3, rxd4, rcpi_word, sec_mode;
	uint16_t fc;
	size_t offset, frame_len, hdr_len;
	uint8_t channel, rcpi, subtype;
	int rssi, noise;
	bool is_mgmt, monitor_active;

	if (dma_len < 24)
		goto drop;
	rxd0 = le32dec(data);
	rxd1 = le32dec(data + 4);
	rxd2 = le32dec(data + 8);
	rxd3 = le32dec(data + 12);
	rxd4 = le32dec(data + 16);
	channel = (rxd3 >> 8) & 0xff;
	/* MT7902's current net80211 channel list is 2.4/5 GHz only. */
	if (channel > 180)
		goto drop;

	mtx_lock(&sc->mtx);
	sc->rx_data_count++;
	sc->rx_last_len = (uint32_t)dma_len;
	sc->rx_last_rxd[0] = rxd0;
	sc->rx_last_rxd[1] = rxd1;
	sc->rx_last_rxd[2] = rxd2;
	sc->rx_last_rxd[3] = rxd3;
	monitor_active = sc->monitor_active;
	mtx_unlock(&sc->mtx);

	/* PKT_TYPE_NORMAL is 2 in the Connac2 RXD0 packet-type field. */
	if (((rxd0 >> 27) & 0x1f) != 2)
		goto drop;
	/*
	 * ICV/CCMP MIC failures have no equivalent generic FreeBSD RX flag,
	 * so keep dropping them.  FCS and TKIP-MIC failures are useful to a
	 * monitor VAP and carry explicit net80211 packet flags below.
	 */
	if ((rxd1 & ((1U << 28) | (1U << 25))) != 0 ||
	    (!monitor_active && (rxd1 & ((1U << 26) | (1U << 27))) != 0) ||
	    (rxd2 & ((1U << 13) | (1U << 23) | (1U << 24) |
	    (1U << 25))) != 0)
		goto drop;
	/* RXD4 payload format != 0 describes a deaggregated A-MSDU. */
	if ((rxd4 & 0x3) != 0)
		goto drop;

	offset = 24;
	if (rxd1 & (1U << 14))
		offset += 16;             /* RXD GROUP_4 */
	if (rxd1 & (1U << 11))
		offset += 16;             /* RXD GROUP_1 */
	if (rxd1 & (1U << 12))
		offset += 8;              /* RXD GROUP_2 */

	rcpi = 0;
	if (rxd1 & (1U << 13)) {
		if (offset > dma_len || dma_len - offset < 8)
			goto drop;
		rcpi_word = le32dec(data + offset + 4);
		rcpi = rcpi_word & 0xff;  /* PRXV RCPI0 */
		offset += 8;              /* RXD GROUP_3 */
		if (rxd1 & (1U << 15)) {
			if (offset > dma_len || dma_len - offset < 72)
				goto drop;
			offset += 24;         /* GROUP_5 prefix */
			rcpi_word = le32dec(data + offset);
			rcpi = rcpi_word & 0xff;
			offset += 48;         /* GROUP_5 RX vector */
		}
	}
	/* RXD2 HDR_OFFSET is measured in 16-bit words. */
	offset += 2 * ((rxd2 >> 14) & 0x3);
	if (offset >= dma_len)
		goto drop;
	frame_len = dma_len - offset;
	if (frame_len < sizeof(*wh))
		goto drop;

	wh = (const struct ieee80211_frame_min *)(const void *)(data + offset);
	fc = le16dec(&wh->i_fc[0]);
	hdr_len = mt7902_rx_80211_hdrlen(fc);
	if (hdr_len == 0 || frame_len < hdr_len)
		goto drop;
	is_mgmt = (fc & 0x000c) == 0;
	subtype = (fc >> 4) & 0x0f;
	if (is_mgmt) {
		mtx_lock(&sc->mtx);
		sc->rx_mgmt_count++;
		if (subtype == 8 || subtype == 5)
			sc->rx_beacon_count++;
		mtx_unlock(&sc->mtx);
	}

	/* RCPI is 0.5 dB steps with a 220 offset, per Connac2 PRXV. */
	noise = -95;
	rssi = rcpi != 0 ? ((int)rcpi - 220) / 2 : noise;
	m = m_get2((int)frame_len, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		goto drop;
	m->m_len = (int)frame_len;
	m->m_pkthdr.len = (int)frame_len;
	memcpy(mtod(m, void *), data + offset, frame_len);

	bzero(&rxs, sizeof(rxs));
	rxs.r_flags = IEEE80211_R_NF | IEEE80211_R_RSSI;
	rxs.c_nf = noise;
	rxs.c_rssi = (int8_t)(rssi - noise);
	if (rxd1 & (1U << 27))
		rxs.c_pktflags |= IEEE80211_RX_F_FAIL_FCSCRC;
	if (rxd1 & (1U << 26))
		rxs.c_pktflags |= IEEE80211_RX_F_FAIL_MMIC;
	/*
	 * mt7921_mac_fill_rx() reports a nonzero hardware cipher with no
	 * CLM/CM exception as authenticated/decrypted data.  Tell net80211
	 * that the hardware removed IV/MIC bytes so its software CCMP path
	 * does not try to decrypt a second time.
	 */
	sec_mode = (rxd1 >> 16) & 0x1f;
	if (sec_mode != 0 && (rxd1 & ((1U << 23) | (1U << 24))) == 0)
		rxs.c_pktflags |= IEEE80211_RX_F_DECRYPTED |
		    IEEE80211_RX_F_IV_STRIP | IEEE80211_RX_F_MMIC_STRIP |
		    IEEE80211_RX_F_MIC_STRIP;
	if (channel != 0) {
		rxs.r_flags |= IEEE80211_R_IEEE | IEEE80211_R_FREQ |
		    IEEE80211_R_BAND;
		rxs.c_ieee = channel;
		rxs.c_freq = ieee80211_ieee2mhz(channel,
		    channel <= 14 ? IEEE80211_CHAN_2GHZ : IEEE80211_CHAN_5GHZ);
		rxs.c_band = channel <= 14 ? IEEE80211_CHAN_2GHZ :
		    IEEE80211_CHAN_5GHZ;
	}
	if (ieee80211_add_rx_params(m, &rxs) == 0) {
		m_freem(m);
		goto drop;
	}
	if (sc->net80211_attached)
		(void)ieee80211_input_mimo_all(&sc->ic, m);
	else
		m_freem(m);
	return;

drop:
	mt7902_rx_drop(sc);
}

static void
mt7902_rx_data_poll(struct mt7902_softc *sc)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	uint32_t control;
	size_t dma_len;
	int idx, count, error;

	ring = &sc->rx_data;
	if (ring->desc == NULL)
		return;
	for (count = 0; count < ring->ndesc; count++) {
		idx = ring->tail;
		desc = &ring->desc[idx];
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) == 0)
			break;
		buf = &ring->buf[idx];
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTREAD);
		dma_len = (control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK;
		if (dma_len == 0 || dma_len > buf->len)
			mt7902_rx_drop(sc);
		else
			mt7902_rx_data_frame(sc, buf->vaddr, dma_len);
		error = mt7902_rx_rearm(sc, ring, idx);
		if (error != 0) {
			device_printf(sc->dev,
			    "RX data ring cursor collision at descriptor %d\n", idx);
			break;
		}
	}
}

static void
mt7902_rx_event_poll(struct mt7902_softc *sc)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	uint8_t *data;
	uint16_t mcu_len;
	uint32_t control;
	size_t dma_len, event_len;
	int idx, count, error;
	uint8_t seq, eid;

	ring = &sc->rx_event;
	if (ring->desc == NULL)
		return;
	for (count = 0; count < ring->ndesc; count++) {
		idx = ring->tail;
		desc = &ring->desc[idx];
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) == 0)
			break;
		buf = &ring->buf[idx];
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTREAD);
		data = buf->vaddr;
		dma_len = (control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK;
		mcu_len = 0;
		if (dma_len >= MT7902_RXD_SIZE && dma_len <= buf->len) {
			event_len = dma_len - MT7902_RXD_SIZE;
			memcpy(&mcu_len, data + MT7902_RXD_LEN_OFFSET,
			    sizeof(mcu_len));
			eid = data[MT7902_RXD_EID_OFFSET];
			seq = data[MT7902_RXD_SEQ_OFFSET];
			mtx_lock(&sc->mtx);
			sc->rx_event_count++;
			/* mt76 matches MCU replies by sequence; firmware replies can
			 * use EIDs other than the generic 0x06 event identifier.
			 */
			if (sc->mcu_waiting && seq == sc->mcu_wait_seq) {
				device_printf(sc->dev,
				    "MCU RX response cmd=%02x seq=%u eid=%02x len=%u status=%02x\n",
				    sc->mcu_wait_cmd, seq, eid,
				    le16toh(mcu_len), data[MT7902_RXD_SIZE - 4]);
				/* PATCH_SEM_CONTROL and PATCH_FINISH_REQ return
				 * their status in the final word of the RXD header,
				 * matching mt7921_mcu_parse_response().
				 */
				if (sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_SEM ||
				    sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_FINISH)
					sc->mcu_error = data[MT7902_RXD_SIZE - 4];
				else
					sc->mcu_error = 0;
				sc->mcu_response_len = MIN(event_len,
				    sizeof(sc->mcu_response));
				if (sc->mcu_response_len != 0)
					memcpy(sc->mcu_response,
					    data + MT7902_RXD_SIZE,
					    sc->mcu_response_len);
				sc->mcu_waiting = 0;
				cv_broadcast(&sc->mcu_cv);
			}
			mtx_unlock(&sc->mtx);
		} else {
			device_printf(sc->dev,
			    "invalid MCU event descriptor length (%ju bytes, buffer %ju)\n",
			    (uintmax_t)dma_len, (uintmax_t)buf->len);
		}
		error = mt7902_rx_rearm(sc, ring, idx);
		if (error != 0) {
			device_printf(sc->dev,
			    "RX event ring cursor collision at descriptor %d\n", idx);
			break;
		}
	}
}

static void
mt7902_intr(void *arg)
{
	struct mt7902_softc *sc;
	uint32_t status;

	sc = arg;
	status = mt7902_read(sc, MT7902_REG_WFDMA_INT_STA);
	if (status == 0 || status == 0xffffffff)
		return;
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, status);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
	mtx_lock(&sc->mtx);
	sc->irq_status |= status;
	sc->irq_last_status = status;
	sc->irq_count++;
	mtx_unlock(&sc->mtx);
	taskqueue_enqueue(taskqueue_thread, &sc->irq_task);
}

int
mt7902_irq_setup(struct mt7902_softc *sc)
{
	int error, count;

	count = 1;
	error = pci_alloc_msi(sc->dev, &count);
	if (error == 0)
		sc->msi_count = count;
	else
		sc->msi_count = 0;
	sc->irq_rid = sc->msi_count != 0 ? 1 : 0;
	sc->irq = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &sc->irq_rid,
	    RF_ACTIVE | (sc->msi_count == 0 ? RF_SHAREABLE : 0));
	if (sc->irq == NULL) {
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (ENXIO);
	}
	error = bus_setup_intr(sc->dev, sc->irq,
	    INTR_TYPE_NET | INTR_MPSAFE, NULL, mt7902_intr, sc,
	    &sc->irq_cookie);
	if (error != 0) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (error);
	}
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, 0xffffffff);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
	return (0);
}

void
mt7902_irq_teardown(struct mt7902_softc *sc)
{
	if (sc->mem != NULL)
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
	if (sc->irq_cookie != NULL) {
		bus_teardown_intr(sc->dev, sc->irq, sc->irq_cookie);
		sc->irq_cookie = NULL;
	}
	/* A queued worker may still be consuming an RX ring. Drain it only
	 * after the interrupt source is detached, and before the caller frees
	 * ring memory.
	 */
	taskqueue_drain(taskqueue_thread, &sc->irq_task);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);
	taskqueue_drain(taskqueue_thread, &sc->tx_task);
	if (sc->irq != NULL) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
	}
	if (sc->msi_count != 0) {
		pci_release_msi(sc->dev);
		sc->msi_count = 0;
	}
}

void
mt7902_irq_task(void *arg, int pending __unused)
{
	struct mt7902_softc *sc;
	uint32_t status, intr_sw;

	sc = arg;
	mtx_lock(&sc->mtx);
	status = sc->irq_status;
	sc->irq_status = 0;
	mtx_unlock(&sc->mtx);

	/*
	 * On PCIe the MCU can signal that data RX should be serviced through
	 * the software interrupt register rather than the host RX-done bit.
	 * This is the MT792x MCU_CMD_WAKE_RX_PCIE path.
	 */
	if (status & MT7902_INT_MCU_CMD) {
		intr_sw = mt7902_read(sc, MT7902_REG_MCU_CMD);
		if (intr_sw != 0 && intr_sw != 0xffffffff) {
			mt7902_write(sc, MT7902_REG_MCU_CMD, intr_sw);
			if (intr_sw & MT7902_MCU_CMD_WAKE_RX_PCIE)
				status |= MT7902_INT_RX_DONE_DATA;
		}
	}
	if (status & MT7902_INT_RX_DONE_WM)
		mt7902_rx_event_poll(sc);
	if (status & MT7902_INT_RX_DONE_DATA)
		taskqueue_enqueue(taskqueue_thread, &sc->rx_task);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
}

void
mt7902_rx_task(void *arg __unused, int pending __unused)
{
	struct mt7902_softc *sc = arg;

	mt7902_rx_data_poll(sc);
}
