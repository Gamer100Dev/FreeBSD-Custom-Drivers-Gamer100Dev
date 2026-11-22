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
	int (*newstate)(struct ieee80211vap *, enum ieee80211_state, int);
};

static int
rtw88_native_station_link_update(struct rtw88_native_softc *sc,
    const uint8_t *bssid, uint16_t aid, int linked)
{
	int error;

	if (linked && (bssid == NULL || (bssid[0] & 0x01) != 0 ||
	    aid == 0 || aid > 0x07ff))
		return (EINVAL);
	mtx_lock(&sc->mtx);
	if (!linked && (sc->hw_stopping || sc->dying || sc->detaching ||
	    !sc->hw_started)) {
		/* Do not block VAP shutdown on a concurrent stop/channel operation. */
		sc->station_linked = 0;
		sc->station_aid = 0;
		bzero(sc->station_bssid, sizeof(sc->station_bssid));
		mtx_unlock(&sc->mtx);
		return (0);
	}
	while (sc->hw_busy && !sc->dying && !sc->detaching &&
	    !sc->hw_stopping)
		cv_wait(&sc->io_drain_cv, &sc->mtx);
	if (sc->dying || sc->detaching || sc->hw_stopping ||
	    !sc->hw_started || !sc->mac_initialized) {
		error = linked ? ENETDOWN : 0;
		if (!linked) {
			sc->station_linked = 0;
			sc->station_aid = 0;
			bzero(sc->station_bssid, sizeof(sc->station_bssid));
		}
		mtx_unlock(&sc->mtx);
		return (error);
	}
	sc->hw_busy = 1;
	mtx_unlock(&sc->mtx);

	error = rtw88_native_mac_set_link(sc, bssid, aid, linked);
	if (error != 0 && linked)
		(void)rtw88_native_mac_set_link(sc, NULL, 0, 0);

	mtx_lock(&sc->mtx);
	if (error == 0 && linked) {
		sc->station_linked = 1;
		sc->station_aid = aid;
		IEEE80211_ADDR_COPY(sc->station_bssid, bssid);
	} else {
		sc->station_linked = 0;
		sc->station_aid = 0;
		bzero(sc->station_bssid, sizeof(sc->station_bssid));
	}
	sc->hw_busy = 0;
	cv_broadcast(&sc->io_drain_cv);
	mtx_unlock(&sc->mtx);

	if (error == 0 && linked)
		device_printf(sc->dev,
		    "STA link programmed: BSSID %02x:%02x:%02x:%02x:%02x:%02x AID %u (managed-linked)\n",
		    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], aid);
	else if (error != 0 && linked)
		device_printf(sc->dev,
		    "STA link register programming failed for AID %u: %d\n",
		    aid, error);
	return (error);
}

static int
rtw88_native_vap_newstate(struct ieee80211vap *vap,
    enum ieee80211_state nstate, int arg)
{
	struct rtw88_native_vap *nvp;
	struct rtw88_native_softc *sc;
	struct ieee80211com *ic;
	struct ieee80211_node *ni;
	uint8_t bssid[IEEE80211_ADDR_LEN];
	uint16_t aid;
	enum ieee80211_state ostate;
	int linked, error;

	nvp = (struct rtw88_native_vap *)vap;
	sc = vap->iv_ic->ic_softc;
	ic = vap->iv_ic;
	ostate = vap->iv_state;
	/*
	 * Match Linux rtw_ops_add_interface()/rtw_vif_port_config():
	 * program the actual VAP MAC into port-0 MACID registers before the
	 * station starts scanning/authenticating. The RTL8822B RCR has APM
	 * enabled, so a missing port address can filter unicast RX while
	 * multicast/EAPOL still appears to work. USB register I/O sleeps;
	 * release the net80211 lock around the operation.
	 */
	if (vap->iv_opmode == IEEE80211_M_STA &&
	    nstate != IEEE80211_S_INIT) {
		IEEE80211_UNLOCK(ic);
		error = rtw88_native_mac_set_address(sc, vap->iv_myaddr);
		IEEE80211_LOCK(ic);
		if (error != 0)
			return (error);
	}
	if (vap->iv_opmode == IEEE80211_M_STA &&
	    ((nstate == IEEE80211_S_RUN && ostate != IEEE80211_S_RUN) ||
	    (ostate == IEEE80211_S_RUN && nstate != IEEE80211_S_RUN))) {
		linked = (nstate == IEEE80211_S_RUN);
		bzero(bssid, sizeof(bssid));
		aid = 0;
		ni = NULL;
		if (linked) {
			if (vap->iv_bss == NULL)
				return (EINVAL);
			ni = ieee80211_ref_node(vap->iv_bss);
			IEEE80211_ADDR_COPY(bssid, ni->ni_bssid);
			aid = IEEE80211_NODE_AID(ni);
		}
		/* USB register accesses sleep; release/reacquire the net80211 lock. */
		IEEE80211_UNLOCK(ic);
		error = rtw88_native_station_link_update(sc, bssid, aid, linked);
		if (ni != NULL)
			ieee80211_free_node(ni);
		IEEE80211_LOCK(ic);
		if (error != 0 && linked)
			return (error);
		if (error != 0)
			device_printf(sc->dev,
			    "could not clear STA link during state %d: %d\n",
			    nstate, error);
	}
	if (nvp->newstate == NULL)
		return (EINVAL);
	return (nvp->newstate(vap, nstate, arg));
}

static void
rtw88_native_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
    struct ieee80211_channel chans[])
{
	static const uint8_t chan_5g_band1[] =
	    { 36, 40, 44, 48, 52, 56, 60, 64 };
	static const uint8_t chan_5g_band2[] =
	    { 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140 };
	static const uint8_t chan_5g_band3[] = { 149, 153, 157, 161 };
	uint8_t bands_2g[IEEE80211_MODE_BYTES];
	uint8_t bands_5g[IEEE80211_MODE_BYTES];
	int count, i;

	(void)ic;
	bzero(bands_2g, sizeof(bands_2g));
	bzero(bands_5g, sizeof(bands_5g));
	setbit(bands_2g, IEEE80211_MODE_11B);
	setbit(bands_2g, IEEE80211_MODE_11G);
	setbit(bands_5g, IEEE80211_MODE_11A);
	count = 0;
	(void)ieee80211_add_channels_default_2ghz(chans, maxchans, &count,
	    bands_2g, 0);
	(void)ieee80211_add_channel_list_5ghz(chans, maxchans, &count,
	    chan_5g_band1, nitems(chan_5g_band1), bands_5g, 0);
	(void)ieee80211_add_channel_list_5ghz(chans, maxchans, &count,
	    chan_5g_band2, nitems(chan_5g_band2), bands_5g, 0);
	(void)ieee80211_add_channel_list_5ghz(chans, maxchans, &count,
	    chan_5g_band3, nitems(chan_5g_band3), bands_5g, 0);
	/* 5-GHz stays passive until regulatory, DFS and TX-power work is done. */
	for (i = 0; i < count; i++) {
		if (IEEE80211_IS_CHAN_5GHZ(&chans[i])) {
			chans[i].ic_flags |= IEEE80211_CHAN_PASSIVE;
			if (chans[i].ic_ieee >= 52 && chans[i].ic_ieee <= 140)
				chans[i].ic_flags |= IEEE80211_CHAN_DFS;
		}
	}
	*nchans = count;
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
	nvp->newstate = vap->iv_newstate;
	vap->iv_newstate = rtw88_native_vap_newstate;
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
	vap->iv_newstate = nvp->newstate;
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
	int error;

	sc = ic->ic_softc;
	mode = (ic->ic_scan != NULL &&
	    (ic->ic_scan->ss_flags & IEEE80211_SCAN_ACTIVE) != 0) ?
	    "active" : "passive";
	/*
	 * Linux RTW88 disables CBSSID_BCN while FIF_BCN_PRBRESP_PROMISC is
	 * requested for scanning. Do the same for the FreeBSD software scan;
	 * otherwise the RTL8822B hardware drops other-BSS beacons before
	 * net80211 can populate the scan cache.
	 */
	error = rtw88_native_mac_set_scan_filter(sc, 1);
	if (error != 0)
		device_printf(sc->dev,
		    "could not disable beacon BSSID filtering for scan: %d\n",
		    error);
	error = rtw88_native_phy_scan_gain(sc, 1);
	if (error != 0)
		device_printf(sc->dev,
		    "could not enable scan receive coverage: %d\n", error);
	device_printf(sc->dev,
	    "starting %s dual-band scan (2.4/5-GHz, 20-MHz channels)\n", mode);
}

static void
rtw88_native_scan_end(struct ieee80211com *ic)
{
	struct rtw88_native_softc *sc;
	int error;

	sc = ic->ic_softc;
	error = rtw88_native_phy_scan_gain(sc, 0);
	if (error != 0)
		device_printf(sc->dev,
		    "could not restore receive gain after scan: %d\n", error);
	error = rtw88_native_mac_set_scan_filter(sc, 0);
	if (error != 0)
		device_printf(sc->dev,
		    "could not restore beacon BSSID filtering after scan: %d\n",
		    error);
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
	if (error == 0) {
		sc->current_channel = channel;
		/* Linux RTW88 marks RFK pending after channel setup. */
		sc->rfk_pending = 1;
	}
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
 * explicit retry series, data padding and FCS). Management and EAPOL
 * use robust fixed legacy rates; ordinary data leaves the explicit-rate bit
 * clear for firmware rate adaptation. Retry, power and explicit raw-TX rate
 * requests are still not fully implemented.
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
 * QSEL_MGMT; group-addressed data uses QSEL_HIGH; unicast data maps the
 * net80211 WME access category to its 802.11 TID. TX is serialized by the
 * sleepable TX task.
 */
static int
rtw88_native_tx_one(struct rtw88_native_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m)
{
	struct ieee80211_frame_min whbuf;
	struct ieee80211_frame_min *wh;
	struct rtw88_native_txdesc_params params;
	uint8_t *buffer;
	uint16_t frame_len, transfer_len;
	uint8_t qsel, endpoint;
	uint8_t type;
	int ac, channel;
	uint16_t seqctl;
	usb_error_t uerr;
	struct ieee80211_key *key;
	int do_rfk, rfk_error, error;

	/*
	 * FreeBSD's net80211 TX contract requires the driver to run crypto
	 * encapsulation for Protected frames. This radio advertises software
	 * ciphers and does not implement hardware key/crypto offload, so let
	 * net80211 encrypt the MPDU and expand it with the CCMP header/MIC here.
	 * Without this step the Protected bit can be set on a plaintext frame.
	 */
	if (m->m_pkthdr.len >= sizeof(whbuf)) {
		m_copydata(m, 0, sizeof(whbuf), (caddr_t)&whbuf);
		if ((whbuf.i_fc[1] & IEEE80211_FC1_PROTECTED) != 0) {
			key = ieee80211_crypto_encap(ni, m);
			if (key == NULL)
				return (ENOBUFS);
			if ((key->wk_flags & IEEE80211_KEY_SWCRYPT) == 0) {
				device_printf(sc->dev,
				    "Protected TX frame selected a non-software key; refusing unencrypted transmit\\n");
				return (EOPNOTSUPP);
			}
		}
	}

	if (m->m_pkthdr.len < sizeof(whbuf))
		return (EINVAL);
	if (m->m_pkthdr.len > 20 * 1024 - RTW88_NATIVE_TX_DESC_SIZE)
		return (EFBIG);

	/* The header may span mbuf segments; copy only the bounded header. */
	m_copydata(m, 0, sizeof(whbuf), (caddr_t)&whbuf);
	wh = &whbuf;
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	/*
	 * Match Linux rtw_chip_prepare_tx(): run firmware IQK after scan/channel
	 * selection and before the first authentication request. A calibration
	 * timeout is diagnostic; it must not suppress the auth frame itself.
	 */
	do_rfk = 0;
	if (type == IEEE80211_FC0_TYPE_MGT &&
	    (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) ==
	    IEEE80211_FC0_SUBTYPE_AUTH) {
		mtx_lock(&sc->mtx);
		if (sc->rfk_pending) {
			sc->rfk_pending = 0;
			do_rfk = 1;
		}
		mtx_unlock(&sc->mtx);
	}
	if (do_rfk) {
		rfk_error = rtw88_native_phy_iqk(sc);
		if (rfk_error == EBUSY) {
			/* Retry on the next auth attempt after channel I/O drains. */
			mtx_lock(&sc->mtx);
			sc->rfk_pending = 1;
			mtx_unlock(&sc->mtx);
		} else if (rfk_error == ENETDOWN) {
			return (ENETDOWN);
		}
	}

	/*
	 * RTW88's BMC descriptor bit and USB QSEL_HIGH selection follow the
	 * 802.11 receiver address (addr1), matching rtw_tx_pkt_info_update()
	 * and rtw_usb_tx_queue_mapping_to_qsel() in the upstream driver.
	 * For ToDS station traffic, addr1 is the AP/BSSID even when addr3
	 * contains a broadcast Ethernet destination.
	 */
	if (type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_CTL) {
		qsel = RTW88_NATIVE_TXQSEL_MGMT;
	} else if (type == IEEE80211_FC0_TYPE_DATA) {
		/*
		 * Group receiver-addressed data uses QSEL_HIGH; unicast receiver
		 * data uses the TID corresponding to net80211's WME access category.
		 * FreeBSD stores AC in M_WME_GETAC(), while the descriptor needs TID.
		 */
		if (IEEE80211_IS_MULTICAST(wh->i_addr1)) {
			qsel = RTW88_NATIVE_TXQSEL_HIGH;
		} else if ((m->m_flags & M_EAPOL) != 0) {
			/*
			 * FreeBSD tags PAE frames M_EAPOL and promotes their WME
			 * category to VO, but transmits them as non-QoS data.
			 * Match the FreeBSD LinuxKPI RTW88 path: put EAPOL on
			 * BE/TID0, rather than mapping the promoted VO category to QSEL_BEACON.
			 */
			qsel = RTW88_NATIVE_TXQSEL_TID0;
		} else {
			ac = M_WME_GETAC(m);
			if (ac < WME_AC_BE || ac > WME_AC_VO)
				return (EINVAL);
			/* Linux maps BE/BK to their TIDs and VI/VO to BEACON. */
			qsel = ac <= WME_AC_BK ? WME_AC_TO_TID(ac) :
			    RTW88_NATIVE_TXQSEL_BEACON;
		}
	} else {
		return (EOPNOTSUPP);
	}

	endpoint = sc->qsel_to_ep[qsel];
	if (endpoint >= RTW88_NATIVE_TX_QUEUES)
		return (EOPNOTSUPP);
	frame_len = (uint16_t)m->m_pkthdr.len;
	transfer_len = frame_len + RTW88_NATIVE_TX_DESC_SIZE;

	/* Snapshot frame classification before the sleepable USB transmit. */
	mtx_lock(&sc->mtx);
	channel = sc->current_channel;
	sc->tx_last_qsel = qsel;
	sc->tx_last_endpoint = endpoint;
	sc->tx_last_frame_control = (int)wh->i_fc[0] |
	    ((int)wh->i_fc[1] << 8);
	sc->tx_last_frame_len = frame_len;
	if (type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_CTL) {
		sc->tx_mgmt_count++;
		if (type == IEEE80211_FC0_TYPE_MGT) {
			switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
			case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
				sc->tx_probe_req_count++;
				break;
			case IEEE80211_FC0_SUBTYPE_AUTH:
				sc->tx_auth_req_count++;
				break;
			case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
			case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
				sc->tx_assoc_req_count++;
				break;
			case IEEE80211_FC0_SUBTYPE_DEAUTH:
				sc->tx_deauth_req_count++;
				break;
			default:
				break;
			}
		}
	} else {
		sc->tx_data_count++;
		if (qsel == RTW88_NATIVE_TXQSEL_HIGH)
			sc->tx_group_count++;
	}
	if ((m->m_flags & M_EAPOL) != 0)
		sc->tx_eapol_count++;
	mtx_unlock(&sc->mtx);

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
	 * CCK is not permitted in 5 GHz. Use the mandatory 6-Mbps OFDM rate
	 * there; retain the robust 1-Mbps CCK profile on 2.4 GHz.
	 */
	if (channel >= 36) {
		params.rate_id = 7;	/* RTW_RATEID_G / 11a OFDM */
		params.data_rate = 4;	/* DESC_RATE6M */
	} else if (type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_CTL ||
	    (m->m_flags & M_EAPOL) != 0) {
		params.rate_id = 8;	/* RTW_RATEID_B_20M */
		params.data_rate = 0;	/* DESC_RATE1M */
	} else {
		/*
		 * Match Linux RTW88's data path: use the band/peer rate profile
		 * and leave the explicit-rate descriptor bit clear. The firmware
		 * rate-adaptation table, rather than a driver-forced 1-Mbps rate,
		 * selects the ordinary data rate.
		 */
		params.rate_id = 6;	/* RTW_RATEID_BG */
		params.data_rate = 4;	/* DESC_RATE6M default; ignored unless USE_RATE */
	}
	/*
	 * Linux RTW88 fixes management and PAE rates, but leaves normal data
	 * frames to firmware rate adaptation unless a debug fixed-rate override
	 * is active. No fixed-rate override is exposed by this driver.
	 */
	params.use_hardware_rate = (type != IEEE80211_FC0_TYPE_DATA ||
	    (m->m_flags & M_EAPOL) != 0);
	params.disable_rate_fallback = params.use_hardware_rate;
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
		mtx_lock(&sc->mtx);
		sc->tx_usb_submit_count++;
		mtx_unlock(&sc->mtx);
		uerr = rtw88_native_usb_tx_sync(sc, endpoint, buffer,
		    transfer_len);
		mtx_lock(&sc->mtx);
		if (uerr == USB_ERR_NORMAL_COMPLETION)
			sc->tx_usb_complete_count++;
		else
			sc->tx_usb_error_count++;
		mtx_unlock(&sc->mtx);
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
		error = enabled ? rtw88_native_tx_one(sc, ni, m) : ENETDOWN;

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
 * use the standard software scan cache. 2.4-GHz and 20-MHz 5-GHz channels are exposed; 5-GHz remains passive-only.
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
	setbit(ic->ic_modecaps, IEEE80211_MODE_11A);
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
	    "net80211 radio attached as %s (2.4/5 GHz 20-MHz; 5-GHz passive scan)\n",
	    ic->ic_name);
	return (0);
}

void
rtw88_native_net80211_detach(struct rtw88_native_softc *sc)
{
	if (sc == NULL || !sc->net80211_attached)
		return;

	/*
	 * Stop new RX work from entering net80211, then drain any task that
	 * already observed net80211_attached before detaching VAPs and nodes.
	 * This is the FreeBSD taskqueue equivalent of synchronizing Linux's
	 * queued RX work before mac80211 teardown.
	 */
	mtx_lock(&sc->mtx);
	sc->net80211_attached = 0;
	mtx_unlock(&sc->mtx);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);

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
	struct ieee80211vap *vap;
	struct ifnet *ifp;
	struct mbuf *m;
	uint64_t ipackets_before, ipackets_after, ipackets_delta;
	int attached, running, input_result;

	sc = arg;
	for (;;) {
		mtx_lock(&sc->mtx);
		m = mbufq_dequeue(&sc->rx_queue);
		/*
		 * Track the same hardware running lifecycle as Linux RTW_FLAG_RUNNING.
		 * This FreeBSD taskqueue also gates queued RX handoff so stale frames
		 * cannot enter net80211 while startup is incomplete or stop is active.
		 * Firmware C2H records are parsed separately before this task.
		 */
		running = sc->hw_started && !sc->hw_stopping &&
		    !sc->detaching && !sc->dying;
		attached = sc->net80211_attached && running;
		mtx_unlock(&sc->mtx);
		if (m == NULL)
			break;

		if (!attached) {
			m_freem(m);
			mtx_lock(&sc->mtx);
			sc->rx_input_drop_count++;
			if (!running)
				sc->rx_not_running_drop_count++;
			mtx_unlock(&sc->mtx);
			continue;
		}

		/*
		 * net80211's input helper consumes the mbuf and returns a status,
		 * but that status is not an Ethernet-delivery counter. Snapshot
		 * the VAP's IPACKETS counter around input as well: FreeBSD's
		 * ieee80211_deliver_data() increments it only when an 802.11 data
		 * frame has been decapsulated and is about to enter if_input().
		 *
		 * This driver supports one VAP, matching rtw88's single station
		 * interface in the current native port.
		 */
		vap = TAILQ_FIRST(&sc->ic.ic_vaps);
		ifp = (vap != NULL) ? vap->iv_ifp : NULL;
		ipackets_before = (ifp != NULL) ?
		    if_get_counter_default(ifp, IFCOUNTER_IPACKETS) : 0;

		input_result = ieee80211_input_mimo_all(&sc->ic, m);

		ipackets_after = (ifp != NULL) ?
		    if_get_counter_default(ifp, IFCOUNTER_IPACKETS) : 0;
		ipackets_delta = (ipackets_after >= ipackets_before) ?
		    ipackets_after - ipackets_before : 0;

		mtx_lock(&sc->mtx);
		sc->rx_input_count++;
		sc->rx_net80211_last_result = input_result;
		if (input_result < 0) {
			sc->rx_net80211_negative_result_count++;
		} else {
			sc->rx_net80211_nonnegative_result_count++;
			switch (input_result) {
			case IEEE80211_FC0_TYPE_MGT:
				sc->rx_net80211_mgmt_result_count++;
				break;
			case IEEE80211_FC0_TYPE_CTL:
				sc->rx_net80211_ctl_result_count++;
				break;
			case IEEE80211_FC0_TYPE_DATA:
				sc->rx_net80211_data_result_count++;
				break;
			default:
				sc->rx_net80211_other_result_count++;
				break;
			}
		}
		sc->rx_vap_ipackets_last_delta = ipackets_delta;
		sc->rx_vap_ipackets_count += ipackets_delta;
		mtx_unlock(&sc->mtx);
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
	sc->hw_stopping = (value == 0);
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
	sc->hw_stopping = 0;
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
	if ((value < 1 || value > 13) &&
	    (value < 36 || value > 161))
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
		    "radio channel %d configuration failed: %d\n", value, error);
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
		uint8_t width;
		uint32_t value;
		usb_error_t error;
	} regs[] = {
		{ "CR", 0x0100, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "TXDMA_PQ_MAP", 0x010c, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXDMA_AGG_PG_TH", 0x0280, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXDMA_MODE", 0x0290, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "MACID0_LO", 0x0610, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "MACID0_HI", 0x0614, 2, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RCR", 0x0608, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXFLTMAP0", 0x06a0, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXFLTMAP1", 0x06a2, 2, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXFLTMAP2", 0x06a4, 2, 0, USB_ERR_NORMAL_COMPLETION },
		{ "SYS_FUNC_EN", 0x0002, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_CTRL", 0x001f, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "WLRF1", 0x00ec, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_SIPI_A", 0x0c90, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_SIPI_B", 0x0e90, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXPSEL", 0x0808, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXIGI_A", 0x0c50, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RXIGI_B", 0x0e50, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_A_CH18", 0x2860, 4, 0, USB_ERR_NORMAL_COMPLETION },
		{ "RF_B_CH18", 0x2c60, 4, 0, USB_ERR_NORMAL_COMPLETION }
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

	for (i = 0; i < nitems(regs); i++) {
		if (regs[i].width == 2) {
			uint16_t value16;

			value16 = 0;
			regs[i].error = rtw88_native_reg_read16(sc, regs[i].reg,
			    &value16);
			regs[i].value = value16;
		} else {
			regs[i].error = rtw88_native_reg_read32(sc, regs[i].reg,
			    &regs[i].value);
		}
	}

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
	    "Tune RFE option 3 to a supported 2.4/5-GHz channel center at 20 MHz") == NULL) {
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
	    "Legacy count of all non-C2H RX records, including management and control frames");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_bad_rate_count", CTLFLAG_RD, &sc->rx_bad_rate_count, 0,
	    "RX descriptors rejected for an out-of-range rate code");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_bad_drvinfo_count", CTLFLAG_RD, &sc->rx_bad_drvinfo_count, 0,
	    "RX descriptors rejected for an invalid PHY-info length");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_phy_page0_count", CTLFLAG_RD, &sc->rx_phy_page0_count, 0,
	    "RTL8822B CCK PHY-status page records decoded");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_phy_page1_count", CTLFLAG_RD, &sc->rx_phy_page1_count, 0,
	    "RTL8822B OFDM/HT/VHT PHY-status page records decoded");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_phy_unknown_count", CTLFLAG_RD, &sc->rx_phy_unknown_count, 0,
	    "RX PHY-status records with an unsupported page identifier");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_no_phy_count", CTLFLAG_RD, &sc->rx_no_phy_count, 0,
	    "RX frames received without PHY-status metadata");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ht_count", CTLFLAG_RD, &sc->rx_ht_count, 0,
	    "RX frames using HT MCS rate descriptors");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_vht_count", CTLFLAG_RD, &sc->rx_vht_count, 0,
	    "RX frames using VHT MCS/NSS rate descriptors");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_mgmt_frame_count", CTLFLAG_RD, &sc->rx_mgmt_frame_count, 0,
	    "Parsed 802.11 management frames");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ctl_frame_count", CTLFLAG_RD, &sc->rx_ctl_frame_count, 0,
	    "Parsed 802.11 control frames");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_data_frame_count", CTLFLAG_RD, &sc->rx_data_frame_count, 0,
	    "Parsed 802.11 data frames");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ext_frame_count", CTLFLAG_RD, &sc->rx_ext_frame_count, 0,
	    "Parsed 802.11 extension or unclassified frames");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_protected_frame_count", CTLFLAG_RD, &sc->rx_protected_frame_count, 0,
	    "Parsed 802.11 frames with the Protected Frame bit set");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_descriptor_swdec_count", CTLFLAG_RD,
	    &sc->rx_descriptor_swdec_count, 0,
	    "RX descriptors indicating software decryption was required");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_descriptor_encrypted_count", CTLFLAG_RD,
	    &sc->rx_descriptor_encrypted_count, 0,
	    "RX descriptors reporting a non-none encryption type");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_mgmt_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_mgmt_result_count, 0,
	    "net80211 input results identifying management frames (type 0)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_ctl_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_ctl_result_count, 0,
	    "net80211 input results identifying control frames (type 4)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_data_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_data_result_count, 0,
	    "net80211 input results identifying data frames (type 8)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_other_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_other_result_count, 0,
	    "net80211 input results with other nonnegative frame-type values");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_decrypted_count", CTLFLAG_RD, &sc->rx_decrypted_count, 0,
	    "RX frames marked as hardware-decrypted for net80211");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ra_unicast_count", CTLFLAG_RD, &sc->rx_ra_unicast_count, 0,
	    "RX frames with a unicast 802.11 receiver address (addr1)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ra_multicast_count", CTLFLAG_RD, &sc->rx_ra_multicast_count, 0,
	    "RX frames with a multicast non-broadcast 802.11 receiver address");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_ra_broadcast_count", CTLFLAG_RD, &sc->rx_ra_broadcast_count, 0,
	    "RX frames with the broadcast 802.11 receiver address");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_to_self_count", CTLFLAG_RD, &sc->rx_to_self_count, 0,
	    "RX unicast frames addressed to this adapter MAC");
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
	    "RX MPDUs submitted to ieee80211_input_mimo_all (not an IP delivery count)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_input_drop_count", CTLFLAG_RD, &sc->rx_input_drop_count, 0,
	    "RX MPDUs discarded before net80211 input");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_not_running_drop_count", CTLFLAG_RD, &sc->rx_not_running_drop_count, 0,
	    "RX MPDUs dropped while the native radio is not in its running state");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_negative_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_negative_result_count, 0,
	    "RX input calls returning -1 (missing RX metadata); not a filter-drop count");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_nonnegative_result_count", CTLFLAG_RD,
	    &sc->rx_net80211_nonnegative_result_count, 0,
	    "RX input calls returning an 802.11 frame-type value; not an acceptance count");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_net80211_last_result", CTLFLAG_RD,
	    &sc->rx_net80211_last_result, 0,
	    "Last net80211 frame type: 0=management, 4=control, 8=data, -1=missing RX metadata");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_vap_ipackets_count", CTLFLAG_RD,
	    &sc->rx_vap_ipackets_count, 0,
	    "Cumulative VAP IPACKETS increments observed during net80211 RX input");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_vap_ipackets_last_delta", CTLFLAG_RD,
	    &sc->rx_vap_ipackets_last_delta, 0,
	    "VAP IPACKETS increment observed while processing the most recent RX MPDU");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_auth_count", CTLFLAG_RD, &sc->rx_auth_count, 0,
	    "Authentication management frames received from the air");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_auth_to_self_count", CTLFLAG_RD, &sc->rx_auth_to_self_count, 0,
	    "Authentication frames addressed to this station MAC");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_assoc_resp_count", CTLFLAG_RD, &sc->rx_assoc_resp_count, 0,
	    "Association/reassociation responses received from the air");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_assoc_resp_to_self_count", CTLFLAG_RD,
	    &sc->rx_assoc_resp_to_self_count, 0,
	    "Association responses addressed to this station");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_assoc_status", CTLFLAG_RD, &sc->rx_last_assoc_status, 0,
	    "Status code in the most recent association response to this station");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_deauth_count", CTLFLAG_RD, &sc->rx_deauth_count, 0,
	    "Deauthentication management frames received from the air");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_disassoc_count", CTLFLAG_RD, &sc->rx_disassoc_count, 0,
	    "Disassociation management frames received from the air");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_disassoc_to_self_count", CTLFLAG_RD,
	    &sc->rx_disassoc_to_self_count, 0,
	    "Disassociation frames addressed to this station");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_disassoc_from_bss_count", CTLFLAG_RD,
	    &sc->rx_disassoc_from_bss_count, 0,
	    "Disassociation frames sourced by the programmed BSSID");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_last_disassoc_reason", CTLFLAG_RD,
	    &sc->rx_last_disassoc_reason, 0,
	    "Reason code in the most recent received disassociation frame");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_mailbox_count", CTLFLAG_RD,
	    &sc->fw_h2c_mailbox_count, 0,
	    "Successful H2C register-mailbox commands");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_mailbox_error_count", CTLFLAG_RD,
	    &sc->fw_h2c_mailbox_error_count, 0,
	    "Failed H2C register-mailbox commands");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_mailbox_error", CTLFLAG_RD,
	    &sc->fw_h2c_mailbox_error, 0,
	    "Last H2C register-mailbox error (0 means success)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_general_count", CTLFLAG_RD, &sc->fw_h2c_general_count, 0,
	    "Successfully transferred firmware General Info H2C packets");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_phydm_count", CTLFLAG_RD, &sc->fw_h2c_phydm_count, 0,
	    "Successfully transferred firmware PHYDM Info H2C packets");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_iqk_count", CTLFLAG_RD, &sc->fw_h2c_iqk_count, 0,
	    "Successfully transferred firmware IQK H2C commands");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_error_count", CTLFLAG_RD, &sc->fw_h2c_error_count, 0,
	    "Failed firmware H2C USB transfers");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "fw_h2c_error", CTLFLAG_RD, &sc->fw_h2c_error, 0,
	    "Last firmware H2C transfer error code");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "wifi_only_coex_initialized", CTLFLAG_RD,
	    &sc->wifi_only_coex_initialized, 0,
	    "Whether the RTL8822B Wi-Fi-only WLAN antenna/grant route is active");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_pending", CTLFLAG_RD, &sc->rfk_pending, 0,
	    "Whether firmware IQK is queued before the next authentication TX");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_done", CTLFLAG_RD, &sc->rfk_done, 0,
	    "Whether the firmware IQK completion marker was observed");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_attempt_count", CTLFLAG_RD, &sc->rfk_attempt_count, 0,
	    "Number of firmware IQK calibration attempts");
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_poll_count", CTLFLAG_RD, &sc->rfk_poll_count, 0,
	    "Number of RF_DTXLOK polls in the most recent IQK attempt");
	SYSCTL_ADD_U32(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_fail_mask", CTLFLAG_RD, &sc->rfk_fail_mask, 0,
	    "RTL8822B IQK per-path failure mask");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_error", CTLFLAG_RD, &sc->rfk_error, 0,
	    "Last firmware IQK error (0 means completion marker observed)");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rfk_reload", CTLFLAG_RD, &sc->rfk_reload, 0,
	    "Whether RTL8822B IQK requested reload of saved calibration");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_probe_resp_count", CTLFLAG_RD, &sc->rx_probe_resp_count, 0,
	    "Probe responses received from access points");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_beacon_count", CTLFLAG_RD, &sc->rx_beacon_count, 0,
	    "Beacon frames received from access points");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "rx_eapol_count", CTLFLAG_RD, &sc->rx_eapol_count, 0,
	    "EAPOL LLC/SNAP data frames received from the air");
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
	    "tx_usb_submit_count", CTLFLAG_RD, &sc->tx_usb_submit_count, 0,
	    "Bulk-OUT submissions attempted after descriptor construction");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_usb_complete_count", CTLFLAG_RD, &sc->tx_usb_complete_count, 0,
	    "Bulk-OUT transfers completed by USB (not an over-the-air ACK)");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_usb_error_count", CTLFLAG_RD, &sc->tx_usb_error_count, 0,
	    "Bulk-OUT transfers that returned a USB error");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_error_count", CTLFLAG_RD, &sc->tx_error_count, 0,
	    "Number of frames that failed before or during USB transmission");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_mgmt_count", CTLFLAG_RD, &sc->tx_mgmt_count, 0,
	    "Management/control frames passed to the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_data_count", CTLFLAG_RD, &sc->tx_data_count, 0,
	    "Data frames passed to the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_group_count", CTLFLAG_RD, &sc->tx_group_count, 0,
	    "Group-addressed data frames routed to QSEL_HIGH");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_eapol_count", CTLFLAG_RD, &sc->tx_eapol_count, 0,
	    "EAPOL frames passed to the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_probe_req_count", CTLFLAG_RD, &sc->tx_probe_req_count, 0,
	    "Probe-request management frames submitted by the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_auth_req_count", CTLFLAG_RD, &sc->tx_auth_req_count, 0,
	    "Authentication-request management frames submitted by the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_assoc_req_count", CTLFLAG_RD, &sc->tx_assoc_req_count, 0,
	    "Association/reassociation request frames submitted by the TX worker");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_deauth_req_count", CTLFLAG_RD, &sc->tx_deauth_req_count, 0,
	    "Deauthentication request frames submitted by the TX worker");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_last_qsel", CTLFLAG_RD, &sc->tx_last_qsel, 0,
	    "Queue-select value for the most recent TX frame");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_last_endpoint", CTLFLAG_RD, &sc->tx_last_endpoint, 0,
	    "Bulk-OUT transfer index for the most recent TX frame");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_last_frame_control", CTLFLAG_RD, &sc->tx_last_frame_control, 0,
	    "Frame-control field for the most recent TX frame");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "tx_last_frame_len", CTLFLAG_RD, &sc->tx_last_frame_len, 0,
	    "MPDU length for the most recent TX frame");
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
