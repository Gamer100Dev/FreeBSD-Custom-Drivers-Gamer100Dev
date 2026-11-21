/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#ifndef _IF_RTW88_NATIVE_H_
#define _IF_RTW88_NATIVE_H_

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/firmware.h>
#include <sys/mutex.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define RTW88_NATIVE_VENDOR_REALTEK		0x0bda
#define RTW88_NATIVE_PRODUCT_RTL8822BU		0xb812

#define RTW88_NATIVE_USB_REQUEST		0x05
#define RTW88_NATIVE_USB_REQUEST_INDEX		0x0000
#define RTW88_NATIVE_USB_TIMEOUT		1000
#define RTW88_NATIVE_MAX_CONTROL_DATA		254

#define RTW88_NATIVE_REG_SYS_CFG1		0x00f0
#define RTW88_NATIVE_REG_MCUFW_CTRL		0x0080
#define RTW88_NATIVE_REG_USB_SEC			0x04e0

#define RTW88_NATIVE_FW_NAME			"rtw88/rtw8822b_fw.bin"
#define RTW88_NATIVE_FW_HEADER_SIZE		64
#define RTW88_NATIVE_FW_CHECKSUM_SIZE		8
#define RTW88_NATIVE_FW_PAGE_SIZE		0x1000
#define RTW88_NATIVE_FW_START_ADDRESS		0x1000
#define RTW88_NATIVE_FW_PAGE_MASK		(0x7U << 16)
#define RTW88_NATIVE_FW_PAGE_SHIFT		16
#define RTW88_NATIVE_FW_USB_BLOCK_SIZE		196

enum {
	RTW88_NATIVE_BULK_RX,
	RTW88_NATIVE_BULK_TX0,
	RTW88_NATIVE_BULK_TX1,
	RTW88_NATIVE_BULK_TX2,
	RTW88_NATIVE_INTERRUPT_RX,
	RTW88_NATIVE_N_XFERS
};

struct rtw88_native_fw_layout {
	const uint8_t	*dmem;
	size_t		dmem_size;
	uint32_t	dmem_addr;
	const uint8_t	*imem;
	size_t		imem_size;
	uint32_t	imem_addr;
	const uint8_t	*emem;
	size_t		emem_size;
	uint32_t	emem_addr;
};

struct rtw88_native_softc {
	device_t		dev;
	struct usb_device	*udev;
	struct usb_xfer		*xfer[RTW88_NATIVE_N_XFERS];
	struct mtx		mtx;
	uint8_t			iface_index;
	int			mtx_initialized;
	int			xfers_initialized;
	const struct firmware	*firmware;
	struct rtw88_native_fw_layout fw_layout;
};

usb_error_t	rtw88_native_usb_setup(struct rtw88_native_softc *);
void		rtw88_native_usb_unsetup(struct rtw88_native_softc *);
usb_error_t	rtw88_native_usb_control(struct rtw88_native_softc *,
		    uint8_t, uint16_t, void *, uint16_t, uint16_t *);

usb_error_t	rtw88_native_raw_reg_read32(struct rtw88_native_softc *,
		    uint16_t, uint32_t *);
usb_error_t	rtw88_native_raw_reg_write(struct rtw88_native_softc *,
		    uint16_t, const void *, uint16_t);

usb_error_t	rtw88_native_reg_read8(struct rtw88_native_softc *,
		    uint16_t, uint8_t *);
usb_error_t	rtw88_native_reg_read16(struct rtw88_native_softc *,
		    uint16_t, uint16_t *);
usb_error_t	rtw88_native_reg_read32(struct rtw88_native_softc *,
		    uint16_t, uint32_t *);
usb_error_t	rtw88_native_reg_write8(struct rtw88_native_softc *,
		    uint16_t, uint8_t);
usb_error_t	rtw88_native_reg_write16(struct rtw88_native_softc *,
		    uint16_t, uint16_t);
usb_error_t	rtw88_native_reg_write32(struct rtw88_native_softc *,
		    uint16_t, uint32_t);

int		rtw88_native_firmware_get(struct rtw88_native_softc *);
void		rtw88_native_firmware_put(struct rtw88_native_softc *);
int		rtw88_native_fw_write_page(struct rtw88_native_softc *, uint8_t,
		    const uint8_t *, size_t);

#endif /* _IF_RTW88_NATIVE_H_ */
