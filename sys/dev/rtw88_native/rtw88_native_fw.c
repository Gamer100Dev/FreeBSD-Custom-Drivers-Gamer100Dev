/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * Native FreeBSD firmware handling and RTL8822B DDMA bootstrap.
 *
 * The control flow and hardware constants are ported from the vendored
 * RTW88 8822B implementation. All USB operations use FreeBSD USBDI.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/firmware.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <dev/usb/usb.h>

#include "if_rtw88_native.h"

#define RTW88_NATIVE_REG_SYS_FUNC_EN		0x0002
#define RTW88_NATIVE_REG_SYS_CLK_CTRL		0x0008
#define RTW88_NATIVE_REG_RSV_CTRL		0x001c
#define RTW88_NATIVE_REG_TXDMA_STATUS		0x0210
#define RTW88_NATIVE_REG_CPU_DMEM_CON		0x1080

#define RTW88_NATIVE_BIT_FEN_CPUEN		(1U << 2)
#define RTW88_NATIVE_BIT_WLMCU_IOIF		(1U << 0)
#define RTW88_NATIVE_BIT_CPU_CLK_EN		(1U << 6)
#define RTW88_NATIVE_BIT_WL_PLATFORM_RST	(1U << 0)
#define RTW88_NATIVE_BIT_BTI_PAGE_OVF		(1U << 2)

#define RTW88_NATIVE_FW_PAGE_MASK		0x0fffU
#define RTW88_NATIVE_FW_POLL_COUNT		1000
#define RTW88_NATIVE_FW_POLL_DELAY_US		100

struct rtw88_native_fw_header {
	uint16_t	signature;
	uint8_t		category;
	uint8_t		function;
	uint16_t	version;
	uint8_t		subversion;
	uint8_t		subindex;
	uint32_t	rsvd;
	uint32_t	feature;
	uint8_t		month;
	uint8_t		day;
	uint8_t		hour;
	uint8_t		minute;
	uint16_t	year;
	uint16_t	rsvd3;
	uint8_t		mem_usage;
	uint8_t		rsvd4[3];
	uint16_t	h2c_fmt_ver;
	uint16_t	rsvd5;
	uint32_t	dmem_addr;
	uint32_t	dmem_size;
	uint32_t	rsvd6;
	uint32_t	rsvd7;
	uint32_t	imem_size;
	uint32_t	emem_size;
	uint32_t	emem_addr;
	uint32_t	imem_addr;
} __packed;
CTASSERT(sizeof(struct rtw88_native_fw_header) == RTW88_NATIVE_FW_HEADER_SIZE);

struct rtw88_native_fw_backup {
	uint8_t		txdma_pq_map_hi;
	uint8_t		cr;
	uint16_t	fifopage_info_1;
	uint32_t	rqpn_ctrl_2;
	uint8_t		bcn_ctrl;
};

/*
 * The firmware header lengths describe payload bytes; every present memory
 * section also carries an eight-byte checksum trailer. Check arithmetic in
 * 64 bits before constructing any pointer into the image.
 */
static int
rtw88_native_fw_parse(struct rtw88_native_softc *sc)
{
	struct rtw88_native_fw_header hdr;
	const uint8_t *data;
	const struct firmware *fw;
	uint64_t dmem_size, imem_size, emem_size, expected;
	uint64_t dmem_addr, imem_addr, emem_addr;
	size_t offset;

	fw = sc->firmware;
	if (fw == NULL || fw->data == NULL ||
	    fw->datasize < RTW88_NATIVE_FW_HEADER_SIZE)
		return (EINVAL);

	memcpy(&hdr, fw->data, sizeof(hdr));
	dmem_size = le32toh(hdr.dmem_size);
	imem_size = le32toh(hdr.imem_size);
	emem_size = (hdr.mem_usage & RTW88_NATIVE_FW_MEM_EMEM) ?
	    le32toh(hdr.emem_size) : 0;

	if (dmem_size == 0 || imem_size == 0)
		return (EINVAL);
	if (dmem_size > SIZE_MAX - RTW88_NATIVE_FW_CHECKSUM_SIZE ||
	    imem_size > SIZE_MAX - RTW88_NATIVE_FW_CHECKSUM_SIZE ||
	    emem_size > SIZE_MAX - RTW88_NATIVE_FW_CHECKSUM_SIZE)
		return (EOVERFLOW);

	dmem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;
	imem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;
	if (emem_size != 0)
		emem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;

	expected = RTW88_NATIVE_FW_HEADER_SIZE + dmem_size +
	    imem_size + emem_size;
	if (expected != fw->datasize)
		return (EINVAL);

	dmem_addr = le32toh(hdr.dmem_addr) & ~0x80000000U;
	imem_addr = le32toh(hdr.imem_addr) & ~0x80000000U;
	emem_addr = le32toh(hdr.emem_addr) & ~0x80000000U;
	if (dmem_size > UINT32_MAX || imem_size > UINT32_MAX ||
	    emem_size > UINT32_MAX ||
	    dmem_addr + dmem_size > (uint64_t)UINT32_MAX + 1 ||
	    imem_addr + imem_size > (uint64_t)UINT32_MAX + 1 ||
	    (emem_size != 0 &&
	    emem_addr + emem_size > (uint64_t)UINT32_MAX + 1))
		return (EOVERFLOW);

	data = fw->data;
	offset = RTW88_NATIVE_FW_HEADER_SIZE;
	sc->fw_layout.dmem = data + offset;
	sc->fw_layout.dmem_size = (size_t)dmem_size;
	sc->fw_layout.dmem_addr = (uint32_t)dmem_addr;
	offset += (size_t)dmem_size;

	sc->fw_layout.imem = data + offset;
	sc->fw_layout.imem_size = (size_t)imem_size;
	sc->fw_layout.imem_addr = (uint32_t)imem_addr;
	offset += (size_t)imem_size;

	if (emem_size != 0) {
		sc->fw_layout.emem = data + offset;
		sc->fw_layout.emem_size = (size_t)emem_size;
		sc->fw_layout.emem_addr = (uint32_t)emem_addr;
	} else {
		sc->fw_layout.emem = NULL;
		sc->fw_layout.emem_size = 0;
		sc->fw_layout.emem_addr = 0;
	}

	return (0);
}

int
rtw88_native_firmware_get(struct rtw88_native_softc *sc)
{
	const struct firmware *fw;
	int error;

	if (sc == NULL)
		return (EINVAL);
	if (sc->firmware != NULL)
		return (EALREADY);

	/*
	 * FIRMWARE(9) resolves this relative image name below /boot/firmware
	 * on FreeBSD 15+, and may perform filesystem I/O. Call only from a
	 * sleepable context with no driver mutex held.
	 */
	fw = firmware_get(RTW88_NATIVE_FW_NAME);
	if (fw == NULL)
		return (ENOENT);

	sc->firmware = fw;
	error = rtw88_native_fw_parse(sc);
	if (error != 0) {
		sc->firmware = NULL;
		bzero(&sc->fw_layout, sizeof(sc->fw_layout));
		firmware_put(fw, 0);
		return (error);
	}

	device_printf(sc->dev,
	    "validated %s: %zu bytes, DMEM %zu, IMEM %zu, EMEM %zu\n",
	    RTW88_NATIVE_FW_NAME, fw->datasize,
	    sc->fw_layout.dmem_size, sc->fw_layout.imem_size,
	    sc->fw_layout.emem_size);
	return (0);
}

void
rtw88_native_firmware_put(struct rtw88_native_softc *sc)
{
	const struct firmware *fw;

	if (sc == NULL || sc->firmware == NULL)
		return;

	fw = sc->firmware;
	sc->firmware = NULL;
	bzero(&sc->fw_layout, sizeof(sc->fw_layout));
	firmware_put(fw, 0);
}

static int
rtw88_native_fw_read8(struct rtw88_native_softc *sc, uint16_t reg,
    uint8_t *value)
{
	return (rtw88_native_reg_read8(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_read16(struct rtw88_native_softc *sc, uint16_t reg,
    uint16_t *value)
{
	return (rtw88_native_reg_read16(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_read32(struct rtw88_native_softc *sc, uint16_t reg,
    uint32_t *value)
{
	return (rtw88_native_reg_read32(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_write8(struct rtw88_native_softc *sc, uint16_t reg,
    uint8_t value)
{
	return (rtw88_native_reg_write8(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_write16(struct rtw88_native_softc *sc, uint16_t reg,
    uint16_t value)
{
	return (rtw88_native_reg_write16(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_write32(struct rtw88_native_softc *sc, uint16_t reg,
    uint32_t value)
{
	return (rtw88_native_reg_write32(sc, reg, value) ==
	    USB_ERR_NORMAL_COMPLETION ? 0 : EIO);
}

static int
rtw88_native_fw_poll32(struct rtw88_native_softc *sc, uint16_t reg,
    uint32_t mask, uint32_t expected)
{
	uint32_t value;
	int i;

	for (i = 0; i < RTW88_NATIVE_FW_POLL_COUNT; i++) {
		if (rtw88_native_fw_read32(sc, reg, &value) != 0)
			return (EIO);
		if ((value & mask) == expected)
			return (0);
		DELAY(RTW88_NATIVE_FW_POLL_DELAY_US);
	}
	return (ETIMEDOUT);
}

static int
rtw88_native_fw_poll16(struct rtw88_native_softc *sc, uint16_t reg,
    uint16_t mask, uint16_t expected)
{
	uint16_t value;
	int i;

	for (i = 0; i < RTW88_NATIVE_FW_POLL_COUNT; i++) {
		if (rtw88_native_fw_read16(sc, reg, &value) != 0)
			return (EIO);
		if ((value & mask) == expected)
			return (0);
		DELAY(RTW88_NATIVE_FW_POLL_DELAY_US);
	}
	return (ETIMEDOUT);
}

/*
 * Realtek USB transfers the firmware DMA source through the high-priority
 * beacon queue. The RTL8822B three-OUT-endpoint mapping sends that queue to
 * endpoint index zero. This builds only the 48-byte TX descriptor and
 * payload format required for these firmware packets.
 */
static int
rtw88_native_fw_send_reserved_packet(struct rtw88_native_softc *sc,
    uint16_t page, const uint8_t *data, size_t size)
{
	struct rtw88_native_txdesc_params params;
	uint8_t *packet;
	uint16_t old_page, page_state, wire_size;
	uint8_t old_bcn, old_cr1, value;
	size_t pad, packet_size, i;
	int error, restore_error;

	if (data == NULL || size == 0 || size > RTW88_NATIVE_TXDMA_CHUNK)
		return (EINVAL);

	pad = (((size + RTW88_NATIVE_TX_DESC_SIZE) & 511U) == 0) ? 1 : 0;
	wire_size = (uint16_t)(size + pad);
	packet_size = RTW88_NATIVE_TX_DESC_SIZE + wire_size;
	packet = malloc(packet_size, M_DEVBUF, M_WAITOK | M_ZERO);
	if (packet == NULL)
		return (ENOMEM);

	/*
	 * Firmware DMA uses a BEACON/high-priority descriptor, with the payload
	 * beginning immediately after the 48-byte RTL8822B descriptor area.
	 * The extra USB-boundary byte is padding only and is not part of the
	 * firmware segment copied by DDMA.
	 */
	bzero(&params, sizeof(params));
	params.packet_size = wire_size;
	params.descriptor_offset = RTW88_NATIVE_TX_DESC_SIZE;
	params.queue_select = RTW88_NATIVE_TXQSEL_BEACON;
	params.last_segment = true;
	error = rtw88_native_txdesc_build(packet,
	    RTW88_NATIVE_TX_DESC_SIZE, &params);
	if (error != 0)
		goto out;
	memcpy(packet + RTW88_NATIVE_TX_DESC_SIZE, data, size);

	/* Save the active beacon-page state before temporarily selecting page. */
	error = rtw88_native_fw_read16(sc,
	    RTW88_NATIVE_REG_FIFOPAGE_CTRL_2, &old_page);
	if (error != 0)
		goto out;
	error = rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_BCN_CTRL, &old_bcn);
	if (error != 0)
		goto out;
	error = rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_CR + 1, &old_cr1);
	if (error != 0)
		goto out;

	error = rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_CTRL_2,
	    (page & RTW88_NATIVE_FW_PAGE_MASK) | RTW88_NATIVE_BIT_BCN_VALID_V1);
	if (error != 0)
		goto restore;
	value = old_cr1 | (RTW88_NATIVE_BIT_ENSWBCN >> 8);
	error = rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CR + 1, value);
	if (error != 0)
		goto restore;
	value = (old_bcn & ~RTW88_NATIVE_BIT_EN_BCN_FUNCTION) |
	    RTW88_NATIVE_BIT_DIS_TSF_UDT;
	error = rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_BCN_CTRL, value);
	if (error != 0)
		goto restore;

	error = (rtw88_native_usb_tx_sync(sc, RTW88_NATIVE_TXQ_HIGH, packet,
	    (uint16_t)packet_size) == USB_ERR_NORMAL_COMPLETION) ? 0 : EIO;
	if (error != 0)
		goto restore;

	error = ETIMEDOUT;
	for (i = 0; i < 100; i++) {
		if (rtw88_native_fw_read16(sc,
		    RTW88_NATIVE_REG_FIFOPAGE_CTRL_2, &page_state) != 0) {
			error = EIO;
			break;
		}
		if (page_state & RTW88_NATIVE_BIT_BCN_VALID_V1) {
			error = 0;
			break;
		}
		pause("rtw88fw", MAX(1, hz / 1000));
	}

restore:
	/*
	 * Restore all three registers even if a prior operation failed. Preserve
	 * the first error and report a restore failure only if there was no
	 * earlier failure.
	 */
	restore_error = 0;
	if (rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_CTRL_2,
	    old_page) != 0)
		restore_error = EIO;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_BCN_CTRL,
	    old_bcn) != 0)
		restore_error = EIO;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CR + 1,
	    old_cr1) != 0)
		restore_error = EIO;
	if (error == 0 && restore_error != 0)
		error = restore_error;
out:
	free(packet, M_DEVBUF);
	return (error);
}

static int
rtw88_native_fw_ddma_chunk(struct rtw88_native_softc *sc, uint32_t dst,
    uint32_t length, bool first)
{
	uint32_t control;
	int error;

	if (length == 0 || length > RTW88_NATIVE_TXDMA_CHUNK)
		return (EINVAL);
	error = rtw88_native_fw_poll32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    RTW88_NATIVE_BIT_DDMACH0_OWN, 0);
	if (error != 0)
		return (error);

	control = RTW88_NATIVE_BIT_DDMACH0_CHKSUM_EN |
	    RTW88_NATIVE_BIT_DDMACH0_OWN |
	    (length & RTW88_NATIVE_BIT_DDMACH0_DLEN);
	if (!first)
		control |= RTW88_NATIVE_BIT_DDMACH0_CHKSUM_CONT;

	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_DDMA_CH0SA,
	    RTW88_NATIVE_OCPBASE_TXBUF + RTW88_NATIVE_TX_DESC_SIZE) != 0 ||
	    rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_DDMA_CH0DA, dst) != 0 ||
	    rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    control) != 0)
		return (EIO);

	return (rtw88_native_fw_poll32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    RTW88_NATIVE_BIT_DDMACH0_OWN, 0));
}

static int
rtw88_native_fw_check_segment_checksum(struct rtw88_native_softc *sc,
    uint32_t dst)
{
	uint32_t ddma_status;
	uint8_t fw_ctrl;
	uint8_t set_bits;

	if (rtw88_native_fw_read32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    &ddma_status) != 0 ||
	    rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    &fw_ctrl) != 0)
		return (EIO);

	if (dst < RTW88_NATIVE_OCPBASE_DMEM) {
		set_bits = RTW88_NATIVE_BIT_IMEM_DW_OK |
		    RTW88_NATIVE_BIT_IMEM_CHKSUM_OK;
		if (ddma_status & RTW88_NATIVE_BIT_DDMACH0_CHKSUM_STS) {
			fw_ctrl |= RTW88_NATIVE_BIT_IMEM_DW_OK;
			fw_ctrl &= ~RTW88_NATIVE_BIT_IMEM_CHKSUM_OK;
			(void)rtw88_native_fw_write8(sc,
			    RTW88_NATIVE_REG_MCUFW_CTRL, fw_ctrl);
			return (EBADMSG);
		}
	} else {
		set_bits = RTW88_NATIVE_BIT_DMEM_DW_OK |
		    RTW88_NATIVE_BIT_DMEM_CHKSUM_OK;
		if (ddma_status & RTW88_NATIVE_BIT_DDMACH0_CHKSUM_STS) {
			fw_ctrl |= RTW88_NATIVE_BIT_DMEM_DW_OK;
			fw_ctrl &= ~RTW88_NATIVE_BIT_DMEM_CHKSUM_OK;
			(void)rtw88_native_fw_write8(sc,
			    RTW88_NATIVE_REG_MCUFW_CTRL, fw_ctrl);
			return (EBADMSG);
		}
	}

	fw_ctrl |= set_bits;
	return (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    fw_ctrl));
}

static int
rtw88_native_fw_download_segment(struct rtw88_native_softc *sc,
    const uint8_t *data, size_t size, uint32_t dst)
{
	uint32_t ddma_control;
	size_t offset, chunk;
	bool first;
	int error;

	if (data == NULL || size == 0)
		return (EINVAL);

	if (rtw88_native_fw_read32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    &ddma_control) != 0)
		return (EIO);
	ddma_control |= RTW88_NATIVE_BIT_DDMACH0_RESET_CHKSUM_STS;
	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_DDMA_CH0CTRL,
	    ddma_control) != 0)
		return (EIO);

	first = true;
	for (offset = 0; offset < size; offset += chunk) {
		chunk = MIN(size - offset, RTW88_NATIVE_TXDMA_CHUNK);
		error = rtw88_native_fw_send_reserved_packet(sc, 0,
		    data + offset, chunk);
		if (error != 0)
			return (error);
		error = rtw88_native_fw_ddma_chunk(sc, dst + (uint32_t)offset,
		    (uint32_t)chunk, first);
		if (error != 0)
			return (error);
		first = false;
	}

	return (rtw88_native_fw_check_segment_checksum(sc, dst));
}

static int
rtw88_native_fw_cpu_enable(struct rtw88_native_softc *sc, bool enable)
{
	uint8_t value;

	if (enable) {
		if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_RSV_CTRL + 1,
		    &value) != 0)
			return (EIO);
		value |= RTW88_NATIVE_BIT_WLMCU_IOIF;
		if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_RSV_CTRL + 1,
		    value) != 0)
			return (EIO);
		if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_SYS_FUNC_EN + 1,
		    &value) != 0)
			return (EIO);
		value |= RTW88_NATIVE_BIT_FEN_CPUEN;
		return (rtw88_native_fw_write8(sc,
		    RTW88_NATIVE_REG_SYS_FUNC_EN + 1, value));
	}

	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_SYS_FUNC_EN + 1,
	    &value) != 0)
		return (EIO);
	value &= ~RTW88_NATIVE_BIT_FEN_CPUEN;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_SYS_FUNC_EN + 1,
	    value) != 0)
		return (EIO);
	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_RSV_CTRL + 1,
	    &value) != 0)
		return (EIO);
	value &= ~RTW88_NATIVE_BIT_WLMCU_IOIF;
	return (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_RSV_CTRL + 1,
	    value));
}

static int
rtw88_native_fw_backup_and_prepare(struct rtw88_native_softc *sc,
    struct rtw88_native_fw_backup *backup, bool *backup_valid)
{
	uint32_t rqpn;
	uint8_t value;

	*backup_valid = false;

	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_TXDMA_PQ_MAP + 1,
	    &backup->txdma_pq_map_hi) != 0 ||
	    rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_CR,
	    &backup->cr) != 0 ||
	    rtw88_native_fw_read16(sc, RTW88_NATIVE_REG_FIFOPAGE_INFO_1,
	    &backup->fifopage_info_1) != 0 ||
	    rtw88_native_fw_read32(sc, RTW88_NATIVE_REG_RQPN_CTRL_2,
	    &backup->rqpn_ctrl_2) != 0 ||
	    rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_BCN_CTRL,
	    &backup->bcn_ctrl) != 0)
		return (EIO);
	*backup_valid = true;

	/* Map high queue to high priority and enable only TX DMA for FW loading. */
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_TXDMA_PQ_MAP + 1,
	    (uint8_t)(3U << 6)) != 0 ||
	    rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CR,
	    RTW88_NATIVE_BIT_HCI_TXDMA_EN | RTW88_NATIVE_BIT_TXDMA_EN) != 0 ||
	    rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_H2CQ_CSR,
	    RTW88_NATIVE_BIT_H2CQ_FULL) != 0 ||
	    rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_INFO_1,
	    0x0200) != 0)
		return (EIO);

	rqpn = backup->rqpn_ctrl_2 | RTW88_NATIVE_BIT_LD_RQPN;
	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_RQPN_CTRL_2,
	    rqpn) != 0)
		return (EIO);

	value = (backup->bcn_ctrl & ~RTW88_NATIVE_BIT_EN_BCN_FUNCTION) |
	    RTW88_NATIVE_BIT_DIS_TSF_UDT;
	return (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_BCN_CTRL, value));
}

static int
rtw88_native_fw_restore(struct rtw88_native_softc *sc,
    const struct rtw88_native_fw_backup *backup)
{
	int error = 0;

	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_TXDMA_PQ_MAP + 1,
	    backup->txdma_pq_map_hi) != 0)
		error = EIO;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CR,
	    backup->cr) != 0)
		error = EIO;
	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_H2CQ_CSR,
	    RTW88_NATIVE_BIT_H2CQ_FULL) != 0)
		error = EIO;
	if (rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_FIFOPAGE_INFO_1,
	    backup->fifopage_info_1) != 0)
		error = EIO;
	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_RQPN_CTRL_2,
	    backup->rqpn_ctrl_2 | RTW88_NATIVE_BIT_LD_RQPN) != 0)
		error = EIO;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_BCN_CTRL,
	    backup->bcn_ctrl) != 0)
		error = EIO;

	return (error);
}

static int
rtw88_native_fw_reset_platform(struct rtw88_native_softc *sc)
{
	uint8_t value;

	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_CPU_DMEM_CON + 2,
	    &value) != 0)
		return (EIO);
	value &= ~RTW88_NATIVE_BIT_WL_PLATFORM_RST;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CPU_DMEM_CON + 2,
	    value) != 0)
		return (EIO);

	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_SYS_CLK_CTRL + 1,
	    &value) != 0)
		return (EIO);
	value &= ~RTW88_NATIVE_BIT_CPU_CLK_EN;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_SYS_CLK_CTRL + 1,
	    value) != 0)
		return (EIO);
	value |= RTW88_NATIVE_BIT_CPU_CLK_EN;
	if (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_SYS_CLK_CTRL + 1,
	    value) != 0)
		return (EIO);

	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_CPU_DMEM_CON + 2,
	    &value) != 0)
		return (EIO);
	value |= RTW88_NATIVE_BIT_WL_PLATFORM_RST;
	return (rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_CPU_DMEM_CON + 2,
	    value));
}

static int
rtw88_native_fw_start(struct rtw88_native_softc *sc)
{
	uint16_t value;

	if (rtw88_native_fw_read16(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    &value) != 0)
		return (EIO);
	value = (value & 0x3800U) | RTW88_NATIVE_BIT_MCUFWDL_EN;
	if (rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    value) != 0)
		return (EIO);

	{
		int error;

		error = rtw88_native_fw_download_segment(sc, sc->fw_layout.dmem,
		    sc->fw_layout.dmem_size, sc->fw_layout.dmem_addr);
		if (error != 0)
			return (error);
		error = rtw88_native_fw_download_segment(sc, sc->fw_layout.imem,
		    sc->fw_layout.imem_size, sc->fw_layout.imem_addr);
		if (error != 0)
			return (error);
		if (sc->fw_layout.emem_size != 0) {
			error = rtw88_native_fw_download_segment(sc,
			    sc->fw_layout.emem, sc->fw_layout.emem_size,
			    sc->fw_layout.emem_addr);
			if (error != 0)
				return (error);
		}
	}

	return (0);
}

#define RTW88_NATIVE_H2C_PACKET_SIZE		32U
#define RTW88_NATIVE_H2C_HEADER_SIZE		8U
#define RTW88_NATIVE_H2C_CATEGORY		1U
#define RTW88_NATIVE_H2C_COMMAND_ID		0xffU
#define RTW88_NATIVE_H2C_GENERAL_INFO		0x0dU
#define RTW88_NATIVE_H2C_PHYDM_INFO		0x11U
#define RTW88_NATIVE_TXQSEL_H2C			19U
#define RTW88_NATIVE_FW_RF_2T2R			2U

static void
rtw88_native_fw_h2c_put32(uint8_t packet[RTW88_NATIVE_H2C_PACKET_SIZE],
    unsigned int word, uint32_t value)
{
	value = htole32(value);
	memcpy(packet + (word * sizeof(value)), &value, sizeof(value));
}

/*
 * Send a 32-byte firmware-offload H2C packet using the same USB path as
 * Linux rtw_usb_write_data_h2c(): prepend the 48-byte 8822B TX descriptor,
 * select queue 19 (H2C), and route it through bulk-OUT endpoint 0 / EP 0x05.
 * A synchronous completion keeps the packet and descriptor alive until the
 * host controller has finished transferring them.
 */
static int
rtw88_native_fw_send_h2c_packet(struct rtw88_native_softc *sc,
    const uint8_t packet[RTW88_NATIVE_H2C_PACKET_SIZE])
{
	struct rtw88_native_txdesc_params params;
	uint8_t tx_packet[RTW88_NATIVE_TX_DESC_SIZE +
	    RTW88_NATIVE_H2C_PACKET_SIZE];
	usb_error_t uerr;
	uint16_t sequence;

	if (sc == NULL || packet == NULL || !sc->firmware_running ||
	    !sc->mac_initialized || !sc->phy_initialized ||
	    !sc->xfers_initialized || sc->dying)
		return (EINVAL);

	bzero(&params, sizeof(params));
	params.packet_size = RTW88_NATIVE_H2C_PACKET_SIZE;
	params.descriptor_offset = 0;
	params.queue_select = RTW88_NATIVE_TXQSEL_H2C;

	bzero(tx_packet, sizeof(tx_packet));
	if (rtw88_native_txdesc_build(tx_packet,
	    RTW88_NATIVE_TX_DESC_SIZE, &params) != 0)
		return (EINVAL);
	memcpy(tx_packet + RTW88_NATIVE_TX_DESC_SIZE, packet,
	    RTW88_NATIVE_H2C_PACKET_SIZE);

	sequence = sc->fw_h2c_seq;
	uerr = rtw88_native_usb_tx_sync(sc, RTW88_NATIVE_TXQ_HIGH,
	    tx_packet, sizeof(tx_packet));
	/* Linux advances the offload sequence after each attempted H2C packet. */
	sc->fw_h2c_seq = (uint16_t)(sequence + 1);
	if (uerr != USB_ERR_NORMAL_COMPLETION) {
		sc->fw_h2c_error_count++;
		sc->fw_h2c_error = (int)uerr;
		device_printf(sc->dev,
		    "firmware H2C transfer failed: seq=%u qsel=%u usb_error=%d\n",
		    sequence, RTW88_NATIVE_TXQSEL_H2C, (int)uerr);
		return (EIO);
	}
	sc->fw_h2c_error = 0;
	return (0);
}

/*
 * Linux main.c sends both offload packets after USB HCI start. General Info
 * advertises the reserved firmware TX buffer boundary; PHYDM Info identifies
 * the RFE network, RF topology, silicon cut and active antenna paths. The
 * RTL8822BU in this project is 2T2R / RFE option 3.
 */
int
rtw88_native_firmware_send_initial_h2c(struct rtw88_native_softc *sc)
{
	uint8_t packet[RTW88_NATIVE_H2C_PACKET_SIZE];
	uint32_t dword0, dword1, dword2;
	uint16_t fw_boundary;
	int error;

	if (sc == NULL || !sc->firmware_running || !sc->mac_initialized ||
	    !sc->phy_initialized || !sc->xfers_initialized ||
	    sc->bulkout_num != 3 || sc->dying)
		return (EINVAL);

	if (sc->rsvd_fw_txbuf_addr <= sc->rsvd_boundary)
		return (EINVAL);
	fw_boundary = sc->rsvd_fw_txbuf_addr - sc->rsvd_boundary;
	if (fw_boundary > 0xff)
		return (ERANGE);

	/* H2C_PKT_GENERAL_INFO (0x0d), total length 8 + 4 bytes. */
	bzero(packet, sizeof(packet));
	dword0 = RTW88_NATIVE_H2C_CATEGORY |
	    (RTW88_NATIVE_H2C_COMMAND_ID << 8) |
	    (RTW88_NATIVE_H2C_GENERAL_INFO << 16);
	dword1 = (RTW88_NATIVE_H2C_HEADER_SIZE + 4U) |
	    ((uint32_t)sc->fw_h2c_seq << 16);
	dword2 = (uint32_t)fw_boundary << 16;
	rtw88_native_fw_h2c_put32(packet, 0, dword0);
	rtw88_native_fw_h2c_put32(packet, 1, dword1);
	rtw88_native_fw_h2c_put32(packet, 2, dword2);

	error = rtw88_native_fw_send_h2c_packet(sc, packet);
	if (error != 0)
		return (error);
	sc->fw_h2c_general_count++;

	/* H2C_PKT_PHYDM_INFO (0x11), total length 8 + 8 bytes. */
	bzero(packet, sizeof(packet));
	dword0 = RTW88_NATIVE_H2C_CATEGORY |
	    (RTW88_NATIVE_H2C_COMMAND_ID << 8) |
	    (RTW88_NATIVE_H2C_PHYDM_INFO << 16);
	dword1 = (RTW88_NATIVE_H2C_HEADER_SIZE + 8U) |
	    ((uint32_t)sc->fw_h2c_seq << 16);
	dword2 = (uint32_t)sc->efuse.rfe_option |
	    ((uint32_t)(sc->rf_path_num > 1 ?
	    RTW88_NATIVE_FW_RF_2T2R : 4U) << 8) |
	    ((uint32_t)sc->cut_version << 16) |
	    ((uint32_t)(sc->antenna_tx & 0x0fU) << 24) |
	    ((uint32_t)(sc->antenna_rx & 0x0fU) << 28);
	rtw88_native_fw_h2c_put32(packet, 0, dword0);
	rtw88_native_fw_h2c_put32(packet, 1, dword1);
	rtw88_native_fw_h2c_put32(packet, 2, dword2);
	rtw88_native_fw_h2c_put32(packet, 3, 0);

	error = rtw88_native_fw_send_h2c_packet(sc, packet);
	if (error != 0)
		return (error);
	sc->fw_h2c_phydm_count++;
	device_printf(sc->dev,
	    "firmware initial H2C complete: general=%u PHYDM=%u boundary=%u RF=%uT%uR RFE=%u cut=%u\n",
	    (unsigned int)sc->fw_h2c_general_count,
	    (unsigned int)sc->fw_h2c_phydm_count,
	    (unsigned int)fw_boundary, sc->rf_path_num, sc->rf_path_num,
	    sc->efuse.rfe_option, sc->cut_version);
	return (0);
}

/*
 * Complete the RTL8822B (3081 MCU) DDMA firmware transfer and ready check.
 * This deliberately runs only when explicitly requested by the future
 * hardware-init path; it is not called from USB attach.
 */
int
rtw88_native_firmware_download(struct rtw88_native_softc *sc)
{
	struct rtw88_native_fw_backup backup;
	uint16_t fw_ctrl;
	uint8_t fw_ctrl8;
	bool backup_valid;
	int error, restore_error;

	if (sc == NULL || sc->firmware == NULL || !sc->xfers_initialized ||
	    !sc->powered || sc->dying)
		return (EINVAL);
	if (sc->firmware_running)
		return (EALREADY);
	sc->firmware_running = 0;
	sc->fw_h2c_seq = 0;
	sc->fw_h2c_error = 0;

	error = rtw88_native_fw_cpu_enable(sc, false);
	if (error != 0)
		return (error);

	error = rtw88_native_fw_backup_and_prepare(sc, &backup, &backup_valid);
	if (error != 0) {
		if (backup_valid)
			(void)rtw88_native_fw_restore(sc, &backup);
		goto fail_cpu;
	}

	error = rtw88_native_fw_reset_platform(sc);
	if (error == 0)
		error = rtw88_native_fw_start(sc);

	restore_error = rtw88_native_fw_restore(sc, &backup);
	if (error == 0 && restore_error != 0)
		error = restore_error;
	if (error != 0)
		goto fail_cpu;

	/* Signal checksum completion, release FWDL_EN, then start the MCU. */
	if (rtw88_native_fw_write32(sc, RTW88_NATIVE_REG_TXDMA_STATUS,
	    RTW88_NATIVE_BIT_BTI_PAGE_OVF) != 0 ||
	    rtw88_native_fw_read16(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    &fw_ctrl) != 0)
		goto fail_cpu;
	if ((fw_ctrl & RTW88_NATIVE_BIT_CHECKSUM_OK) !=
	    RTW88_NATIVE_BIT_CHECKSUM_OK)
		goto fail_cpu;
	fw_ctrl |= RTW88_NATIVE_BIT_FW_DW_RDY;
	fw_ctrl &= ~RTW88_NATIVE_BIT_MCUFWDL_EN;
	if (rtw88_native_fw_write16(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    fw_ctrl) != 0)
		goto fail_cpu;

	error = rtw88_native_fw_cpu_enable(sc, true);
	if (error != 0)
		return (error);
	error = rtw88_native_fw_poll16(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    RTW88_NATIVE_FW_READY_MASK, RTW88_NATIVE_FW_READY);
	if (error != 0) {
		device_printf(sc->dev, "firmware did not reach READY state: %d\n",
		    error);
		(void)rtw88_native_fw_cpu_enable(sc, false);
		return (error);
	}
	sc->firmware_running = 1;
	return (0);

fail_cpu:
	/* Leave the MCU download-enable bit clear and return CPU control. */
	if (rtw88_native_fw_read8(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
	    &fw_ctrl8) == 0) {
		fw_ctrl8 &= ~RTW88_NATIVE_BIT_MCUFWDL_EN;
		(void)rtw88_native_fw_write8(sc, RTW88_NATIVE_REG_MCUFW_CTRL,
		    fw_ctrl8);
	}
	(void)rtw88_native_fw_cpu_enable(sc, true);
	return (error != 0 ? error : EIO);
}
