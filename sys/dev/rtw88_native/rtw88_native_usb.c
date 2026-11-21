/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 */

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/endian.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"


/*
 * Convert one verified non-C2H aggregate record into a net80211-owned mbuf.
 * RCR.APP_FCS is set by rtw88_native_8822b_mac_init(), so the four-byte FCS
 * is removed before input. PHY status is used only for the basic RSSI/rate
 * metadata needed by the software scan cache; advanced rate/chain reporting
 * remains future work.
 */
static void
rtw88_native_rx_enqueue(struct rtw88_native_softc *sc, const uint8_t *desc,
    const uint8_t *frame, size_t packet_length, size_t driver_info,
    size_t shift, uint32_t w0)
{
	static const uint8_t legacy_rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	struct ieee80211_rx_stats rxs;
	struct mbuf *m;
	uint32_t w3, w5, phy0;
	uint8_t rate, raw_a, raw_b;
	int rssi, channel;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->net80211_attached || sc->current_channel < 1 ||
	    sc->current_channel > 13)
		return;
	if ((w0 & (RTW88_NATIVE_RX_DESC_W0_CRCERR |
	    RTW88_NATIVE_RX_DESC_W0_ICVERR)) != 0 ||
	    packet_length <= RTW88_NATIVE_RX_FCS_LEN ||
	    packet_length > RTW88_NATIVE_RX_MAX_FRAME) {
		sc->rx_input_drop_count++;
		return;
	}

	packet_length -= RTW88_NATIVE_RX_FCS_LEN;
	m = m_get2((int)packet_length, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		sc->rx_input_drop_count++;
		return;
	}
	m->m_len = (int)packet_length;
	m->m_pkthdr.len = (int)packet_length;
	memcpy(mtod(m, void *), frame, packet_length);

	bzero(&rxs, sizeof(rxs));
	memcpy(&w3, desc + 12, sizeof(w3));
	memcpy(&w5, desc + 20, sizeof(w5));
	rate = (uint8_t)(le32toh(w3) & 0x7f);
	if (rate < nitems(legacy_rates)) {
		rxs.c_rate = legacy_rates[rate];
		if (rate <= 3)
			rxs.c_pktflags |= IEEE80211_RX_F_CCK;
	}

	channel = sc->current_channel;
	rxs.r_flags = IEEE80211_R_IEEE | IEEE80211_R_FREQ |
	    IEEE80211_R_BAND | IEEE80211_R_NF | IEEE80211_R_RSSI |
	    IEEE80211_R_TSF32;
	rxs.c_ieee = channel;
	rxs.c_freq = ieee80211_ieee2mhz(channel, IEEE80211_CHAN_2GHZ);
	rxs.c_band = IEEE80211_CHAN_2GHZ;
	rxs.c_rx_tsf = le32toh(w5);
	rxs.c_nf = -95;
	rssi = -95;
	if ((w0 & RTW88_NATIVE_RX_DESC_W0_PHYST) != 0 &&
	    driver_info >= sizeof(phy0)) {
		memcpy(&phy0, desc + RTW88_NATIVE_RX_DESC_SIZE + shift,
		    sizeof(phy0));
		phy0 = le32toh(phy0);
		raw_a = (uint8_t)((phy0 >> 8) & 0xff);
		raw_b = (uint8_t)((phy0 >> 16) & 0xff);
		if ((rxs.c_pktflags & IEEE80211_RX_F_CCK) != 0)
			rssi = (int)raw_a - 110;
		else
			rssi = MAX((int)raw_a, (int)raw_b) - 110;
		if (rssi < -105 || rssi > -10)
			rssi = -95;
	}
	rxs.c_rssi = rssi - rxs.c_nf;
	if (ieee80211_add_rx_params(m, &rxs) == 0) {
		m_freem(m);
		sc->rx_input_drop_count++;
		return;
	}

	if (mbufq_enqueue(&sc->rx_queue, m) != 0) {
		m_freem(m);
		sc->rx_input_drop_count++;
		return;
	}
	taskqueue_enqueue(taskqueue_thread, &sc->rx_task);
}

/*
 * Parse only the framing information required to safely walk an RTW88 USB
 * RX aggregation. This intentionally does not decode PHY status or pass
 * 802.11 frames to net80211 yet. C2H records are identified using RXD word 2
 * and retained as bounded metadata for the later event worker.
 */
static void
rtw88_native_rx_parse_batch(struct rtw88_native_softc *sc,
    const uint8_t *buf, size_t length)
{
	uint32_t w0, w2;
	size_t offset, remaining, driver_info, shift, packet_offset;
	size_t packet_length, record_length, aligned_length, c2h_length;
	const uint8_t *c2h;

	offset = 0;
	while (offset < length) {
		remaining = length - offset;
		if (remaining < RTW88_NATIVE_RX_DESC_SIZE) {
			/* USB aggregation may end with a short pad fragment. */
			if (remaining > 3)
				sc->rx_malformed_count++;
			break;
		}

		/* Avoid unaligned loads from the USB DMA buffer. */
		memcpy(&w0, buf + offset, sizeof(w0));
		memcpy(&w2, buf + offset + 8, sizeof(w2));
		w0 = le32toh(w0);
		w2 = le32toh(w2);
		if (w0 == 0 && w2 == 0)
			break;

		packet_length = w0 & RTW88_NATIVE_RX_DESC_W0_PKT_LEN;
		driver_info = ((w0 & RTW88_NATIVE_RX_DESC_W0_DRVINFO) >> 16) * 8;
		shift = (w0 & RTW88_NATIVE_RX_DESC_W0_SHIFT) >> 24;
		packet_offset = RTW88_NATIVE_RX_DESC_SIZE + driver_info + shift;
		if (packet_length == 0 || packet_offset > remaining ||
		    packet_length > remaining - packet_offset) {
			sc->rx_malformed_count++;
			break;
		}

		sc->rx_desc_count++;
		if (w2 & RTW88_NATIVE_RX_DESC_W2_C2H) {
			if (packet_length < RTW88_NATIVE_RX_C2H_HEADER_SIZE) {
				sc->rx_malformed_count++;
			} else {
				c2h = buf + offset + packet_offset;
				c2h_length = packet_length -
				    RTW88_NATIVE_RX_C2H_HEADER_SIZE;
				sc->rx_last_c2h_id = c2h[0];
				sc->rx_last_c2h_seq = c2h[1];
				sc->rx_last_c2h_len = (int)MIN(c2h_length,
				    (size_t)RTW88_NATIVE_RX_C2H_PAYLOAD_MAX);
				if (sc->rx_last_c2h_len != 0)
					memcpy(sc->rx_last_c2h_payload, c2h + 2,
					    sc->rx_last_c2h_len);
				if ((size_t)sc->rx_last_c2h_len <
				    sizeof(sc->rx_last_c2h_payload))
					bzero(sc->rx_last_c2h_payload +
					    sc->rx_last_c2h_len,
					    sizeof(sc->rx_last_c2h_payload) -
					    sc->rx_last_c2h_len);
				sc->rx_c2h_count++;
			}
		} else {
			sc->rx_data_count++;
			rtw88_native_rx_enqueue(sc, buf + offset,
			    buf + offset + packet_offset, packet_length,
			    driver_info, shift, w0);
		}

		record_length = packet_offset + packet_length;
		if (record_length > SIZE_MAX - 7) {
			sc->rx_malformed_count++;
			break;
		}
		aligned_length = (record_length + 7) & ~(size_t)7;
		if (aligned_length > remaining) {
			/* Last descriptor can end on the last byte without pad. */
			if (record_length == remaining)
				break;
			sc->rx_malformed_count++;
			break;
		}
		offset += aligned_length;
	}
}

static void
rtw88_native_rx_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct rtw88_native_softc *sc;
	uint32_t actlen;

	sc = usbd_xfer_softc(xfer);
	mtx_assert(&sc->mtx, MA_OWNED);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);
		actlen = MIN(actlen, sizeof(sc->rx_last));
		if (actlen != 0) {
			memcpy(sc->rx_last, sc->rx_buf, actlen);
			rtw88_native_rx_parse_batch(sc, sc->rx_buf, actlen);
		}
		sc->rx_last_len = actlen;
		sc->rx_count++;
		sc->rx_error = USB_ERR_NORMAL_COMPLETION;
		sc->rx_error_code = USB_ERR_NORMAL_COMPLETION;
		/* FALLTHROUGH */
	case USB_ST_SETUP:
		if (!sc->rx_started || sc->dying)
			break;
		usbd_xfer_set_frame_data(xfer, 0, sc->rx_buf,
		    sizeof(sc->rx_buf));
		sc->rx_submit_count++;
		usbd_transfer_submit(xfer);
		break;
	case USB_ST_ERROR:
		sc->rx_error = error;
		sc->rx_error_code = (int)error;
		if (error != USB_ERR_CANCELLED)
			sc->rx_error_count++;
		if (error != USB_ERR_CANCELLED && sc->rx_started &&
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
	if (sc->rx_started && !sc->dying) {
		usbd_xfer_set_frame_data(xfer, 0, sc->rx_buf,
		    sizeof(sc->rx_buf));
		sc->rx_submit_count++;
		usbd_transfer_submit(xfer);
	}
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
		.endpoint = RTW88_NATIVE_USB_EP_BULK_IN,
		.ep_index = 0,
		.direction = UE_DIR_IN,
		.bufsize = 32 * 1024,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_rx_callback,
		.timeout = 0
	},
	[RTW88_NATIVE_BULK_TX0] = {
		.type = UE_BULK,
		.endpoint = RTW88_NATIVE_USB_EP_BULK_OUT_HIGH,
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
		.endpoint = RTW88_NATIVE_USB_EP_BULK_OUT_NORMAL,
		.ep_index = 0,
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
		.endpoint = RTW88_NATIVE_USB_EP_BULK_OUT_LOW,
		.ep_index = 0,
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
		.endpoint = RTW88_NATIVE_USB_EP_INTERRUPT_IN,
		.ep_index = 0,
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


/*
 * Start continuous bulk-IN collection. The 32 KiB buffer stores USB
 * aggregation batches; this stage preserves the last completed raw batch
 * for inspection but does not yet parse RTW88 RX descriptors or pass frames
 * to net80211.
 */
int
rtw88_native_usb_rx_start(struct rtw88_native_softc *sc)
{
	struct usb_xfer *xfer;

	if (sc == NULL || !sc->mtx_initialized || !sc->xfers_initialized)
		return (EINVAL);
	mtx_lock(&sc->mtx);
	if (sc->dying) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (sc->rx_started) {
		mtx_unlock(&sc->mtx);
		return (EALREADY);
	}
	xfer = sc->xfer[RTW88_NATIVE_BULK_RX];
	if (xfer == NULL) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	bzero(sc->rx_buf, sizeof(sc->rx_buf));
	bzero(sc->rx_last, sizeof(sc->rx_last));
	sc->rx_last_len = 0;
	sc->rx_error = USB_ERR_NORMAL_COMPLETION;
	sc->rx_error_code = USB_ERR_NORMAL_COMPLETION;
	sc->rx_started = 1;
	usbd_transfer_start(xfer);
	mtx_unlock(&sc->mtx);
	return (0);
}

/*
 * Stop and drain bulk-IN. A drain is required before reusing/freeing
 * external DMA buffers or tearing down the device-private softc.
 */
void
rtw88_native_usb_rx_stop(struct rtw88_native_softc *sc)
{
	struct usb_xfer *xfer;

	if (sc == NULL || !sc->mtx_initialized)
		return;
	mtx_lock(&sc->mtx);
	sc->rx_started = 0;
	xfer = sc->xfers_initialized ? sc->xfer[RTW88_NATIVE_BULK_RX] : NULL;
	mtx_unlock(&sc->mtx);

	if (xfer != NULL)
		usbd_transfer_drain(xfer);
}
