/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"

static void
rtw88_native_xfer_callback(struct usb_xfer *xfer, usb_error_t error)
{
	/*
	 * Transfer objects are allocated during transport bring-up but are not
	 * submitted until firmware and the device-specific DMA state machine
	 * have been implemented.  Keep this callback inert for now.
	 */
	(void)xfer;
	(void)error;
}

static const struct usb_config rtw88_native_xfer_config[RTW88_NATIVE_N_XFERS] = {
	[RTW88_NATIVE_BULK_RX] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = 32 * 1024,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1
		},
		.callback = rtw88_native_xfer_callback
	},
	[RTW88_NATIVE_BULK_TX0] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 0,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1
		},
		.callback = rtw88_native_xfer_callback
	},
	[RTW88_NATIVE_BULK_TX1] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 1,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1
		},
		.callback = rtw88_native_xfer_callback
	},
	[RTW88_NATIVE_BULK_TX2] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 2,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1
		},
		.callback = rtw88_native_xfer_callback
	},
	[RTW88_NATIVE_INTERRUPT_RX] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = 64,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1
		},
		.callback = rtw88_native_xfer_callback
	}
};

usb_error_t
rtw88_native_usb_setup(struct rtw88_native_softc *sc)
{
	usb_error_t error;

	if (sc == NULL || sc->udev == NULL || !sc->mtx_initialized)
		return (USB_ERR_INVAL);
	if (sc->xfers_initialized)
		return (USB_ERR_IN_USE);

	error = usbd_transfer_setup(sc->udev, &sc->iface_index, sc->xfer,
	    rtw88_native_xfer_config, RTW88_NATIVE_N_XFERS, sc, &sc->mtx);
	if (error != USB_ERR_NORMAL_COMPLETION) {
		/*
		 * USBDI requires the transfer array to be unsetup after a setup
		 * attempt, including partial allocation failures.
		 */
		usbd_transfer_unsetup(sc->xfer, RTW88_NATIVE_N_XFERS);
		return (error);
	}

	sc->xfers_initialized = 1;
	return (USB_ERR_NORMAL_COMPLETION);
}

void
rtw88_native_usb_unsetup(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->xfers_initialized)
		return;

	usbd_transfer_unsetup(sc->xfer, RTW88_NATIVE_N_XFERS);
	bzero(sc->xfer, sizeof(sc->xfer));
	sc->xfers_initialized = 0;
}

usb_error_t
rtw88_native_usb_control(struct rtw88_native_softc *sc, uint8_t request_type,
    uint16_t reg, void *data, uint16_t len, uint16_t *actlen)
{
	usb_device_request_t req;
	usb_error_t error;
	uint16_t actual;

	if (sc == NULL || sc->udev == NULL || !sc->mtx_initialized ||
	    data == NULL || len == 0 || len > RTW88_NATIVE_MAX_CONTROL_DATA)
		return (USB_ERR_INVAL);

	bzero(&req, sizeof(req));
	req.bmRequestType = request_type;
	req.bRequest = RTW88_NATIVE_USB_REQUEST;
	USETW(req.wValue, reg);
	USETW(req.wIndex, RTW88_NATIVE_USB_REQUEST_INDEX);
	USETW(req.wLength, len);

	/*
	 * The FreeBSD implementation releases and reacquires the supplied
	 * mutex around the synchronous control request.  The caller therefore
	 * enters with this mutex owned and returns with it owned.
	 */
	mtx_lock(&sc->mtx);
	actual = 0;
	error = usbd_do_request_flags(sc->udev, &sc->mtx, &req, data, 0,
	    &actual, RTW88_NATIVE_USB_TIMEOUT);
	mtx_unlock(&sc->mtx);

	if (actlen != NULL)
		*actlen = actual;
	if (error == USB_ERR_NORMAL_COMPLETION && actual != len)
		return (USB_ERR_SHORT_XFER);

	return (error);
}
