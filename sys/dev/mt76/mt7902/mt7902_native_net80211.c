/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native net80211 integration for MT7902. The state machine and scan cache
 * are FreeBSD net80211; hardware channel changes use Connac2 MCU commands.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>

#include "if_mt7902_native.h"

#define MT7902_TXWI_LEN		32
#define MT7902_TXP_LEN		32
#define MT7902_TXD_LEN		(MT7902_TXWI_LEN + MT7902_TXP_LEN)
#define MT7902_TXD_QIDX_SHIFT	25
#define MT7902_TXD_PKT_FMT_SHIFT	23
#define MT7902_TXD_LONG		(1U << 31)
#define MT7902_TXD_HDR_80211	(2U << 16)
#define MT7902_TXD_WLAN_IDX_SHIFT	0
#define MT7902_TXD_FIX_RATE	(1U << 31)
#define MT7902_TXD_RETRY	(15U << 11)
#define MT7902_TXP_LAST		(1U << 15)
#define MT7902_STA_REC_BASIC	0
#define MT7902_STA_REC_STATE	7
#define MT7902_STA_CONN_CONNECT	1
#define MT7902_STA_CONN_DISCONNECT	0
#define MT7902_STA_STATE_ASSOC	2
#define MT7902_STA_STATE_NONE	0
#define MT7902_BSS_IDX		0
#define MT7902_BSS_WCID		19
#define MT7902_PEER_WCID	1
#define MT7902_CONN_INFRA_STA	((1U << 0) | (1U << 16))
#define MT7902_CONN_INFRA_AP	((1U << 1) | (1U << 16))

struct mt7902_txp_ptr {
	uint32_t buf0;
	uint16_t len0;
	uint16_t len1;
	uint32_t buf1;
} __packed;

struct mt7902_txp {
	uint16_t msdu_id[4];
	struct mt7902_txp_ptr ptr[2];
} __packed;

CTASSERT(sizeof(struct mt7902_txp_ptr) == 12);
CTASSERT(sizeof(struct mt7902_txp) == MT7902_TXP_LEN);
CTASSERT(MT7902_TXD_LEN == 64);

struct mt7902_sta_req_hdr {
	uint8_t bss_idx;
	uint8_t wlan_idx_lo;
	uint16_t tlv_num;
	uint8_t is_tlv_append;
	uint8_t muar_idx;
	uint8_t wlan_idx_hi;
	uint8_t reserved;
} __packed;

struct mt7902_sta_rec_basic {
	uint16_t tag;
	uint16_t len;
	uint32_t conn_type;
	uint8_t conn_state;
	uint8_t qos;
	uint16_t aid;
	uint8_t peer_addr[IEEE80211_ADDR_LEN];
	uint16_t extra_info;
} __packed;

struct mt7902_sta_rec_state {
	uint16_t tag;
	uint16_t len;
	uint32_t flags;
	uint8_t state;
	uint8_t vht_opmode;
	uint8_t action;
	uint8_t reserved;
} __packed;

struct mt7902_bss_basic {
	uint16_t tag;
	uint16_t len;
	uint8_t active;
	uint8_t omac_idx;
	uint8_t hw_bss_idx;
	uint8_t band_idx;
	uint32_t conn_type;
	uint8_t conn_state;
	uint8_t wmm_idx;
	uint8_t bssid[IEEE80211_ADDR_LEN];
	uint16_t bmc_tx_wlan_idx;
	uint16_t bcn_interval;
	uint8_t dtim_period;
	uint8_t phymode;
	uint16_t sta_idx;
	uint16_t nonht_basic_phy;
	uint8_t phymode_ext;
	uint8_t link_idx;
} __packed;

struct mt7902_bss_update {
	uint8_t bss_idx;
	uint8_t reserved[3];
	struct mt7902_bss_basic basic;
} __packed;

struct mt7902_vap {
	struct ieee80211vap vap;
	int (*newstate)(struct ieee80211vap *, enum ieee80211_state, int);
	bool linked;
};

static const uint8_t mt7902_5ghz_channels[] = {
	36, 40, 44, 48, 52, 56, 60, 64,
	100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
	149, 153, 157, 161, 165
};

static void
mt7902_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
	struct ieee80211_channel chans[])
{
	uint8_t bands2[IEEE80211_MODE_BYTES], bands5[IEEE80211_MODE_BYTES];
	int count, i;

	(void)ic;
	bzero(bands2, sizeof(bands2));
	bzero(bands5, sizeof(bands5));
	setbit(bands2, IEEE80211_MODE_11B);
	setbit(bands2, IEEE80211_MODE_11G);
	setbit(bands5, IEEE80211_MODE_11A);
	count = 0;
	(void)ieee80211_add_channels_default_2ghz(chans, maxchans, &count,
	    bands2, 0);
	(void)ieee80211_add_channel_list_5ghz(chans, maxchans, &count,
	    mt7902_5ghz_channels, nitems(mt7902_5ghz_channels), bands5, 0);
	for (i = 0; i < count; i++) {
		if (IEEE80211_IS_CHAN_5GHZ(&chans[i])) {
			chans[i].ic_flags |= IEEE80211_CHAN_PASSIVE;
			if (chans[i].ic_ieee >= 52 && chans[i].ic_ieee <= 144)
				chans[i].ic_flags |= IEEE80211_CHAN_DFS;
		}
	}
	*nchans = count;
}

int
mt7902_set_channel(struct mt7902_softc *sc, struct ieee80211_channel *c)
{
	struct {
		uint8_t control_ch;
		uint8_t center_ch;
		uint8_t bw;
		uint8_t tx_streams;
		uint8_t rx_streams;
		uint8_t switch_reason;
		uint8_t band_idx;
		uint8_t center_ch2;
		uint16_t cac_case;
		uint8_t channel_band;
		uint8_t reserved0;
		uint32_t outband_freq;
		uint8_t txpower_drop;
		uint8_t ap_bw;
		uint8_t ap_center_ch;
		uint8_t reserved1[57];
	} __packed req;

	if (sc == NULL || c == NULL || !sc->hw_started)
		return (ENETDOWN);
	bzero(&req, sizeof(req));
	req.control_ch = c->ic_ieee;
	req.center_ch = c->ic_ieee;
	req.tx_streams = 1;
	req.rx_streams = 1;
	mtx_lock(&sc->mtx);
	req.switch_reason = sc->scan_active ? 9 : 0;
	mtx_unlock(&sc->mtx);
	req.channel_band = IEEE80211_IS_CHAN_2GHZ(c) ? 0 : 1;
	/* Connac2 MCU_EXT_CMD(CHANNEL_SWITCH), followed by the ordinary
	 * management/data MAC queue setup. Keep this firmware command
	 * asynchronous because net80211 calls ic_set_channel under its lock.
	 */
	return (mt7902_mcu_send_cmd(sc, MT7902_MCU_EXT_CMD(0x08), &req,
	    sizeof(req), false));
}

static void
mt7902_set_channel_cb(struct ieee80211com *ic)
{
	struct mt7902_softc *sc;
	int error;

	sc = ic->ic_softc;
	if (ic->ic_curchan == NULL)
		return;
	error = mt7902_set_channel(sc, ic->ic_curchan);
	if (error == 0) {
		sc->current_channel = ic->ic_curchan->ic_ieee;
		sc->current_band = IEEE80211_IS_CHAN_2GHZ(ic->ic_curchan) ? 2 : 5;
	} else {
		device_printf(sc->dev, "channel %u command failed: %d\n",
		    ic->ic_curchan->ic_ieee, error);
	}
}

static void
mt7902_scan_start(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	bool active;

	active = ic->ic_scan != NULL &&
	    (ic->ic_scan->ss_flags & IEEE80211_SCAN_ACTIVE) != 0;
	mtx_lock(&sc->mtx);
	sc->scan_active = true;
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev, "net80211 %s scan started\n",
	    active ? "active" : "passive");
}

static void
mt7902_scan_end(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	uint64_t rx, dropped, mgmt, beacon;
	uint32_t last_len, rxd0, rxd1, rxd2, rxd3;

	mtx_lock(&sc->mtx);
	sc->scan_active = false;
	rx = sc->rx_data_count;
	dropped = sc->rx_drop_count;
	mgmt = sc->rx_mgmt_count;
	beacon = sc->rx_beacon_count;
	last_len = sc->rx_last_len;
	rxd0 = sc->rx_last_rxd[0];
	rxd1 = sc->rx_last_rxd[1];
	rxd2 = sc->rx_last_rxd[2];
	rxd3 = sc->rx_last_rxd[3];
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev,
	    "net80211 scan ended: rx=%ju dropped=%ju mgmt=%ju beacon/probe=%ju last=%u RXD=%08x/%08x/%08x/%08x\n",
	    (uintmax_t)rx, (uintmax_t)dropped, (uintmax_t)mgmt,
	    (uintmax_t)beacon, last_len, rxd0, rxd1, rxd2, rxd3);
}

static int
mt7902_link_update(struct ieee80211vap *vap, struct ieee80211_node *ni,
	bool linked)
{
	struct mt7902_softc *sc = vap->iv_ic->ic_softc;
	struct mt7902_vap *mvp = (struct mt7902_vap *)vap;
	struct mt7902_bss_update bss;
	struct mt7902_sta_req_hdr hdr;
	struct mt7902_sta_rec_basic basic;
	struct mt7902_sta_rec_state state;
	uint8_t peer[IEEE80211_ADDR_LEN];
	uint16_t aid;
	int error;

	if (linked) {
		if (ni == NULL)
			return (EINVAL);
		IEEE80211_ADDR_COPY(peer, ni->ni_bssid);
		aid = IEEE80211_NODE_AID(ni);
	} else {
		IEEE80211_ADDR_COPY(peer, vap->iv_myaddr);
		aid = 0;
	}
	bzero(&bss, sizeof(bss));
	bss.bss_idx = MT7902_BSS_IDX;
	bss.basic.tag = htole16(0); /* UNI_BSS_INFO_BASIC */
	bss.basic.len = htole16(sizeof(bss.basic));
	bss.basic.active = linked;
	bss.basic.omac_idx = 0;
	bss.basic.hw_bss_idx = 0;
	bss.basic.band_idx = 0;
	bss.basic.conn_type = htole32(MT7902_CONN_INFRA_STA);
	bss.basic.conn_state = linked ? 0 : 1;
	bss.basic.wmm_idx = 0;
	IEEE80211_ADDR_COPY(bss.basic.bssid, peer);
	bss.basic.bmc_tx_wlan_idx = htole16(MT7902_BSS_WCID);
	bss.basic.bcn_interval = htole16(ni != NULL ? ni->ni_intval : 100);
	bss.basic.dtim_period = ni != NULL ? ni->ni_dtim_period : 1;
	bss.basic.phymode = 0x07; /* 2.4-GHz 11b/g plus 5-GHz 11a legacy */
	bss.basic.sta_idx = htole16(MT7902_BSS_WCID);
	error = mt7902_mcu_send_response(sc,
	    MT7902_MCU_UNI_CMD(0x02) /* UNI_BSS_INFO_UPDATE */,
	    &bss, sizeof(bss), NULL, &(size_t){0});
	if (error != 0)
		return (error);

	if (!linked) {
		uint8_t abort_req[4] = { MT7902_BSS_IDX, 0, 0, 0 };

		error = mt7902_mcu_send_cmd(sc,
		    MT7902_MCU_CE_CMD(0x17) /* SET_BSS_ABORT */,
		    abort_req, sizeof(abort_req), false);
		mvp->linked = false;
		sc->station_linked = 0;
		sc->station_aid = 0;
		return (error);
	}

	/* Tell firmware the association AID/DTIM parameters after BSS setup. */
	{
		struct {
			uint8_t bss_idx;
			uint8_t dtim_period;
			uint16_t aid;
			uint16_t beacon_interval;
			uint16_t atim_window;
			uint8_t uapsd;
			uint8_t bmc_delivered_ac;
			uint8_t bmc_triggered_ac;
			uint8_t pad;
		} __packed conn;

		bzero(&conn, sizeof(conn));
		conn.bss_idx = MT7902_BSS_IDX;
		conn.dtim_period = ni->ni_dtim_period;
		conn.aid = htole16(aid);
		conn.beacon_interval = htole16(ni->ni_intval);
		error = mt7902_mcu_send_cmd(sc, MT7902_MCU_CE_CMD(0x16),
		    &conn, sizeof(conn), false);
		if (error != 0)
			return (error);
	}

	bzero(&hdr, sizeof(hdr));
	hdr.bss_idx = MT7902_BSS_IDX;
	hdr.wlan_idx_lo = MT7902_PEER_WCID;
	hdr.tlv_num = htole16(2);
	hdr.is_tlv_append = 1;
	memset(&basic, 0, sizeof(basic));
	basic.tag = htole16(MT7902_STA_REC_BASIC);
	basic.len = htole16(sizeof(basic));
	basic.conn_type = htole32(MT7902_CONN_INFRA_AP);
	basic.conn_state = MT7902_STA_CONN_CONNECT;
	basic.qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
	basic.aid = htole16(aid);
	IEEE80211_ADDR_COPY(basic.peer_addr, peer);
	basic.extra_info = htole16(3); /* version + newly created */
	memset(&state, 0, sizeof(state));
	state.tag = htole16(MT7902_STA_REC_STATE);
	state.len = htole16(sizeof(state));
	state.state = MT7902_STA_STATE_ASSOC;
	{
		uint8_t request[sizeof(hdr) + sizeof(basic) + sizeof(state)];

		memcpy(request, &hdr, sizeof(hdr));
		memcpy(request + sizeof(hdr), &basic, sizeof(basic));
		memcpy(request + sizeof(hdr) + sizeof(basic), &state,
		    sizeof(state));
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
		    request, sizeof(request), NULL, &(size_t){0});
	}
	if (error == 0)
		mvp->linked = true;
	if (error == 0) {
		sc->station_linked = 1;
		sc->station_aid = aid;
	}
	return (error);
}

static int
mt7902_vap_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate,
	int arg)
{
	struct mt7902_vap *mvp = (struct mt7902_vap *)vap;
	struct ieee80211_node *ni = NULL;
	struct ieee80211com *ic = vap->iv_ic;
	struct mt7902_softc *sc = ic->ic_softc;
	bool linked;
	int error;

	if (mvp->newstate == NULL)
		return (EINVAL);
	linked = nstate == IEEE80211_S_RUN;
	if (linked != mvp->linked) {
		if (linked && vap->iv_bss != NULL)
			ni = ieee80211_ref_node(vap->iv_bss);
		IEEE80211_UNLOCK(ic);
		error = mt7902_link_update(vap, ni, linked);
		if (ni != NULL)
			ieee80211_free_node(ni);
		IEEE80211_LOCK(ic);
		if (error != 0 && linked) {
			device_printf(sc->dev,
			    "could not configure associated BSS in firmware: %d\n",
			    error);
			return (error);
		}
	}
	return (mvp->newstate(vap, nstate, arg));
}

static struct ieee80211vap *
mt7902_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ],
	int unit, enum ieee80211_opmode opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct mt7902_vap *mvp;
	struct ieee80211vap *vap;

	if ((opmode != IEEE80211_M_STA && opmode != IEEE80211_M_MONITOR) ||
	    !TAILQ_EMPTY(&ic->ic_vaps))
		return (NULL);
	mvp = malloc(sizeof(*mvp), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &mvp->vap;
	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
	    flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		free(mvp, M_80211_VAP);
		return (NULL);
	}
	mvp->newstate = vap->iv_newstate;
	vap->iv_newstate = mt7902_vap_newstate;
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	return (vap);
}

static void
mt7902_vap_delete(struct ieee80211vap *vap)
{
	struct mt7902_vap *mvp = (struct mt7902_vap *)vap;

	ieee80211_stop(vap);
	vap->iv_newstate = mvp->newstate;
	ieee80211_vap_detach(vap);
	free(mvp, M_80211_VAP);
}

static void
mt7902_parent(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;

	if (!sc->hw_started && ic->ic_nrunning != 0)
		device_printf(sc->dev,
		    "Wi-Fi firmware is stopped; set dev.mt7902_native.%d.hw_start=1\n",
		    device_get_unit(sc->dev));
}

static int
mt7902_tx_enqueue(struct mt7902_softc *sc, struct mbuf *m)
{
	int error;

	if (sc == NULL || m == NULL || (m->m_flags & M_PKTHDR) == 0)
		return (EINVAL);
	mtx_lock(&sc->mtx);
	if (sc->dying || !sc->hw_started || !sc->net80211_attached)
		error = ENETDOWN;
	else
		error = mbufq_enqueue(&sc->txq, m);
	mtx_unlock(&sc->mtx);
	if (error == 0)
		taskqueue_enqueue(taskqueue_thread, &sc->tx_task);
	return (error);
}

static int
mt7902_raw_params_supported(const struct ieee80211_bpf_params *params,
	struct mbuf *m)
{
	static const uint8_t rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	struct ieee80211_frame_min wh;
	unsigned int i;

	if (params == NULL)
		return (1);
	/* net80211's software scanner uses this ordinary legacy-rate profile. */
	if (params->ibp_vers != 0 || params->ibp_len != 0 ||
	    (params->ibp_flags & ~IEEE80211_BPF_NOACK) != 0 ||
	    params->ibp_pri > WME_AC_VO || params->ibp_try0 > 15 ||
	    params->ibp_ctsrate != 0 || params->ibp_try1 != 0 ||
	    params->ibp_rate1 != 0 || params->ibp_try2 != 0 ||
	    params->ibp_rate2 != 0 || params->ibp_try3 != 0 ||
	    params->ibp_rate3 != 0)
		return (0);
	if (params->ibp_rate0 != 0) {
		for (i = 0; i < nitems(rates); i++)
			if (params->ibp_rate0 == rates[i])
				break;
		if (i == nitems(rates))
			return (0);
	}
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
mt7902_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
	const struct ieee80211_bpf_params *params)
{
	struct mt7902_softc *sc;

	if (ni == NULL || m == NULL) {
		if (m != NULL)
			m_freem(m);
		return (EINVAL);
	}
	if (!mt7902_raw_params_supported(params, m)) {
		if (m->m_flags & M_TXCB)
			ieee80211_process_callback(ni, m, 1);
		m_freem(m);
		return (EOPNOTSUPP);
	}
	sc = ni->ni_ic->ic_softc;
	{
		int error = mt7902_tx_enqueue(sc, m);

		if (error != 0) {
			if (m->m_flags & M_TXCB)
				ieee80211_process_callback(ni, m, 1);
			m_freem(m);
		}
		return (error);
	}
}

static int
mt7902_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	return (mt7902_tx_enqueue(ic->ic_softc, m));
}

static int
mt7902_tx_one(struct mt7902_softc *sc, struct mbuf *m)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	struct mt7902_txp *txp;
	struct ieee80211_frame_min wh;
	uint32_t *txwi, fc, val, control;
	size_t len, total, hdrlen;
	int idx, retry, error;

	ring = &sc->tx_data;
	len = m->m_pkthdr.len;
	total = MT7902_TXD_LEN + len;
	if (len < sizeof(wh) || total > ring->buf[0].len ||
	    total > MT7902_DMA_LEN_MASK)
		return (EMSGSIZE);
	m_copydata(m, 0, sizeof(wh), (caddr_t)&wh);
	fc = le16toh(wh.i_fc[0] | ((uint16_t)wh.i_fc[1] << 8));
	hdrlen = sizeof(wh);
	if ((fc & 0x000c) == 0x0004)
		hdrlen = ((fc & 0x00f0) == 0x00c0 ||
		    (fc & 0x00f0) == 0x00d0) ? 10 : 16;
	else if ((fc & 0x000c) == 0x0008) {
		if ((fc & 0x0300) == 0x0300)
			hdrlen += 6;
		if ((fc & 0x0080) != 0)
			hdrlen += 2;
		if ((fc & 0x8000) != 0)
			hdrlen += 4;
	}
	mtx_lock(&sc->mtx);
	idx = ring->head;
	desc = &ring->desc[idx];
	buf = &ring->buf[idx];
	bus_dmamap_sync(ring->desc_tag, ring->desc_map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	control = le32toh(desc->control);
	if ((control & MT7902_DMA_DESC_DONE) == 0) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	memset(buf->vaddr, 0, MT7902_TXD_LEN);
	m_copydata(m, 0, len, (caddr_t)buf->vaddr + MT7902_TXD_LEN);
	txwi = buf->vaddr;
	val = (uint32_t)(len + MT7902_TXWI_LEN) |
	    (0U << MT7902_TXD_PKT_FMT_SHIFT) | (0U << MT7902_TXD_QIDX_SHIFT);
	txwi[0] = htole32(val);
	val = MT7902_TXD_LONG | ((uint32_t)(sc->station_linked ?
	    MT7902_PEER_WCID : MT7902_BSS_WCID) << MT7902_TXD_WLAN_IDX_SHIFT);
	txwi[1] = htole32(val);
	val = MT7902_TXD_HDR_80211 |
	    (((uint32_t)(hdrlen / 2) & 0x1f) << 11) |
	    (((fc & 0x0c) >> 2) << 4) | ((fc & 0xf0) >> 4);
	if (ETHER_IS_MULTICAST(wh.i_addr1))
		val |= 1U << 10;
	if ((fc & 0x000c) == 0 || ETHER_IS_MULTICAST(wh.i_addr1))
		val |= MT7902_TXD_FIX_RATE;
	txwi[2] = htole32(val);
	txwi[3] = htole32(MT7902_TXD_RETRY);
	txwi[4] = 0;
	txwi[5] = 0;
	val = 1U << 2;	/* fixed 20 MHz legacy rate */
	if (sc->current_band == 5)
		val |= 1U << 6;	/* OFDM 6 Mbps in 5 GHz */
	txwi[6] = htole32(val);
	txwi[7] = htole32((((fc & 0x0c) >> 2) << 20) |
	    (((fc & 0xf0) >> 4) << 16));
	if ((fc & 0x000c) == 0)
		txwi[2] |= htole32(1U << 13);
	txp = (struct mt7902_txp *)((uint8_t *)buf->vaddr + MT7902_TXWI_LEN);
	txp->msdu_id[0] = htole16((uint16_t)idx | (1U << 15));
	txp->ptr[0].buf0 = htole32((uint32_t)(buf->paddr + MT7902_TXD_LEN));
	txp->ptr[0].len0 = htole16((uint16_t)len | MT7902_TXP_LAST);
	bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_PREWRITE);
	desc->buf0 = htole32((uint32_t)buf->paddr);
	desc->buf1 = 0;
	desc->info = 0;
	desc->control = htole32(((uint32_t)total <<
	    MT7902_DMA_LEN_SHIFT) | MT7902_DMA_DESC_LAST);
	bus_dmamap_sync(ring->desc_tag, ring->desc_map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
	ring->head = (idx + 1) % ring->ndesc;
	mt7902_write(sc, ring->reg_base + 0x8, ring->head);
	mtx_unlock(&sc->mtx);

	for (retry = 0; retry < 10000; retry++) {
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		if (le32toh(desc->control) & MT7902_DMA_DESC_DONE)
			break;
		DELAY(10);
	}
	error = retry == 10000 ? ETIMEDOUT : 0;
	bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTWRITE);
	if (error == 0) {
		mtx_lock(&sc->mtx);
		desc->control = htole32(MT7902_DMA_DESC_DONE);
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
		ring->tail = (idx + 1) % ring->ndesc;
		mtx_unlock(&sc->mtx);
	}
	return (error);
}

int
mt7902_net80211_attach(struct mt7902_softc *sc)
{
	struct ieee80211com *ic;

	if (sc == NULL || sc->net80211_attached)
		return (sc == NULL ? EINVAL : 0);
	if (ETHER_IS_MULTICAST(sc->macaddr) || sc->macaddr[0] == 0)
		return (EINVAL);
	ic = &sc->ic;
	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(sc->dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->macaddr);
	ic->ic_caps = IEEE80211_C_STA | IEEE80211_C_MONITOR | IEEE80211_C_WPA;
	setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11A);
	mt7902_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
	    ic->ic_channels);
	if (ic->ic_nchans == 0)
		return (ENXIO);
	/*
	 * Ask Connac2 to pass frames from other BSSs to the host.  The
	 * software scanner relies on beacon and probe-response RX on the
	 * ordinary data ring; without OTHER_BSS the MCU can filter them before
	 * the native net80211 input path ever sees them.
	 */
	{
		int error;

		error = mt7902_mcu_set_rxfilter(sc,
		    MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS);
		if (error != 0)
			device_printf(sc->dev,
			    "could not widen MCU RX filter for scanning: %d\n", error);
	}
	ieee80211_ifattach(ic);
	ic->ic_getradiocaps = mt7902_getradiocaps;
	ic->ic_vap_create = mt7902_vap_create;
	ic->ic_vap_delete = mt7902_vap_delete;
	ic->ic_parent = mt7902_parent;
	ic->ic_set_channel = mt7902_set_channel_cb;
	ic->ic_scan_start = mt7902_scan_start;
	ic->ic_scan_end = mt7902_scan_end;
	ic->ic_raw_xmit = mt7902_raw_xmit;
	ic->ic_transmit = mt7902_transmit;
	ieee80211_set_software_ciphers(ic, IEEE80211_CRYPTO_AES_CCM);
	mbufq_init(&sc->txq, MT7902_TX_RING_SIZE);
	sc->net80211_attached = 1;
	device_printf(sc->dev,
	    "net80211 registered (%s, station/monitor, 2.4/5 GHz)\n",
	    ic->ic_name);
	return (0);
}

void
mt7902_net80211_detach(struct mt7902_softc *sc)
{
	if (sc == NULL || !sc->net80211_attached)
		return;
	ieee80211_ifdetach(&sc->ic);
	sc->net80211_attached = 0;
}

void
mt7902_tx_task(void *arg, int pending __unused)
{
	struct mt7902_softc *sc = arg;
	struct ieee80211_node *ni;
	struct mbuf *m;
	int error;

	for (;;) {
		mtx_lock(&sc->mtx);
		m = mbufq_dequeue(&sc->txq);
		mtx_unlock(&sc->mtx);
		if (m == NULL)
			break;
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;
		error = mt7902_tx_one(sc, m);
		if (ni != NULL)
			ieee80211_tx_complete(ni, m, error != 0);
		else
			m_freem(m);
	}
}

