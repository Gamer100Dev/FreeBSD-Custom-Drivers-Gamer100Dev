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

#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_proto.h>

#define MT7902_TXWI_LEN		32
#define MT7902_TXP_LEN		32
#define MT7902_TXD_LEN		(MT7902_TXWI_LEN + MT7902_TXP_LEN)
#define MT7902_TXD_QIDX_SHIFT	25
#define MT7902_TXD_PKT_FMT_SHIFT	23
#define MT7902_TXD_LONG		(1U << 31)
#define MT7902_TXD_HDR_80211	(2U << 16)
#define MT7902_TXD_TID_SHIFT	20
#define MT7902_TXD_HDR_INFO_SHIFT	11
#define MT7902_TXD_FRAG_SHIFT	14
#define MT7902_TXD_WLAN_IDX_SHIFT	0
#define MT7902_TXD_FIX_RATE	(1U << 31)
#define MT7902_TXD_RETRY	(15U << 11)
#define MT7902_TXD_BA_DISABLE	(1U << 28)
#define MT7902_TXD_NO_ACK	(1U << 0)
#define MT7902_TXD_HTC_VALID	(1U << 13)
#define MT7902_TXD6_FIXED_BW	(1U << 2)
#define MT7902_TXD6_TX_RATE_SHIFT	16
#define MT7902_TXP_LAST		(1U << 15)
#define MT7902_STA_REC_BASIC	0
#define MT7902_UNI_BSS_INFO_BASIC	0
#define MT7902_UNI_BSS_INFO_QBSS	15
#define MT7902_STA_REC_STATE	7
#define MT7902_STA_REC_RA		1
#define MT7902_STA_REC_PHY		0x15
#define MT7902_RA_LEGACY_CCK		0x000fU
#define MT7902_STA_CONN_PORT_SECURE	2
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

struct mt7902_mcu_tlv_hdr {
	uint16_t tag;
	uint16_t len;
} __packed;

struct mt7902_wtbl_req_hdr {
	uint8_t wlan_idx_lo;
	uint8_t operation;
	uint16_t tlv_num;
	uint8_t wlan_idx_hi;
	uint8_t reserved[3];
} __packed;

struct mt7902_wtbl_generic {
	uint16_t tag;
	uint16_t len;
	uint8_t peer_addr[IEEE80211_ADDR_LEN];
	uint8_t muar_idx;
	uint8_t skip_tx;
	uint8_t cf_ack;
	uint8_t qos;
	uint8_t mesh;
	uint8_t adm;
	uint16_t partial_aid;
	uint8_t baf_en;
	uint8_t aad_om;
} __packed;

struct mt7902_wtbl_rx {
	uint16_t tag;
	uint16_t len;
	uint8_t rcid;
	uint8_t rca1;
	uint8_t rca2;
	uint8_t rv;
	uint8_t reserved[4];
} __packed;

struct mt7902_wtbl_hdr_trans {
	uint16_t tag;
	uint16_t len;
	uint8_t to_ds;
	uint8_t from_ds;
	uint8_t no_rx_trans;
	uint8_t reserved;
} __packed;

struct mt7902_sta_wtbl {
	struct mt7902_mcu_tlv_hdr tlv;
	struct mt7902_wtbl_req_hdr request;
	struct mt7902_wtbl_generic generic;
	struct mt7902_wtbl_rx rx;
	struct mt7902_wtbl_hdr_trans hdr_trans;
} __packed;

struct mt7902_sta_rec_phy {
	uint16_t tag;
	uint16_t len;
	uint16_t basic_rate;
	uint8_t phy_type;
	uint8_t ampdu;
	uint8_t rts_policy;
	uint8_t rcpi;
	uint8_t max_ampdu_len;
	uint8_t reserved;
} __packed;

struct mt7902_sta_rec_ra {
	uint16_t tag;
	uint16_t len;
	uint16_t legacy;
	uint8_t rx_mcs_bitmask[10];
} __packed;

CTASSERT(sizeof(struct mt7902_sta_rec_phy) == 12);
CTASSERT(sizeof(struct mt7902_sta_rec_ra) == 16);

CTASSERT(sizeof(struct mt7902_mcu_tlv_hdr) == 4);
CTASSERT(sizeof(struct mt7902_wtbl_req_hdr) == 8);
CTASSERT(sizeof(struct mt7902_wtbl_generic) == 20);
CTASSERT(sizeof(struct mt7902_wtbl_rx) == 12);
CTASSERT(sizeof(struct mt7902_wtbl_hdr_trans) == 8);

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

struct mt7902_bss_qos {
	uint16_t tag;
	uint16_t len;
	uint8_t qos;
	uint8_t pad[3];
} __packed;

struct mt7902_bss_update {
	uint8_t bss_idx;
	uint8_t reserved[3];
	struct mt7902_bss_basic basic;
	struct mt7902_bss_qos qos;
} __packed;

struct mt7902_vap {
	struct ieee80211vap vap;
	int (*newstate)(struct ieee80211vap *, enum ieee80211_state, int);
	uint8_t peer_bssid[IEEE80211_ADDR_LEN];
	uint16_t peer_aid;
	uint16_t peer_beacon_interval;
	uint8_t peer_dtim_period;
	bool peer_qos;
	bool linked;
};

/*
 * Remove the STA peer and reset its WTBL entry, matching the disable path
 * in mt76_connac_mcu_sta_cmd().  With enable=false mt76 emits STA_REC_BASIC
 * followed by a WTBL RESET_AND_SET container with no nested WTBL records.
 */
static int
mt7902_sta_disconnect(struct mt7902_softc *sc,
    const uint8_t peer[IEEE80211_ADDR_LEN], uint16_t aid, bool qos)
{
	struct mt7902_sta_req_hdr hdr;
	struct mt7902_sta_rec_basic basic;
	struct mt7902_mcu_tlv_hdr wtbl_tlv;
	struct mt7902_wtbl_req_hdr wtbl_req;
	uint8_t request[sizeof(hdr) + sizeof(basic) +
	    sizeof(wtbl_tlv) + sizeof(wtbl_req)];
	size_t offset;

	bzero(&hdr, sizeof(hdr));
	hdr.bss_idx = MT7902_BSS_IDX;
	hdr.wlan_idx_lo = MT7902_PEER_WCID;
	hdr.tlv_num = htole16(2);
	hdr.is_tlv_append = 1;

	bzero(&basic, sizeof(basic));
	basic.tag = htole16(MT7902_STA_REC_BASIC);
	basic.len = htole16(sizeof(basic));
	basic.conn_type = htole32(MT7902_CONN_INFRA_AP);
	basic.conn_state = MT7902_STA_CONN_DISCONNECT;
	basic.qos = qos;
	basic.aid = htole16(aid);
	IEEE80211_ADDR_COPY(basic.peer_addr, peer);
	basic.extra_info = htole16(1); /* EXTRA_INFO_VER, not a new entry */

	bzero(&wtbl_tlv, sizeof(wtbl_tlv));
	wtbl_tlv.tag = htole16(13); /* STA_REC_WTBL */
	wtbl_tlv.len = htole16(sizeof(wtbl_tlv) + sizeof(wtbl_req));
	bzero(&wtbl_req, sizeof(wtbl_req));
	wtbl_req.wlan_idx_lo = MT7902_PEER_WCID;
	wtbl_req.operation = 1; /* WTBL_RESET_AND_SET */

	offset = 0;
	memcpy(request + offset, &hdr, sizeof(hdr));
	offset += sizeof(hdr);
	memcpy(request + offset, &basic, sizeof(basic));
	offset += sizeof(basic);
	memcpy(request + offset, &wtbl_tlv, sizeof(wtbl_tlv));
	offset += sizeof(wtbl_tlv);
	memcpy(request + offset, &wtbl_req, sizeof(wtbl_req));
	return (mt7902_mcu_send_response(sc,
	    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
	    request, sizeof(request), NULL, &(size_t){0}));
}

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
		mtx_lock(&sc->mtx);
		sc->current_channel = ic->ic_curchan->ic_ieee;
		sc->current_band = IEEE80211_IS_CHAN_2GHZ(ic->ic_curchan) ? 2 : 5;
		mtx_unlock(&sc->mtx);
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
	sc->rx_data_count = 0;
	sc->rx_data_desc_count = 0;
	sc->rx_drop_count = 0;
	sc->rx_mgmt_count = 0;
	sc->rx_beacon_count = 0;
	sc->rx_event_count = 0;
	sc->rx_event_txs_count = 0;
	sc->rx_event_txs_records = 0;
	sc->rx_event_tx_free_count = 0;
	sc->rx_event_short_count = 0;
	sc->rx_event_unknown_count = 0;
	sc->rx_last_len = 0;
	sc->rx_event_last_rxd0 = 0;
	sc->rx_event_last_len = 0;
	sc->rx_event_last_type = 0;
	bzero(sc->rx_last_rxd, sizeof(sc->rx_last_rxd));
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev, "net80211 %s scan started\n",
	    active ? "active" : "passive");
}

static void
mt7902_scan_end(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	uint64_t rx, rx_desc, dropped, mgmt, beacon, events, txs, txs_records;
	uint64_t tx_free, short_events, unknown, irq_count;
	uint32_t last_len, rxd0, rxd1, rxd2, rxd3, last_event_rxd0, last_irq;
	uint16_t last_event_len;
	uint8_t last_event_type;
	int rx_head, rx_tail, rx_queued;

	mtx_lock(&sc->mtx);
	sc->scan_active = false;
	rx = sc->rx_data_count;
	rx_desc = sc->rx_data_desc_count;
	dropped = sc->rx_drop_count;
	mgmt = sc->rx_mgmt_count;
	beacon = sc->rx_beacon_count;
	events = sc->rx_event_count;
	txs = sc->rx_event_txs_count;
	txs_records = sc->rx_event_txs_records;
	tx_free = sc->rx_event_tx_free_count;
	short_events = sc->rx_event_short_count;
	unknown = sc->rx_event_unknown_count;
	irq_count = sc->irq_count;
	last_irq = sc->irq_last_status;
	last_len = sc->rx_last_len;
	rxd0 = sc->rx_last_rxd[0];
	rxd1 = sc->rx_last_rxd[1];
	rxd2 = sc->rx_last_rxd[2];
	rxd3 = sc->rx_last_rxd[3];
	last_event_rxd0 = sc->rx_event_last_rxd0;
	last_event_len = sc->rx_event_last_len;
	last_event_type = sc->rx_event_last_type;
	rx_head = sc->rx_data.head;
	rx_tail = sc->rx_data.tail;
	rx_queued = sc->rx_data.queued;
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev,
	    "net80211 scan ended: rx=%ju data_desc=%ju dropped=%ju mgmt=%ju beacon/probe=%ju events=%ju txs=%ju/%ju tx_free=%ju short=%ju unknown=%ju irq=%ju last_irq=%08x ring2=%d/%d/%d last_evt=%u/%u/%08x last_rx=%u:%08x/%08x/%08x/%08x\\n",
	    (uintmax_t)rx, (uintmax_t)rx_desc, (uintmax_t)dropped,
	    (uintmax_t)mgmt, (uintmax_t)beacon, (uintmax_t)events,
	    (uintmax_t)txs, (uintmax_t)txs_records, (uintmax_t)tx_free,
	    (uintmax_t)short_events, (uintmax_t)unknown,
	    (uintmax_t)irq_count, last_irq, rx_head, rx_tail, rx_queued,
	    last_event_type, last_event_len, last_event_rxd0, last_len,
	    rxd0, rxd1, rxd2, rxd3);
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
	struct mt7902_sta_rec_phy phy;
	struct mt7902_sta_rec_ra ra;
	struct mt7902_sta_wtbl wtbl;
	static const uint8_t legacy_rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	static const uint8_t legacy_hw_rates[] = {
		10, 20, 55, 110, 11, 15, 10, 14, 9, 13, 8, 12
	};
	uint8_t peer[IEEE80211_ADDR_LEN], rate;
	uint16_t aid, supp_rates, basic_rates, legacy, beacon_interval;
	uint8_t dtim_period;
	bool band2, rate_basic, qos;
	int i, j, error;

	if (ni != NULL) {
		IEEE80211_ADDR_COPY(peer, ni->ni_bssid);
		qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
		beacon_interval = ni->ni_intval;
		dtim_period = ni->ni_dtim_period;
	} else if (linked) {
		return (EINVAL);
	} else if (mvp->linked) {
		IEEE80211_ADDR_COPY(peer, mvp->peer_bssid);
		qos = mvp->peer_qos;
		beacon_interval = mvp->peer_beacon_interval;
		dtim_period = mvp->peer_dtim_period;
	} else {
		IEEE80211_ADDR_COPY(peer, vap->iv_myaddr);
		qos = false;
		beacon_interval = 100;
		dtim_period = 1;
	}
	aid = linked ? IEEE80211_NODE_AID(ni) : mvp->peer_aid;
	bzero(&bss, sizeof(bss));
	bss.bss_idx = MT7902_BSS_IDX;
	bss.basic.tag = htole16(MT7902_UNI_BSS_INFO_BASIC);
	bss.basic.len = htole16(sizeof(bss.basic));
	bss.qos.tag = htole16(MT7902_UNI_BSS_INFO_QBSS);
	bss.qos.len = htole16(sizeof(bss.qos));
	bss.qos.qos = qos;
	/* Keep the STA BSS object configured; conn_state tracks link state. */
	bss.basic.active = 1;
	bss.basic.omac_idx = 0;
	bss.basic.hw_bss_idx = 0;
	bss.basic.band_idx = 0;
	bss.basic.conn_type = htole32(MT7902_CONN_INFRA_STA);
	bss.basic.conn_state = linked ? 0 : 1;
	bss.basic.wmm_idx = 0;
	IEEE80211_ADDR_COPY(bss.basic.bssid, peer);
	bss.basic.bmc_tx_wlan_idx = htole16(MT7902_BSS_WCID);
	bss.basic.bcn_interval = htole16(beacon_interval);
	bss.basic.dtim_period = dtim_period;
	bss.basic.phymode = 0x07; /* 2.4-GHz 11b/g plus 5-GHz 11a legacy */
	bss.basic.sta_idx = htole16(MT7902_BSS_WCID);
	error = mt7902_mcu_send_response(sc,
	    MT7902_MCU_UNI_CMD(0x02) /* UNI_BSS_INFO_UPDATE */,
	    &bss, sizeof(bss), NULL, &(size_t){0});
	if (error != 0)
		return (error);

	/*
	 * mt7921_mcu_set_bss_pm() first aborts any stale BSS connection on
	 * both connect and disconnect, then sends SET_BSS_CONNECTED only on
	 * the connect path.
	 */
	{
		uint8_t abort_req[4] = { MT7902_BSS_IDX, 0, 0, 0 };

		error = mt7902_mcu_send_cmd(sc,
		    MT7902_MCU_CE_CMD(0x17) /* SET_BSS_ABORT */,
		    abort_req, sizeof(abort_req), false);
		if (error != 0)
			return (error);
	}
	if (!linked) {
		mvp->linked = false;
		mtx_lock(&sc->mtx);
		sc->station_linked = 0;
		sc->station_aid = 0;
		sc->station_basic_rate = 0;
		mtx_unlock(&sc->mtx);
		error = mt7902_sta_disconnect(sc, peer, aid, qos);
		bzero(mvp->peer_bssid, sizeof(mvp->peer_bssid));
		mvp->peer_aid = 0;
		mvp->peer_beacon_interval = 0;
		mvp->peer_dtim_period = 0;
		mvp->peer_qos = false;
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
		conn.dtim_period = dtim_period;
		conn.aid = htole16(aid);
		conn.beacon_interval = htole16(beacon_interval);
		error = mt7902_mcu_send_cmd(sc, MT7902_MCU_CE_CMD(0x16),
		    &conn, sizeof(conn), false);
		if (error != 0)
			return (error);
	}

	bzero(&hdr, sizeof(hdr));
	hdr.bss_idx = MT7902_BSS_IDX;
	hdr.wlan_idx_lo = MT7902_PEER_WCID;
	hdr.tlv_num = htole16(5); /* BASIC, PHY, RA, STATE, WTBL */
	hdr.is_tlv_append = 1;
	memset(&basic, 0, sizeof(basic));
	basic.tag = htole16(MT7902_STA_REC_BASIC);
	basic.len = htole16(sizeof(basic));
	basic.conn_type = htole32(MT7902_CONN_INFRA_AP);
	basic.conn_state = MT7902_STA_CONN_PORT_SECURE;
	basic.qos = qos;
	basic.aid = htole16(aid);
	IEEE80211_ADDR_COPY(basic.peer_addr, peer);
	basic.extra_info = htole16(3); /* version + newly created */
	memset(&state, 0, sizeof(state));
	/*
	 * Build Connac2's legacy CCK/OFDM rate bitmap from the negotiated
	 * FreeBSD node rateset.  HT/VHT/HE masks stay zero because this
	 * interface advertises legacy modes only.
	 */
	band2 = IEEE80211_IS_CHAN_2GHZ(ni->ni_chan);
	supp_rates = 0;
	basic_rates = 0;
	for (i = 0; i < ni->ni_rates.rs_nrates; i++) {
		rate_basic = (ni->ni_rates.rs_rates[i] & 0x80) != 0;
		rate = ni->ni_rates.rs_rates[i] & 0x7f;
		for (j = 0; j < nitems(legacy_rates); j++) {
			if (legacy_rates[j] != rate)
				continue;
			if (band2) {
				supp_rates |= (uint16_t)(1U << j);
				if (rate_basic)
					basic_rates |= (uint16_t)(1U << j);
			} else if (j >= 4) {
				supp_rates |= (uint16_t)(1U << (j - 4));
				if (rate_basic)
					basic_rates |= (uint16_t)(1U << (j - 4));
			}
			break;
		}
	}
	if (band2)
		legacy = (supp_rates & MT7902_RA_LEGACY_CCK) |
		    (((supp_rates >> 4) & 0xffU) << 6);
	else
		legacy = (supp_rates & 0xffU) << 6;

	/*
	 * Pick the first AP-advertised basic rate for management/multicast
	 * frames. Connac2 encodes mode in bits 9:6 and rate index in bits 5:0.
	 * Keep CCK mode 0 for 2.4 GHz CCK rates; all OFDM rates use mode 1.
	 */
	{
		uint16_t tx_basic;
		int n_basic;

		tx_basic = 0;
		n_basic = band2 ? 12 : 8;
		for (j = 0; j < n_basic; j++) {
			if ((basic_rates & (1U << j)) == 0)
				continue;
			tx_basic = legacy_hw_rates[band2 ? j : j + 4];
			if (!band2 || j >= 4)
				tx_basic |= (1U << 6); /* OFDM */
			break;
		}
		if (tx_basic == 0)
			tx_basic = band2 ? 10 : ((1U << 6) | 11);
		mtx_lock(&sc->mtx);
		sc->station_basic_rate = tx_basic;
		mtx_unlock(&sc->mtx);
	}

	bzero(&phy, sizeof(phy));
	phy.tag = htole16(MT7902_STA_REC_PHY);
	phy.len = htole16(sizeof(phy));
	phy.basic_rate = htole16(basic_rates);
	/* PHY_MODE_B|PHY_MODE_G for 2.4 GHz, PHY_MODE_A for 5 GHz. */
	phy.phy_type = band2 ? ((1U << 1) | (1U << 2)) : (1U << 0);

	bzero(&ra, sizeof(ra));
	ra.tag = htole16(MT7902_STA_REC_RA);
	ra.len = htole16(sizeof(ra));
	ra.legacy = htole16(legacy);

	bzero(&state, sizeof(state));
	state.tag = htole16(MT7902_STA_REC_STATE);
	state.len = htole16(sizeof(state));
	state.state = MT7902_STA_STATE_ASSOC;

	/*
	 * Connac2 requires a WTBL record alongside STA_REC.  These are the
	 * minimum station-mode records used by mt76: GENERIC, RX and HDR_TRANS.
	 * Keep receive header translation disabled because the native FreeBSD
	 * RX path consumes ordinary 802.11 MAC headers.
	 */
	bzero(&wtbl, sizeof(wtbl));
	wtbl.tlv.tag = htole16(13); /* STA_REC_WTBL */
	wtbl.tlv.len = htole16(sizeof(wtbl));
	wtbl.request.wlan_idx_lo = MT7902_PEER_WCID;
	wtbl.request.operation = 1; /* WTBL_RESET_AND_SET */
	wtbl.request.tlv_num = htole16(3);
	wtbl.generic.tag = htole16(0); /* WTBL_GENERIC */
	wtbl.generic.len = htole16(sizeof(wtbl.generic));
	IEEE80211_ADDR_COPY(wtbl.generic.peer_addr, peer);
	wtbl.generic.muar_idx = 0;
	wtbl.generic.qos = qos;
	wtbl.generic.partial_aid = htole16(aid);
	wtbl.rx.tag = htole16(1); /* WTBL_RX */
	wtbl.rx.len = htole16(sizeof(wtbl.rx));
	wtbl.rx.rca1 = 1; /* station interface */
	wtbl.rx.rca2 = 1;
	wtbl.rx.rv = 1;
	wtbl.hdr_trans.tag = htole16(6); /* WTBL_HDR_TRANS */
	wtbl.hdr_trans.len = htole16(sizeof(wtbl.hdr_trans));
	wtbl.hdr_trans.to_ds = 1;
	wtbl.hdr_trans.from_ds = 0;
	wtbl.hdr_trans.no_rx_trans = 1;

	{
		uint8_t request[sizeof(hdr) + sizeof(basic) + sizeof(phy) +
		    sizeof(ra) + sizeof(state) + sizeof(wtbl)];
		size_t offset;

		offset = 0;
		memcpy(request + offset, &hdr, sizeof(hdr));
		offset += sizeof(hdr);
		memcpy(request + offset, &basic, sizeof(basic));
		offset += sizeof(basic);
		memcpy(request + offset, &phy, sizeof(phy));
		offset += sizeof(phy);
		memcpy(request + offset, &ra, sizeof(ra));
		offset += sizeof(ra);
		memcpy(request + offset, &state, sizeof(state));
		offset += sizeof(state);
		memcpy(request + offset, &wtbl, sizeof(wtbl));
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
		    request, sizeof(request), NULL, &(size_t){0});
	}
	if (error == 0) {
		mvp->linked = true;
		IEEE80211_ADDR_COPY(mvp->peer_bssid, peer);
		mvp->peer_aid = aid;
		mvp->peer_beacon_interval = beacon_interval;
		mvp->peer_dtim_period = dtim_period;
		mvp->peer_qos = qos;
		mtx_lock(&sc->mtx);
		sc->station_linked = 1;
		sc->station_aid = aid;
		mtx_unlock(&sc->mtx);
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
		if (vap->iv_bss != NULL)
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
	if (opmode == IEEE80211_M_MONITOR) {
		struct mt7902_softc *sc = ic->ic_softc;
		int error;

		mtx_lock(&sc->mtx);
		sc->monitor_active = true;
		mtx_unlock(&sc->mtx);
		error = mt7902_mcu_set_rxfilter(sc,
		    MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS |
		    MT7902_RX_FILTER_CONTROL | MT7902_RX_FILTER_FCSFAIL);
		if (error != 0)
			device_printf(sc->dev,
			    "could not enable monitor receive filters: %d\n", error);
	}
	return (vap);
}

static void
mt7902_vap_delete(struct ieee80211vap *vap)
{
	struct mt7902_vap *mvp = (struct mt7902_vap *)vap;
	struct mt7902_softc *sc = vap->iv_ic->ic_softc;
	int error;

	if (vap->iv_opmode == IEEE80211_M_MONITOR) {
		mtx_lock(&sc->mtx);
		sc->monitor_active = false;
		mtx_unlock(&sc->mtx);
		if (sc->hw_started) {
			error = mt7902_mcu_set_rxfilter(sc,
			    MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS);
			if (error != 0)
				device_printf(sc->dev,
				    "could not restore station receive filters: %d\n",
				    error);
		}
	}
	ieee80211_stop(vap);
	vap->iv_newstate = mvp->newstate;
	ieee80211_vap_detach(vap);
	free(mvp, M_80211_VAP);
}

/*
 * Program the four EDCA access categories using the Connac2 CE command.
 * FreeBSD stores CWmin/CWmax as log2 exponents; firmware expects the actual
 * contention-window values (2^ECW - 1). Firmware ACI order is BK, BE, VI, VO.
 */
static int
mt7902_wme_update(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	struct chanAccParams chp;
	struct mt7902_edca_entry {
		uint16_t cw_min;
		uint16_t cw_max;
		uint16_t txop;
		uint16_t aifs;
		uint8_t guardtime;
		uint8_t acm;
	} __packed;
	struct mt7902_edca_request {
		struct mt7902_edca_entry edca[WME_NUM_AC];
		uint8_t bss_idx;
		uint8_t qos;
		uint8_t wmm_idx;
		uint8_t reserved;
	} __packed req;
	static const uint8_t to_aci[WME_NUM_AC] = { 1, 0, 2, 3 };
	struct wmeParams *wmep;
	struct mt7902_edca_entry *e;
	uint32_t cw_min, cw_max;
	int ac;

	/* Firmware startup calls this after the MCU/WFDMA rings are ready. */
	if (sc->tx_mcu.desc == NULL)
		return (0);
	ieee80211_wme_ic_getparams(ic, &chp);
	bzero(&req, sizeof(req));
	req.bss_idx = 0;
	req.qos = 1;
	req.wmm_idx = 0;
	for (ac = 0; ac < WME_NUM_AC; ac++) {
		wmep = &chp.cap_wmeParams[ac];
		e = &req.edca[to_aci[ac]];
		cw_min = (1U << MIN(wmep->wmep_logcwmin, 15)) - 1;
		cw_max = (1U << MIN(wmep->wmep_logcwmax, 15)) - 1;
		e->cw_min = htole16((uint16_t)cw_min);
		e->cw_max = htole16((uint16_t)cw_max);
		e->txop = htole16((uint16_t)wmep->wmep_txopLimit);
		e->aifs = htole16((uint16_t)wmep->wmep_aifsn);
		e->guardtime = 0;
		e->acm = wmep->wmep_acm;
	}
	return (mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_CE_CMD(MT7902_MCU_CE_SET_EDCA_PARMS),
	    &req, sizeof(req), false));
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
	if (sc->dying || sc->suspended || !sc->hw_started ||
	    !sc->net80211_attached)
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
	    params->ibp_power != 0 || params->ibp_ctsrate != 0 ||
	    params->ibp_try1 != 0 ||
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
	struct ieee80211_bpf_params tx_params;

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
	/*
	 * ieee80211_raw_output() normally attaches this tag before invoking
	 * ic_raw_xmit().  Attach it here as well for callers that invoke the
	 * driver raw-xmit callback directly.  TX worker reads the same tag
	 * after the mbuf has entered the asynchronous queue.
	 */
	if (params != NULL &&
	    ieee80211_get_xmit_params(m, &tx_params) != 0 &&
	    !ieee80211_add_xmit_params(m, params)) {
		if (m->m_flags & M_TXCB)
			ieee80211_process_callback(ni, m, 1);
		m_freem(m);
		return (ENOMEM);
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

/*
 * Return the actual 802.11 MAC header length represented by frame control.
 * The first 16 bytes are common to most control frames, but management and
 * data frames include addr3 and sequence control before optional fields.
 */
static size_t
mt7902_80211_hdrlen(uint16_t fc)
{
	size_t len;
	uint16_t type, subtype;

	type = fc & 0x000c;
	subtype = (fc >> 4) & 0x0f;
	switch (type) {
	case 0x0000:             /* management */
		len = sizeof(struct ieee80211_frame);
		if (fc & 0x8000)       /* Order / HT control */
			len += 4;
		break;
	case 0x0004:             /* control */
		if (subtype == 0x0c || subtype == 0x0d)
			len = sizeof(struct ieee80211_frame_cts);
		else
			len = sizeof(struct ieee80211_frame_min);
		break;
	case 0x0008:             /* data */
		len = sizeof(struct ieee80211_frame);
		if ((fc & 0x0300) == 0x0300)
			len += IEEE80211_ADDR_LEN;
		if (subtype & 0x08)
			len += 2;       /* QoS control */
		if (fc & 0x8000)
			len += 4;       /* HT control */
		break;
	default:
		len = 0;
		break;
	}
	return (len);
}

static int
mt7902_tx_one(struct mt7902_softc *sc, struct mbuf *m)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	struct mt7902_txp *txp;
	struct ieee80211_frame_min wh;
	struct ieee80211_bpf_params tx_params;
	uint32_t *txwi, fc, val, control, rate;
	size_t len, total, hdrlen, qos_offset;
	uint16_t tx_basic_rate, qos_control, seq_control;
	uint8_t current_band, tid, frag_idx;
	int idx, retry, error, queue, i;
	bool fixed_rate, station_linked, more_frag, has_tx_params, noack;
	static const uint8_t raw_rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	static const uint8_t raw_hw_rates[] = {
		10, 20, 55, 110, 11, 15, 10, 14, 9, 13, 8, 12
	};

	has_tx_params = ieee80211_get_xmit_params(m, &tx_params) == 0;
	noack = has_tx_params &&
	    (tx_params.ibp_flags & IEEE80211_BPF_NOACK) != 0;
	/* FreeBSD WME order is BE/BK/VI/VO; Connac2 LMAC is reversed. */
	queue = 3 - (has_tx_params ? tx_params.ibp_pri : M_WME_GETAC(m));
	if (queue < 0 || queue >= MT7902_TX_DATA_QUEUES)
		return (EINVAL);
	ring = &sc->tx_data[queue];
	len = m->m_pkthdr.len;
	total = MT7902_TXD_LEN + len;
	if (len < sizeof(struct ieee80211_frame_cts) ||
	    total > ring->buf[0].len || len > MT7902_TXP_LEN_MASK)
		return (EMSGSIZE);
	bzero(&wh, sizeof(wh));
	m_copydata(m, 0, MIN(len, sizeof(wh)), (caddr_t)&wh);
	fc = le16toh(wh.i_fc[0] | ((uint16_t)wh.i_fc[1] << 8));
	hdrlen = mt7902_80211_hdrlen(fc);
	if (hdrlen == 0 || len < hdrlen)
		return (EMSGSIZE);

	/*
	 * QoS control follows the optional addr4 field.  Connac2 carries a
	 * three-bit TID in TXD1; use the low TID bits, as the HW field does.
	 */
	tid = 0;
	if ((fc & 0x000c) == 0x0008 && ((fc & 0x00f0) & 0x0080) != 0) {
		qos_offset = sizeof(struct ieee80211_frame);
		if ((fc & 0x0300) == 0x0300)
			qos_offset += IEEE80211_ADDR_LEN;
		if (len < qos_offset + sizeof(qos_control))
			return (EMSGSIZE);
		m_copydata(m, qos_offset, sizeof(qos_control),
		    (caddr_t)&qos_control);
		tid = le16toh(qos_control) & 0x07;
	}

	/* TXD2 fragmentation class follows the 802.11 sequence control. */
	more_frag = (fc & 0x0400) != 0;
	frag_idx = 0; /* MT_TX_FRAG_NONE */
	if ((fc & 0x000c) == 0 || (fc & 0x000c) == 0x0008) {
		if (len < 24)
			return (EMSGSIZE);
		m_copydata(m, 22, sizeof(seq_control), (caddr_t)&seq_control);
		seq_control = le16toh(seq_control);
		if (more_frag)
			frag_idx = (seq_control & 0x0f) == 0 ? 1 : 2;
		else if ((seq_control & 0x0f) != 0)
			frag_idx = 3; /* MT_TX_FRAG_LAST */
	}
	mtx_lock(&sc->mtx);
	idx = ring->head;
	desc = &ring->desc[idx];
	buf = &ring->buf[idx];
	station_linked = sc->station_linked != 0;
	current_band = sc->current_band;
	tx_basic_rate = sc->station_basic_rate;
	mtx_unlock(&sc->mtx);

	/*
	 * A previous transmission can outlive its software timeout.  Wait in
	 * process context for this descriptor to return to the host before
	 * touching its DMA buffer; never hold the device mutex while polling.
	 */
	for (retry = 0; retry < 10000; retry++) {
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) != 0)
			break;
		DELAY(10);
	}
	if (retry == 10000)
		return (EBUSY);

	mtx_lock(&sc->mtx);
	if (ring->head != idx) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	if (buf->dma_pending) {
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTWRITE);
		buf->dma_pending = false;
	}
	memset(buf->vaddr, 0, MT7902_TXD_LEN);
	m_copydata(m, 0, len, (caddr_t)buf->vaddr + MT7902_TXD_LEN);
	txwi = buf->vaddr;
	val = (uint32_t)(len + MT7902_TXWI_LEN) |
	    (0U << MT7902_TXD_PKT_FMT_SHIFT) |
	    ((uint32_t)queue << MT7902_TXD_QIDX_SHIFT);
	txwi[0] = htole32(val);
	/*
	 * Connac2 TXD1: long format, WCID, 802.11 header format, header length
	 * and TID.  These header-format fields must not be written to TXD2.
	 */
	val = MT7902_TXD_LONG |
	    ((uint32_t)(station_linked ? MT7902_PEER_WCID :
	    MT7902_BSS_WCID) << MT7902_TXD_WLAN_IDX_SHIFT) |
	    MT7902_TXD_HDR_80211 |
	    (((uint32_t)(hdrlen / 2) & 0x1f) << MT7902_TXD_HDR_INFO_SHIFT) |
	    ((uint32_t)tid << MT7902_TXD_TID_SHIFT);
	txwi[1] = htole32(val);

	/* Connac2 TXD2 contains frame type/subtype, multicast, rate and frag. */
	val = (((fc & 0x0c) >> 2) << 4) | ((fc & 0xf0) >> 4) |
	    ((uint32_t)frag_idx << MT7902_TXD_FRAG_SHIFT);
	if (ETHER_IS_MULTICAST(wh.i_addr1))
		val |= 1U << 10;
	fixed_rate = (fc & 0x000c) != 0x0008 ||
	    ETHER_IS_MULTICAST(wh.i_addr1) ||
	    (has_tx_params && tx_params.ibp_rate0 != 0);
	if (fixed_rate)
		val |= MT7902_TXD_FIX_RATE;
	txwi[2] = htole32(val);
	val = MT7902_TXD_RETRY;
	if (has_tx_params && !noack)
		val = (uint32_t)tx_params.ibp_try0 << 11;
	txwi[3] = htole32(val);
	if (noack)
		txwi[3] |= htole32(MT7902_TXD_NO_ACK);
	txwi[4] = 0;
	txwi[5] = 0;
	txwi[6] = 0;
	/*
	 * Connac2 MT_TX_RATE_MODE / MT_TX_RATE_IDX live in TXD6[29:16].
	 * Prefer an explicit BPF rate, then the AP's negotiated basic rate,
	 * then the 1 Mbps CCK / 6 Mbps OFDM default for unassociated operation.
	 */
	if (fixed_rate) {
		rate = 0;
		if (has_tx_params && tx_params.ibp_rate0 != 0) {
			for (i = 0; i < nitems(raw_rates); i++) {
				if (tx_params.ibp_rate0 != raw_rates[i])
					continue;
				rate = raw_hw_rates[i];
				if (i >= 4)
					rate |= 1U << 6; /* OFDM */
				break;
			}
		}
		if (rate == 0)
			rate = tx_basic_rate;
		if (rate == 0)
			rate = current_band == 5 ?
			    ((1U << 6) | 11U) : 10U;
		txwi[2] |= htole32(MT7902_TXD_HTC_VALID);
		txwi[3] |= htole32(MT7902_TXD_BA_DISABLE);
		txwi[6] = htole32((rate << MT7902_TXD6_TX_RATE_SHIFT) |
		    MT7902_TXD6_FIXED_BW);
	}
	txwi[7] = htole32((((fc & 0x0c) >> 2) << 20) |
	    (((fc & 0xf0) >> 4) << 16));
	txp = (struct mt7902_txp *)((uint8_t *)buf->vaddr + MT7902_TXWI_LEN);
	txp->msdu_id[0] = htole16((uint16_t)idx | (1U << 15));
	txp->ptr[0].buf0 = htole32((uint32_t)(buf->paddr + MT7902_TXD_LEN));
	txp->ptr[0].len0 = htole16((uint16_t)(len &
	    MT7902_TXP_LEN_MASK) | MT7902_TXP_LAST);
	bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_PREWRITE);
	buf->dma_pending = true;
	desc->buf0 = htole32((uint32_t)buf->paddr);
	desc->buf1 = 0;
	desc->info = 0;
	/*
	 * Connac2 WFDMA fetches only TXWI+TXP from this descriptor.  The
	 * TXP's buf0/len0 then point at the frame payload in the same DMA
	 * allocation; setting the WFDMA length to total would make it consume
	 * the payload twice.
	 */
	desc->control = htole32(((uint32_t)MT7902_TXD_LEN <<
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
	if (error == 0) {
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTWRITE);
		buf->dma_pending = false;
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

	if (sc == NULL)
		return (EINVAL);
	if (sc->net80211_attached) {
		int error;

		error = mt7902_wme_update(&sc->ic);
		if (error != 0)
			device_printf(sc->dev,
			    "could not restore EDCA parameters after firmware start: %d\n",
			    error);
		return (0);
	}
	if (ETHER_IS_MULTICAST(sc->macaddr) || sc->macaddr[0] == 0)
		return (EINVAL);
	ic = &sc->ic;
	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(sc->dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->macaddr);
	ic->ic_caps = IEEE80211_C_STA | IEEE80211_C_MONITOR |
	    IEEE80211_C_WPA | IEEE80211_C_WME;
	setbit(ic->ic_modecaps, IEEE80211_MODE_11B);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11G);
	setbit(ic->ic_modecaps, IEEE80211_MODE_11A);
	mt7902_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
	    ic->ic_channels);
	if (ic->ic_nchans == 0)
		return (ENXIO);
	ieee80211_ifattach(ic);
	ic->ic_getradiocaps = mt7902_getradiocaps;
	ic->ic_vap_create = mt7902_vap_create;
	ic->ic_vap_delete = mt7902_vap_delete;
	ic->ic_parent = mt7902_parent;
	ic->ic_set_channel = mt7902_set_channel_cb;
	ic->ic_scan_start = mt7902_scan_start;
	ic->ic_scan_end = mt7902_scan_end;
	ic->ic_raw_xmit = mt7902_raw_xmit;
	ic->ic_wme.wme_update = mt7902_wme_update;
	ic->ic_transmit = mt7902_transmit;
	ieee80211_set_software_ciphers(ic, IEEE80211_CRYPTO_AES_CCM);
	mbufq_init(&sc->txq, MT7902_TX_RING_SIZE);
	sc->net80211_attached = 1;
	{
		int error;

		error = mt7902_wme_update(ic);
		if (error != 0)
			device_printf(sc->dev,
			    "could not program initial EDCA parameters: %d\n",
			    error);
	}
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

