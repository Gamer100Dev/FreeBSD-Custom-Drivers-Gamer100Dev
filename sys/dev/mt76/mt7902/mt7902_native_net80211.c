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
#include "mt7902_native_ba.h"

#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_ht.h>

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
remove local BA constants#define MT7902_MCU_UNI_DEV_INFO_UPDATE	0x01
#define MT7902_MCU_UNI_BSS_INFO_UPDATE	0x02
#define MT7902_MCU_EXT_MAC_INIT_CTRL	0x46
#define MT7902_MCU_EXT_SET_RX_PATH	0x4e
#define MT7902_UNI_DEV_INFO_ACTIVE	0
#define MT7902_UNI_BSS_INFO_BASIC	0
#define MT7902_UNI_BSS_INFO_QBSS	15
#define MT7902_STA_REC_STATE	7
#define MT7902_STA_REC_RA		1
#define MT7902_STA_REC_HT		9
#define MT7902_STA_REC_VHT		10
#define MT7902_STA_REC_WTBL		13
#define MT7902_WTBL_SET		2
#define MT7902_PHY_TYPE_HR_DSSS	(1U << 0)
#define MT7902_PHY_TYPE_ERP		(1U << 1)
#define MT7902_PHY_TYPE_OFDM		(1U << 3)
#define MT7902_PHY_TYPE_HT		(1U << 4)
#define MT7902_PHY_TYPE_VHT		(1U << 5)
#define MT7902_VHT_OPMODE_BW_20_40	0
#define MT7902_VHT_OPMODE_BW_80		1
#define MT7902_VHT_OPMODE_RX_NSS_SHIFT	4
#define MT7902_BSS_PHY_A		(1U << 0)
#define MT7902_BSS_PHY_B		(1U << 1)
#define MT7902_BSS_PHY_G		(1U << 2)
#define MT7902_STA_REC_PHY		0x15
#define MT7902_RA_LEGACY_CCK		0x000fU
#define MT7902_STA_CONN_PORT_SECURE	2
#define MT7902_STA_CONN_DISCONNECT	0
#define MT7902_STA_STATE_ASSOC	2
#define MT7902_STA_STATE_NONE	0
#define MT7902_BSS_IDX		0
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

struct mt7902_sta_rec_ht {
	uint16_t tag;
	uint16_t len;
	uint16_t ht_cap;
	uint16_t reserved;
} __packed;

struct mt7902_sta_rec_vht {
	uint16_t tag;
	uint16_t len;
	uint32_t vht_cap;
	uint16_t vht_rx_mcs_map;
	uint16_t vht_tx_mcs_map;
	uint8_t rts_bw_sig;
	uint8_t reserved[3];
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

struct mt7902_wtbl_ht {
	uint16_t tag;
	uint16_t len;
	uint8_t ht;
	uint8_t ldpc;
	uint8_t ampdu_factor;
	uint8_t ampdu_density;
	uint8_t reserved[4];
} __packed;

struct mt7902_wtbl_vht {
	uint16_t tag;
	uint16_t len;
	uint8_t ldpc;
	uint8_t dyn_bw;
	uint8_t vht;
	uint8_t txop_ps;
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
CTASSERT(sizeof(struct mt7902_sta_rec_ht) == 8);
CTASSERT(sizeof(struct mt7902_sta_rec_vht) == 16);
CTASSERT(sizeof(struct mt7902_wtbl_ht) == 12);
CTASSERT(sizeof(struct mt7902_wtbl_vht) == 12);

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
	uint8_t peer_channel;
	bool peer_qos;
	bool linked;
	bool fw_registered;
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

/*
 * Register the interface with Connac2 firmware before scans begin. Linux
 * mt7921_add_interface() performs DEV_INFO_UPDATE followed by an initial
 * BSS_INFO_UPDATE; without these records the firmware has no active OMAC
 * and BSS context associated with this net80211 VAP.
 */
static int
mt7902_vap_fw_update(struct mt7902_softc *sc, struct ieee80211vap *vap,
    bool enable)
{
	struct {
		struct {
			uint8_t omac_idx;
			uint8_t band_idx;
			uint16_t pad;
		} __packed hdr;
		struct {
			uint16_t tag;
			uint16_t len;
			uint8_t active;
			uint8_t link_idx;
			uint8_t omac_addr[IEEE80211_ADDR_LEN];
		} __packed tlv;
	} __packed dev_req;
	struct {
		struct {
			uint8_t bss_idx;
			uint8_t pad[3];
		} __packed hdr;
		struct mt7902_bss_basic basic;
	} __packed bss_req;
	uint32_t conn_type;
	int error, rollback_error;

	conn_type = vap->iv_opmode == IEEE80211_M_MONITOR ?
	    MT7902_CONN_INFRA_AP : MT7902_CONN_INFRA_STA;

	bzero(&dev_req, sizeof(dev_req));
	dev_req.hdr.omac_idx = 0;
	dev_req.hdr.band_idx = 0;
	dev_req.tlv.tag = htole16(MT7902_UNI_DEV_INFO_ACTIVE);
	dev_req.tlv.len = htole16(sizeof(dev_req.tlv));
	dev_req.tlv.active = enable;
	dev_req.tlv.link_idx = 0;
	IEEE80211_ADDR_COPY(dev_req.tlv.omac_addr, vap->iv_myaddr);

	bzero(&bss_req, sizeof(bss_req));
	bss_req.hdr.bss_idx = MT7902_BSS_IDX;
	bss_req.basic.tag = htole16(MT7902_UNI_BSS_INFO_BASIC);
	bss_req.basic.len = htole16(sizeof(bss_req.basic));
	bss_req.basic.active = enable;
	bss_req.basic.omac_idx = 0;
	bss_req.basic.hw_bss_idx = 0;
	bss_req.basic.band_idx = 0;
	bss_req.basic.conn_type = htole32(conn_type);
	/* Match mt76_connac_mcu_uni_add_dev(): BSS conn_state starts CONNECT. */
	bss_req.basic.conn_state = 1;
	bss_req.basic.wmm_idx = 0;
	bss_req.basic.bmc_tx_wlan_idx = htole16(MT7902_BSS_WCID);
	bss_req.basic.sta_idx = htole16(MT7902_BSS_WCID);

	if (enable) {
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_DEV_INFO_UPDATE),
		    &dev_req, sizeof(dev_req), NULL, &(size_t){0});
		if (error != 0) {
			device_printf(sc->dev,
			    "VAP firmware registration: DEV_INFO_UPDATE failed: %d\n",
			    error);
			return (error);
		}
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_BSS_INFO_UPDATE),
		    &bss_req, sizeof(bss_req), NULL, &(size_t){0});
		if (error != 0) {
			dev_req.tlv.active = 0;
			rollback_error = mt7902_mcu_send_response(sc,
			    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_DEV_INFO_UPDATE),
			    &dev_req, sizeof(dev_req), NULL, &(size_t){0});
			device_printf(sc->dev,
			    "VAP firmware registration: BSS_INFO_UPDATE failed: %d; DEV rollback=%d\n",
			    error, rollback_error);
			return (error);
		}
		device_printf(sc->dev,
		    "VAP firmware registered: DEV_INFO_UPDATE+BSS_INFO_UPDATE omac=0 bss=0 wcid=%u mode=%s\n",
		    MT7902_BSS_WCID,
		    vap->iv_opmode == IEEE80211_M_MONITOR ? "monitor" : "station");
		return (0);
	}

	/* Remove the BSS before releasing its OMAC device record. */
	error = mt7902_mcu_send_response(sc,
	    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_BSS_INFO_UPDATE),
	    &bss_req, sizeof(bss_req), NULL, &(size_t){0});
	if (error != 0) {
		device_printf(sc->dev,
		    "VAP firmware removal: BSS_INFO_UPDATE failed: %d\n", error);
		return (error);
	}
	error = mt7902_mcu_send_response(sc,
	    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_DEV_INFO_UPDATE),
	    &dev_req, sizeof(dev_req), NULL, &(size_t){0});
	if (error != 0)
		device_printf(sc->dev,
		    "VAP firmware removal: DEV_INFO_UPDATE failed: %d\n", error);
	else
		device_printf(sc->dev,
		    "VAP firmware unregistered: omac=0 bss=0\n");
	return (error);
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
	struct mt7902_softc *sc;
	uint8_t bands2[IEEE80211_MODE_BYTES], bands5[IEEE80211_MODE_BYTES];
	int count, i, cbw2, cbw5;
	bool ht2, ht5, vht5;

	sc = ic->ic_softc;
	ht2 = sc->nic_caps.has_2ghz && sc->nic_caps.phy.ht;
	ht5 = sc->nic_caps.has_5ghz && sc->nic_caps.phy.ht;
	vht5 = ht5 && sc->nic_caps.phy.vht &&
	    sc->nic_caps.phy.max_bw >= 2;

	bzero(bands2, sizeof(bands2));
	bzero(bands5, sizeof(bands5));
	setbit(bands2, IEEE80211_MODE_11B);
	setbit(bands2, IEEE80211_MODE_11G);
	setbit(bands5, IEEE80211_MODE_11A);
	cbw2 = 0;
	cbw5 = 0;
	if (ht2) {
		setbit(bands2, IEEE80211_MODE_11NG);
		if ((ic->ic_htcaps & IEEE80211_HTCAP_CHWIDTH40) != 0)
			cbw2 |= NET80211_CBW_FLAG_HT40;
	}
	if (ht5) {
		setbit(bands5, IEEE80211_MODE_11NA);
		if ((ic->ic_htcaps & IEEE80211_HTCAP_CHWIDTH40) != 0)
			cbw5 |= NET80211_CBW_FLAG_HT40;
	}
	if (vht5) {
		setbit(bands5, IEEE80211_MODE_VHT_5GHZ);
		if ((ic->ic_htcaps & IEEE80211_HTCAP_CHWIDTH40) != 0)
			cbw5 |= NET80211_CBW_FLAG_VHT80;
	}

	count = 0;
	if (sc->nic_caps.has_2ghz)
		(void)ieee80211_add_channels_default_2ghz(chans, maxchans,
		    &count, bands2, cbw2);
	if (sc->nic_caps.has_5ghz)
		(void)ieee80211_add_channel_list_5ghz(chans, maxchans, &count,
		    mt7902_5ghz_channels, nitems(mt7902_5ghz_channels),
		    bands5, cbw5);
	for (i = 0; i < count; i++) {
		if (IEEE80211_IS_CHAN_5GHZ(&chans[i])) {
			chans[i].ic_flags |= IEEE80211_CHAN_PASSIVE;
			if (chans[i].ic_ieee >= 52 && chans[i].ic_ieee <= 144)
				chans[i].ic_flags |= IEEE80211_CHAN_DFS;
		}
	}
	*nchans = count;
}

struct mt7902_chan_info {
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
} __packed;

CTASSERT(sizeof(struct mt7902_chan_info) == 76);

/*
 * Fill the Connac2 channel tuple from the selected net80211 channel. The
 * primary channel and center channel differ for HT40/VHT80; reporting the
 * primary channel in both fields configures the wrong RF segment.
 */
static void
mt7902_chan_info_init(struct mt7902_softc *sc, struct ieee80211_channel *c,
    struct mt7902_chan_info *req)
{
	uint32_t center_freq;
	uint8_t nss;

	center_freq = ieee80211_get_channel_center_freq1(c);
	if (center_freq == 0)
		center_freq = c->ic_freq;
	nss = MIN(sc->nic_caps.phy.nss, 4);
	if (nss == 0)
		nss = 1;

	bzero(req, sizeof(*req));
	req->control_ch = c->ic_ieee;
	req->center_ch = ieee80211_mhz2ieee(center_freq,
	    IEEE80211_IS_CHAN_2GHZ(c) ? IEEE80211_CHAN_2GHZ :
	    IEEE80211_CHAN_5GHZ);
	req->bw = IEEE80211_IS_CHAN_VHT80(c) ? 2 :
	    (IEEE80211_IS_CHAN_HT40(c) ? 1 : 0);
	req->tx_streams = nss;
	req->rx_streams = nss;
	req->band_idx = 0;
	req->channel_band = IEEE80211_IS_CHAN_2GHZ(c) ? 0 : 1;
}

/*
 * Linux mt7921_start() enables the firmware MAC and selects the RX path
 * before the first CHANNEL_SWITCH.  Firmware boot and BSS registration alone
 * do not start the radio: without MAC_INIT_CTRL the MCU will acknowledge
 * channel changes and TX resource releases while the data RX ring stays idle.
 */
static int
mt7902_radio_start(struct mt7902_softc *sc, struct ieee80211_channel *c)
{
	struct {
		uint8_t enable;
		uint8_t band;
		uint8_t reserved[2];
	} __packed mac_req;
	struct mt7902_chan_info path_req;
	bool started;
	int error;

	mtx_lock(&sc->mtx);
	started = sc->radio_enabled;
	mtx_unlock(&sc->mtx);
	if (started)
		return (0);

	bzero(&mac_req, sizeof(mac_req));
	mac_req.enable = 1;
	mac_req.band = 0;
	error = mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_EXT_CMD(MT7902_MCU_EXT_MAC_INIT_CTRL),
	    &mac_req, sizeof(mac_req), true);
	if (error != 0) {
		device_printf(sc->dev,
		    "radio start: MAC_INIT_CTRL enable failed: %d\n", error);
		return (error);
	}

	/*
	 * Keep WM firmware's regulatory/channel table in sync with the
	 * FreeBSD net80211 channel list before selecting the RX path.
	 */
	error = mt7902_mcu_set_channel_domain(sc);
	if (error != 0) {
		device_printf(sc->dev,
		    "radio start: SET_CHAN_DOMAIN submission failed: %d\n",
		    error);
		return (error);
	}

	mt7902_chan_info_init(sc, c, &path_req);
	/* SET_RX_PATH takes an RX antenna mask; CHANNEL_SWITCH takes a count. */
	path_req.rx_streams = (1U << path_req.rx_streams) - 1;
	path_req.switch_reason = 0; /* CH_SWITCH_NORMAL */
	error = mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_EXT_CMD(MT7902_MCU_EXT_SET_RX_PATH),
	    &path_req, sizeof(path_req), true);
	if (error != 0) {
		device_printf(sc->dev,
		    "radio start: SET_RX_PATH failed on channel %u: %d\n",
		    c->ic_ieee, error);
		return (error);
	}

	mtx_lock(&sc->mtx);
	sc->radio_enabled = true;
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev,
	    "radio start complete: MAC enabled, RX path configured (channel=%u band=%u RX mask=0x%02x)\n",
	    c->ic_ieee, path_req.channel_band, path_req.rx_streams);
	return (0);
}

int
mt7902_set_channel(struct mt7902_softc *sc, struct ieee80211_channel *c)
{
	struct mt7902_chan_info req;
	int error;

	if (sc == NULL || c == NULL || !sc->hw_started)
		return (ENETDOWN);
	error = mt7902_radio_start(sc, c);
	if (error != 0)
		return (error);
	mt7902_chan_info_init(sc, c, &req);
	mtx_lock(&sc->mtx);
	req.switch_reason = sc->scan_active ? 9 : 0;
	mtx_unlock(&sc->mtx);
	/* Match mt7921_mcu_set_chan_info(): wait for the firmware's channel
	 * switch acknowledgement before the software scanner advances.
	 */
	return (mt7902_mcu_send_cmd(sc, MT7902_MCU_EXT_CMD(0x08), &req,
	    sizeof(req), true));
}

/*
 * Read the RX MIB counters used by mt792x_mac_update_mib_stats(). This is a
 * single-PHY device: upstream reads MIB bank 0 for both 2.4 and 5 GHz.
 * SDR5 counts received MPDUs; SDR3[31:16] counts FCS errors. Sample before
 * each channel change to attribute the completed dwell to the prior channel.
 */
static void
mt7902_rx_mib_sample(struct mt7902_softc *sc, const char *stage,
    uint8_t channel, uint8_t band)
{
	uint32_t mpdu, fcs;

	/* MT7921-style single PHY: MIB bank 0 covers both RF bands. */
	mpdu = mt7902_read(sc, MT7902_REG_MIB_SDR5(0));
	fcs = mt7902_read(sc, MT7902_REG_MIB_SDR3(0));
	device_printf(sc->dev,
	    "RX MIB stage=%s channel=%u channel_band=%u mib_bank=0 mpdu=%u fcs_error=%u raw_sdr3=%08x raw_sdr5=%08x\n",
	    stage, channel, band, mpdu,
	    (fcs & MT7902_MIB_SDR3_FCS_ERR_MASK) >>
	    MT7902_MIB_SDR3_FCS_ERR_SHIFT, fcs, mpdu);
}

static void
mt7902_set_channel_cb(struct ieee80211com *ic)
{
	struct mt7902_softc *sc;
	struct ieee80211_channel *c;
	uint8_t previous_channel, previous_band;
	bool scanning;
	int error, debug;

	sc = ic->ic_softc;
	c = ic->ic_curchan;
	if (c == NULL)
		return;
	mtx_lock(&sc->mtx);
	scanning = sc->scan_active;
	previous_channel = sc->current_channel;
	previous_band = sc->current_band;
	mtx_unlock(&sc->mtx);
	if (scanning && previous_channel != 0)
		mt7902_rx_mib_sample(sc, "dwell-end", previous_channel,
		    previous_band);
	error = mt7902_set_channel(sc, c);
	mtx_lock(&sc->mtx);
	sc->chan_switch_count++;
	sc->chan_last_request = c->ic_ieee;
	sc->chan_last_band = IEEE80211_IS_CHAN_2GHZ(c) ? 2 : 5;
	sc->chan_last_flags = c->ic_flags;
	sc->chan_last_result = error;
	sc->chan_last_scan = sc->scan_active;
	if (error == 0) {
		sc->current_channel = c->ic_ieee;
		sc->current_band = IEEE80211_IS_CHAN_2GHZ(c) ? 2 : 5;
	} else
		sc->chan_switch_failures++;
	debug = sc->rx_debug;
	scanning = sc->scan_active;
	mtx_unlock(&sc->mtx);
	if (debug > 0 || scanning)
		device_printf(sc->dev,
		    "CHANNEL switch %s ch=%u freq=%u band=%s flags=%08x passive=%u scan=%u result=%d\n",
		    error == 0 ? "complete" : "FAILED", c->ic_ieee,
		    c->ic_freq, IEEE80211_IS_CHAN_2GHZ(c) ? "2g" : "5g",
		    c->ic_flags, (c->ic_flags & IEEE80211_CHAN_PASSIVE) != 0,
		    scanning, error);
}

static void
mt7902_scan_start(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	bool active;
	int i, passive, active_channels, debug, filter_error;
	uint32_t scan_flags, filter_flags;

	active = ic->ic_scan != NULL &&
	    (ic->ic_scan->ss_flags & IEEE80211_SCAN_ACTIVE) != 0;
	scan_flags = ic->ic_scan != NULL ? ic->ic_scan->ss_flags : 0;
	passive = active_channels = 0;
	for (i = 0; i < ic->ic_nchans; i++) {
		if (ic->ic_channels[i].ic_flags & IEEE80211_CHAN_PASSIVE)
			passive++;
		else
			active_channels++;
	}
	mtx_lock(&sc->mtx);
	sc->scan_active = true;
	sc->rx_data_count = 0;
	sc->rx_data_desc_count = 0;
	sc->rx_data_fallback_poll_count = 0;
	sc->rx_drop_count = 0;
	sc->rx_mgmt_count = 0;
	sc->rx_beacon_count = 0;
	sc->rx_monitor_error_count = 0;
	sc->rx_event_count = 0;
	sc->rx_event_txs_count = 0;
	sc->rx_event_txs_records = 0;
	sc->rx_event_txs_failures = 0;
	sc->rx_event_txs_ack_timeouts = 0;
	sc->rx_event_txs_rts_timeouts = 0;
	sc->rx_event_txs_queue_timeouts = 0;
	sc->rx_event_txs_retries = 0;
	sc->rx_event_txs_bytes = 0;
	sc->rx_event_txs_last_wcid = 0;
	sc->rx_event_txs_last_pid = 0;
	sc->rx_event_txs_last_status = 0;
	sc->rx_event_txs_last_rate = 0;
	sc->rx_event_txs_last_fail_count = 0;
	sc->rx_event_txs_last_retry_count = 0;
	sc->rx_event_tx_free_count = 0;
	sc->tx_free_records = 0;
	sc->tx_free_failures = 0;
	sc->tx_free_retries = 0;
	sc->tx_free_unknown = 0;
	sc->tx_free_pairs = 0;
	sc->tx_free_bad = 0;
	sc->tx_raw_count = 0;
	sc->tx_raw_reject_count = 0;
	sc->tx_raw_enqueue_errors = 0;
	sc->tx_submit_count = 0;
	sc->tx_dma_done_count = 0;
	sc->tx_dma_timeouts = 0;
	sc->rx_event_short_count = 0;
	sc->rx_event_unknown_count = 0;
	sc->rx_last_len = 0;
	sc->rx_event_last_rxd0 = 0;
	sc->rx_event_last_len = 0;
	sc->rx_event_last_type = 0;
	sc->irq_count = 0;
	sc->irq_last_status = 0;
	bzero(sc->rx_last_rxd, sizeof(sc->rx_last_rxd));
	sc->rx_last_rxd4 = 0;
	sc->rx_last_fc = 0;
	sc->rx_last_frame_len = 0;
	sc->rx_last_hdr_len = 0;
	sc->rx_last_offset = 0;
	sc->rx_last_pkt_type = 0;
	sc->rx_last_addr_type = 0;
	sc->rx_last_payload_format = 0;
	sc->rx_last_channel = 0;
	bzero(sc->rx_drop_reason, sizeof(sc->rx_drop_reason));
	bzero(sc->rx_frame_type, sizeof(sc->rx_frame_type));
	bzero(sc->rx_mgmt_subtype, sizeof(sc->rx_mgmt_subtype));
	bzero(sc->rx_channel_frames, sizeof(sc->rx_channel_frames));
	bzero(sc->rx_event_type_count, sizeof(sc->rx_event_type_count));
	bzero(sc->irq_bit_count, sizeof(sc->irq_bit_count));
	bzero(sc->rx_last_drop_rxd, sizeof(sc->rx_last_drop_rxd));
	sc->rx_last_drop_len = 0;
	sc->rx_last_drop_reason = 0;
	sc->rx_input_count = 0;
	sc->chan_switch_count = 0;
	sc->chan_switch_failures = 0;
	sc->chan_last_request = 0;
	sc->chan_last_band = 0;
	sc->chan_last_flags = 0;
	sc->chan_last_result = 0;
	sc->chan_last_scan = false;
	debug = sc->rx_debug;
	/*
	 * Reassert the complete Connac2 receive policy at each scan boundary.
	 * Like rtw88's configure-filter path, keep the desired policy explicit
	 * rather than assuming the firmware retained an earlier interface-time
	 * setting.  MT76's SET_RX_FILTER command is asynchronous, so this only
	 * confirms host submission; RX counters remain the hardware-side check.
	 */
	filter_flags = MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS;
	if (sc->monitor_active)
		filter_flags |= MT7902_RX_FILTER_CONTROL |
		    MT7902_RX_FILTER_FCSFAIL;
	mtx_unlock(&sc->mtx);
	if (sc->hw_started) {
		filter_error = mt7902_mcu_set_rxfilter(sc, filter_flags);
		if (filter_error != 0)
			device_printf(sc->dev,
			    "scan-start RX filter refresh failed: %d\n",
			    filter_error);
	}
	/* Drain pre-scan hardware counters before the first channel dwell. */
	mt7902_rx_mib_sample(sc, "scan-baseline", 0, 0);
	device_printf(sc->dev,
	    "net80211 %s scan started: flags=%08x channels=%d active-capable=%d passive-marked=%d rx_filter=%08x rx_debug=%d\n",
	    active ? "active" : "passive", scan_flags, ic->ic_nchans,
	    active_channels, passive, filter_flags, debug);
}

static void
mt7902_scan_end(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	uint64_t rx, rx_desc, rx_fallback, dropped, mgmt, beacon;
	uint64_t monitor_errors;
	uint64_t events, txs, txs_records, tx_free, short_events, unknown;
	uint64_t txs_failures, txs_ack_timeouts, txs_rts_timeouts;
	uint64_t txs_queue_timeouts, txs_retries, txs_bytes;
	uint64_t tx_free_records, tx_free_failures, tx_free_retries;
	uint64_t tx_free_unknown, tx_free_pairs, tx_free_bad, tx_dma_timeouts;
	uint64_t tx_pending_count, irq_count;
	uint64_t tx_raw, tx_raw_reject, tx_raw_enqueue_errors;
	uint64_t tx_submit, tx_dma_done;
	uint32_t last_len, rxd0, rxd1, rxd2, rxd3, last_event_rxd0, last_irq;
	uint32_t last_rxd4;
	uint16_t last_fc, last_frame_len, last_hdr_len, last_offset;
	uint8_t last_pkt_type, last_addr_type, last_payload_format, last_channel;
	uint32_t data_hw_idx, event_hw_idx, data_tail_ctl, event_tail_ctl;
	uint16_t last_event_len, txs_last_wcid, txs_last_rate;
	uint16_t txs_last_fail_count, txs_last_retry_count;
	uint8_t last_event_type, txs_last_pid, txs_last_status;
	uint8_t mib_channel, mib_band;
	uint64_t drop_reason[MT7902_RX_DROP_REASON_COUNT];
	uint64_t frame_type[4], mgmt_subtype[16], channel_frames[256];
	uint64_t event_type_count[16], irq_bits[32];
	uint64_t input_count, chan_switch_count, chan_switch_failures;
	uint64_t rx_filter_count, rx_filter_bitmap_count;
	uint32_t rx_filter_flags, rx_filter_bitmap, chan_last_flags;
	uint8_t rx_filter_bit_op, chan_last_request, chan_last_band;
	int rx_filter_error, rx_filter_bitmap_error, rx_debug, chan_last_result;
	bool chan_last_scan;
	int rx_head, rx_tail, rx_queued, i;

	mtx_lock(&sc->mtx);
	sc->scan_active = false;
	rx = sc->rx_data_count;
	rx_desc = sc->rx_data_desc_count;
	rx_fallback = sc->rx_data_fallback_poll_count;
	dropped = sc->rx_drop_count;
	mgmt = sc->rx_mgmt_count;
	beacon = sc->rx_beacon_count;
	monitor_errors = sc->rx_monitor_error_count;
	events = sc->rx_event_count;
	txs = sc->rx_event_txs_count;
	txs_records = sc->rx_event_txs_records;
	txs_failures = sc->rx_event_txs_failures;
	txs_ack_timeouts = sc->rx_event_txs_ack_timeouts;
	txs_rts_timeouts = sc->rx_event_txs_rts_timeouts;
	txs_queue_timeouts = sc->rx_event_txs_queue_timeouts;
	txs_retries = sc->rx_event_txs_retries;
	txs_bytes = sc->rx_event_txs_bytes;
	txs_last_wcid = sc->rx_event_txs_last_wcid;
	txs_last_pid = sc->rx_event_txs_last_pid;
	txs_last_status = sc->rx_event_txs_last_status;
	txs_last_rate = sc->rx_event_txs_last_rate;
	txs_last_fail_count = sc->rx_event_txs_last_fail_count;
	txs_last_retry_count = sc->rx_event_txs_last_retry_count;
	tx_free = sc->rx_event_tx_free_count;
	tx_free_records = sc->tx_free_records;
	tx_free_failures = sc->tx_free_failures;
	tx_free_retries = sc->tx_free_retries;
	tx_free_unknown = sc->tx_free_unknown;
	tx_free_pairs = sc->tx_free_pairs;
	tx_free_bad = sc->tx_free_bad;
	tx_dma_timeouts = sc->tx_dma_timeouts;
	tx_pending_count = sc->tx_pending_count;
	tx_raw = sc->tx_raw_count;
	tx_raw_reject = sc->tx_raw_reject_count;
	tx_raw_enqueue_errors = sc->tx_raw_enqueue_errors;
	tx_submit = sc->tx_submit_count;
	tx_dma_done = sc->tx_dma_done_count;
	short_events = sc->rx_event_short_count;
	unknown = sc->rx_event_unknown_count;
	irq_count = sc->irq_count;
	last_irq = sc->irq_last_status;
	last_len = sc->rx_last_len;
	rxd0 = sc->rx_last_rxd[0];
	rxd1 = sc->rx_last_rxd[1];
	rxd2 = sc->rx_last_rxd[2];
	rxd3 = sc->rx_last_rxd[3];
	last_rxd4 = sc->rx_last_rxd4;
	last_fc = sc->rx_last_fc;
	last_frame_len = sc->rx_last_frame_len;
	last_hdr_len = sc->rx_last_hdr_len;
	last_offset = sc->rx_last_offset;
	last_pkt_type = sc->rx_last_pkt_type;
	last_addr_type = sc->rx_last_addr_type;
	last_payload_format = sc->rx_last_payload_format;
	last_channel = sc->rx_last_channel;
	last_event_rxd0 = sc->rx_event_last_rxd0;
	last_event_len = sc->rx_event_last_len;
	last_event_type = sc->rx_event_last_type;
	rx_head = sc->rx_data.head;
	rx_tail = sc->rx_data.tail;
	rx_queued = sc->rx_data.queued;
	memcpy(drop_reason, sc->rx_drop_reason, sizeof(drop_reason));
	memcpy(frame_type, sc->rx_frame_type, sizeof(frame_type));
	memcpy(mgmt_subtype, sc->rx_mgmt_subtype, sizeof(mgmt_subtype));
	memcpy(channel_frames, sc->rx_channel_frames, sizeof(channel_frames));
	memcpy(event_type_count, sc->rx_event_type_count,
	    sizeof(event_type_count));
	memcpy(irq_bits, sc->irq_bit_count, sizeof(irq_bits));
	input_count = sc->rx_input_count;
	chan_switch_count = sc->chan_switch_count;
	chan_switch_failures = sc->chan_switch_failures;
	rx_filter_count = sc->rx_filter_count;
	rx_filter_flags = sc->rx_filter_flags;
	rx_filter_error = sc->rx_filter_error;
	rx_filter_bitmap_count = sc->rx_filter_bitmap_count;
	rx_filter_bitmap = sc->rx_filter_bitmap;
	rx_filter_bit_op = sc->rx_filter_bit_op;
	rx_filter_bitmap_error = sc->rx_filter_bitmap_error;
	rx_debug = sc->rx_debug;
	chan_last_request = sc->chan_last_request;
	chan_last_band = sc->chan_last_band;
	chan_last_flags = sc->chan_last_flags;
	chan_last_result = sc->chan_last_result;
	chan_last_scan = sc->chan_last_scan;
	mib_channel = sc->current_channel;
	mib_band = sc->current_band;
	mtx_unlock(&sc->mtx);
	mt7902_rx_mib_sample(sc, "scan-end", mib_channel, mib_band);
	data_hw_idx = mt7902_read(sc, sc->rx_data.reg_base + 0x0c);
	event_hw_idx = mt7902_read(sc, sc->rx_event.reg_base + 0x0c);
	data_tail_ctl = sc->rx_data.desc != NULL ?
	    le32toh(sc->rx_data.desc[rx_tail].control) : 0;
	event_tail_ctl = sc->rx_event.desc != NULL ?
	    le32toh(sc->rx_event.desc[sc->rx_event.tail].control) : 0;
	device_printf(sc->dev,
	    "net80211 scan ended: rx=%ju data_desc=%ju fallback_polls=%ju dropped=%ju mgmt=%ju beacon/probe=%ju monitor_errors=%ju events=%ju txs=%ju/%ju tx_free=%ju records=%ju fail=%ju retries=%ju pending=%ju unknown=%ju pairs=%ju bad=%ju dma_timeout=%ju short=%ju unknown_rx=%ju irq=%ju last_irq=%08x ring2=%d/%d/%d hwidx=%x/%x tailctl=%08x/%08x last_evt=%u/%u/%08x last_rx=%u:%08x/%08x/%08x/%08x txs_detail=%ju/%ju/%ju/%ju/%ju bytes=%ju last_txs=%u/%u/%u/%u/%u/%u last_frame=%u:%u/%u/%u fc=%04x hdr=%u rxd4=%08x addr=%u payload=%u tx_path=%ju/%ju/%ju/%ju/%ju\n",
	    (uintmax_t)rx, (uintmax_t)rx_desc, (uintmax_t)rx_fallback,
	    (uintmax_t)dropped, (uintmax_t)mgmt, (uintmax_t)beacon,
	    (uintmax_t)monitor_errors, (uintmax_t)events,
	    (uintmax_t)txs, (uintmax_t)txs_records, (uintmax_t)tx_free,
	    (uintmax_t)tx_free_records, (uintmax_t)tx_free_failures,
	    (uintmax_t)tx_free_retries, (uintmax_t)tx_pending_count,
	    (uintmax_t)tx_free_unknown, (uintmax_t)tx_free_pairs,
	    (uintmax_t)tx_free_bad, (uintmax_t)tx_dma_timeouts,
	    (uintmax_t)short_events, (uintmax_t)unknown,
	    (uintmax_t)irq_count, last_irq, rx_head, rx_tail, rx_queued,
	    data_hw_idx, event_hw_idx, data_tail_ctl, event_tail_ctl,
	    last_event_type, last_event_len, last_event_rxd0, last_len,
	    rxd0, rxd1, rxd2, rxd3,
	    (uintmax_t)txs_failures, (uintmax_t)txs_ack_timeouts,
	    (uintmax_t)txs_rts_timeouts, (uintmax_t)txs_queue_timeouts,
	    (uintmax_t)txs_retries, (uintmax_t)txs_bytes,
	    txs_last_wcid, txs_last_pid,
	    txs_last_status, txs_last_rate, txs_last_fail_count,
	    txs_last_retry_count, last_pkt_type, last_channel, last_offset,
	    last_frame_len, last_fc, last_hdr_len, last_rxd4, last_addr_type,
	    last_payload_format, (uintmax_t)tx_raw, (uintmax_t)tx_raw_reject,
	    (uintmax_t)tx_raw_enqueue_errors, (uintmax_t)tx_submit,
	    (uintmax_t)tx_dma_done);
	if (rx_debug > 0) {
		device_printf(sc->dev,
		    "RX DIAG: descriptors=%ju parsed=%ju handed_to_net80211=%ju dropped=%ju monitor_only=%ju\n",
		    (uintmax_t)rx_desc, (uintmax_t)rx, (uintmax_t)input_count,
		    (uintmax_t)dropped, (uintmax_t)monitor_errors);
		device_printf(sc->dev,
		    "RX DROP REASONS: short_rxd=%ju bad_channel=%ju bad_type=%ju hdr_trans=%ju icv=%ju fcs=%ju mic=%ju rxd_error=%ju payload=%ju group_truncated=%ju bad_offset=%ju short_frame=%ju bad_header=%ju nomem=%ju rx_params=%ju no_net80211=%ju invalid_dma=%ju\n",
		    (uintmax_t)drop_reason[MT7902_RX_DROP_SHORT],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_BAD_CHANNEL],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_BAD_PACKET_TYPE],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_HDR_TRANS],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_ICV],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_FCS],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_MIC],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_RXD_ERROR],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_PAYLOAD_FORMAT],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_GROUP_TRUNCATED],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_BAD_OFFSET],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_SHORT_FRAME],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_BAD_HEADER],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_NOMEM],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_RX_PARAMS],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_NOT_ATTACHED],
		    (uintmax_t)drop_reason[MT7902_RX_DROP_INVALID_DMA]);
		device_printf(sc->dev,
		    "RX FRAME TYPES: management=%ju control=%ju data=%ju extension=%ju mgmt_seen=%ju\n",
		    (uintmax_t)frame_type[0], (uintmax_t)frame_type[1],
		    (uintmax_t)frame_type[2], (uintmax_t)frame_type[3],
		    (uintmax_t)mgmt);
		device_printf(sc->dev,
		    "CHANNEL/FILTER: switches=%ju failed=%ju last_request=%u band=%u flags=%08x scan=%u result=%d filter_commands=%ju filter=%08x filter_error=%d bitmap_updates=%ju bitmap=%08x bit_op=%u bitmap_error=%d\n",
		    (uintmax_t)chan_switch_count,
		    (uintmax_t)chan_switch_failures, chan_last_request,
		    chan_last_band, chan_last_flags, chan_last_scan,
		    chan_last_result, (uintmax_t)rx_filter_count,
		    rx_filter_flags, rx_filter_error,
		    (uintmax_t)rx_filter_bitmap_count, rx_filter_bitmap,
		    rx_filter_bit_op, rx_filter_bitmap_error);
		for (i = 0; i < 16; i++) {
			if (mgmt_subtype[i] != 0)
				device_printf(sc->dev,
				    "RX MGMT SUBTYPE %u count=%ju\n",
				    i, (uintmax_t)mgmt_subtype[i]);
		}
		for (i = 0; i < 256; i++) {
			if (channel_frames[i] != 0)
				device_printf(sc->dev,
				    "RX CHANNEL ch=%d frames=%ju\n",
				    i, (uintmax_t)channel_frames[i]);
		}
		for (i = 0; i < 16; i++) {
			if (event_type_count[i] != 0)
				device_printf(sc->dev,
				    "RX RING0 TYPE %d count=%ju\n",
				    i, (uintmax_t)event_type_count[i]);
		}
		for (i = 0; i < 32; i++) {
			if (irq_bits[i] != 0)
				device_printf(sc->dev,
				    "IRQ BIT %d count=%ju\n",
				    i, (uintmax_t)irq_bits[i]);
		}
	}
}

static int
mt7902_link_update(struct ieee80211vap *vap, struct ieee80211_node *ni,
	bool linked)
{
	struct mt7902_softc *sc = vap->iv_ic->ic_softc;
	struct mt7902_vap *mvp = (struct mt7902_vap *)vap;
	struct ieee80211_node *old_ni;
	struct mt7902_bss_update bss;
	struct mt7902_sta_req_hdr hdr;
	struct mt7902_sta_rec_basic basic;
	struct mt7902_sta_rec_state state;
	struct mt7902_sta_rec_phy phy;
	struct mt7902_sta_rec_ra ra;
	struct mt7902_sta_rec_ht ht;
	struct mt7902_sta_rec_vht vht;
	struct mt7902_mcu_tlv_hdr wtbl_tlv;
	struct mt7902_wtbl_req_hdr wtbl_req;
	struct mt7902_wtbl_generic wtbl_generic;
	struct mt7902_wtbl_rx wtbl_rx;
	struct mt7902_wtbl_ht wtbl_ht;
	struct mt7902_wtbl_vht wtbl_vht;
	struct mt7902_wtbl_hdr_trans wtbl_hdr_trans;
	static const uint8_t legacy_rates[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	static const uint8_t legacy_hw_rates[] = {
		10, 20, 55, 110, 11, 15, 10, 14, 9, 13, 8, 12
	};
	uint8_t peer[IEEE80211_ADDR_LEN], rate, phy_type, peer_channel;
	uint16_t aid, supp_rates, basic_rates, legacy, beacon_interval;
	uint8_t dtim_period;
	bool band2, rate_basic, qos, peer_ht, peer_vht;
	struct ieee80211_channel *chan;
	int i, j, error;
	uint16_t vht_rx_map, vht_tx_map;
	uint8_t peer_rx_nss, mcs;

	peer_channel = 0;
	if (ni != NULL) {
		IEEE80211_ADDR_COPY(peer, ni->ni_bssid);
		qos = (ni->ni_flags & IEEE80211_NODE_QOS) != 0;
		beacon_interval = ni->ni_intval;
		dtim_period = ni->ni_dtim_period;
		if (ni->ni_chan != NULL)
			peer_channel = ni->ni_chan->ic_ieee;
	} else if (linked) {
		return (EINVAL);
	} else if (mvp->linked) {
		IEEE80211_ADDR_COPY(peer, mvp->peer_bssid);
		qos = mvp->peer_qos;
		beacon_interval = mvp->peer_beacon_interval;
		dtim_period = mvp->peer_dtim_period;
		peer_channel = mvp->peer_channel;
	} else {
		IEEE80211_ADDR_COPY(peer, vap->iv_myaddr);
		qos = false;
		beacon_interval = 100;
		dtim_period = 1;
	}

	/* A disconnect may arrive without a live ni; use the saved/current channel. */
	if (peer_channel == 0) {
		chan = vap->iv_ic->ic_bsschan;
		if (chan == NULL || chan->ic_ieee == 0)
			chan = vap->iv_ic->ic_curchan;
		if (chan != NULL)
			peer_channel = chan->ic_ieee;
	}
	if (peer_channel == 0) {
		mtx_lock(&sc->mtx);
		peer_channel = sc->current_channel;
		band2 = sc->current_band == 2;
		mtx_unlock(&sc->mtx);
	} else {
		band2 = peer_channel <= 14;
	}
	peer_ht = (ni->ni_flags & IEEE80211_NODE_HT) != 0;
	peer_vht = !band2 && (ni->ni_flags & IEEE80211_NODE_VHT) != 0;
	/* An 802.11ac peer is also an HT peer. */
	peer_ht = peer_ht || peer_vht;
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
	bss.basic.conn_state = 0; /* BSS stays DISCONNECT; SET_BSS_CONNECTED carries link state. */
	bss.basic.wmm_idx = 0;
	IEEE80211_ADDR_COPY(bss.basic.bssid, peer);
	phy_type = band2 ?
	    (MT7902_PHY_TYPE_HR_DSSS | MT7902_PHY_TYPE_ERP) :
	    MT7902_PHY_TYPE_OFDM;
	bss.basic.phymode = band2 ?
	    (MT7902_BSS_PHY_B | MT7902_BSS_PHY_G) : MT7902_BSS_PHY_A;
	bss.basic.nonht_basic_phy = htole16(phy_type);
	bss.basic.sta_idx = htole16(MT7902_PEER_WCID);
	bss.basic.bmc_tx_wlan_idx = htole16(MT7902_BSS_WCID);
	bss.basic.bcn_interval = htole16(beacon_interval);
	bss.basic.dtim_period = dtim_period;
	bss.basic.phymode = 0x07 | (peer_ht ? MT7902_PHY_TYPE_HT : 0) |
	    (peer_vht ? MT7902_PHY_TYPE_VHT : 0);
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
		old_ni = sc->station_node;
		sc->station_node = NULL;
		sc->station_linked = 0;
		sc->station_aid = 0;
		sc->station_basic_rate = 0;
		mtx_unlock(&sc->mtx);
		if (old_ni != NULL)
			ieee80211_free_node(old_ni);
		error = mt7902_sta_disconnect(sc, peer, aid, qos);
		bzero(mvp->peer_bssid, sizeof(mvp->peer_bssid));
		mvp->peer_aid = 0;
		mvp->peer_beacon_interval = 0;
		mvp->peer_dtim_period = 0;
		mvp->peer_qos = false;
		mvp->peer_channel = 0;
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
	/* Build the Connac2 legacy CCK/OFDM mask from the negotiated rates. */
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

	if (peer_ht)
		phy_type |= MT7902_PHY_TYPE_HT;
	if (peer_vht)
		phy_type |= MT7902_PHY_TYPE_VHT;

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
	phy.phy_type = phy_type;
	if (peer_ht)
		phy.ampdu = ni->ni_htparam;

	bzero(&ra, sizeof(ra));
	ra.tag = htole16(MT7902_STA_REC_RA);
	ra.len = htole16(sizeof(ra));
	ra.legacy = htole16(legacy);
	if (peer_ht) {
		for (i = 0; i < ni->ni_htrates.rs_nrates; i++) {
			mcs = ni->ni_htrates.rs_rates[i] & 0x7f;
			if (mcs < sizeof(ra.rx_mcs_bitmask) * NBBY)
				ra.rx_mcs_bitmask[mcs / NBBY] |=
				    (uint8_t)(1U << (mcs % NBBY));
		}
	}

	bzero(&ht, sizeof(ht));
	if (peer_ht) {
		ht.tag = htole16(MT7902_STA_REC_HT);
		ht.len = htole16(sizeof(ht));
		ht.ht_cap = htole16(ni->ni_htcap);
	}
	bzero(&vht, sizeof(vht));
	vht_rx_map = vht_tx_map = 0xffff;
	peer_rx_nss = 1;
	if (peer_vht) {
		vht.tag = htole16(MT7902_STA_REC_VHT);
		vht.len = htole16(sizeof(vht));
		vht.vht_cap = htole32(ni->ni_vhtcap);
		vht_rx_map = ni->ni_vht_mcsinfo.rx_mcs_map;
		vht_tx_map = ni->ni_vht_mcsinfo.tx_mcs_map;
		vht.vht_rx_mcs_map = htole16(vht_rx_map);
		vht.vht_tx_mcs_map = htole16(vht_tx_map);
		peer_rx_nss = 0;
		/* The peer's RX map describes how many streams it can receive. */
		for (i = 0; i < MT7902_VHT_MAX_NSS; i++) {
			if (((vht_rx_map >> (2 * i)) & 0x3) ==
			    MT7902_VHT_MCS_NONE)
				break;
			peer_rx_nss++;
		}
		if (peer_rx_nss == 0)
			peer_rx_nss = 1;
	}

	bzero(&state, sizeof(state));
	state.tag = htole16(MT7902_STA_REC_STATE);
	state.len = htole16(sizeof(state));
	state.state = MT7902_STA_STATE_ASSOC;
	if (peer_vht) {
		uint8_t vht_width;

		/* VHT operating mode encodes 20/40 as 0 and 80 MHz as 1. */
		vht_width = ni->ni_chw == NET80211_STA_RX_BW_80 ?
		    MT7902_VHT_OPMODE_BW_80 : MT7902_VHT_OPMODE_BW_20_40;
		state.vht_opmode = vht_width |
		    ((uint8_t)(MIN(peer_rx_nss, MT7902_VHT_MAX_NSS) - 1) <<
		    MT7902_VHT_OPMODE_RX_NSS_SHIFT);
	}

	/*
	 * Connac2 requires a WTBL record alongside STA_REC. Include negotiated
	 * HT/VHT entries only for peers that support those PHY modes. Receive
	 * header translation remains disabled for FreeBSD's native 802.11 path.
	 */
	bzero(&wtbl_tlv, sizeof(wtbl_tlv));
	bzero(&wtbl_req, sizeof(wtbl_req));
	bzero(&wtbl_generic, sizeof(wtbl_generic));
	bzero(&wtbl_rx, sizeof(wtbl_rx));
	bzero(&wtbl_ht, sizeof(wtbl_ht));
	bzero(&wtbl_vht, sizeof(wtbl_vht));
	bzero(&wtbl_hdr_trans, sizeof(wtbl_hdr_trans));
	wtbl_tlv.tag = htole16(13); /* STA_REC_WTBL */
	wtbl_req.wlan_idx_lo = MT7902_PEER_WCID;
	wtbl_req.operation = 1; /* WTBL_RESET_AND_SET */
	wtbl_req.tlv_num = htole16(3 + (peer_ht ? 1 : 0) +
	    (peer_vht ? 1 : 0));
	wtbl_generic.tag = htole16(0); /* WTBL_GENERIC */
	wtbl_generic.len = htole16(sizeof(wtbl_generic));
	IEEE80211_ADDR_COPY(wtbl_generic.peer_addr, peer);
	wtbl_generic.muar_idx = 0;
	wtbl_generic.qos = qos;
	wtbl_generic.partial_aid = htole16(aid);
	wtbl_rx.tag = htole16(1); /* WTBL_RX */
	wtbl_rx.len = htole16(sizeof(wtbl_rx));
	wtbl_rx.rca1 = 1; /* station interface */
	wtbl_rx.rca2 = 1;
	wtbl_rx.rv = 1;
	if (peer_ht) {
		wtbl_ht.tag = htole16(2); /* WTBL_HT */
		wtbl_ht.len = htole16(sizeof(wtbl_ht));
		wtbl_ht.ht = 1;
		wtbl_ht.ldpc = (ni->ni_htcap & IEEE80211_HTCAP_LDPC) != 0;
		wtbl_ht.ampdu_factor = (ni->ni_htparam &
		    IEEE80211_HTCAP_MAXRXAMPDU) >> IEEE80211_HTCAP_MAXRXAMPDU_S;
		wtbl_ht.ampdu_density = (ni->ni_htparam &
		    IEEE80211_HTCAP_MPDUDENSITY) >>
		    IEEE80211_HTCAP_MPDUDENSITY_S;
	}
	if (peer_vht) {
		wtbl_vht.tag = htole16(3); /* WTBL_VHT */
		wtbl_vht.len = htole16(sizeof(wtbl_vht));
		wtbl_vht.ldpc = (ni->ni_vhtcap & IEEE80211_VHTCAP_RXLDPC) != 0;
		wtbl_vht.vht = 1;
		wtbl_ht.ampdu_factor = MAX(wtbl_ht.ampdu_factor,
		    (ni->ni_vhtcap & IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK) >>
		    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK_S);
	}
	wtbl_hdr_trans.tag = htole16(6); /* WTBL_HDR_TRANS */
	wtbl_hdr_trans.len = htole16(sizeof(wtbl_hdr_trans));
	wtbl_hdr_trans.to_ds = 1;
	wtbl_hdr_trans.from_ds = 0;
	wtbl_hdr_trans.no_rx_trans = 1;
	wtbl_tlv.len = htole16(sizeof(wtbl_tlv) + sizeof(wtbl_req) +
	    sizeof(wtbl_generic) + sizeof(wtbl_rx) +
	    (peer_ht ? sizeof(wtbl_ht) : 0) +
	    (peer_vht ? sizeof(wtbl_vht) : 0) + sizeof(wtbl_hdr_trans));

	{
		uint8_t request[sizeof(hdr) + sizeof(basic) + sizeof(phy) +
		    sizeof(ra) + sizeof(state) + sizeof(ht) + sizeof(vht) +
		    sizeof(wtbl_tlv) + sizeof(wtbl_req) + sizeof(wtbl_generic) +
		    sizeof(wtbl_rx) + sizeof(wtbl_ht) + sizeof(wtbl_vht) +
		    sizeof(wtbl_hdr_trans)];
		size_t offset;

		hdr.tlv_num = htole16(5 + (peer_ht ? 1 : 0) +
		    (peer_vht ? 1 : 0));
		offset = 0;
		memcpy(request + offset, &hdr, sizeof(hdr));
		offset += sizeof(hdr);
		memcpy(request + offset, &basic, sizeof(basic));
		offset += sizeof(basic);
		if (peer_ht) {
			memcpy(request + offset, &ht, sizeof(ht));
			offset += sizeof(ht);
		}
		if (peer_vht) {
			memcpy(request + offset, &vht, sizeof(vht));
			offset += sizeof(vht);
		}
		memcpy(request + offset, &phy, sizeof(phy));
		offset += sizeof(phy);
		memcpy(request + offset, &ra, sizeof(ra));
		offset += sizeof(ra);
		memcpy(request + offset, &state, sizeof(state));
		offset += sizeof(state);
		memcpy(request + offset, &wtbl_tlv, sizeof(wtbl_tlv));
		offset += sizeof(wtbl_tlv);
		memcpy(request + offset, &wtbl_req, sizeof(wtbl_req));
		offset += sizeof(wtbl_req);
		memcpy(request + offset, &wtbl_generic, sizeof(wtbl_generic));
		offset += sizeof(wtbl_generic);
		memcpy(request + offset, &wtbl_rx, sizeof(wtbl_rx));
		offset += sizeof(wtbl_rx);
		if (peer_ht) {
			memcpy(request + offset, &wtbl_ht, sizeof(wtbl_ht));
			offset += sizeof(wtbl_ht);
		}
		if (peer_vht) {
			memcpy(request + offset, &wtbl_vht, sizeof(wtbl_vht));
			offset += sizeof(wtbl_vht);
		}
		memcpy(request + offset, &wtbl_hdr_trans, sizeof(wtbl_hdr_trans));
		offset += sizeof(wtbl_hdr_trans);
		error = mt7902_mcu_send_response(sc,
		    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
		    request, offset, NULL, &(size_t){0});
	}
	if (error == 0) {
		mvp->linked = true;
		IEEE80211_ADDR_COPY(mvp->peer_bssid, peer);
		mvp->peer_aid = aid;
		mvp->peer_beacon_interval = beacon_interval;
		mvp->peer_dtim_period = dtim_period;
		mvp->peer_qos = qos;
		mvp->peer_channel = peer_channel;
		mtx_lock(&sc->mtx);
		old_ni = sc->station_node;
		sc->station_node = ieee80211_ref_node(ni);
		sc->station_linked = 1;
		sc->station_aid = aid;
		mtx_unlock(&sc->mtx);
		if (old_ni != NULL)
			ieee80211_free_node(old_ni);
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
	struct mt7902_softc *sc = ic->ic_softc;
	struct mt7902_vap *mvp;
	struct ieee80211vap *vap;
	int error;

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

	/*
	 * wlan_clone_create() invokes ic_vap_create without ic_comlock held.
	 * ieee80211_vap_attach() takes and releases that lock internally, so
	 * do not unlock/relock it here.  This callback runs in the ifconfig
	 * ioctl context and may sleep while waiting for firmware responses.
	 */
	if (sc->hw_started) {
		error = mt7902_vap_fw_update(sc, vap, true);
		if (error != 0) {
			vap->iv_newstate = mvp->newstate;
			ieee80211_vap_detach(vap);
			free(mvp, M_80211_VAP);
			return (NULL);
		}
		mvp->fw_registered = true;
	} else {
		device_printf(sc->dev,
		    "VAP created before firmware start; firmware registration deferred\n");
	}

	if (opmode == IEEE80211_M_MONITOR) {
		mtx_lock(&sc->mtx);
		sc->monitor_active = true;
		mtx_unlock(&sc->mtx);
		if (sc->hw_started) {
			error = mt7902_mcu_set_rxfilter(sc,
			    MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS |
			    MT7902_RX_FILTER_CONTROL | MT7902_RX_FILTER_FCSFAIL);
			if (error != 0)
				device_printf(sc->dev,
				    "could not enable monitor receive filters: %d\n", error);
		}
	} else if (sc->hw_started) {
		/*
		 * hw_start programs the receive filter before a station VAP has
		 * been registered with firmware.  Refresh it after DEV_INFO_UPDATE
		 * and BSS_INFO_UPDATE so the active station interface scans with
		 * the same OTHER_BSS policy used by Linux mt7921_configure_filter().
		 * mt7902_mcu_set_rxfilter() also clears DROP_OTHER_BEACON (RFCR bit 11),
		 * which must not suppress beacons from neighbouring BSSs.
		 *
		 * VAP creation runs in the ifconfig ioctl context and may sleep while
		 * waiting for MCU/WFDMA submission, unlike the scan callback.
		 */
		error = mt7902_mcu_set_rxfilter(sc,
		    MT7902_RX_FILTER_ENABLE | MT7902_RX_FILTER_OTHER_BSS);
		if (error != 0)
			device_printf(sc->dev,
			    "could not refresh station scan receive filters: %d\n", error);
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
	if (mvp->fw_registered && sc->hw_started) {
		/*
		 * wlan_clone_destroy() invokes ic_vap_delete without ic_comlock
		 * held.  The firmware transaction is allowed to sleep here.
		 */
		error = mt7902_vap_fw_update(sc, vap, false);
		if (error == 0)
			mvp->fw_registered = false;
	}
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


static int
mt7902_radio_stop(struct mt7902_softc *sc)
{
	struct {
		uint8_t enable;
		uint8_t band;
		uint8_t reserved[2];
	} __packed req;
	bool started;
	int error;

	mtx_lock(&sc->mtx);
	started = sc->radio_enabled;
	mtx_unlock(&sc->mtx);
	if (!started)
		return (0);

	bzero(&req, sizeof(req));
	req.enable = 0;
	req.band = 0;
	error = mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_EXT_CMD(MT7902_MCU_EXT_MAC_INIT_CTRL),
	    &req, sizeof(req), true);
	mtx_lock(&sc->mtx);
	sc->radio_enabled = false;
	mtx_unlock(&sc->mtx);
	if (error != 0)
		device_printf(sc->dev, "radio stop: MAC_INIT_CTRL disable failed: %d\n",
		    error);
	else
		device_printf(sc->dev, "radio stopped: MAC disabled\n");
	return (error);
}

/*
 * net80211 calls ic_parent when the first VAP is brought up and when the
 * last VAP is stopped.  Firmware/WFDMA are loaded at PCI attach, but the
 * radio MAC must be started here, before ieee80211_start_all() advances the
 * station state machine into scanning.
 */
static void
mt7902_parent(struct ieee80211com *ic)
{
	struct mt7902_softc *sc = ic->ic_softc;
	struct ieee80211vap *vap;
	int error;

	if (ic->ic_nrunning == 0) {
		(void)mt7902_radio_stop(sc);
		return;
	}
	if (!sc->hw_started) {
		device_printf(sc->dev,
		    "cannot start radio: firmware is stopped; set dev.mt7902_native.%d.hw_start=1\n",
		    device_get_unit(sc->dev));
		error = ENETDOWN;
	} else if (ic->ic_curchan == NULL) {
		device_printf(sc->dev,
		    "cannot start radio: net80211 has no current channel\n");
		error = EINVAL;
	} else {
		error = mt7902_set_channel(sc, ic->ic_curchan);
	}
	if (error != 0) {
		device_printf(sc->dev, "radio startup failed: %d; stopping VAPs\n",
		    error);
		IEEE80211_LOCK(ic);
		TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next)
			ieee80211_stop_locked(vap);
		IEEE80211_UNLOCK(ic);
		return;
	}
	ieee80211_start_all(ic);
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
		taskqueue_enqueue(sc->tq, &sc->tx_task);
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
	/*
	 * FreeBSD's scan probe path sets ibp_power from ni_txpower, which
	 * defaults to IEEE80211_TXPOWER_MAX (100) even when no per-packet
	 * power override is requested.  MT7902 currently relies on firmware
	 * calibration/regulatory power control, so accept but do not override
	 * that value here.  Rejecting it suppresses every normal probe request.
	 */
	/* net80211's software scanner uses this ordinary legacy-rate profile. */
	if (params->ibp_vers != 0 || params->ibp_len != 0 ||
	    (params->ibp_flags & ~IEEE80211_BPF_NOACK) != 0 ||
	    params->ibp_pri > WME_AC_VO || params->ibp_try0 > 15 ||
	    params->ibp_ctsrate != 0 ||
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
	sc = ni->ni_ic->ic_softc;
	mtx_lock(&sc->mtx);
	sc->tx_raw_count++;
	mtx_unlock(&sc->mtx);
	if (!mt7902_raw_params_supported(params, m)) {
		mtx_lock(&sc->mtx);
		sc->tx_raw_reject_count++;
		mtx_unlock(&sc->mtx);
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
	{
		int error = mt7902_tx_enqueue(sc, m);

		if (error != 0) {
			mtx_lock(&sc->mtx);
			sc->tx_raw_enqueue_errors++;
			mtx_unlock(&sc->mtx);
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
mt7902_tx_one(struct mt7902_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	struct mt7902_txp *txp;
	struct ieee80211_frame wh;
	struct ieee80211_bpf_params tx_params;
	uint8_t tx_dump[64];
	uint32_t *txwi, fc, val, control, rate;
	size_t len, total, hdrlen, qos_offset;
	uint16_t tx_basic_rate, qos_control, seq_control, msdu_id;
	uint8_t current_band, current_channel, tid, frag_idx;
	int idx, retry, error, queue, i, debug;
	bool fixed_rate, station_linked, more_frag, has_tx_params, noack;
	bool scanning;
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
	/* Record the pre-encapsulation 802.11 frame for BPF/radiotap users. */
	if (ni != NULL && ieee80211_radiotap_active_vap(ni->ni_vap)) {
		sc->tx_tap.wt_flags = 0;
		sc->tx_tap.wt_pad = 0;
		sc->tx_tap.wt_chan_freq = htole16(ni->ni_chan->ic_freq);
		sc->tx_tap.wt_chan_flags = htole16(ni->ni_chan->ic_flags);
		ieee80211_radiotap_tx(ni->ni_vap, m);
	}
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
	msdu_id = (uint16_t)(queue * MT7902_TX_RING_SIZE + idx);
	if (sc->tx_pending[msdu_id].active) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	station_linked = sc->station_linked != 0;
	current_band = sc->current_band;
	current_channel = sc->current_channel;
	tx_basic_rate = sc->station_basic_rate;
	debug = sc->rx_debug;
	scanning = sc->scan_active;
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

	/*
	 * A successful TX_FREE only proves that firmware released the token;
	 * it does not prove that an over-the-air probe was transmitted.  Dump
	 * the complete probe prefix and the TXWI used for active scan requests.
	 */
	if ((debug > 0 || scanning) && (fc & 0x000c) == 0 &&
	    ((fc >> 4) & 0x0f) == 4) {
		bzero(tx_dump, sizeof(tx_dump));
		m_copydata(m, 0, MIN(len, sizeof(tx_dump)), tx_dump);
		device_printf(sc->dev,
		    "TX PROBE ch=%u band=%s len=%ju hdr=%ju queue=%d rate0=%u try0=%u flags=%04x noack=%u fixed=%u txrate=%08x addr1=%6D addr2=%6D addr3=%6D bytes=%64D TXWI=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
		    (unsigned int)current_channel,
		    current_band == 2 ? "2g" : "5g",
		    (uintmax_t)len, (uintmax_t)hdrlen, queue,
		    has_tx_params ? (unsigned int)tx_params.ibp_rate0 : 0U,
		    has_tx_params ? (unsigned int)tx_params.ibp_try0 : 0U,
		    has_tx_params ? (unsigned int)tx_params.ibp_flags : 0U,
		    (unsigned int)noack, (unsigned int)fixed_rate,
		    le32toh(txwi[6]), wh.i_addr1, ":", wh.i_addr2, ":",
		    wh.i_addr3, ":", tx_dump, " ",
		    le32toh(txwi[0]), le32toh(txwi[1]), le32toh(txwi[2]),
		    le32toh(txwi[3]), le32toh(txwi[4]), le32toh(txwi[5]),
		    le32toh(txwi[6]), le32toh(txwi[7]));
	}
	txp = (struct mt7902_txp *)((uint8_t *)buf->vaddr + MT7902_TXWI_LEN);
	txp->msdu_id[0] = htole16(msdu_id | (1U << 15));
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
	/*
	 * Publish the software token before ringing the WFDMA doorbell: a very
	 * short packet may generate TX_FREE before this worker returns.
	 */
	if (sc->tx_pending[msdu_id].active)
		panic("mt7902: duplicate TX token %u", msdu_id);
	sc->tx_pending[msdu_id].m = m;
	sc->tx_pending[msdu_id].ni = ni;
	sc->tx_pending[msdu_id].active = true;
	sc->tx_pending_count++;
	sc->tx_submit_count++;
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
		sc->tx_dma_done_count++;
		mtx_unlock(&sc->mtx);
	} else {
		/*
		 * DMA ownership is retained and the packet remains pending until
		 * firmware reports TX_FREE. Do not complete the mbuf on a host-side
		 * poll timeout; the copied descriptor may still be in flight.
		 */
		mtx_lock(&sc->mtx);
		sc->tx_dma_timeouts++;
		mtx_unlock(&sc->mtx);
		device_printf(sc->dev,
		    "TX queue %d descriptor %d DMA wait timed out; awaiting TX_FREE\n",
		    queue, idx);
	}
	return (0);
}

/*
 * Connac2 requires two ordered updates for a Block Ack session: the WTBL
 * BA entry first, then STA_REC_BA. These are submitted without waiting for
 * an MCU response because ADDBA/DELBA callbacks may execute on the driver's
 * only event/RX worker; a synchronous wait there would deadlock RX event
 * processing. Submission errors are still propagated to the net80211
 * callback, but firmware acknowledgement is asynchronous.
 */
static int
mt7902_wtbl_ba_update(struct mt7902_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, uint16_t ssn, uint16_t winsize, bool tx, bool enable)
{
	struct mt7902_sta_req_hdr hdr;
	struct mt7902_mcu_tlv_hdr tlv;
	struct mt7902_wtbl_req_hdr wtbl_req;
	uint8_t ba[MT7902_BA_WTBL_LEN];
	uint8_t request[sizeof(hdr) + sizeof(tlv) + sizeof(wtbl_req) +
	    sizeof(ba)];
	size_t offset;

	if (ni == NULL || tid >= MT7902_BA_TID_COUNT)
		return (EINVAL);
	bzero(&hdr, sizeof(hdr));
	hdr.bss_idx = MT7902_BSS_IDX;
	hdr.wlan_idx_lo = MT7902_PEER_WCID;
	hdr.tlv_num = htole16(1);
	hdr.is_tlv_append = 1;

	bzero(&tlv, sizeof(tlv));
	tlv.tag = htole16(MT7902_STA_REC_WTBL);
	tlv.len = htole16(sizeof(tlv) + sizeof(wtbl_req) + sizeof(ba));
	bzero(&wtbl_req, sizeof(wtbl_req));
	wtbl_req.wlan_idx_lo = MT7902_PEER_WCID;
	wtbl_req.operation = MT7902_WTBL_SET;
	wtbl_req.tlv_num = htole16(1);

	if (mt7902_ba_encode_wtbl(ba, tid, ssn, winsize,
	    ni->ni_macaddr, tx, enable) != 0)
		return (EINVAL);

	offset = 0;
	memcpy(request + offset, &hdr, sizeof(hdr));
	offset += sizeof(hdr);
	memcpy(request + offset, &tlv, sizeof(tlv));
	offset += sizeof(tlv);
	memcpy(request + offset, &wtbl_req, sizeof(wtbl_req));
	offset += sizeof(wtbl_req);
	memcpy(request + offset, ba, sizeof(ba));
	return (mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
	    request, sizeof(request), false));
}

static int
mt7902_sta_ba_update(struct mt7902_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, uint16_t ssn, uint16_t winsize, bool tx, bool enable)
{
	struct mt7902_sta_req_hdr hdr;
	uint8_t ba[MT7902_BA_STA_REC_LEN];
	uint8_t request[sizeof(hdr) + sizeof(ba)];
	int error;

	if (ni == NULL || tid >= MT7902_BA_TID_COUNT)
		return (EINVAL);
	winsize = enable ? MIN(winsize, IEEE80211_AGGR_BAWMAX) : 0;
	if (enable && winsize == 0)
		winsize = IEEE80211_AGGR_BAWMAX;

	error = mt7902_wtbl_ba_update(sc, ni, tid, ssn, winsize, tx, enable);
	if (error != 0)
		return (error);

	bzero(&hdr, sizeof(hdr));
	hdr.bss_idx = MT7902_BSS_IDX;
	hdr.wlan_idx_lo = MT7902_PEER_WCID;
	hdr.tlv_num = htole16(1);
	hdr.is_tlv_append = 1;
	if (mt7902_ba_encode_sta_rec(ba, tid, ssn, winsize,
	    tx, enable) != 0)
		return (EINVAL);
	memcpy(request, &hdr, sizeof(hdr));
	memcpy(request + sizeof(hdr), ba, sizeof(ba));

	error = mt7902_mcu_send_cmd(sc,
	    MT7902_MCU_UNI_CMD(MT7902_MCU_UNI_STA_REC_UPDATE),
	    request, sizeof(request), false);
	if (error != 0 && enable)
		(void)mt7902_wtbl_ba_update(sc, ni, tid, 0, 0, tx, false);
	return (error);
}

static int
mt7902_addba_response(struct ieee80211_node *ni, struct ieee80211_tx_ampdu *tap,
    int status, int baparamset, int batimeout)
{
	struct ieee80211com *ic;
	struct mt7902_softc *sc;
	uint16_t winsize;
	int error;

	ic = ni->ni_ic;
	sc = ic->ic_softc;
	if (status == IEEE80211_STATUS_SUCCESS) {
		winsize = _IEEE80211_MASKSHIFT(baparamset,
		    IEEE80211_BAPS_BUFSIZ);
		if (winsize == 0)
			winsize = IEEE80211_AGGR_BAWMAX;
		winsize = MIN(winsize, IEEE80211_AGGR_BAWMAX);
		if (tap->txa_tid >= MT7902_BA_TID_COUNT)
			return (EINVAL);
		error = mt7902_sta_ba_update(sc, ni, tap->txa_tid,
		    tap->txa_start, winsize, true, true);
		if (error != 0) {
			(void)sc->ht_addba_response(ni, tap,
			    IEEE80211_STATUS_UNSPECIFIED, baparamset, batimeout);
			return (error);
		}
	}
	return (sc->ht_addba_response(ni, tap, status, baparamset,
	    batimeout));
}

static void
mt7902_addba_stop(struct ieee80211_node *ni, struct ieee80211_tx_ampdu *tap)
{
	struct mt7902_softc *sc = ni->ni_ic->ic_softc;

	if (tap->txa_tid < MT7902_BA_TID_COUNT)
		(void)mt7902_sta_ba_update(sc, ni, tap->txa_tid, 0, 0,
		    true, false);
	sc->ht_addba_stop(ni, tap);
}

static int
mt7902_ampdu_rx_start(struct ieee80211_node *ni,
    struct ieee80211_rx_ampdu *rap, int baparamset, int batimeout,
    int baseqctl)
{
	struct mt7902_softc *sc;
	uint16_t ssn, winsize;
	int tid, error;

	sc = ni->ni_ic->ic_softc;
	tid = (int)(rap - ni->ni_rx_ampdu);
	if (tid < 0 || tid >= MT7902_BA_TID_COUNT)
		return (EINVAL);
	ssn = _IEEE80211_MASKSHIFT(baseqctl, IEEE80211_BASEQ_START);
	winsize = _IEEE80211_MASKSHIFT(baparamset, IEEE80211_BAPS_BUFSIZ);
	if (winsize == 0)
		winsize = IEEE80211_AGGR_BAWMAX;
	winsize = MIN(winsize, IEEE80211_AGGR_BAWMAX);

	error = sc->ht_ampdu_rx_start(ni, rap, baparamset, batimeout,
	    baseqctl);
	if (error != 0)
		return (error);
	error = mt7902_sta_ba_update(sc, ni, (uint8_t)tid, ssn, winsize,
	    false, true);
	if (error != 0) {
		sc->ht_ampdu_rx_stop(ni, rap);
		/*
		 * net80211 15.1 currently ignores this callback's return value
		 * when building the ADDBA response. Send a DELBA best-effort so
		 * the peer does not continue sending aggregates for an unprogrammed
		 * firmware reorder context.
		 */
		{
			uint16_t args[4] = {
				(uint16_t)tid, IEEE80211_DELBAPS_INIT,
				IEEE80211_REASON_UNSPECIFIED, 0
			};
			(void)ni->ni_ic->ic_send_action(ni,
			    IEEE80211_ACTION_CAT_BA,
			    IEEE80211_ACTION_BA_DELBA, args);
		}
	}
	return (error);
}

static void
mt7902_ampdu_rx_stop(struct ieee80211_node *ni,
    struct ieee80211_rx_ampdu *rap)
{
	struct mt7902_softc *sc;
	int tid;

	sc = ni->ni_ic->ic_softc;
	tid = (int)(rap - ni->ni_rx_ampdu);
	if (tid >= 0 && tid < MT7902_BA_TID_COUNT)
		(void)mt7902_sta_ba_update(sc, ni, (uint8_t)tid, 0, 0,
		    false, false);
	sc->ht_ampdu_rx_stop(ni, rap);
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
	ic->ic_txstream = MIN(sc->nic_caps.phy.nss, 4);
	ic->ic_rxstream = MIN(sc->nic_caps.phy.nss, 4);

	/*
	 * Advertise HT/VHT only when reported by firmware. Keep AMPDU-TX and
	 * A-MSDU-TX software capability bits clear until the complete
	 * aggregation/Block Ack data path is integrated.
	 */
	if (sc->nic_caps.phy.ht &&
	    (sc->nic_caps.has_2ghz || sc->nic_caps.has_5ghz)) {
		ic->ic_htcaps = IEEE80211_HTC_HT | IEEE80211_HTC_AMPDU |
		    IEEE80211_HTCAP_SMPS_OFF |
		    IEEE80211_HTCAP_SHORTGI20 |
		    IEEE80211_HTCAP_MAXAMSDU_3839;
		if (sc->nic_caps.phy.max_bw >= 1)
			ic->ic_htcaps |= IEEE80211_HTCAP_CHWIDTH40 |
			    IEEE80211_HTCAP_SHORTGI40;
		if (sc->nic_caps.phy.rx_ldpc)
			ic->ic_htcaps |= IEEE80211_HTCAP_LDPC;
		if (sc->nic_caps.phy.tx_ldpc)
			ic->ic_htcaps |= IEEE80211_HTC_TXLDPC;
		if (sc->nic_caps.phy.tx_stbc)
			ic->ic_htcaps |= IEEE80211_HTCAP_TXSTBC;
		if (sc->nic_caps.phy.rx_stbc)
			ic->ic_htcaps |= MIN(sc->nic_caps.phy.rx_stbc, 3) <<
			    IEEE80211_HTCAP_RXSTBC_S;
		if (sc->nic_caps.has_2ghz)
			setbit(ic->ic_modecaps, IEEE80211_MODE_11NG);
		if (sc->nic_caps.has_5ghz)
			setbit(ic->ic_modecaps, IEEE80211_MODE_11NA);
	}

	if (sc->nic_caps.phy.vht && sc->nic_caps.has_5ghz &&
	    sc->nic_caps.phy.ht && sc->nic_caps.phy.max_bw >= 2) {
		uint16_t mcs_map;

		ic->ic_flags_ext |= IEEE80211_FEXT_VHT;
		ic->ic_vht_cap.vht_cap_info = IEEE80211_VHTCAP_SHORT_GI_80;
		if (sc->nic_caps.phy.rx_ldpc)
			ic->ic_vht_cap.vht_cap_info |= IEEE80211_VHTCAP_RXLDPC;
		if (sc->nic_caps.phy.tx_stbc)
			ic->ic_vht_cap.vht_cap_info |= IEEE80211_VHTCAP_TXSTBC;
		if (sc->nic_caps.phy.rx_stbc)
			ic->ic_vht_cap.vht_cap_info |=
			    MIN(sc->nic_caps.phy.rx_stbc, 7) <<
			    IEEE80211_VHTCAP_RXSTBC_MASK_S;
		mcs_map = mt7902_vht_mcs_map(sc->nic_caps.phy.nss);
		ic->ic_vht_cap.supp_mcs.rx_mcs_map = htole16(mcs_map);
		ic->ic_vht_cap.supp_mcs.tx_mcs_map = htole16(mcs_map);
		setbit(ic->ic_modecaps, IEEE80211_MODE_VHT_5GHZ);
	}

	mt7902_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
	    ic->ic_channels);
	if (ic->ic_nchans == 0)
		return (ENXIO);
	ieee80211_ifattach(ic);

	/*
	 * Radiotap is attached after ifattach, following the FreeBSD native
	 * wireless drivers. Rate/TSFT fields are omitted until the MT7902 RX
	 * parser has a reliable PHY-rate and TSF decoder.
	 */
	ieee80211_radiotap_attach(ic,
	    &sc->tx_tap.wt_ihdr, sizeof(sc->tx_tap),
	    MT7902_TX_RADIOTAP_PRESENT,
	    &sc->rx_tap.wr_ihdr, sizeof(sc->rx_tap),
	    MT7902_RX_RADIOTAP_PRESENT);
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
	/* Preserve the net80211 reorder/state machine; wrap BA setup in firmware. */
	sc->ht_addba_response = ic->ic_addba_response;
	sc->ht_addba_stop = ic->ic_addba_stop;
	sc->ht_ampdu_rx_start = ic->ic_ampdu_rx_start;
	sc->ht_ampdu_rx_stop = ic->ic_ampdu_rx_stop;
	ic->ic_addba_response = mt7902_addba_response;
	ic->ic_addba_stop = mt7902_addba_stop;
	ic->ic_ampdu_rx_start = mt7902_ampdu_rx_start;
	ic->ic_ampdu_rx_stop = mt7902_ampdu_rx_stop;
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
	struct ieee80211_node *ni;

	if (sc == NULL || !sc->net80211_attached)
		return;
	mtx_lock(&sc->mtx);
	ni = sc->station_node;
	sc->station_node = NULL;
	mtx_unlock(&sc->mtx);
	if (ni != NULL)
		ieee80211_free_node(ni);
	if (sc->ht_addba_response != NULL) {
		sc->ic.ic_addba_response = sc->ht_addba_response;
		sc->ic.ic_addba_stop = sc->ht_addba_stop;
		sc->ic.ic_ampdu_rx_start = sc->ht_ampdu_rx_start;
		sc->ic.ic_ampdu_rx_stop = sc->ht_ampdu_rx_stop;
		sc->ht_addba_response = NULL;
		sc->ht_addba_stop = NULL;
		sc->ht_ampdu_rx_start = NULL;
		sc->ht_ampdu_rx_stop = NULL;
	}
	ieee80211_ifdetach(&sc->ic);
	sc->net80211_attached = 0;
}

/*
 * Complete any firmware-pending packets with failure after WFDMA/IRQ has
 * stopped. This is used by suspend, explicit hw_stop, attach failure and
 * detach, and runs without the device mutex held across net80211 callbacks.
 */
void
mt7902_tx_pending_drain(struct mt7902_softc *sc)
{
	struct ieee80211_node *ni;
	struct mbuf *m;
	int i;

	for (i = 0; i < MT7902_TX_PENDING_COUNT; i++) {
		mtx_lock(&sc->mtx);
		if (!sc->tx_pending[i].active) {
			mtx_unlock(&sc->mtx);
			continue;
		}
		m = sc->tx_pending[i].m;
		ni = sc->tx_pending[i].ni;
		bzero(&sc->tx_pending[i], sizeof(sc->tx_pending[i]));
		if (sc->tx_pending_count != 0)
			sc->tx_pending_count--;
		mtx_unlock(&sc->mtx);
		if (m != NULL) {
			if (ni != NULL)
				ieee80211_tx_complete(ni, m, 1);
			else
				m_freem(m);
		} else if (ni != NULL) {
			ieee80211_free_node(ni);
		}
	}
}

void
mt7902_tx_task(void *arg, int pending __unused)
{
	struct mt7902_softc *sc = arg;
	struct ieee80211_node *ni;
	struct mbuf *m;
	int error, budget;
	bool more;

	/*
	 * TX shares the system taskqueue with IRQ/RX/MCU work. Bound each
	 * invocation so a continuously busy transmit queue cannot monopolize
	 * the worker and delay receive processing or firmware responses.
	 */
	for (budget = 0; budget < 64; budget++) {
		mtx_lock(&sc->mtx);
		m = mbufq_dequeue(&sc->txq);
		mtx_unlock(&sc->mtx);
		if (m == NULL)
			break;
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;
		error = mt7902_tx_one(sc, m, ni);
		if (error != 0) {
			if (ni != NULL)
				ieee80211_tx_complete(ni, m, 1);
			else
				m_freem(m);
		}
	}

	mtx_lock(&sc->mtx);
	more = !mbufq_empty(&sc->txq);
	mtx_unlock(&sc->mtx);
	if (more)
		taskqueue_enqueue(sc->tq, &sc->tx_task);
}

