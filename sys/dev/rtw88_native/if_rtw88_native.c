/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * This file implements the native FreeBSD USB and net80211 glue for the
 * Realtek RTL8822BU (USB ID 0bda:b812). It currently supports station and
 * monitor VAP creation, active/passive scanning, and queued 802.11 frame
 * transmission for management/authentication/association and data frames.
 * Hardware key installation, advanced rate control and full TX status
 * reporting remain follow-on work.
 *
 * Register request format is the one used by the RTW88 USB hardware
 * interface: vendor control request 0x05, wValue = register address,
 * wIndex = 0, and a little-endian payload.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/taskqueue.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include "if_rtw88_native.h"
#include "rtw88_native_build.h"

/*
 * Keep the initial port deliberately narrow: bind only to the adapter that
 * is being developed and only to its vendor-specific interface.
 */
static const STRUCT_USB_HOST_ID rtw88_native_devs[] = {
	{
		USB_VP(RTW88_NATIVE_VENDOR_REALTEK,
		    RTW88_NATIVE_PRODUCT_RTL8822BU),
		USB_IFACE_CLASS(0xff),
		USB_IFACE_SUBCLASS(0xff),
		USB_IFACE_PROTOCOL(0xff)
	},
};

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


struct rtw88_native_vap {
	struct ieee80211vap	vap;
};

static void
rtw88_native_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
    struct ieee80211_channel chans[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];
	int count, i, out;

	(void)ic;
	bzero(bands, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	count = 0;
	(void)ieee80211_add_channels_default_2ghz(chans, maxchans, &count,
	    bands, 0);

	/* This bring-up port only tunes channels 1-13 at 20 MHz. */
	out = 0;
	for (i = 0; i < count; i++) {
		if (chans[i].ic_ieee >= 1 && chans[i].ic_ieee <= 13)
			chans[out++] = chans[i];
	}
	*nchans = out;
}

static struct ieee80211vap *
rtw88_native_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ],
    int unit, enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct rtw88_native_vap *nvp;
	struct ieee80211vap *vap;

	/* Do not advertise AP/IBSS operation before a working TX path exists. */
	if (opmode != IEEE80211_M_STA && opmode != IEEE80211_M_MONITOR)
		return (NULL);
	if (!TAILQ_EMPTY(&ic->ic_vaps))
		return (NULL);

	nvp = malloc(sizeof(*nvp), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &nvp->vap;
	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
	    flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		free(nvp, M_80211_VAP);
		return (NULL);
	}
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	return (vap);
}

static void
rtw88_native_vap_delete(struct ieee80211vap *vap)
{
	struct rtw88_native_vap *nvp;

	nvp = (struct rtw88_native_vap *)vap;
	ieee80211_stop(vap);
	ieee80211_vap_detach(vap);
	free(nvp, M_80211_VAP);
}

/*
 * The software scanner calls this after it has populated ss_flags. Keep the
 * requested scan mode: the default net80211 scanner sends probe requests
 * through ic_raw_xmit for active scans, and listens only for passive scans.
 */
static void
rtw88_native_scan_start(struct ieee80211com *ic)
{
	struct rtw88_native_softc *sc;
	const char *mode;

	sc = ic->ic_softc;
	mode = (ic->ic_scan != NULL &&
	    (ic->ic_scan->ss_flags & IEEE80211_SCAN_ACTIVE) != 0) ?
	    "active" : "passive";
	device_printf(sc->dev,
	    "starting %s 2.4-GHz scan (channels 1-13)\n", mode);
}

static void
rtw88_native_scan_end(struct ieee80211com *ic)
{
	struct rtw88_native_softc *sc;

	sc = ic->ic_softc;
	device_printf(sc->dev, "net80211 scan finished\n");
}

static void
rtw88_native_set_channel(struct ieee80211com *ic)
{
	struct rtw88_native_softc *sc;
	struct ieee80211_channel *c;
	int channel, error;

	sc = ic->ic_softc;
	c = ic->ic_curchan;
	if (c == NULL)
		return;
	channel = c->ic_ieee;

	/* Serialize scan channel changes against sysctl start/stop/manual tune. */
	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->hw_started || sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		device_printf(sc->dev,
		    "skipping channel %d change while hardware is busy or stopped\n",
		    channel);
		return;
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	error = rtw88_native_phy_set_channel(sc, (uint8_t)channel);

	mtx_lock(&sc->mtx);
	if (error == 0)
		sc->current_channel = channel;
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev,
		    "net80211 channel change to %d failed: %d\n",
		    channel, error);
}


/*
 * Enqueue one already-encapsulated 802.11 frame. Net80211 transfers its node
 * reference with m_pkthdr.rcvif when ic_transmit/ic_raw_xmit returns success.
 * USB submission is deferred because the transfer helper sleeps.
 */
static int
rtw88_native_tx_enqueue(struct rtw88_native_softc *sc, struct mbuf *m)
{
	int error;

	if (sc == NULL || m == NULL || (m->m_flags & M_PKTHDR) == 0 ||
	    m->m_pkthdr.rcvif == NULL)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->net80211_attached ||
	    !sc->tx_enabled) {
		error = ENETDOWN;
	} else {
		error = mbufq_enqueue(&sc->tx_queue, m);
		if (error == 0) {
			sc->tx_queue_depth++;
			sc->tx_queued_count++;
		}
	}
	mtx_unlock(&sc->mtx);
	if (error == 0)
		taskqueue_enqueue(taskqueue_thread, &sc->tx_task);
	return (error);
}

/*
 * FreeBSD's normal scanner and STA management state machine populate the
 * BPF parameter structure even for ordinary kernel-generated frames. For
 * example, ProbeReq uses ibp_pri/rate0/try0/power and sets NOACK for its
 * broadcast destination; authentication and association use VO, a
 * management rate, retries and power. Those are not user radiotap overrides.
 *
 * Accept those stack-generated defaults, while rejecting descriptor features
 * this first TX path cannot honour (crypto offload, RTS/CTS, short preamble,
 * explicit retry series, data padding and FCS). The TX descriptor currently
 * uses a conservative fixed 1-Mbps rate; retry/power/rate requests are
 * validated but not yet applied.
 */
static int
rtw88_native_raw_params_supported(const struct ieee80211_bpf_params *params,
    struct mbuf *m)
{
	static const uint8_t legacy_rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	struct ieee80211_frame_min wh;
	unsigned int i;

	if (params == NULL)
		return (1);
	if (params->ibp_vers != 0 || params->ibp_len != 0 ||
	    (params->ibp_flags & ~IEEE80211_BPF_NOACK) != 0 ||
	    params->ibp_pri > WME_AC_VO ||
	    params->ibp_try0 > 15 || params->ibp_ctsrate != 0 ||
	    params->ibp_try1 != 0 || params->ibp_rate1 != 0 ||
	    params->ibp_try2 != 0 || params->ibp_rate2 != 0 ||
	    params->ibp_try3 != 0 || params->ibp_rate3 != 0)
		return (0);

	if (params->ibp_rate0 != 0) {
		for (i = 0; i < nitems(legacy_rates); i++) {
			if (params->ibp_rate0 == legacy_rates[i])
				break;
		}
		if (i == nitems(legacy_rates))
			return (0);
	}

	/* NOACK is only accepted for a group-addressed frame. */
	if ((params->ibp_flags & IEEE80211_BPF_NOACK) != 0) {
		if (m == NULL || m->m_pkthdr.len < sizeof(wh))
			return (0);
		m_copydata(m, 0, sizeof(wh), (caddr_t)&wh);
		if (!IEEE80211_IS_MULTICAST(wh.i_addr1))
			return (0);
	}
	return (1);
}

static int
rtw88_native_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params)
{
	struct rtw88_native_softc *sc;
	int error;

	if (ni == NULL || m == NULL) {
		if (m != NULL)
			m_freem(m);
		return (EINVAL);
	}
	sc = ni->ni_ic->ic_softc;
	mtx_lock(&sc->mtx);
	sc->tx_raw_count++;
	mtx_unlock(&sc->mtx);

	if (!rtw88_native_raw_params_supported(params, m)) {
		mtx_lock(&sc->mtx);
		sc->tx_raw_reject_count++;
		mtx_unlock(&sc->mtx);
		if (m->m_flags & M_TXCB)
			ieee80211_process_callback(ni, m, 1);
		m_freem(m);
		return (EOPNOTSUPP);
	}

	error = rtw88_native_tx_enqueue(sc, m);
	if (error != 0) {
		mtx_lock(&sc->mtx);
		sc->tx_raw_reject_count++;
		mtx_unlock(&sc->mtx);
		if (m->m_flags & M_TXCB)
			ieee80211_process_callback(ni, m, 1);
		m_freem(m);
	}
	return (error);
}

static int
rtw88_native_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	struct rtw88_native_softc *sc;

	sc = ic->ic_softc;
	return (rtw88_native_tx_enqueue(sc, m));
}

/*
 * Send one 802.11 MPDU through the RTW88 48-byte descriptor and the USB
 * endpoint selected by the RTL8822B queue map. Management/control frames use
 * QSEL_MGMT (18 -> high endpoint 0x05); data frames use TID0/BE (0 -> low
 * endpoint 0x08). TX is serialized by the sleepable TX task.
 */
static int
rtw88_native_tx_one(struct rtw88_native_softc *sc, struct mbuf *m)
{
	struct ieee80211_frame_min whbuf;
	struct ieee80211_frame_min *wh;
	struct rtw88_native_txdesc_params params;
	uint8_t *buffer;
	uint16_t frame_len, transfer_len;
	uint8_t qsel, endpoint;
	uint8_t type;
	uint16_t seqctl;
	usb_error_t uerr;
	int error;

	if (m->m_pkthdr.len < sizeof(whbuf))
		return (EINVAL);
	if (m->m_pkthdr.len > 20 * 1024 - RTW88_NATIVE_TX_DESC_SIZE)
		return (EFBIG);

	/* The header may span mbuf segments; copy only the bounded header. */
	m_copydata(m, 0, sizeof(whbuf), (caddr_t)&whbuf);
	wh = &whbuf;
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	if (type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_CTL)
		qsel = 18;		/* TX_DESC_QSEL_MGMT */
	else if (type == IEEE80211_FC0_TYPE_DATA)
		qsel = 0;		/* TX_DESC_QSEL_TID0 / best effort */
	else
		return (EOPNOTSUPP);

	endpoint = sc->qsel_to_ep[qsel];
	if (endpoint >= RTW88_NATIVE_TX_QUEUES)
		return (EOPNOTSUPP);
	frame_len = (uint16_t)m->m_pkthdr.len;
	transfer_len = frame_len + RTW88_NATIVE_TX_DESC_SIZE;

	buffer = malloc(transfer_len, M_DEVBUF, M_WAITOK);
	if (buffer == NULL)
		return (ENOMEM);
	bzero(buffer, transfer_len);
	bzero(&params, sizeof(params));
	params.packet_size = frame_len;
	params.descriptor_offset = RTW88_NATIVE_TX_DESC_SIZE;
	params.mac_id = 0;
	params.queue_select = qsel;
	/*
	 * Use the 2.4-GHz B rate table for management frames at 1 Mbps. Until
	 * association-specific rate control is ported, data uses the BG legacy
	 * table at the universally supported 1-Mbps basic rate.
	 */
	if (type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_CTL) {
		params.rate_id = 8;	/* RTW_RATEID_B_20M */
		params.data_rate = 0;	/* DESC_RATE1M */
	} else {
		params.rate_id = 6;	/* RTW_RATEID_BG */
		params.data_rate = 0;	/* DESC_RATE1M */
	}
	params.use_hardware_rate = true;
	params.disable_rate_fallback = true;
	params.last_segment = true;
	params.bmc = IEEE80211_IS_MULTICAST(wh->i_addr1);
	params.more_data = (m->m_flags & M_MORE_DATA) != 0;
	if (type == IEEE80211_FC0_TYPE_MGT) {
		/* Match RTW88 management frames: hardware sequence on QSEL_MGMT. */
		params.disable_qsel_sequence = true;
		params.enable_hardware_sequence = true;
	} else if (type == IEEE80211_FC0_TYPE_DATA) {
		/* Preserve the sequence number already assigned by net80211. */
		if (frame_len < offsetof(struct ieee80211_frame, i_seq) +
		    sizeof(seqctl)) {
			error = EINVAL;
			goto out;
		}
		m_copydata(m, offsetof(struct ieee80211_frame, i_seq),
		    sizeof(seqctl), (caddr_t)&seqctl);
		params.software_sequence =
		    (le16toh(seqctl) >> IEEE80211_SEQ_SEQ_SHIFT) & 0x0fff;
	}

	error = rtw88_native_txdesc_build(buffer,
	    RTW88_NATIVE_TX_DESC_SIZE, &params);
	if (error == 0)
		m_copydata(m, 0, frame_len,
		    buffer + RTW88_NATIVE_TX_DESC_SIZE);
	if (error == 0) {
		uerr = rtw88_native_usb_tx_sync(sc, endpoint, buffer,
		    transfer_len);
		if (uerr != USB_ERR_NORMAL_COMPLETION)
			error = EIO;
	}
out:
	free(buffer, M_DEVBUF);
	return (error);
}

void
rtw88_native_tx_task(void *arg, int pending __unused)
{
	struct rtw88_native_softc *sc;
	struct ieee80211_node *ni;
	struct mbuf *m;
	int error, enabled;

	sc = arg;
	for (;;) {
		mtx_lock(&sc->mtx);
		m = mbufq_dequeue(&sc->tx_queue);
		if (m != NULL)
			sc->tx_queue_depth--;
		enabled = sc->tx_enabled && !sc->dying && !sc->detaching;
		mtx_unlock(&sc->mtx);
		if (m == NULL)
			break;

		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;
		error = enabled ? rtw88_native_tx_one(sc, m) : ENETDOWN;

		mtx_lock(&sc->mtx);
		if (error == 0)
			sc->tx_complete_count++;
		else {
			sc->tx_error_count++;
			sc->tx_last_error = error;
		}
		mtx_unlock(&sc->mtx);

		/* Completes the mbuf and the node reference transferred by net80211. */
		ieee80211_tx_complete(ni, m, error == 0 ? 0 : 1);
	}
}

static void
rtw88_native_parent(struct ieee80211com *ic)
{
	struct rtw88_native_softc *sc;
	int hw_started;

	sc = ic->ic_softc;
	mtx_lock(&sc->mtx);
	hw_started = sc->hw_started;
	mtx_unlock(&sc->mtx);
	if (!hw_started && ic->ic_nrunning != 0)
		device_printf(sc->dev,
		    "wlan VAP is up but hardware is stopped; set dev.rtw88_native.%d.hw_start=1\n",
		    device_get_unit(sc->dev));
}

/*
 * Attach a minimal native net80211 radio after firmware and PHY startup have
 * completed. A user-created wlan VAP will then be visible to ifconfig and can
 * use the standard software scan cache. Only 2.4-GHz channels 1-13 are exposed.
 */
int
rtw88_native_net80211_attach(struct rtw88_native_softc *sc)
{
	struct ieee80211com *ic;

	if (sc == NULL || !sc->phy_initialized || !sc->efuse.macaddr_valid)
		return (EINVAL);
	if (sc->net80211_attached)
		return (0);

	ic = &sc->ic;
	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(sc->dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->efuse.macaddr);
	ic->ic_caps = IEEE80211_C_STA | IEEE80211_C_MONITOR |
	    IEEE80211_C_WPA;
	setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
	rtw88_native_getradiocaps(ic, IEEE80211_CHAN_MAX,
	    &ic->ic_nchans, ic->ic_channels);
	if (ic->ic_nchans == 0)
		return (ENXIO);

	ieee80211_ifattach(ic);
	ic->ic_getradiocaps = rtw88_native_getradiocaps;
	ic->ic_vap_create = rtw88_native_vap_create;
	ic->ic_vap_delete = rtw88_native_vap_delete;
	ic->ic_parent = rtw88_native_parent;
	ic->ic_set_channel = rtw88_native_set_channel;
	ic->ic_scan_start = rtw88_native_scan_start;
	ic->ic_scan_end = rtw88_native_scan_end;
	ic->ic_raw_xmit = rtw88_native_raw_xmit;
	ic->ic_transmit = rtw88_native_transmit;

	/* Keep authentication/association crypto in net80211 software for now. */
	ieee80211_set_software_ciphers(ic, IEEE80211_CRYPTO_WEP |
	    IEEE80211_CRYPTO_TKIP | IEEE80211_CRYPTO_AES_CCM);

	mtx_lock(&sc->mtx);
	sc->net80211_attached = 1;
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev,
	    "net80211 radio attached as %s (2.4 GHz ch 1-13; management/data TX enabled)\n",
	    ic->ic_name);
	return (0);
}

void
rtw88_native_net80211_detach(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->net80211_attached)
		return;

	mtx_lock(&sc->mtx);
	sc->net80211_attached = 0;
	mtx_unlock(&sc->mtx);
	ieee80211_ifdetach(&sc->ic);
	bzero(&sc->ic, sizeof(sc->ic));
}

/*
 * USB receive callbacks enqueue owned mbufs. Net80211 input is deferred to a
 * taskqueue thread so its scan-cache/state machinery never runs under the USB
 * transfer mutex.
 */
void
rtw88_native_rx_task(void *arg, int pending __unused)
{
	struct rtw88_native_softc *sc;
	struct mbuf *m;
	int attached;

	sc = arg;
	for (;;) {
		mtx_lock(&sc->mtx);
		m = mbufq_dequeue(&sc->rx_queue);
		attached = sc->net80211_attached && !sc->dying;
		mtx_unlock(&sc->mtx);
		if (m == NULL)
			break;
		if (attached) {
			(void)ieee80211_input_mimo_all(&sc->ic, m);
			mtx_lock(&sc->mtx);
			sc->rx_input_count++;
			mtx_unlock(&sc->mtx);
		} else {
			m_freem(m);
			mtx_lock(&sc->mtx);
			sc->rx_input_drop_count++;
			mtx_unlock(&sc->mtx);
		}
	}
}

static int
rtw88_native_sysctl_hw_start(SYSCTL_HANDLER_ARGS)
{
	struct rtw88_native_softc *sc;
	int value, error;

	sc = arg1;
	mtx_lock(&sc->mtx);
	value = sc->hw_started;
	mtx_unlock(&sc->mtx);

	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (value != 0 && value != 1)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	if (value == sc->hw_started) {
		mtx_unlock(&sc->mtx);
		return (0);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	/*
	 * FIRMWARE(9) warns that firmware_get() can access the filesystem and
	 * must not run with a driver lock held. hw_busy serializes this sleepable
	 * transition while leaving the USB/register helpers free to take mtx.
	 */
	if (value != 0)
		error = rtw88_native_hw_start(sc);
	else {
		rtw88_native_hw_stop(sc);
		error = 0;
	}

	mtx_lock(&sc->mtx);
	if (error == 0)
		sc->hw_started = value;
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev, "hardware %s failed: %d\n",
		    value ? "start" : "stop", error);
	return (error);
}


static int
rtw88_native_sysctl_channel(SYSCTL_HANDLER_ARGS)
{
	struct rtw88_native_softc *sc;
	int value, error;

	sc = arg1;
	mtx_lock(&sc->mtx);
	value = sc->current_channel;
	mtx_unlock(&sc->mtx);

	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (value < 1 || value > 13)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (!sc->hw_started || !sc->phy_initialized) {
		mtx_unlock(&sc->mtx);
		return (EAGAIN);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	error = rtw88_native_phy_set_channel(sc, (uint8_t)value);

	mtx_lock(&sc->mtx);
	if (error == 0)
		sc->current_channel = value;
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error != 0)
		device_printf(sc->dev,
		    "2.4-GHz channel %d configuration failed: %d\n", value, error);
	return (error);
}

/*
 * Read back the live MAC/USB RX configuration without disturbing the radio.
 * This is a process-context diagnostic: hw_busy prevents the scanner's
 * sleepable channel callback from interleaving its RF register writes.
 */
static int
rtw88_native_sysctl_rx_diag(SYSCTL_HANDLER_ARGS)
{
	struct rtw88_native_softc *sc;
	struct {
		const char *name;
		uint16_t reg;
		uint32_t value;
		usb_error_t error;
	} regs[] = {
		{ "CR", 0x0100, 0, USB_ERR_NORMAL_COMPLETION },
		{ "TXDMA_PQ_MAP", 0x010c, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXDMA_AGG_PG_TH", 0x0280, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXDMA_MODE", 0x0290, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RCR", 0x0608, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXFLTMAP0", 0x06a0, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXFLTMAP2", 0x06a4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "SYS_FUNC_EN", 0x0002, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_CTRL", 0x001f, 0, USB_ERR_NORMAL_COMPLETION },
		{ "WLRF1", 0x00ec, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_SIPI_A", 0x0c90, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_SIPI_B", 0x0e90, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXPSEL", 0x0808, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXIGI_A", 0x0c50, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXIGI_B", 0x0e50, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_A_CH18", 0x2860, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_B_CH18", 0x2c60, 0, USB_ERR_NORMAL_COMPLETION }
	};
	int value, error, i, channel;
	uint64_t rx_count, rx_submits, rx_errors;
	int rx_error_code, rx_started;

	sc = arg1;
	value = 0;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (value != 1)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	if (sc->dying || sc->detaching || !sc->hw_started) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	if (sc->hw_busy) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	for (i = 0; i < nitems(regs); i++)
		regs[i].error = rtw88_native_reg_read32(sc, regs[i].reg,
		    &regs[i].value);

	mtx_lock(&sc->mtx);
	channel = sc->current_channel;
	rx_started = sc->rx_started;
	rx_count = sc->rx_count;
	rx_submits = sc->rx_submit_count;
	rx_errors = sc->rx_error_count;
	rx_error_code = sc->rx_error_code;
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	device_printf(sc->dev,
	    "RX diag: channel=%d started=%d xfers=%u submits=%ju completions=%ju errors=%ju last_error=%d\n",
	    channel, rx_started, RTW88_NATIVE_RX_XFERS,
	    (uintmax_t)rx_submits, (uintmax_t)rx_count,
	    (uintmax_t)rx_errors, rx_error_code);
	error = 0;
	for (i = 0; i < nitems(regs); i++) {
		if (regs[i].error == USB_ERR_NORMAL_COMPLETION) {
			device_printf(sc->dev, "RX diag: %-14s [0x%04x] = 0x%08x\n",
			    regs[i].name, regs[i].reg, regs[i].value);
		} else {
			device_printf(sc->dev,
			    "RX diag: %-14s [0x%04x] read failed: %s\n",
			    regs[i].name, regs[i].reg,
			    usbd_errstr(regs[i].error));
			error = EIO;
		}
	}
	return (error);
}


static int
rtw88_native_attach(device_t dev)
{
	struct rtw88_native_softc *sc;
	struct usb_attach_arg *uaa;
	uint32_t sys_cfg1;
	usb_error_t uerr;
	int i;

	sc = device_get_softc(dev);
	bzero(sc, sizeof(*sc));
	uaa = device_get_ivars(dev);
	sc->dev = dev;
	if (uaa == NULL || uaa->device == NULL)
		return (ENXIO);

	sc->udev = uaa->device;
	sc->iface_index = uaa->info.bIfaceIndex;

	mtx_init(&sc->mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK, MTX_DEF);
	sc->mtx_initialized = 1;
	for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
		cv_init(&sc->tx_cv[i], "rtw88 native tx");
	sc->tx_cv_initialized = 1;
	mbufq_init(&sc->rx_queue, 64);
	TASK_INIT(&sc->rx_task, 0, rtw88_native_rx_task, sc);
	mbufq_init(&sc->tx_queue, 64);
	TASK_INIT(&sc->tx_task, 0, rtw88_native_tx_task, sc);
	cv_init(&sc->io_drain_cv, "rtw88 native io");
	sc->io_cv_initialized = 1;

	device_printf(dev,
	    "native FreeBSD USB transport attached (RTL8822BU, 0bda:b812)\n");
	device_printf(dev, "native driver build ID: %s\n",
	    RTW88_NATIVE_BUILD_ID);

	/*
	 * Allocate endpoint-backed resources through the native FreeBSD USB
	 * layer. Endpoint addresses are resolved from interface descriptors.
	 */
	uerr = rtw88_native_usb_setup(sc);
	if (uerr != USB_ERR_NORMAL_COMPLETION) {
		device_printf(dev, "could not allocate native USB transfers: %s\n",
		    usbd_errstr(uerr));
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENXIO);
	}
	device_printf(dev,
	    "allocated native USB pipes: 4 bulk IN, 3 bulk OUT, 1 interrupt IN (idle)\n");

	/*
	 * REG_SYS_CFG1 (0x00f0) is the read-only system configuration register
	 * used by the upstream RTW88 chip-identification path.  Probe it once
	 * without changing device state; this tests the native control pipe.
	 */
	uerr = rtw88_native_raw_reg_read32(sc, RTW88_NATIVE_REG_SYS_CFG1, &sys_cfg1);
	if (uerr == USB_ERR_NORMAL_COMPLETION) {
		device_printf(dev,
		    "native vendor-control read succeeded: SYS_CFG1=0x%08x\n",
		    sys_cfg1);
	} else {
		device_printf(dev,
		    "native vendor-control read of SYS_CFG1 failed: %s\n",
		    usbd_errstr(uerr));
	}

	/*
	 * Hardware startup is intentionally deferred until the root filesystem
	 * is mounted. The sysctl handler runs in sleepable process context.
	 */
	if (SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "hw_start", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    sc, 0, rtw88_native_sysctl_hw_start, "I",
	    "Start (1) or stop (0) RTL8822BU hardware and interrupt capture") == NULL) {
		rtw88_native_usb_unsetup(sc);
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENOMEM);
	}
	if (SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_diag", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    sc, 0, rtw88_native_sysctl_rx_diag, "I",
	    "Read back live RTL8822B RX/MAC/RF configuration (write 1)") == NULL) {
		rtw88_native_usb_unsetup(sc);
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENOMEM);
	}
	if (SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "channel", CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    sc, 0, rtw88_native_sysctl_channel, "I",
	    "Tune RFE option 3 to a 2.4-GHz channel (1-13), 20 MHz") == NULL) {
		rtw88_native_usb_unsetup(sc);
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
		mtx_destroy(&sc->mtx);
		sc->mtx_initialized = 0;
		return (ENOMEM);
	}
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "intr_count", CTLFLAG_RD, &sc->intr_count, 0,
	    "Number of USB interrupt reports captured");
	SYSCTL_ADD_U16(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "intr_last_len", CTLFLAG_RD, &sc->last_intr_len, 0,
	    "Length of the most recent USB interrupt report");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_count", CTLFLAG_RD, &sc->rx_count, 0,
	    "Number of USB bulk-IN aggregates received");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_desc_count", CTLFLAG_RD, &sc->rx_desc_count, 0,
	    "Number of RX descriptors parsed");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_data_count", CTLFLAG_RD, &sc->rx_data_count, 0,
	    "Number of non-C2H RX frames identified");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_c2h_count", CTLFLAG_RD, &sc->rx_c2h_count, 0,
	    "Number of firmware C2H RX records identified");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_malformed_count", CTLFLAG_RD, &sc->rx_malformed_count, 0,
	    "Number of malformed/truncated RX records");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_input_count", CTLFLAG_RD, &sc->rx_input_count, 0,
	    "Number of RX frames passed to net80211");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_input_drop_count", CTLFLAG_RD, &sc->rx_input_drop_count, 0,
	    "Number of RX frames discarded before net80211 input");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_raw_count", CTLFLAG_RD, &sc->tx_raw_count, 0,
	    "Number of raw net80211 frames passed to the driver");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_raw_reject_count", CTLFLAG_RD, &sc->tx_raw_reject_count, 0,
	    "Number of raw net80211 frames rejected before queueing");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_queued_count", CTLFLAG_RD, &sc->tx_queued_count, 0,
	    "Number of 802.11 frames queued for USB transmission");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_complete_count", CTLFLAG_RD, &sc->tx_complete_count, 0,
	    "Number of frames completed on USB bulk-OUT");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_error_count", CTLFLAG_RD, &sc->tx_error_count, 0,
	    "Number of frames that failed before or during USB transmission");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_queue_depth", CTLFLAG_RD, &sc->tx_queue_depth, 0,
	    "Number of 802.11 frames waiting for USB transmission");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_last_error", CTLFLAG_RD, &sc->tx_last_error, 0,
	    "Last transmit error (zero means no error)");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_enabled", CTLFLAG_RD, &sc->tx_enabled, 0,
	    "Whether the transmit worker may submit frames");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "net80211_attached", CTLFLAG_RD, &sc->net80211_attached, 0,
	    "Whether the net80211 radio has been attached");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_submit_count", CTLFLAG_RD, &sc->rx_submit_count, 0,
	    "Number of bulk-IN transfer submit attempts");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_error_count", CTLFLAG_RD, &sc->rx_error_count, 0,
	    "Number of non-cancelled bulk-IN transfer errors");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_error_code", CTLFLAG_RD, &sc->rx_error_code, 0,
	    "Last USB bulk-IN transfer error code (0 means no error)");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_started", CTLFLAG_RD, &sc->rx_started, 0,
	    "Whether continuous bulk-IN collection is armed");
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_len", CTLFLAG_RD, &sc->rx_last_len, 0,
	    "Length of the latest raw USB bulk-IN aggregate");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_id", CTLFLAG_RD, &sc->rx_last_c2h_id, 0,
	    "C2H command ID from the latest firmware event");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_seq", CTLFLAG_RD, &sc->rx_last_c2h_seq, 0,
	    "Sequence number from the latest firmware C2H event");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_c2h_len", CTLFLAG_RD, &sc->rx_last_c2h_len, 0,
	    "Payload length from the latest firmware C2H event");

	device_printf(dev,
	    "deferred hardware start available at dev.rtw88_native.%d.hw_start\n",
	    device_get_unit(dev));

	return (0);
}

static int
rtw88_native_detach(device_t dev)
{
	struct rtw88_native_softc *sc;
	int i;

	sc = device_get_softc(dev);

	/*
	 * Prevent new sysctl-triggered hardware transitions and wait for any
	 * current start/stop operation before draining interrupt I/O and power.
	 */
	if (sc->mtx_initialized) {
		mtx_lock(&sc->mtx);
		sc->detaching = 1;
		while (sc->hw_busy)
			cv_wait(&sc->io_drain_cv, &sc->mtx);
		mtx_unlock(&sc->mtx);
	}
	rtw88_native_hw_stop(sc);
	rtw88_native_net80211_detach(sc);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);
	mtx_lock(&sc->mtx);
	{
		struct mbuf *m;

		while ((m = mbufq_dequeue(&sc->rx_queue)) != NULL) {
			sc->rx_input_drop_count++;
			m_freem(m);
		}
	}
	mtx_unlock(&sc->mtx);
	if (sc->mtx_initialized) {
		mtx_lock(&sc->mtx);
		sc->hw_started = 0;
		sc->dying = 1;
		mtx_unlock(&sc->mtx);
	}

	/* Stop DMA callbacks first, then wait for synchronous I/O callers. */
	rtw88_native_usb_unsetup(sc);
	if (sc->mtx_initialized && sc->io_cv_initialized) {
		mtx_lock(&sc->mtx);
		while (sc->io_users != 0)
			cv_wait(&sc->io_drain_cv, &sc->mtx);
		mtx_unlock(&sc->mtx);
	}

	rtw88_native_firmware_put(sc);
	if (sc->tx_cv_initialized) {
		for (i = 0; i < RTW88_NATIVE_TX_QUEUES; i++)
			cv_destroy(&sc->tx_cv[i]);
		sc->tx_cv_initialized = 0;
	}
	if (sc->io_cv_initialized) {
		cv_destroy(&sc->io_drain_cv);
		sc->io_cv_initialized = 0;
	}
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
MODULE_DEPEND(rtw88_native, wlan, 1, 1, 1);
MODULE_VERSION(rtw88_native, 1);
USB_PNP_HOST_INFO(rtw88_native_devs);
