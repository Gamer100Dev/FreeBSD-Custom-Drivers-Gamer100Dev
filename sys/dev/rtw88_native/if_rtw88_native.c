/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * This file implements the initial native FreeBSD USB transport for the
 * Realtek RTL8822BU (USB ID 0bda:b812).  It intentionally does not register
 * a wlan interface yet; chipset startup, firmware download, data transfers,
 * and net80211 integration are subsequent porting stages.
 *
 * Register request format is the one used by the RTW88 USB hardware
 * interface: vendor control request 0x05, wValue = register address,
 * wIndex = 0, and a little-endian payload.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"

struct rtw88_native_softc {
	device_t		dev;
	struct usb_device	*udev;
	struct mtx		mtx;
	int			mtx_initialized;
};

/*
 * Keep the initial port deliberately narrow: bind only to the adapter that
 * is being developed and only to its vendor-specific interface.
 */
static STRUCT_USB_HOST_ID rtw88_native_devs[] = {
	{
		USB_VP(RTW88_NATIVE_VENDOR_REALTEK,
		    RTW88_NATIVE_PRODUCT_RTL8822BU),
		USB_IFACE_CLASS(0xff),
		USB_IFACE_SUBCLASS(0xff),
		USB_IFACE_PROTOCOL(0xff)
	},
};

static usb_error_t
rtw88_native_control(struct rtw88_native_softc *sc, uint8_t request_type,
    uint16_t reg, void *data, uint16_t len)
{
	usb_device_request_t req;
	usb_error_t error;

	if (data == NULL || len == 0 ||
	    len > RTW88_NATIVE_MAX_CONTROL_DATA)
		return (USB_ERR_INVAL);

	memset(&req, 0, sizeof(req));
	req.bmRequestType = request_type;
	req.bRequest = RTW88_NATIVE_USB_REQUEST;
	USETW(req.wValue, reg);
	USETW(req.wIndex, RTW88_NATIVE_USB_REQUEST_INDEX);
	USETW(req.wLength, len);

	/*
	 * usbd_do_request_flags() is the synchronous FreeBSD USB control
	 * transfer API.  The caller must not be in a USB transfer callback.
	 * Serialize register requests with the device-private mutex.
	 */
	mtx_lock(&sc->mtx);
	error = usbd_do_request_flags(sc->udev, &sc->mtx, &req, data, 0,
	    NULL, RTW88_NATIVE_USB_TIMEOUT);
	mtx_unlock(&sc->mtx);

	return (error);
}

usb_error_t
rtw88_native_reg_read32(struct rtw88_native_softc *sc, uint16_t reg,
    uint32_t *value)
{
	uint32_t value_le;
	usb_error_t error;

	if (value == NULL)
		return (USB_ERR_INVAL);

	value_le = 0;
	error = rtw88_native_control(sc, UT_READ_VENDOR_DEVICE, reg,
	    &value_le, sizeof(value_le));
	if (error == USB_ERR_NORMAL_COMPLETION)
		*value = le32toh(value_le);

	return (error);
}

usb_error_t
rtw88_native_reg_write(struct rtw88_native_softc *sc, uint16_t reg,
    const void *data, uint16_t len)
{
	/*
	 * The hardware consumes register values and firmware blocks in
	 * little-endian byte order.  Multi-byte caller buffers must already
	 * be encoded accordingly; this routine deliberately does not reinterpret
	 * arbitrary firmware payloads.
	 */
	return (rtw88_native_control(sc, UT_WRITE_VENDOR_DEVICE, reg,
	    __DECONST(void *, data), len));
}

static int
rtw88_native_probe(device_t dev)
{
	struct usb_attach_arg *uaa;

	uaa = device_get_ivars(dev);
	if (uaa == NULL || uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	return (usbd_lookup_id_by_uaa(rtw88_native_devs,
	    sizeof(rtw88_native_devs), uaa));
}

static int
rtw88_native_attach(device_t dev)
{
	struct rtw88_native_softc *sc;
	struct usb_attach_arg *uaa;
	uint32_t sys_cfg1;
	usb_error_t uerr;

	sc = device_get_softc(dev);
	uaa = device_get_ivars(dev);
	sc->dev = dev;
	sc->udev = uaa->device;

	mtx_init(&sc->mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK, MTX_DEF);
	sc->mtx_initialized = 1;

	device_printf(dev,
	    "native FreeBSD USB transport attached (RTL8822BU, 0bda:b812)\n");

	/*
	 * REG_SYS_CFG1 (0x00f0) is the read-only system configuration register
	 * used by the upstream RTW88 chip-identification path.  Probe it once
	 * without changing device state; this tests the native control pipe.
	 */
	uerr = rtw88_native_reg_read32(sc, RTW88_NATIVE_REG_SYS_CFG1, &sys_cfg1);
	if (uerr == USB_ERR_NORMAL_COMPLETION) {
		device_printf(dev,
		    "native vendor-control read succeeded: SYS_CFG1=0x%08x\n",
		    sys_cfg1);
	} else {
		device_printf(dev,
		    "native vendor-control read of SYS_CFG1 failed: %s\n",
		    usbd_errstr(uerr));
	}

	device_printf(dev,
	    "transport milestone only: firmware, bulk data paths and wlan "
	    "registration are not implemented yet\n");

	return (0);
}

static int
rtw88_native_detach(device_t dev)
{
	struct rtw88_native_softc *sc;

	sc = device_get_softc(dev);
	if (sc->mtx_initialized) {
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
	}
	return (0);
}

static device_method_t rtw88_native_methods[] = {
	DEVMETHOD(device_probe, 	rtw88_native_probe),
	DEVMETHOD(device_attach,	rtw88_native_attach),
	DEVMETHOD(device_detach,	rtw88_native_detach),
	DEVMETHOD_END
};

static driver_t rtw88_native_driver = {
	"rtw88_native",
	rtw88_native_methods,
	sizeof(struct rtw88_native_softc)
};

DRIVER_MODULE(rtw88_native, uhub, rtw88_native_driver, NULL, NULL);
MODULE_DEPEND(rtw88_native, usb, 1, 1, 1);
MODULE_VERSION(rtw88_native, 1);
USB_PNP_HOST_INFO(rtw88_native_devs);
