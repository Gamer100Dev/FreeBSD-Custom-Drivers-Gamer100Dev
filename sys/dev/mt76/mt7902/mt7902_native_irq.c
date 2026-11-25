/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native PCI interrupt and MCU event-ring handling.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>

#include "if_mt7902_native.h"
#include "mt7902_native_rxdesc.h"
#include "mt7902_native_rxtrans.h"

#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_ratectl.h>
#include <net80211/ieee80211_ht.h>

#define MT7902_RXD_SIZE	36
#define MT7902_RX_BATCH	64
#define MT7902_RXD_EID_OFFSET	28
#define MT7902_RXD_SEQ_OFFSET	29
#define MT7902_RXD_EVENT	0x06
#define MT7902_RXD_LEN_OFFSET	24
#define MT7902_RX_LEN_SHIFT	16
#define MT7902_RX_LEN_MASK	0x3fffU
#define MT7902_RXD0_LEN_MASK	0xffffU
#define MT7902_RXD0_TYPE_SHIFT	27
#define MT7902_PKT_TYPE_TXS		0
#define MT7902_PKT_TYPE_NORMAL		2
#define MT7902_PKT_TYPE_TXRX_NOTIFY	6
#define MT7902_PKT_TYPE_RX_EVENT	7
#define MT7902_PKT_TYPE_NORMAL_MCU	8
#define MT7902_TX_FREE_MSDU_CNT_MASK	0x03ffU
#define MT7902_TX_FREE_COUNT_MASK	0x1fffU
#define MT7902_TX_FREE_STATUS_MASK	0x3U
#define MT7902_TX_FREE_STATUS_SHIFT	13
#define MT7902_TX_FREE_MSDU_ID_SHIFT	16
#define MT7902_TX_FREE_PAIR		(1U << 31)
#define MT7902_RXD1_GROUP1	(1U << 11)
#define MT7902_RXD1_GROUP2	(1U << 12)
#define MT7902_RXD1_GROUP3	(1U << 13)
#define MT7902_RXD1_GROUP4	(1U << 14)
#define MT7902_RXD1_GROUP5	(1U << 15)
#define MT7902_RXD2_PAD_MASK	(3U << 14)
#define MT7902_RXD2_PAD_SHIFT	14

/*
 * Return the reserved producer slot to WFDMA.  RX buffers stay paired with
 * their descriptors for their entire lifetime: swapping buffer structs here
 * leaves the descriptor-to-buffer mapping inconsistent after the ring wraps.
 */
/*
 * Refill the reserved producer slot using the completed slot's buffer.
 * mt76's dma dequeue/fill path does the same ownership rotation: the
 * consumed buffer is recycled at head, while the formerly reserved buffer
 * remains associated with the consumed descriptor until the ring wraps.
 */
static int
mt7902_rx_rearm(struct mt7902_softc *sc, struct mt7902_ring *ring, int idx)
{
	struct mt7902_dma_buffer tmp, *buf;
	struct mt7902_dma_desc *desc;
	int refill_idx;

	refill_idx = ring->head;
	if (refill_idx == idx || ring->queued <= 0 ||
	    ring->queued >= ring->ndesc)
		return (EBUSY);

	/* Remove the completed tail and rotate its buffer to the producer. */
	ring->queued--;
	tmp = ring->buf[idx];
	ring->buf[idx] = ring->buf[refill_idx];
	ring->buf[refill_idx] = tmp;
	buf = &ring->buf[refill_idx];
	desc = &ring->desc[refill_idx];

	bus_dmamap_sync(ring->buf_tag, buf->map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	bus_dmamap_sync(ring->buf_tag, buf->map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	desc->buf0 = htole32((uint32_t)buf->paddr);
	desc->buf1 = 0;
	desc->info = 0;
	desc->control = htole32(((uint32_t)buf->len &
	    MT7902_DMA_LEN_MASK) << MT7902_DMA_LEN_SHIFT);
	bus_dmamap_sync(ring->desc_tag, ring->desc_map,
	    BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	ring->tail = (idx + 1) % ring->ndesc;
	ring->head = (refill_idx + 1) % ring->ndesc;
	ring->queued++;
	mt7902_write(sc, ring->reg_base + 0x8, ring->head);
	return (0);
}

static const char *
mt7902_rx_drop_name(enum mt7902_rx_drop_reason reason)
{
	switch (reason) {
	case MT7902_RX_DROP_SHORT: return ("short-rxd");
	case MT7902_RX_DROP_BAD_CHANNEL: return ("bad-channel");
	case MT7902_RX_DROP_BAD_PACKET_TYPE: return ("bad-packet-type");
	case MT7902_RX_DROP_HDR_TRANS: return ("header-translated");
	case MT7902_RX_DROP_ICV: return ("icv-monitor-only");
	case MT7902_RX_DROP_FCS: return ("fcs-error");
	case MT7902_RX_DROP_MIC: return ("mic-error");
	case MT7902_RX_DROP_RXD_ERROR: return ("rxd-error-bits");
	case MT7902_RX_DROP_PAYLOAD_FORMAT: return ("amsdu-payload-format");
	case MT7902_RX_DROP_GROUP_TRUNCATED: return ("truncated-rx-group");
	case MT7902_RX_DROP_BAD_OFFSET: return ("bad-frame-offset");
	case MT7902_RX_DROP_SHORT_FRAME: return ("short-80211-frame");
	case MT7902_RX_DROP_BAD_HEADER: return ("invalid-80211-header");
	case MT7902_RX_DROP_NOMEM: return ("mbuf-allocation");
	case MT7902_RX_DROP_RX_PARAMS: return ("rx-params-attach");
	case MT7902_RX_DROP_NOT_ATTACHED: return ("net80211-not-attached");
	case MT7902_RX_DROP_INVALID_DMA: return ("invalid-dma-length");
	default: return ("unknown");
	}
}

static void
mt7902_rx_drop(struct mt7902_softc *sc,
    enum mt7902_rx_drop_reason reason, size_t dma_len, uint32_t rxd0,
    uint32_t rxd1, uint32_t rxd2, uint32_t rxd3, uint32_t rxd4,
    size_t offset, uint16_t fc)
{
	int debug;
	bool scanning;

	mtx_lock(&sc->mtx);
	sc->rx_drop_count++;
	if (reason < MT7902_RX_DROP_REASON_COUNT)
		sc->rx_drop_reason[reason]++;
	sc->rx_last_drop_reason = reason;
	sc->rx_last_drop_len = (uint16_t)MIN(dma_len, UINT16_MAX);
	sc->rx_last_drop_rxd[0] = rxd0;
	sc->rx_last_drop_rxd[1] = rxd1;
	sc->rx_last_drop_rxd[2] = rxd2;
	sc->rx_last_drop_rxd[3] = rxd3;
	sc->rx_last_drop_rxd[4] = rxd4;
	debug = sc->rx_debug;
	scanning = sc->scan_active;
	mtx_unlock(&sc->mtx);
	if (debug > 0 && scanning)
		device_printf(sc->dev,
		    "RX DROP reason=%s(%u) len=%ju off=%ju fc=%04x RXD=%08x/%08x/%08x/%08x/%08x\n",
		    mt7902_rx_drop_name(reason), reason, (uintmax_t)dma_len,
		    (uintmax_t)offset, fc, rxd0, rxd1, rxd2, rxd3, rxd4);
}

static size_t
mt7902_rx_80211_hdrlen(uint16_t fc)
{
	size_t len;
	uint16_t type, subtype;

	type = fc & 0x000c;
	subtype = (fc >> 4) & 0x0f;
	switch (type) {
	case 0x0000:             /* management */
		len = sizeof(struct ieee80211_frame);
		if (fc & 0x8000)
			len += 4;
		break;
	case 0x0004:             /* control */
		len = (subtype == 0x0c || subtype == 0x0d) ?
		    sizeof(struct ieee80211_frame_cts) :
		    sizeof(struct ieee80211_frame_min);
		break;
	case 0x0008:             /* data */
		len = sizeof(struct ieee80211_frame);
		if ((fc & 0x0300) == 0x0300)
			len += IEEE80211_ADDR_LEN;
		if (subtype & 0x08)
			len += 2;
		if (fc & 0x8000)
			len += 4;
		break;
	default:
		len = 0;
		break;
	}
	return (len);
}

/*
 * Connac2 normal RXD parser.  The six-word base RXD is followed by optional
 * groups in the order used by mt7921_mac_fill_rx(): GROUP_4, GROUP_1,
 * GROUP_2, GROUP_3 and (inside GROUP_3) GROUP_5.  Header-translated frames
 * are deliberately rejected for now because net80211 expects native 802.11
 * headers; the management frames needed for scanning are not translated.
 */
static void
mt7902_rx_data_frame(struct mt7902_softc *sc, const uint8_t *data,
    size_t dma_len)
{
	struct ieee80211_rx_stats rxs;
	const struct ieee80211_frame_min *wh;
	struct ieee80211_node *ni;
	struct mt7902_rx_phy rxphy;
	struct mt7902_rx_amsdu rx_amsdu;
	struct mt7902_rx_translated rxtrans;
	struct mbuf *m;
	uint32_t rxd0, rxd1, rxd2, rxd3, rxd4, rcpi_word, sec_mode;
	uint16_t fc;
	uint8_t channel, rcpi, subtype, pkt_type, pkt_flag, tap_channel;
	uint8_t frame_type;
	uint16_t tap_freq, tap_chan_flags, qos_control;
	int rssi, noise, debug;
	size_t offset, frame_len, hdr_len, qos_offset;
	enum mt7902_rx_drop_reason drop_reason;
	bool is_mgmt, monitor_active, rcpi_valid, monitor_only;
	bool hdr_trans, scanning, attached, phy_valid, qos_data;
	struct mt7902_rxdesc_layout rx_layout;

	rxd0 = rxd1 = rxd2 = rxd3 = rxd4 = rcpi_word = sec_mode = 0;
	bzero(&rxphy, sizeof(rxphy));
	bzero(&rx_amsdu, sizeof(rx_amsdu));
	bzero(&rxtrans, sizeof(rxtrans));
	phy_valid = false;
	qos_control = 0;
	qos_offset = 0;
	fc = 0;
	offset = frame_len = hdr_len = 0;
	pkt_type = pkt_flag = channel = rcpi = subtype = tap_channel = 0;
	drop_reason = MT7902_RX_DROP_SHORT;
	if (dma_len < 24)
		goto drop;
	rxd0 = le32dec(data);
	rxd1 = le32dec(data + 4);
	rxd2 = le32dec(data + 8);
	rxd3 = le32dec(data + 12);
	rxd4 = le32dec(data + 16);
	channel = (rxd3 >> 8) & 0xff;
	pkt_type = (rxd0 >> 27) & 0x1f;
	pkt_flag = (rxd0 >> 16) & 0x0f;
	if (pkt_type == MT7902_PKT_TYPE_RX_EVENT && pkt_flag == 1)
		pkt_type = MT7902_PKT_TYPE_NORMAL_MCU;
	/* MT7902's current net80211 channel list is 2.4/5 GHz only. */
	if (channel > 180) {
		drop_reason = MT7902_RX_DROP_BAD_CHANNEL;
		goto drop;
	}

	mtx_lock(&sc->mtx);
	sc->rx_data_count++;
	sc->rx_last_len = (uint32_t)dma_len;
	sc->rx_last_rxd[0] = rxd0;
	sc->rx_last_rxd[1] = rxd1;
	sc->rx_last_rxd[2] = rxd2;
	sc->rx_last_rxd[3] = rxd3;
	monitor_active = sc->monitor_active;
	mtx_unlock(&sc->mtx);

	/*
	 * mt7921_queue_rx_skb() treats both PKT_TYPE_NORMAL and
	 * PKT_TYPE_NORMAL_MCU as native 802.11 frames after RXD parsing.
	 */
	if (pkt_type != MT7902_PKT_TYPE_NORMAL &&
	    pkt_type != MT7902_PKT_TYPE_NORMAL_MCU) {
		drop_reason = MT7902_RX_DROP_BAD_PACKET_TYPE;
		goto drop;
	}
	/*
	 * ICV failures are monitor-only (mt7921_mac_fill_rx sets
	 * RX_FLAG_ONLY_MONITOR). FCS and TKIP-MIC errors can be delivered to
	 * a monitor VAP with FreeBSD's packet flags; normal VAPs never receive
	 * those frames.
	 */
	monitor_only = (rxd1 & (1U << 25)) != 0;
	/*
	 * Connac2 RXD1 bit 28 is BAND_IDX, not HDR_TRANS. This driver currently
	 * owns band 0 only. Header-translated unicast data is reconstructed from
	 * RXD group 4 and the active station node below.
	 */
	if ((rxd1 & MT7902_RXD1_BAND_IDX) != 0) {
		drop_reason = MT7902_RX_DROP_BAD_CHANNEL;
		goto drop;
	}
	hdr_trans = (rxd2 & MT7902_RXD2_HDR_TRANS) != 0;
	if (hdr_trans && (rxd1 & MT7902_RXD1_GROUP_4) == 0) {
		drop_reason = MT7902_RX_DROP_GROUP_TRUNCATED;
		goto drop;
	}
	if (monitor_only && !monitor_active) {
		drop_reason = MT7902_RX_DROP_ICV;
		goto drop;
	}
	if (!monitor_active && (rxd1 & (1U << 27)) != 0) {
		drop_reason = MT7902_RX_DROP_FCS;
		goto drop;
	}
	if (!monitor_active && (rxd1 & (1U << 26)) != 0) {
		drop_reason = MT7902_RX_DROP_MIC;
		goto drop;
	}
	if ((rxd2 & ((1U << 23) | (1U << 24) | (1U << 25))) != 0) {
		drop_reason = MT7902_RX_DROP_RXD_ERROR;
		goto drop;
	}

	if (mt7902_rxdesc_get_layout(rxd1, rxd2, dma_len,
	    &rx_layout) != 0) {
		drop_reason = MT7902_RX_DROP_GROUP_TRUNCATED;
		goto drop;
	}
	offset = rx_layout.frame_offset;

	rcpi = 0;
	rcpi_valid = false;
	if ((rxd1 & MT7902_RXD1_GROUP_3) != 0) {
		phy_valid = mt7902_rxdesc_decode_phy(
		    le32dec(data + rx_layout.group3_offset), &rxphy) == 0;
		rcpi_word = le32dec(data + rx_layout.group3_offset + 4);
		rcpi = rcpi_word & 0xff;  /* PRXV RCPI0 */
		rcpi_valid = true;
		if ((rxd1 & MT7902_RXD1_GROUP_5) != 0) {
			rcpi_word = le32dec(data + rx_layout.group5_rcpi_offset);
			rcpi = rcpi_word & 0xff;
		}
	}
	frame_len = dma_len - offset;
	qos_control = 0;
	if (hdr_trans) {
		uint32_t rxd6, rxd8, rxd9;
		size_t eth_len;
		struct ieee80211_node *station;

		/* Reverse translation is defined for unicast-to-me data frames. */
		if (((rxd3 >> 16) & 0x3) != 1 ||
		    frame_len < MT7902_RXTRANS_ETH_HDR_LEN ||
		    (rxd4 & MT7902_RXD4_PAYLOAD_FORMAT) !=
		    MT7902_RXD4_PAYLOAD_NONE) {
			drop_reason = MT7902_RX_DROP_HDR_TRANS;
			goto drop;
		}
		rxd6 = le32dec(data + rx_layout.group4_offset);
		rxd8 = le32dec(data + rx_layout.group4_offset + 8);
		rxd9 = le32dec(data + rx_layout.group4_offset + 12);
		fc = rxd6 & 0xffff;
		mtx_lock(&sc->mtx);
		station = sc->station_node != NULL ?
		    ieee80211_ref_node(sc->station_node) : NULL;
		mtx_unlock(&sc->mtx);
		if (station == NULL) {
			drop_reason = MT7902_RX_DROP_HDR_TRANS;
			goto drop;
		}
		eth_len = frame_len;
		if (mt7902_rxtrans_decode(data + offset, eth_len, fc,
		    (uint16_t)rxd8, (uint16_t)(rxd8 >> 16), rxd9,
		    sc->ic.ic_macaddr, station->ni_macaddr, station->ni_bssid,
		    &rxtrans) != 0) {
			ieee80211_free_node(station);
			drop_reason = MT7902_RX_DROP_HDR_TRANS;
			goto drop;
		}
		ieee80211_free_node(station);
		frame_len = rxtrans.frame_len;
		hdr_len = rxtrans.header_len;
		qos_control = rxtrans.qos_control;
		qos_data = rxtrans.qos_data != 0;
	} else {
		if (frame_len < sizeof(*wh)) {
			drop_reason = MT7902_RX_DROP_SHORT_FRAME;
			goto drop;
		}
		wh = (const struct ieee80211_frame_min *)(const void *)(data + offset);
		fc = le16dec(&wh->i_fc[0]);
		hdr_len = mt7902_rx_80211_hdrlen(fc);
		if (hdr_len == 0 || frame_len < hdr_len) {
			drop_reason = MT7902_RX_DROP_BAD_HEADER;
			goto drop;
		}
		frame_type = (fc >> 2) & 0x3;
		subtype = (fc >> 4) & 0x0f;
		qos_data = frame_type == 2 && (subtype & 0x08) != 0;
		if (mt7902_rxdesc_decode_amsdu(rxd4, qos_data, frame_len,
		    hdr_len, &rx_amsdu) != 0) {
			drop_reason = MT7902_RX_DROP_PAYLOAD_FORMAT;
			goto drop;
		}
		if (qos_data) {
			/* QoS control follows Addr4 on four-address data frames. */
			qos_offset = sizeof(struct ieee80211_frame);
			if ((fc & 0x0300) == 0x0300)
				qos_offset += IEEE80211_ADDR_LEN;
			if (frame_len < qos_offset + sizeof(qos_control)) {
				drop_reason = MT7902_RX_DROP_BAD_HEADER;
				goto drop;
			}
			qos_control = le16dec(data + offset + qos_offset);
		}
		if (rx_amsdu.present) {
			/* One native-header A-MSDU subframe is returned per RXD. */
			if (!qos_data ||
			    (qos_control & IEEE80211_QOS_AMSDU) == 0) {
				drop_reason = MT7902_RX_DROP_PAYLOAD_FORMAT;
				goto drop;
			}
			frame_len = rx_amsdu.frame_len;
		}
	}
	is_mgmt = (fc & 0x000c) == 0;
	subtype = (fc >> 4) & 0x0f;
	frame_type = (fc >> 2) & 0x3;
	mtx_lock(&sc->mtx);
	sc->rx_last_rxd4 = rxd4;
	sc->rx_last_fc = fc;
	sc->rx_last_frame_len = (uint16_t)MIN(frame_len, UINT16_MAX);
	sc->rx_last_hdr_len = (uint16_t)hdr_len;
	sc->rx_last_offset = (uint16_t)MIN(offset, UINT16_MAX);
	sc->rx_last_pkt_type = pkt_type;
	sc->rx_last_addr_type = (rxd3 >> 16) & 0x3;
	sc->rx_last_payload_format = rxd4 & 0x3;
	sc->rx_last_channel = channel;
	mtx_unlock(&sc->mtx);
	mtx_lock(&sc->mtx);
	sc->rx_frame_type[frame_type]++;
	sc->rx_channel_frames[channel]++;
	if (is_mgmt) {
		sc->rx_mgmt_count++;
		sc->rx_mgmt_subtype[subtype]++;
		if (subtype == 8 || subtype == 5)
			sc->rx_beacon_count++;
	}
	debug = sc->rx_debug;
	scanning = sc->scan_active;
	mtx_unlock(&sc->mtx);

	/* RCPI is 0.5 dB steps with a 220 offset, per Connac2 PRXV. */
	noise = -95;
	rssi = rcpi_valid ? MAX(-110, MIN(0, ((int)rcpi - 220) / 2)) :
	    noise;
	if (debug > 0 && scanning)
		device_printf(sc->dev,
		    "RX FRAME ch=%u band=%s type=%u subtype=%u fc=%04x len=%ju hdr=%ju off=%ju rssi=%d rcpi=%u valid=%u addr_type=%u payload=%u sec=%u flags=%08x RXD=%08x/%08x/%08x/%08x/%08x\n",
		    channel, channel <= 14 ? "2g" : "5g", frame_type, subtype, fc,
		    (uintmax_t)frame_len, (uintmax_t)hdr_len, (uintmax_t)offset,
		    rssi, rcpi, rcpi_valid, (rxd3 >> 16) & 0x3, rxd4 & 0x3,
		    (rxd1 >> 16) & 0x1f, rxd1, rxd0, rxd1, rxd2, rxd3, rxd4);
	m = m_get2((int)frame_len, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		drop_reason = MT7902_RX_DROP_NOMEM;
		goto drop;
	}
	m->m_len = (int)frame_len;
	m->m_pkthdr.len = (int)frame_len;
	if (hdr_trans) {
		uint8_t *dst;

		dst = mtod(m, uint8_t *);
		memcpy(dst, rxtrans.header, rxtrans.header_len);
		memcpy(dst + rxtrans.header_len, rxtrans.llc, rxtrans.llc_len);
		memcpy(dst + rxtrans.header_len + rxtrans.llc_len,
		    data + offset + rxtrans.payload_offset,
		    rxtrans.payload_len);
	} else if (rx_amsdu.present) {
		memcpy(mtod(m, void *), data + offset, hdr_len);
		memcpy((uint8_t *)mtod(m, void *) + hdr_len,
		    data + offset + hdr_len + 2, frame_len - hdr_len);
	} else
		memcpy(mtod(m, void *), data + offset, frame_len);

	bzero(&rxs, sizeof(rxs));
	rxs.r_flags = IEEE80211_R_NF;
	rxs.c_nf = noise;
	if (rcpi_valid) {
		rxs.r_flags |= IEEE80211_R_RSSI;
		rxs.c_rssi = (int8_t)(rssi - noise);
	}
	if (rxd1 & (1U << 27))
		rxs.c_pktflags |= IEEE80211_RX_F_FAIL_FCSCRC;
	if (rxd1 & (1U << 26))
		rxs.c_pktflags |= IEEE80211_RX_F_FAIL_MMIC;
	if (rx_amsdu.present) {
		rxs.c_pktflags |= IEEE80211_RX_F_AMSDU;
		if (rx_amsdu.more)
			rxs.c_pktflags |= IEEE80211_RX_F_AMSDU_MORE;
	}
	if ((rxd1 & MT7902_RXD1_GROUP_2) != 0) {
		rxs.r_flags |= IEEE80211_R_TSF32 | IEEE80211_R_TSF_START;
		rxs.c_rx_tsf = le32dec(data + rx_layout.group2_offset);
	}
	if (phy_valid) {
		static const uint8_t cck_rates[] = { 2, 4, 11, 22 };
		static const uint8_t ofdm_rates[] = {
			12, 18, 24, 36, 48, 72, 96, 108
		};

		rxs.c_width = rxphy.bandwidth + 1;
		rxs.c_vhtnss = rxphy.nss;
		if (rxphy.gi != 0)
			rxs.c_pktflags |= IEEE80211_RX_F_SHORTGI;
		if (rxphy.stbc != 0)
			rxs.c_pktflags |= IEEE80211_RX_F_STBC;
		if (rxphy.ldpc)
			rxs.c_pktflags |= IEEE80211_RX_F_LDPC;
		switch (rxphy.mode) {
		case MT7902_PHY_CCK:
			rxs.c_pktflags |= IEEE80211_RX_F_CCK;
			rxs.c_phytype = IEEE80211_RX_FP_11B;
			rxs.c_rate = cck_rates[rxphy.rate_idx];
			break;
		case MT7902_PHY_OFDM:
			rxs.c_pktflags |= IEEE80211_RX_F_OFDM;
			rxs.c_phytype = channel <= 14 ? IEEE80211_RX_FP_11G :
			    IEEE80211_RX_FP_11A;
			rxs.c_rate = ofdm_rates[rxphy.rate_idx];
			break;
		case MT7902_PHY_HT:
		case MT7902_PHY_HT_GF:
			rxs.c_pktflags |= IEEE80211_RX_F_HT;
			rxs.c_phytype = channel <= 14 ? IEEE80211_RX_FP_11NG :
			    IEEE80211_RX_FP_11NA;
			rxs.c_rate = IEEE80211_RATE_MCS | rxphy.rate_idx;
			break;
		case MT7902_PHY_VHT:
			rxs.c_pktflags |= IEEE80211_RX_F_VHT;
			rxs.c_phytype = IEEE80211_RX_FP_11NA;
			rxs.c_rate = rxphy.rate_idx;
			break;
		default:
			break;
		}
	}
	/*
	 * Hardware key installation is not implemented in this native port.
	 * Preserve the protected frame and let FreeBSD net80211's software
	 * crypto path authenticate/decrypt it. A cipher indication in RXD1
	 * alone is not proof that a key was installed or that IV/MIC bytes
	 * were stripped, so never set RX_F_DECRYPTED/IV_STRIP here.
	 */
	sec_mode = (rxd1 >> 16) & 0x1f;
	if (channel != 0) {
		rxs.r_flags |= IEEE80211_R_IEEE | IEEE80211_R_FREQ |
		    IEEE80211_R_BAND;
		rxs.c_ieee = channel;
		rxs.c_freq = ieee80211_ieee2mhz(channel,
		    channel <= 14 ? IEEE80211_CHAN_2GHZ : IEEE80211_CHAN_5GHZ);
		rxs.c_band = channel <= 14 ? IEEE80211_CHAN_2GHZ :
		    IEEE80211_CHAN_5GHZ;
	}
	/*
	 * Update the radiotap template consumed by net80211's monitor/BPF
	 * path. RX rate and TSFT are not advertised until the corresponding
	 * Connac2 PHY fields have been decoded and validated.
	 */
	tap_channel = channel;
	if (tap_channel == 0) {
		mtx_lock(&sc->mtx);
		tap_channel = sc->current_channel;
		mtx_unlock(&sc->mtx);
	}
	tap_freq = 0;
	tap_chan_flags = 0;
	if (tap_channel != 0) {
		tap_freq = ieee80211_ieee2mhz(tap_channel,
		    tap_channel <= 14 ? IEEE80211_CHAN_2GHZ :
		    IEEE80211_CHAN_5GHZ);
		tap_chan_flags = tap_channel <= 14 ?
		    (IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_CCK |
		    IEEE80211_CHAN_OFDM) :
		    (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_OFDM);
	}
	sc->rx_tap.wr_flags = (rxd1 & (1U << 27)) != 0 ?
	    IEEE80211_RADIOTAP_F_BADFCS : 0;
	sc->rx_tap.wr_pad = 0;
	sc->rx_tap.wr_chan_freq = htole16(tap_freq);
	sc->rx_tap.wr_chan_flags = htole16(tap_chan_flags);
	sc->rx_tap.wr_dbm_antsignal = rcpi_valid ? (int8_t)rssi : -127;
	sc->rx_tap.wr_dbm_antnoise = (int8_t)noise;

	if (ieee80211_add_rx_params(m, &rxs) == 0) {
		m_freem(m);
		drop_reason = MT7902_RX_DROP_RX_PARAMS;
		goto drop;
	}
	/* FreeBSD net80211 performs software BA reordering for HT peers. */
	if (qos_data) {
		uint8_t tid;

		tid = qos_control & 0x0f;
		ni = ieee80211_find_rxnode(&sc->ic,
		    (const struct ieee80211_frame_min *)mtod(m, const void *));
		if (ni != NULL) {
			/* Only frames covered by a negotiated RX BA session reorder. */
			if ((ni->ni_flags & IEEE80211_NODE_HT) != 0 &&
			    (ni->ni_rx_ampdu[tid].rxa_flags &
			    IEEE80211_AGGR_RUNNING) != 0)
				m->m_flags |= M_AMPDU;
			ieee80211_free_node(ni);
		}
	}
	if (monitor_only) {
		mtx_lock(&sc->mtx);
		sc->rx_monitor_error_count++;
		mtx_unlock(&sc->mtx);
		/* Do not let an ICV-failed frame enter the normal STA path. */
		ieee80211_radiotap_rx_all(&sc->ic, m);
		m_freem(m);
		return;
	}
	mtx_lock(&sc->mtx);
	attached = sc->net80211_attached != 0;
	if (attached)
		sc->rx_input_count++;
	mtx_unlock(&sc->mtx);
	if (!attached) {
		m_freem(m);
		drop_reason = MT7902_RX_DROP_NOT_ATTACHED;
		goto drop;
	}
	if (debug > 0 && scanning)
		device_printf(sc->dev,
		    "RX DELIVER ch=%u fc=%04x subtype=%u len=%ju flags=%08x\n",
		    channel, fc, subtype, (uintmax_t)frame_len, rxs.c_pktflags);
	(void)ieee80211_input_mimo_all(&sc->ic, m);
	return;

drop:
	mt7902_rx_drop(sc, drop_reason, dma_len, rxd0, rxd1, rxd2, rxd3,
	    rxd4, offset, fc);
}

static void
mt7902_rx_data_poll(struct mt7902_softc *sc)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	uint32_t control;
	size_t dma_len;
	int idx, count, error, debug;
	bool scanning;

	ring = &sc->rx_data;
	if (ring->desc == NULL)
		return;
	for (count = 0; count < MIN(ring->ndesc, MT7902_RX_BATCH); count++) {
		idx = ring->tail;
		desc = &ring->desc[idx];
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) == 0)
			break;
		buf = &ring->buf[idx];
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTREAD);
		mtx_lock(&sc->mtx);
		sc->rx_data_desc_count++;
		mtx_unlock(&sc->mtx);
		dma_len = (control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK;
		mtx_lock(&sc->mtx);
		debug = sc->rx_debug;
		scanning = sc->scan_active;
		mtx_unlock(&sc->mtx);
		if (debug >= 2 && scanning)
			device_printf(sc->dev,
			    "RX DATA DESC idx=%d control=%08x len=%ju buf_len=%ju head=%d tail=%d hwidx=%08x\n",
			    idx, control, (uintmax_t)dma_len, (uintmax_t)buf->len,
			    ring->head, ring->tail,
			    mt7902_read(sc, ring->reg_base + 0x0c));
		if (dma_len == 0 || dma_len > buf->len)
			mt7902_rx_drop(sc, MT7902_RX_DROP_INVALID_DMA, dma_len,
			    0, 0, 0, 0, 0, 0, 0);
		else
			mt7902_rx_data_frame(sc, buf->vaddr, dma_len);
		error = mt7902_rx_rearm(sc, ring, idx);
		if (error != 0) {
			device_printf(sc->dev,
			    "RX data ring cursor collision at descriptor %d\n", idx);
			break;
		}
	}
	if (count == MT7902_RX_BATCH)
		taskqueue_enqueue(sc->tq, &sc->rx_task);
}

/*
 * Process a Connac2 TX_FREE notification. The TXP token is queue*ring_size
 * + descriptor_index, so it is unique across all four host data queues.
 * Each non-pair record releases one original mbuf/node held until firmware
 * reports an over-the-air result.
 */
static void
mt7902_rx_tx_free(struct mt7902_softc *sc, const uint8_t *data, size_t len)
{
	struct ieee80211_node *ni;
	struct mbuf *m;
	uint32_t info;
	uint16_t count;
	uint16_t msdu_id;
	uint8_t status;
	size_t offset, max_records;
	uint64_t records, failures, retries, unknown, pairs;
	unsigned int expected, processed, attempts;

	records = failures = retries = unknown = pairs = 0;
	if (len < 8) {
		mtx_lock(&sc->mtx);
		sc->tx_free_bad++;
		mtx_unlock(&sc->mtx);
		return;
	}

	count = le16dec(data + 2) & MT7902_TX_FREE_MSDU_CNT_MASK;
	offset = 8; /* struct mt76_connac_tx_free */
	max_records = (len - offset) / sizeof(uint32_t);
	expected = count;
	if (expected > max_records) {
		mtx_lock(&sc->mtx);
		sc->tx_free_bad++;
		mtx_unlock(&sc->mtx);
		return;
	}

	for (processed = 0; processed < expected; processed++) {
		if (offset + sizeof(uint32_t) > len) {
			mtx_lock(&sc->mtx);
			sc->tx_free_bad++;
			mtx_unlock(&sc->mtx);
			break;
		}
		info = le32dec(data + offset);
		offset += sizeof(uint32_t);

		/* A pair record establishes the WCID for following MSDUs. */
		if ((info & MT7902_TX_FREE_PAIR) != 0) {
			pairs++;
			expected++;
			if (expected > max_records) {
				mtx_lock(&sc->mtx);
				sc->tx_free_bad++;
				mtx_unlock(&sc->mtx);
				break;
			}
			continue;
		}

		msdu_id = (info >> MT7902_TX_FREE_MSDU_ID_SHIFT) & 0x7fff;
		status = (info >> MT7902_TX_FREE_STATUS_SHIFT) &
		    MT7902_TX_FREE_STATUS_MASK;
		attempts = info & MT7902_TX_FREE_COUNT_MASK;
		records++;
		if (status != 0)
			failures++;
		if (attempts > 0)
			retries += attempts - 1;

		if (msdu_id >= MT7902_TX_PENDING_COUNT) {
			unknown++;
			continue;
		}
		mtx_lock(&sc->mtx);
		if (!sc->tx_pending[msdu_id].active) {
			unknown++;
			mtx_unlock(&sc->mtx);
			continue;
		}
		m = sc->tx_pending[msdu_id].m;
		ni = sc->tx_pending[msdu_id].ni;
		bzero(&sc->tx_pending[msdu_id],
		    sizeof(sc->tx_pending[msdu_id]));
		if (sc->tx_pending_count != 0)
			sc->tx_pending_count--;
		mtx_unlock(&sc->mtx);

		/* ieee80211_tx_complete may acquire net80211 locks. */
		if (m != NULL) {
			if (ni != NULL)
				ieee80211_tx_complete(ni, m, status != 0);
			else
				m_freem(m);
		} else if (ni != NULL) {
			ieee80211_free_node(ni);
		}
	}

	mtx_lock(&sc->mtx);
	sc->tx_free_records += records;
	sc->tx_free_failures += failures;
	sc->tx_free_retries += retries;
	sc->tx_free_unknown += unknown;
	sc->tx_free_pairs += pairs;
	mtx_unlock(&sc->mtx);
}

#define MT7902_TXS0_ACK_ERROR_MASK (7U << 16)
#define MT7902_TX_RATE_IDX_MASK       0x3fU
#define MT7902_TX_RATE_NSS_MASK       (7U << 10)
#define MT7902_TX_RATE_MODE_MASK      (0x0fU << 6)
#define MT7902_TX_RATE_STBC           (1U << 13)

/*
 * Feed Connac2 TXS results to FreeBSD net80211. Firmware still performs
 * packet-by-packet fallback using the peer's negotiated RA mask; this path
 * publishes the observed final rate and lets the selected net80211 ratectl
 * consume ACK/retry outcomes for its node statistics/adaptive state.
 */
static void
mt7902_txs_rate_update(struct mt7902_softc *sc, uint16_t wcid,
    uint32_t txs0, uint16_t fail_count, uint16_t retry_count,
    uint32_t tx_bytes)
{
	static const uint8_t cck_rates[] = { 2, 4, 11, 22 };
	static const uint8_t ofdm_rates[] = {
		12, 18, 24, 36, 48, 72, 96, 108
	};
	struct ieee80211_ratectl_tx_status status;
	struct ieee80211_node *ni;
	uint32_t tx_rate;
	uint8_t mode, idx, nss, retries;
	int final_rate;
	bool have_final_rate;

	if (wcid != MT7902_PEER_WCID)
		return;

	mtx_lock(&sc->mtx);
	ni = sc->station_node != NULL ?
	    ieee80211_ref_node(sc->station_node) : NULL;
	mtx_unlock(&sc->mtx);
	if (ni == NULL)
		return;

	tx_rate = txs0 & 0x3fffU;
	idx = tx_rate & MT7902_TX_RATE_IDX_MASK;
	mode = (tx_rate & MT7902_TX_RATE_MODE_MASK) >> 6;
	nss = ((tx_rate & MT7902_TX_RATE_NSS_MASK) >> 10) + 1;
	if ((tx_rate & MT7902_TX_RATE_STBC) != 0 && nss > 1)
		nss >>= 1;

	bzero(&status, sizeof(status));
	status.flags = IEEE80211_RATECTL_STATUS_LONG_RETRY;
	status.pktlen = (int)MIN(tx_bytes, (uint32_t)INT_MAX);
	retries = (uint8_t)MIN((uint32_t)retry_count, (uint32_t)UINT8_MAX);
	if (fail_count != 0 && retries != UINT8_MAX)
		retries++;
	status.long_retries = retries;
	status.status = (txs0 & MT7902_TXS0_ACK_ERROR_MASK) == 0 ?
	    IEEE80211_RATECTL_TX_SUCCESS : IEEE80211_RATECTL_TX_FAIL_LONG;
	final_rate = 0;
	have_final_rate = false;

	switch (mode) {
	case MT7902_PHY_CCK:
		if (idx >= nitems(cck_rates))
			break;
		ieee80211_node_set_txrate_dot11rate(ni, cck_rates[idx]);
		final_rate = cck_rates[idx];
		have_final_rate = true;
		break;
	case MT7902_PHY_OFDM:
		if (idx >= nitems(ofdm_rates))
			break;
		ieee80211_node_set_txrate_dot11rate(ni, ofdm_rates[idx]);
		final_rate = ofdm_rates[idx];
		have_final_rate = true;
		break;
	case MT7902_PHY_HT:
	case MT7902_PHY_HT_GF:
		if (idx > 31 || (ni->ni_flags & IEEE80211_NODE_HT) == 0)
			break;
		ieee80211_node_set_txrate_ht_mcsrate(ni, idx);
		final_rate = IEEE80211_RATE_MCS | idx;
		have_final_rate = true;
		break;
	case MT7902_PHY_VHT:
		if (idx > 9 || nss == 0 ||
		    (ni->ni_flags & IEEE80211_NODE_VHT) == 0)
			break;
		ieee80211_node_set_txrate_vht_rate(ni, nss, idx);
		/*
		 * FreeBSD's generic ratectl final_rate field has no NSS component.
		 * Publish the VHT rate in ni_txrate, but do not mislabel it as HT.
		 */
		break;
	default:
		break;
	}

	if (have_final_rate) {
		status.flags |= IEEE80211_RATECTL_STATUS_FINAL_RATE;
		status.final_rate = final_rate;
	}
	ieee80211_ratectl_tx_complete(ni, &status);
	ieee80211_free_node(ni);
}

/*
 * RX ring 0 is shared by MCU events and TX status notifications on MT7902.
 * Dispatch by RXD0 packet type just as mt7921_queue_rx_skb() does. In
 * particular, TXS notifications can be shorter than a full MCU RXD header;
 * treating every descriptor as a 36-byte MCU event caused the repeated
 * "invalid MCU event descriptor length (16 bytes)" message.
 */
static void
mt7902_rx_event_poll(struct mt7902_softc *sc)
{
	struct mt7902_ring *ring;
	struct mt7902_dma_desc *desc;
	struct mt7902_dma_buffer *buf;
	uint8_t *data;
	uint16_t mcu_len;
	uint32_t control, rxd0, txs0, txs2, txs3, txs5, txs6, txs7;
	uint16_t txs_wcid, txs_fail_count, txs_retry_count;
	uint8_t txs_pid, txs_status;
	size_t dma_len, event_len, offset;
	int idx, count, error;
	uint8_t seq, eid, pkt_type, pkt_flag;
	bool log_short, log_unknown, scanning;
	int debug;

	ring = &sc->rx_event;
	if (ring->desc == NULL)
		return;
	for (count = 0; count < MIN(ring->ndesc, MT7902_RX_BATCH); count++) {
		idx = ring->tail;
		desc = &ring->desc[idx];
		bus_dmamap_sync(ring->desc_tag, ring->desc_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		control = le32toh(desc->control);
		if ((control & MT7902_DMA_DESC_DONE) == 0)
			break;
		buf = &ring->buf[idx];
		bus_dmamap_sync(ring->buf_tag, buf->map, BUS_DMASYNC_POSTREAD);
		data = buf->vaddr;
		dma_len = (control >> MT7902_RX_LEN_SHIFT) & MT7902_RX_LEN_MASK;
		log_short = false;
		log_unknown = false;

		if (dma_len < sizeof(rxd0) || dma_len > buf->len) {
			mtx_lock(&sc->mtx);
			sc->rx_event_short_count++;
			log_short = sc->rx_event_short_count == 1;
			mtx_unlock(&sc->mtx);
			if (log_short)
				device_printf(sc->dev,
				    "short RX ring0 descriptor: len=%ju buffer=%ju\n",
				    (uintmax_t)dma_len, (uintmax_t)buf->len);
			goto rearm;
		}

		rxd0 = le32dec(data);
		pkt_type = (rxd0 >> 27) & 0x1f;
		pkt_flag = (rxd0 >> 16) & 0x0f;
		/* RX_EVENT with flag 1 is a normal MCU RX frame, not an event. */
		if (pkt_type == MT7902_PKT_TYPE_RX_EVENT && pkt_flag == 1)
			pkt_type = MT7902_PKT_TYPE_NORMAL_MCU;

		mtx_lock(&sc->mtx);
		sc->rx_event_count++;
		sc->rx_event_last_rxd0 = rxd0;
		sc->rx_event_last_len = (uint16_t)dma_len;
		sc->rx_event_last_type = pkt_type;
		if (pkt_type < nitems(sc->rx_event_type_count))
			sc->rx_event_type_count[pkt_type]++;
		debug = sc->rx_debug;
		scanning = sc->scan_active;
		mtx_unlock(&sc->mtx);
		if (debug >= 2 && scanning)
			device_printf(sc->dev,
			    "RX EVENT DESC idx=%d control=%08x len=%ju RXD0=%08x type=%u flag=%u hwidx=%08x\n",
			    idx, control, (uintmax_t)dma_len, rxd0, pkt_type,
			    pkt_flag, mt7902_read(sc, ring->reg_base + 0x0c));

		switch (pkt_type) {
		case MT7902_PKT_TYPE_TXS:
			/*
			 * TXS has a two-dword header followed by zero or more
			 * eight-dword records. A 16-byte descriptor contains only
			 * the header; it is valid and simply has no complete TXS
			 * record to consume.
			 */
			for (offset = 8; offset + 32 <= dma_len; offset += 32) {
				txs0 = le32dec(data + offset);
				/* mt7921_mac_add_txs() handles TXS formats 0 and 1. */
				if (((txs0 >> 23) & 0x3) > 1)
					continue;
				txs2 = le32dec(data + offset + 8);
				txs3 = le32dec(data + offset + 12);
				txs5 = le32dec(data + offset + 20);
				txs6 = le32dec(data + offset + 24);
				txs7 = le32dec(data + offset + 28);
				txs_wcid = (txs2 >> 16) & 0x3ff;
				txs_pid = (txs3 >> 24) & 0xff;
				txs_status = (txs0 >> 16) & 0x7;
				txs_fail_count = (txs6 >> 23) & 0x1ff;
				txs_retry_count = (txs7 >> 23) & 0x1ff;
				mtx_lock(&sc->mtx);
				sc->rx_event_txs_records++;
				if (txs_status != 0 || txs_fail_count != 0)
					sc->rx_event_txs_failures++;
				if (txs_status & (1U << 0))
					sc->rx_event_txs_ack_timeouts++;
				if (txs_status & (1U << 1))
					sc->rx_event_txs_rts_timeouts++;
				if (txs_status & (1U << 2))
					sc->rx_event_txs_queue_timeouts++;
				sc->rx_event_txs_retries += txs_retry_count;
				sc->rx_event_txs_bytes += txs5 & 0x7fffff;
				sc->rx_event_txs_last_wcid = txs_wcid;
				sc->rx_event_txs_last_pid = txs_pid;
				sc->rx_event_txs_last_status = txs_status;
				sc->rx_event_txs_last_rate = txs0 & 0x3fff;
				sc->rx_event_txs_last_fail_count = txs_fail_count;
				sc->rx_event_txs_last_retry_count = txs_retry_count;
				mtx_unlock(&sc->mtx);
				mt7902_txs_rate_update(sc, txs_wcid, txs0,
				    txs_fail_count, txs_retry_count,
				    txs5 & 0x7fffffU);
			}
			mtx_lock(&sc->mtx);
			sc->rx_event_txs_count++;
			mtx_unlock(&sc->mtx);
			break;

		case MT7902_PKT_TYPE_TXRX_NOTIFY:
			mtx_lock(&sc->mtx);
			sc->rx_event_tx_free_count++;
			mtx_unlock(&sc->mtx);
			mt7902_rx_tx_free(sc, data, dma_len);
			break;

		case MT7902_PKT_TYPE_NORMAL:
		case MT7902_PKT_TYPE_NORMAL_MCU:
			mt7902_rx_data_frame(sc, data, dma_len);
			break;

		case MT7902_PKT_TYPE_RX_EVENT:
			if (dma_len < MT7902_RXD_SIZE) {
				mtx_lock(&sc->mtx);
				sc->rx_event_short_count++;
				log_short = sc->rx_event_short_count == 1;
				mtx_unlock(&sc->mtx);
				if (log_short)
					device_printf(sc->dev,
					    "short MCU RX event: len=%ju RXD0=%08x flag=%u\n",
					    (uintmax_t)dma_len, rxd0, pkt_flag);
				break;
			}

			event_len = dma_len - MT7902_RXD_SIZE;
			memcpy(&mcu_len, data + MT7902_RXD_LEN_OFFSET,
			    sizeof(mcu_len));
			eid = data[MT7902_RXD_EID_OFFSET];
			seq = data[MT7902_RXD_SEQ_OFFSET];
			mtx_lock(&sc->mtx);
			if (sc->mcu_waiting && seq == sc->mcu_wait_seq) {
				device_printf(sc->dev,
				    "MCU RX response cmd=%02x seq=%u eid=%02x len=%u status=%02x\n",
				    sc->mcu_wait_cmd, seq, eid,
				    le16toh(mcu_len), data[MT7902_RXD_SIZE - 4]);
				/*
				 * PATCH_SEM_CONTROL and PATCH_FINISH_REQ return
				 * their status in the final word of the RXD header,
				 * as mt7921_mcu_parse_response() specifies.
				 */
				if (sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_SEM ||
				    sc->mcu_wait_cmd == MT7902_MCU_CMD_PATCH_FINISH)
					sc->mcu_error = data[MT7902_RXD_SIZE - 4];
				else
					sc->mcu_error = 0;
				sc->mcu_response_len = MIN(event_len,
				    MIN((size_t)le16toh(mcu_len),
				    sizeof(sc->mcu_response)));
				if (sc->mcu_response_len != 0)
					memcpy(sc->mcu_response,
					    data + MT7902_RXD_SIZE,
				    sc->mcu_response_len);
				sc->mcu_waiting = 0;
				cv_broadcast(&sc->mcu_cv);
			}
			mtx_unlock(&sc->mtx);
			break;

		default:
			mtx_lock(&sc->mtx);
			sc->rx_event_unknown_count++;
			log_unknown = sc->rx_event_unknown_count == 1;
			mtx_unlock(&sc->mtx);
			if (log_unknown)
				device_printf(sc->dev,
				    "unhandled RX ring0 packet type=%u flag=%u len=%ju RXD0=%08x\n",
				    pkt_type, pkt_flag, (uintmax_t)dma_len, rxd0);
			break;
		}

rearm:
		error = mt7902_rx_rearm(sc, ring, idx);
		if (error != 0) {
			device_printf(sc->dev,
			    "RX event ring cursor collision at descriptor %d\n", idx);
			break;
		}
	}
	if (count == MT7902_RX_BATCH)
		taskqueue_enqueue(sc->tq, &sc->event_task);
}

static void
mt7902_intr(void *arg)
{
	struct mt7902_softc *sc;
	uint32_t status;
	int bit;

	sc = arg;
	status = mt7902_read(sc, MT7902_REG_WFDMA_INT_STA);
	if (status == 0 || status == 0xffffffff)
		return;
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, status);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
	mtx_lock(&sc->mtx);
	if (sc->irq_quiescing) {
		mtx_unlock(&sc->mtx);
		return;
	}
	sc->irq_status |= status;
	sc->irq_last_status = status;
	sc->irq_count++;
	for (bit = 0; bit < 32; bit++)
		if ((status & (1U << bit)) != 0)
			sc->irq_bit_count[bit]++;
	mtx_unlock(&sc->mtx);
	taskqueue_enqueue(sc->tq, &sc->irq_task);
}

int
mt7902_irq_setup(struct mt7902_softc *sc)
{
	int error, count;

	count = 1;
	error = pci_alloc_msi(sc->dev, &count);
	if (error == 0)
		sc->msi_count = count;
	else
		sc->msi_count = 0;
	sc->irq_rid = sc->msi_count != 0 ? 1 : 0;
	sc->irq = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &sc->irq_rid,
	    RF_ACTIVE | (sc->msi_count == 0 ? RF_SHAREABLE : 0));
	if (sc->irq == NULL) {
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (ENXIO);
	}
	error = bus_setup_intr(sc->dev, sc->irq,
	    INTR_TYPE_NET | INTR_MPSAFE, NULL, mt7902_intr, sc,
	    &sc->irq_cookie);
	if (error != 0) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
		if (sc->msi_count != 0)
			pci_release_msi(sc->dev);
		sc->msi_count = 0;
		return (error);
	}
	mtx_lock(&sc->mtx);
	sc->irq_quiescing = false;
	mtx_unlock(&sc->mtx);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, 0xffffffff);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
	return (0);
}

void
mt7902_irq_teardown(struct mt7902_softc *sc)
{
	mtx_lock(&sc->mtx);
	sc->irq_quiescing = true;
	mtx_unlock(&sc->mtx);
	if (sc->mem != NULL) {
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN, 0);
		/* Keep the PCIe MAC forwarding gate symmetric with DMA startup. */
		mt7902_write(sc, MT7902_REG_PCIE_MAC_INT_ENABLE, 0);
	}
	if (sc->irq_cookie != NULL) {
		bus_teardown_intr(sc->dev, sc->irq, sc->irq_cookie);
		sc->irq_cookie = NULL;
	}
	/* A queued worker may still be consuming an RX ring. Drain it only
	 * after the interrupt source is detached, and before the caller frees
	 * ring memory.
	 */
	if (sc->tq != NULL) {
		taskqueue_drain(sc->tq, &sc->irq_task);
		taskqueue_drain(sc->tq, &sc->event_task);
		taskqueue_drain(sc->tq, &sc->rx_task);
		taskqueue_drain(sc->tq, &sc->tx_task);
	}
	if (sc->irq != NULL) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
	}
	if (sc->msi_count != 0) {
		pci_release_msi(sc->dev);
		sc->msi_count = 0;
	}
}

void
mt7902_irq_task(void *arg, int pending __unused)
{
	struct mt7902_softc *sc;
	uint32_t status, intr_sw;
	int debug;

	sc = arg;
	mtx_lock(&sc->mtx);
	if (sc->irq_quiescing) {
		sc->irq_status = 0;
		mtx_unlock(&sc->mtx);
		return;
	}
	status = sc->irq_status;
	sc->irq_status = 0;
	debug = sc->rx_debug;
	mtx_unlock(&sc->mtx);
	if (debug >= 3)
		device_printf(sc->dev,
		    "IRQ TASK status=%08x int_sta=%08x int_en=%08x mcu_cmd=%08x rx_data_idx=%08x rx_event_idx=%08x\n",
		    status, mt7902_read(sc, MT7902_REG_WFDMA_INT_STA),
		    mt7902_read(sc, MT7902_REG_WFDMA_INT_EN),
		    mt7902_read(sc, MT7902_REG_MCU_CMD),
		    mt7902_read(sc, sc->rx_data.reg_base + 0x0c),
		    mt7902_read(sc, sc->rx_event.reg_base + 0x0c));

	/*
	 * On PCIe the MCU can signal that data RX should be serviced through
	 * the software interrupt register rather than the host RX-done bit.
	 * This is the MT792x MCU_CMD_WAKE_RX_PCIE path.
	 */
	if (status & MT7902_INT_MCU_CMD) {
		intr_sw = mt7902_read(sc, MT7902_REG_MCU_CMD);
		if (intr_sw != 0 && intr_sw != 0xffffffff) {
			mt7902_write(sc, MT7902_REG_MCU_CMD, intr_sw);
			if (intr_sw & MT7902_MCU_CMD_WAKE_RX_PCIE)
				status |= MT7902_INT_RX_DONE_DATA;
		}
	}
	if (status & MT7902_INT_RX_DONE_WM)
		taskqueue_enqueue(sc->tq, &sc->event_task);
	/*
	 * MT7902 shares RX ring0 between WM events and TX status. Poll ring2
	 * after either RX interrupt: some firmware revisions raise the WM
	 * interrupt while data descriptors are already complete, and the
	 * dedicated ring2 interrupt can be coalesced with it.
	 */
	if (status & MT7902_INT_RX_DONE_WM &&
	    (status & MT7902_INT_RX_DONE_DATA) == 0) {
		mtx_lock(&sc->mtx);
		sc->rx_data_fallback_poll_count++;
		mtx_unlock(&sc->mtx);
	}
	if (status & (MT7902_INT_RX_DONE_DATA | MT7902_INT_RX_DONE_WM))
		taskqueue_enqueue(sc->tq, &sc->rx_task);
	mtx_lock(&sc->mtx);
	if (!sc->irq_quiescing)
		mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
		    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
	mtx_unlock(&sc->mtx);
}

void
mt7902_event_task(void *arg __unused, int pending __unused)
{
	struct mt7902_softc *sc = arg;

	mt7902_rx_event_poll(sc);
}

void
mt7902_rx_task(void *arg __unused, int pending __unused)
{
	struct mt7902_softc *sc = arg;

	mt7902_rx_data_poll(sc);
}
