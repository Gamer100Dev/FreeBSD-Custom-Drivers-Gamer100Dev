/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#ifndef _IF_RTW88_NATIVE_H_
#define _IF_RTW88_NATIVE_H_

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define RTW88_NATIVE_VENDOR_REALTEK		0x0bda
#define RTW88_NATIVE_PRODUCT_RTL8822BU		0xb812

#define RTW88_NATIVE_USB_REQUEST		0x05
#define RTW88_NATIVE_USB_REQUEST_INDEX		0x0000
#define RTW88_NATIVE_USB_TIMEOUT		1000
#define RTW88_NATIVE_MAX_CONTROL_DATA		254

#define RTW88_NATIVE_REG_SYS_CFG1		0x00f0

struct rtw88_native_softc;

usb_error_t	rtw88_native_raw_reg_read32(struct rtw88_native_softc *,
		    uint16_t, uint32_t *);
usb_error_t	rtw88_native_raw_reg_write(struct rtw88_native_softc *,
		    uint16_t, const void *, uint16_t);

#endif /* _IF_RTW88_NATIVE_H_ */
