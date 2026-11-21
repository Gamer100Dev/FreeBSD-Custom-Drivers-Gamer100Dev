/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"

/*
 * RTL8822B uses an additional USB security-section write after normal
 * register accesses in the always-on (0x0000-0x00ff) and 0x1000-0x10ff
 * register windows.  Keep this as a raw vendor request to avoid recursion.
 */
static usb_error_t
rtw88_native_reg_sec(struct rtw88_native_softc *sc, uint16_t addr,
    const uint8_t *data)
{
	uint8_t value;

	if (!(addr <= 0x00ff || (addr >= 0x1000 && addr <= 0x10ff)))
		return (USB_ERR_NORMAL_COMPLETION);

	value = data[0];
	return (rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE,
	    RTW88_NATIVE_REG_USB_SEC, &value, sizeof(value), NULL));
}

usb_error_t
rtw88_native_raw_reg_read32(struct rtw88_native_softc *sc, uint16_t addr,
    uint32_t *value)
{
	uint32_t value_le;
	usb_error_t error;

	if (sc == NULL || value == NULL)
		return (USB_ERR_INVAL);

	value_le = 0;
	error = rtw88_native_usb_control(sc, UT_READ_VENDOR_DEVICE, addr,
	    &value_le, sizeof(value_le), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION)
		*value = le32toh(value_le);

	return (error);
}

usb_error_t
rtw88_native_raw_reg_write(struct rtw88_native_softc *sc, uint16_t addr,
    const void *data, uint16_t len)
{
	if (sc == NULL)
		return (USB_ERR_INVAL);

	return (rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE, addr,
	    __DECONST(void *, data), len, NULL));
}

usb_error_t
rtw88_native_reg_read8(struct rtw88_native_softc *sc, uint16_t addr,
    uint8_t *value)
{
	usb_error_t error;

	if (value == NULL)
		return (USB_ERR_INVAL);
	error = rtw88_native_usb_control(sc, UT_READ_VENDOR_DEVICE, addr,
	    value, sizeof(*value), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION)
		error = rtw88_native_reg_sec(sc, addr, value);
	return (error);
}

usb_error_t
rtw88_native_reg_read16(struct rtw88_native_softc *sc, uint16_t addr,
    uint16_t *value)
{
	uint16_t value_le;
	usb_error_t error;

	if (value == NULL)
		return (USB_ERR_INVAL);
	value_le = 0;
	error = rtw88_native_usb_control(sc, UT_READ_VENDOR_DEVICE, addr,
	    &value_le, sizeof(value_le), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION) {
		*value = le16toh(value_le);
		error = rtw88_native_reg_sec(sc, addr, (const uint8_t *)&value_le);
	}
	return (error);
}

usb_error_t
rtw88_native_reg_read32(struct rtw88_native_softc *sc, uint16_t addr,
    uint32_t *value)
{
	uint32_t value_le;
	usb_error_t error;

	if (value == NULL)
		return (USB_ERR_INVAL);
	value_le = 0;
	error = rtw88_native_usb_control(sc, UT_READ_VENDOR_DEVICE, addr,
	    &value_le, sizeof(value_le), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION) {
		*value = le32toh(value_le);
		error = rtw88_native_reg_sec(sc, addr, (const uint8_t *)&value_le);
	}
	return (error);
}

usb_error_t
rtw88_native_reg_write8(struct rtw88_native_softc *sc, uint16_t addr,
    uint8_t value)
{
	usb_error_t error;

	error = rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE, addr,
	    &value, sizeof(value), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION)
		error = rtw88_native_reg_sec(sc, addr, &value);
	return (error);
}

usb_error_t
rtw88_native_reg_write16(struct rtw88_native_softc *sc, uint16_t addr,
    uint16_t value)
{
	uint16_t value_le;
	usb_error_t error;

	value_le = htole16(value);
	error = rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE, addr,
	    &value_le, sizeof(value_le), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION)
		error = rtw88_native_reg_sec(sc, addr, (const uint8_t *)&value_le);
	return (error);
}

usb_error_t
rtw88_native_reg_write32(struct rtw88_native_softc *sc, uint16_t addr,
    uint32_t value)
{
	uint32_t value_le;
	usb_error_t error;

	value_le = htole32(value);
	error = rtw88_native_usb_control(sc, UT_WRITE_VENDOR_DEVICE, addr,
	    &value_le, sizeof(value_le), NULL);
	if (error == USB_ERR_NORMAL_COMPLETION)
		error = rtw88_native_reg_sec(sc, addr, (const uint8_t *)&value_le);
	return (error);
}
