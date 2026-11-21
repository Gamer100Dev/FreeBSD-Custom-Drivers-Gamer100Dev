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
rtw88_native_rx_callback(struct usb_xfer *xfer, usb_error_t error)
{
	/* Bulk RX stays stopped until the RX descriptor/aggregation parser exists. */
	(void)xfer;
	(void)error;
}

static void
rtw88_native_intr_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct rtw88_native_softc *sc;
	uint32_t actlen;

	sc = usbd_xfer_softc(xfer);
	mtx_assert(&sc->mtx, MA_OWNED);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);
		actlen = MIN(actlen, sizeof(sc->last_intr));
		memcpy(sc->last_intr, sc->intr_buf, actlen);
		sc->last_intr_len = (uint16_t)actlen;
		sc->intr_count++;
		sc->intr_error = USB_ERR_NORMAL_COMPLETION;
		/* FALLTHROUGH */
	case USB_ST_SETUP:
		if (!sc->intr_started || sc->dying)
			break;
		usbd_xfer_set_frame_data(xfer, 0, sc->intr_buf,
		    sizeof(sc->intr_buf));
		usbd_transfer_submit(xfer);
		break;
	case USB_ST_ERROR:
		sc->intr_error = error;
		if (error != USB_ERR_CANCELLED && sc->intr_started &&
		    !sc->dying) {
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		break;
	default:
		break;
	}
	return;
tr_setup:
	if (sc->intr_started && !sc->dying) {
		usbd_xfer_set_frame_data(xfer, 0, sc->intr_buf,
		    sizeof(sc->intr_buf));
		usbd_transfer_submit(xfer);
	}
}

static void
rtw88_native_tx_callback_common(struct usb_xfer *xfer, usb_error_t error,
    uint8_t queue)
{
	struct rtw88_native_softc *sc;

	sc = usbd_xfer_softc(xfer);
	mtx_assert(&sc->mtx, MA_OWNED);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		if (!sc->tx_busy[queue])
			break;
		usbd_xfer_set_frame_data(xfer, 0,
		    __DECONST(void *, sc->tx_data[queue]), sc->tx_len[queue]);
		usbd_transfer_submit(xfer);
		break;
	case USB_ST_TRANSFERRED:
		sc->tx_error[queue] = USB_ERR_NORMAL_COMPLETION;
		sc->tx_done[queue] = 1;
		cv_broadcast(&sc->tx_cv[queue]);
		break;
	case USB_ST_ERROR:
		sc->tx_error[queue] = error;
		sc->tx_done[queue] = 1;
		if (error != USB_ERR_CANCELLED)
			usbd_xfer_set_stall(xfer);
		cv_broadcast(&sc->tx_cv[queue]);
		break;
	default:
		break;
	}
}

static void
rtw88_native_tx0_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_tx_callback_common(xfer, error, 0);
}

static void
rtw88_native_tx1_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_tx_callback_common(xfer, error, 1);
}

static void
rtw88_native_tx2_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_tx_callback_common(xfer, error, 2);
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
		.callback = rtw88_native_rx_callback
	},
	[RTW88_NATIVE_BULK_TX0] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 0,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_tx0_callback,
		.timeout = 5000
	},
	[RTW88_NATIVE_BULK_TX1] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 1,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_tx1_callback,
		.timeout = 5000
	},
	[RTW88_NATIVE_BULK_TX2] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.ep_index = 2,
		.direction = UE_DIR_OUT,
		.bufsize = 20 * 1024,
		.flags = {
			.pipe_bof = 1,
			.force_short_xfer = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_tx2_callback,
		.timeout = 5000
	},
	[RTW88_NATIVE_INTERRUPT_RX] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = 64,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_intr_callback,
		.timeout = 0
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
		/* USBDI requires unsetup after a setup attempt, including partial
		 * allocation failure. NULL transfer slots are accepted.
		 */
		usbd_transfer_unsetup(sc->xfer, RTW88_NATIVE_N_XFERS);
		return (error);
	}

	sc->xfers_initialized = 1;
	/* All three descriptor-indexed bulk-OUT transfers were allocated. */
	sc->bulkout_num = 3;
	return (USB_ERR_NORMAL_COMPLETION);
}

void
rtw88_native_usb_unsetup(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->xfers_initialized)
		return;

	/*
	 * This may sleep and waits for callbacks. It must not be called with the
	 * private USB mutex held.
	 */
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
	 * FreeBSD's usbd_do_request_flags() releases and reacquires the supplied
	 * private mutex around the control request. It returns with the mutex
	 * owned, matching the entry/exit contract used here.
	 */
	mtx_lock(&sc->mtx);
	if (sc->dying) {
		mtx_unlock(&sc->mtx);
		return (USB_ERR_CANCELLED);
	}
	sc->io_users++;
	actual = 0;
	error = usbd_do_request_flags(sc->udev, &sc->mtx, &req, data, 0,
	    &actual, RTW88_NATIVE_USB_TIMEOUT);
	sc->io_users--;
	if (sc->io_users == 0 && sc->io_cv_initialized)
		cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (actlen != NULL)
		*actlen = actual;
	if (error == USB_ERR_NORMAL_COMPLETION && actual != len)
		return (USB_ERR_SHORT_XFER);

	return (error);
}

/*
 * Send one bulk-OUT buffer and wait for the callback to report completion.
 * The caller retains ownership of the buffer, which must stay valid until
 * this function returns. This routine sleeps and must not be called from a
 * USB callback, interrupt filter, or with sc->mtx already held.
 */
usb_error_t
rtw88_native_usb_tx_sync(struct rtw88_native_softc *sc, uint8_t queue,
    const void *data, uint16_t len)
{
	struct usb_xfer *xfer;
	usb_error_t error;
	uint8_t index;

	if (sc == NULL || data == NULL || len == 0 ||
	    queue >= RTW88_NATIVE_TX_QUEUES || !sc->mtx_initialized ||
	    !sc->tx_cv_initialized || !sc->io_cv_initialized)
		return (USB_ERR_INVAL);

	index = RTW88_NATIVE_BULK_TX0 + queue;
	mtx_lock(&sc->mtx);
	if (sc->dying || !sc->xfers_initialized) {
		mtx_unlock(&sc->mtx);
		return (USB_ERR_CANCELLED);
	}
	if (sc->tx_busy[queue]) {
		mtx_unlock(&sc->mtx);
		return (USB_ERR_IN_USE);
	}
	xfer = sc->xfer[index];
	if (xfer == NULL || len > usbd_xfer_max_len(xfer)) {
		mtx_unlock(&sc->mtx);
		return (USB_ERR_BAD_BUFSIZE);
	}

	sc->io_users++;
	sc->tx_busy[queue] = 1;
	sc->tx_done[queue] = 0;
	sc->tx_error[queue] = USB_ERR_CANCELLED;
	sc->tx_data[queue] = data;
	sc->tx_len[queue] = len;

	/* USBDI requires the private transfer mutex to be held here. */
	usbd_transfer_start(xfer);
	while (!sc->tx_done[queue])
		cv_wait(&sc->tx_cv[queue], &sc->mtx);

	error = sc->tx_error[queue];
	sc->tx_data[queue] = NULL;
	sc->tx_len[queue] = 0;
	sc->tx_busy[queue] = 0;
	sc->tx_done[queue] = 0;
	sc->io_users--;
	if (sc->io_users == 0)
		cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	return (error);
}


/*
 * Begin collecting raw RTL8822B USB interrupt reports. Call only after the
 * hardware has completed power-on and firmware startup.
 */
int
rtw88_native_usb_intr_start(struct rtw88_native_softc *sc)
{
	struct usb_xfer *xfer;

	if (sc == NULL || !sc->mtx_initialized || !sc->xfers_initialized)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (sc->intr_started) {
		mtx_unlock(&sc->mtx);
		return (EALREADY);
	}
	xfer = sc->xfer[RTW88_NATIVE_INTERRUPT_RX];
	if (xfer == NULL) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	bzero(sc->intr_buf, sizeof(sc->intr_buf));
	bzero(sc->last_intr, sizeof(sc->last_intr));
	sc->last_intr_len = 0;
	sc->intr_error = USB_ERR_NORMAL_COMPLETION;
	sc->intr_started = 1;
	usbd_transfer_start(xfer);
	mtx_unlock(&sc->mtx);
	return (0);
}

/*
 * Drain the interrupt transfer before allowing its external DMA buffer or
 * private softc to be reused/freed. usbd_transfer_drain() may sleep and must
 * be called without the USB transfer mutex held.
 */
void
rtw88_native_usb_intr_stop(struct rtw88_native_softc *sc)
{
	struct usb_xfer *xfer;

	if (sc == NULL || !sc->mtx_initialized)
		return;
	mtx_lock(&sc->mtx);
	sc->intr_started = 0;
	xfer = sc->xfers_initialized ?
	    sc->xfer[RTW88_NATIVE_INTERRUPT_RX] : NULL;
	mtx_unlock(&sc->mtx);

	if (xfer != NULL)
		usbd_transfer_drain(xfer);
}
