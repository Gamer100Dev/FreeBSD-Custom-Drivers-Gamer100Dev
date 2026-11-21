/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/firmware.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>

#include "if_rtw88_native.h"

#define RTW88_NATIVE_FW_MEM_EMEM	(1U << 4)
#define RTW88_NATIVE_FW_PAGE_SHIFT	12
#define RTW88_NATIVE_FW_REG_PAGE		RTW88_NATIVE_REG_MCUFW_CTRL

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

static int
rtw88_native_fw_parse(struct rtw88_native_softc *sc)
{
	struct rtw88_native_fw_header hdr;
	const uint8_t *data;
	const struct firmware *fw;
	uint64_t dmem_size, imem_size, emem_size, expected;
	size_t offset;

	fw = sc->firmware;
	if (fw == NULL || fw->data == NULL ||
	    fw->datasize < RTW88_NATIVE_FW_HEADER_SIZE)
		return (EINVAL);

	/*
	 * Copy the packed wire header before decoding fields.  This avoids
	 * unaligned accesses to the firmware buffer on strict-alignment CPUs.
	 */
	memcpy(&hdr, fw->data, sizeof(hdr));
	dmem_size = le32toh(hdr.dmem_size);
	imem_size = le32toh(hdr.imem_size);
	emem_size = (hdr.mem_usage & RTW88_NATIVE_FW_MEM_EMEM) ?
	    le32toh(hdr.emem_size) : 0;

	/* Each present memory image is followed by an 8-byte checksum trailer. */
	dmem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;
	imem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;
	if (emem_size != 0)
		emem_size += RTW88_NATIVE_FW_CHECKSUM_SIZE;

	expected = RTW88_NATIVE_FW_HEADER_SIZE;
	expected += dmem_size;
	expected += imem_size;
	expected += emem_size;
	if (expected != fw->datasize)
		return (EINVAL);

	data = fw->data;
	offset = RTW88_NATIVE_FW_HEADER_SIZE;
	sc->fw_layout.dmem = data + offset;
	sc->fw_layout.dmem_size = (size_t)dmem_size;
	sc->fw_layout.dmem_addr = le32toh(hdr.dmem_addr) & ~0x80000000U;
	offset += (size_t)dmem_size;

	sc->fw_layout.imem = data + offset;
	sc->fw_layout.imem_size = (size_t)imem_size;
	sc->fw_layout.imem_addr = le32toh(hdr.imem_addr) & ~0x80000000U;
	offset += (size_t)imem_size;

	if (emem_size != 0) {
		sc->fw_layout.emem = data + offset;
		sc->fw_layout.emem_size = (size_t)emem_size;
		sc->fw_layout.emem_addr = le32toh(hdr.emem_addr) & ~0x80000000U;
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
	 * FIRMWARE(9) permits the image name to be the relative path below
	 * /boot/firmware. firmware_get() must run without driver mutexes held
	 * and only once the filesystem is mounted. Call this from a sleepable
	 * device-init task, never from a USB callback or with sc->mtx held.
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

/*
 * Write one 4 KiB RTW88 firmware page through the USB vendor control pipe.
 * The 8822B USB HCI sends 196-byte chunks, then 8-byte and 1-byte tails.
 * This is a transport primitive; the full 8822B DDMA/firmware boot sequence
 * is deliberately implemented separately from this page writer.
 */
int
rtw88_native_fw_write_page(struct rtw88_native_softc *sc, uint8_t page,
    const uint8_t *data, size_t size)
{
	uint32_t ctrl;
	uint16_t actlen;
	uint16_t addr;
	size_t chunk;
	usb_error_t uerr;
	int error;

	if (sc == NULL || data == NULL || size == 0 ||
	    size > RTW88_NATIVE_FW_PAGE_SIZE || page > 7)
		return (EINVAL);

	error = rtw88_native_reg_read32(sc, RTW88_NATIVE_FW_REG_PAGE, &ctrl);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	ctrl &= ~RTW88_NATIVE_FW_PAGE_MASK;
	ctrl |= ((uint32_t)page << RTW88_NATIVE_FW_PAGE_SHIFT) &
	    RTW88_NATIVE_FW_PAGE_MASK;
	uerr = rtw88_native_reg_write32(sc, RTW88_NATIVE_FW_REG_PAGE, ctrl);
	if (uerr != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	addr = RTW88_NATIVE_FW_START_ADDRESS;
	while (size != 0) {
		if (size >= RTW88_NATIVE_FW_USB_BLOCK_SIZE)
			chunk = RTW88_NATIVE_FW_USB_BLOCK_SIZE;
		else if (size >= 8)
			chunk = 8;
		else
			chunk = 1;

		actlen = 0;
		uerr = rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE,
		    addr, __DECONST(uint8_t *, data), (uint16_t)chunk, &actlen);
		if (uerr != USB_ERR_NORMAL_COMPLETION || actlen != chunk)
			return (EIO);

		addr += (uint16_t)chunk;
		data += chunk;
		size -= chunk;
	}
	return (0);
}
