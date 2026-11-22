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
	static const uint8_t eapol_snap[8] = {
		0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e
	};
	struct ieee80211_rx_stats rxs;
	struct ieee80211_frame *wh;
	struct mbuf *m;
	uint32_t w3, w4, w5, phy0, phy1, phy3, enc_type;
	uint8_t rate, page, raw_a, raw_b, rxsc, fc0, fc1;
	size_t hdrlen;
	int power_a, power_b, signal_dbm, channel;
	int phy_valid;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->net80211_attached || sc->current_channel < 1 ||
	    sc->current_channel > 13) {
		sc->rx_input_drop_count++;
		return;
	}
	if ((w0 & (RTW88_NATIVE_RX_DESC_W0_CRCERR |
	    RTW88_NATIVE_RX_DESC_W0_ICVERR)) != 0 ||
	    packet_length < RTW88_NATIVE_RX_FCS_LEN +
	    sizeof(struct ieee80211_frame) ||
	    packet_length > RTW88_NATIVE_RX_MAX_FRAME ||
	    (driver_info != 0 &&
	    driver_info != RTW88_NATIVE_RX_PHY_STATUS_SIZE)) {
		sc->rx_input_drop_count++;
		return;
	}

	memcpy(&w3, desc + 12, sizeof(w3));
	memcpy(&w4, desc + 16, sizeof(w4));
	memcpy(&w5, desc + 20, sizeof(w5));
	rate = (uint8_t)(le32toh(w3) & 0x7f);
	if (rate >= RTW88_NATIVE_RX_RATE_MAX) {
		sc->rx_bad_rate_count++;
		sc->rx_input_drop_count++;
		return;
	}

	if ((w0 & RTW88_NATIVE_RX_DESC_W0_PHYST) != 0 &&
	    driver_info != RTW88_NATIVE_RX_PHY_STATUS_SIZE) {
		sc->rx_bad_drvinfo_count++;
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

	wh = mtod(m, struct ieee80211_frame *);
	fc0 = wh->i_fc[0];
	fc1 = wh->i_fc[1];

	/* Classify received management responses before net80211 consumes them. */
	if ((fc0 & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT) {
		switch (fc0 & IEEE80211_FC0_SUBTYPE_MASK) {
		case IEEE80211_FC0_SUBTYPE_AUTH:
			sc->rx_auth_count++;
			if (IEEE80211_ADDR_EQ(wh->i_addr1, sc->ic.ic_macaddr))
				sc->rx_auth_to_self_count++;
			break;
		case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
		case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
			sc->rx_assoc_resp_count++;
			break;
		case IEEE80211_FC0_SUBTYPE_DEAUTH:
			sc->rx_deauth_count++;
			break;
		case IEEE80211_FC0_SUBTYPE_DISASSOC:
			sc->rx_disassoc_count++;
			break;
		case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
			sc->rx_probe_resp_count++;
			break;
		case IEEE80211_FC0_SUBTYPE_BEACON:
			sc->rx_beacon_count++;
			break;
		default:
			break;
		}
	} else if ((fc0 & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA) {
		hdrlen = sizeof(struct ieee80211_frame);
		if ((fc1 & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_DSTODS)
			hdrlen += IEEE80211_ADDR_LEN;
		if ((fc0 & IEEE80211_FC0_SUBTYPE_QOS_MASK_ANY) != 0) {
			hdrlen += 2;
			if ((fc1 & IEEE80211_FC1_ORDER) != 0)
				hdrlen += 4;
		}
		if (packet_length >= hdrlen + sizeof(eapol_snap) &&
		    memcmp((const uint8_t *)wh + hdrlen, eapol_snap,
		    sizeof(eapol_snap)) == 0)
			sc->rx_eapol_count++;
	}

	bzero(&rxs, sizeof(rxs));
	switch (rate) {
	case 0x00 ... 0x03:
		rxs.c_rate = legacy_rates[rate];
		rxs.c_pktflags |= IEEE80211_RX_F_CCK;
		rxs.c_phytype = IEEE80211_RX_FP_11B;
		break;
	case 0x04 ... 0x0b:
		rxs.c_rate = legacy_rates[rate];
		rxs.c_pktflags |= IEEE80211_RX_F_OFDM;
		rxs.c_phytype = IEEE80211_RX_FP_11G;
		break;
	case 0x0c ... 0x2b:
		rxs.c_rate = IEEE80211_RATE_MCS | (rate - 0x0c);
		rxs.c_pktflags |= IEEE80211_RX_F_HT;
		rxs.c_phytype = IEEE80211_RX_FP_11NG;
		sc->rx_ht_count++;
		break;
	case 0x2c ... 0x53:
		rxs.c_vhtnss = (uint8_t)((rate - 0x2c) / 10) + 1;
		rxs.c_rate = (uint8_t)((rate - 0x2c) % 10);
		rxs.c_pktflags |= IEEE80211_RX_F_VHT;
		rxs.c_phytype = IEEE80211_RX_FP_11NG;
		sc->rx_vht_count++;
		break;
	default:
		/* The descriptor rate was range-checked above. */
		sc->rx_bad_rate_count++;
		m_freem(m);
		sc->rx_input_drop_count++;
		return;
	}

	/*
	 * W4 carries the hardware bandwidth code: 0=20, 1=40, 2=80 MHz.
	 * The 8822B PHY-status page can refine this from its receive subchannel.
	 */
	switch ((le32toh(w4) >> 4) & 0x3) {
	case 1:
		rxs.c_width = IEEE80211_RX_FW_40MHZ;
		break;
	case 2:
		rxs.c_width = IEEE80211_RX_FW_80MHZ;
		break;
	default:
		rxs.c_width = IEEE80211_RX_FW_20MHZ;
		break;
	}

	channel = sc->current_channel;
	rxs.r_flags = IEEE80211_R_IEEE | IEEE80211_R_FREQ |
	    IEEE80211_R_BAND | IEEE80211_R_TSF32;
	rxs.c_ieee = channel;
	rxs.c_freq = ieee80211_ieee2mhz(channel, IEEE80211_CHAN_2GHZ);
	rxs.c_band = IEEE80211_CHAN_2GHZ;
	rxs.c_rx_tsf = le32toh(w5);

	/*
	 * RTW88 marks a frame decrypted when ENC_TYPE is non-zero and SWDEC
	 * is clear. net80211 consumes this flag to bypass software decryption;
	 * M_WEP alone is not an equivalent RX-status indication.
	 */
	enc_type = (w0 & RTW88_NATIVE_RX_DESC_W0_ENC_TYPE) >> 20;
	if (enc_type != 0 && (w0 & RTW88_NATIVE_RX_DESC_W0_SWDEC) == 0) {
		rxs.c_pktflags |= IEEE80211_RX_F_DECRYPTED;
		sc->rx_decrypted_count++;
	}

	phy_valid = 0;
	signal_dbm = -120;
	if ((w0 & RTW88_NATIVE_RX_DESC_W0_PHYST) != 0 &&
	    driver_info == RTW88_NATIVE_RX_PHY_STATUS_SIZE) {
		memcpy(&phy0, desc + RTW88_NATIVE_RX_DESC_SIZE + shift,
		    sizeof(phy0));
		phy0 = le32toh(phy0);
		page = (uint8_t)(phy0 & 0x0f);
		raw_a = (uint8_t)((phy0 >> 8) & 0xff);
		raw_b = (uint8_t)((phy0 >> 16) & 0xff);
		power_a = (int)raw_a - 110;
		power_b = (int)raw_b - 110;

		if (page == 0) {
			/* CCK PHY status contains only the RF-A receive power. */
			signal_dbm = power_a;
			rxs.c_width = IEEE80211_RX_FW_20MHZ;
			sc->rx_phy_page0_count++;
			phy_valid = 1;
		} else if (page == 1) {
			memcpy(&phy1, desc + RTW88_NATIVE_RX_DESC_SIZE + shift + 4,
			    sizeof(phy1));
			memcpy(&phy3, desc + RTW88_NATIVE_RX_DESC_SIZE + shift + 12,
			    sizeof(phy3));
			phy1 = le32toh(phy1);
			phy3 = le32toh(phy3);
			/* A one-path board must not use RF-B's unused PWDB byte. */
			signal_dbm = sc->rf_path_num > 1 ?
			    MAX(power_a, power_b) : power_a;

			if (rate > 0x03 && rate < 0x0c)
				rxsc = (uint8_t)((phy1 >> 8) & 0x0f);
			else
				rxsc = (uint8_t)((phy1 >> 12) & 0x0f);
			if (rxsc >= 1 && rxsc <= 8)
				rxs.c_width = IEEE80211_RX_FW_20MHZ;
			else if (rxsc >= 9 && rxsc <= 12)
				rxs.c_width = IEEE80211_RX_FW_40MHZ;
			else if (rxsc >= 13)
				rxs.c_width = IEEE80211_RX_FW_80MHZ;
			else {
				switch ((phy3 >> 28) & 0x3) {
				case 1:
					rxs.c_width = IEEE80211_RX_FW_40MHZ;
					break;
				case 2:
					rxs.c_width = IEEE80211_RX_FW_80MHZ;
					break;
				default:
					rxs.c_width = IEEE80211_RX_FW_20MHZ;
					break;
				}
			}
			sc->rx_phy_page1_count++;
			phy_valid = 1;
		} else {
			sc->rx_phy_unknown_count++;
		}
	} else {
		sc->rx_no_phy_count++;
	}

	/* Do not claim a signal measurement when PHY status is absent or invalid. */
	if (phy_valid && signal_dbm >= -120 && signal_dbm <= 0) {
		rxs.c_nf = -95;
		rxs.c_rssi = signal_dbm - rxs.c_nf;
		rxs.r_flags |= IEEE80211_R_NF | IEEE80211_R_RSSI;
	}

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
 * Safely walk an RTW88 USB RX aggregation, retain bounded C2H metadata, and
 * enqueue valid 802.11 frames for deferred net80211 input. C2H dispatch and
 * per-chain RSSI/SNR/EVM decoding remain follow-on work.
 */
static void
rtw88_native_rx_parse_batch(struct rtw88_native_softc *sc,
    const uint8_t *buf, size_t length)
{
	uint32_t w0, w2, w3;
	uint8_t rate;
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
		memcpy(&w3, buf + offset + 12, sizeof(w3));
		w0 = le32toh(w0);
		w2 = le32toh(w2);
		w3 = le32toh(w3);
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

		rate = (uint8_t)(w3 & 0x7f);
		if (rate >= RTW88_NATIVE_RX_RATE_MAX) {
			sc->rx_bad_rate_count++;
			sc->rx_malformed_count++;
			break;
		}
		if ((driver_info != 0 &&
		    driver_info != RTW88_NATIVE_RX_PHY_STATUS_SIZE) ||
		    ((w0 & RTW88_NATIVE_RX_DESC_W0_PHYST) != 0 &&
		    driver_info != RTW88_NATIVE_RX_PHY_STATUS_SIZE)) {
			sc->rx_bad_drvinfo_count++;
			sc->rx_malformed_count++;
			break;
		}
		if ((w2 & RTW88_NATIVE_RX_DESC_W2_C2H) == 0 &&
		    packet_length <= RTW88_NATIVE_RX_FCS_LEN) {
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
rtw88_native_rx_callback_common(struct usb_xfer *xfer, usb_error_t error,
    uint8_t rx_index)
{
	struct rtw88_native_softc *sc;
	uint8_t *rx_buf;
	uint32_t actlen;

	sc = usbd_xfer_softc(xfer);
	mtx_assert(&sc->mtx, MA_OWNED);
	KASSERT(rx_index < RTW88_NATIVE_RX_XFERS,
	    ("invalid RTL8822BU RX transfer index %u", rx_index));
	rx_buf = sc->rx_buf[rx_index];

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);
		actlen = MIN(actlen, sizeof(sc->rx_last));
		if (actlen != 0) {
			memcpy(sc->rx_last, rx_buf, actlen);
			rtw88_native_rx_parse_batch(sc, rx_buf, actlen);
		}
		sc->rx_last_len = actlen;
		sc->rx_count++;
		sc->rx_error = USB_ERR_NORMAL_COMPLETION;
		sc->rx_error_code = USB_ERR_NORMAL_COMPLETION;
		/* FALLTHROUGH */
	case USB_ST_SETUP:
		if (!sc->rx_started || sc->dying)
			break;
		usbd_xfer_set_frame_data(xfer, 0, rx_buf,
		    RTW88_NATIVE_RX_BUFSIZE);
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
		usbd_xfer_set_frame_data(xfer, 0, rx_buf,
		    RTW88_NATIVE_RX_BUFSIZE);
		sc->rx_submit_count++;
		usbd_transfer_submit(xfer);
	}
}

static void
rtw88_native_rx0_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_rx_callback_common(xfer, error, 0);
}

static void
rtw88_native_rx1_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_rx_callback_common(xfer, error, 1);
}

static void
rtw88_native_rx2_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_rx_callback_common(xfer, error, 2);
}

static void
rtw88_native_rx3_callback(struct usb_xfer *xfer, usb_error_t error)
{
	rtw88_native_rx_callback_common(xfer, error, 3);
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
	[RTW88_NATIVE_BULK_RX0] = {
		.type = UE_BULK,
		.endpoint = RTW88_NATIVE_USB_EP_BULK_IN,
		.ep_index = 0,
		.direction = UE_DIR_IN,
		.bufsize = RTW88_NATIVE_RX_BUFSIZE,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_rx0_callback,
		.timeout = 0
	},
	[RTW88_NATIVE_BULK_RX1] = {
		.type = UE_BULK,
		.endpoint = RTW88_NATIVE_USB_EP_BULK_IN,
		.ep_index = 0,
		.direction = UE_DIR_IN,
		.bufsize = RTW88_NATIVE_RX_BUFSIZE,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_rx1_callback,
		.timeout = 0
	},
	[RTW88_NATIVE_BULK_RX2] = {
		.type = UE_BULK,
		.endpoint = RTW88_NATIVE_USB_EP_BULK_IN,
		.ep_index = 0,
		.direction = UE_DIR_IN,
		.bufsize = RTW88_NATIVE_RX_BUFSIZE,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_rx2_callback,
		.timeout = 0
	},
	[RTW88_NATIVE_BULK_RX3] = {
		.type = UE_BULK,
		.endpoint = RTW88_NATIVE_USB_EP_BULK_IN,
		.ep_index = 0,
		.direction = UE_DIR_IN,
		.bufsize = RTW88_NATIVE_RX_BUFSIZE,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
			.ext_buffer = 1
		},
		.callback = rtw88_native_rx3_callback,
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
 * Start the four concurrent 32 KiB bulk-IN requests used by Linux RTW88.
 * Each request has an independent DMA buffer and is immediately resubmitted
 * after completion so USB RX aggregation can progress without gaps. Raw
 * batches are parsed and queued to the net80211 input task.
 */
int
rtw88_native_usb_rx_start(struct rtw88_native_softc *sc)
{
	struct usb_xfer *xfer;
	unsigned int i;

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
	for (i = 0; i < RTW88_NATIVE_RX_XFERS; i++) {
		xfer = sc->xfer[RTW88_NATIVE_BULK_RX0 + i];
		if (xfer == NULL) {
			mtx_unlock(&sc->mtx);
			return (ENXIO);
		}
	}
	bzero(sc->rx_buf, sizeof(sc->rx_buf));
	bzero(sc->rx_last, sizeof(sc->rx_last));
	sc->rx_last_len = 0;
	sc->rx_count = 0;
	sc->rx_submit_count = 0;
	sc->rx_error_count = 0;
	sc->rx_error = USB_ERR_NORMAL_COMPLETION;
	sc->rx_error_code = USB_ERR_NORMAL_COMPLETION;
	sc->rx_started = 1;
	for (i = 0; i < RTW88_NATIVE_RX_XFERS; i++)
		usbd_transfer_start(sc->xfer[RTW88_NATIVE_BULK_RX0 + i]);
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
	struct usb_xfer *xfer[RTW88_NATIVE_RX_XFERS];
	unsigned int i;

	if (sc == NULL || !sc->mtx_initialized)
		return;
	mtx_lock(&sc->mtx);
	sc->rx_started = 0;
	for (i = 0; i < RTW88_NATIVE_RX_XFERS; i++)
		xfer[i] = sc->xfers_initialized ?
		    sc->xfer[RTW88_NATIVE_BULK_RX0 + i] : NULL;
	mtx_unlock(&sc->mtx);

	for (i = 0; i < RTW88_NATIVE_RX_XFERS; i++) {
		if (xfer[i] != NULL)
			usbd_transfer_drain(xfer[i]);
	}
}
