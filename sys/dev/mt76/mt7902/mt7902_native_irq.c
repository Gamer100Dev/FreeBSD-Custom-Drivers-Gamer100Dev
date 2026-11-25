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

#include <net80211/ieee80211_freebsd.h>
#include <net80211/ieee80211_proto.h>

#define MT7902_RXD_SIZE	36
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
	struct mbuf *m;
	uint32_t rxd0, rxd1, rxd2, rxd3, rxd4, rcpi_word, sec_mode;
	uint16_t fc;
	uint8_t channel, rcpi, subtype, pkt_type, pkt_flag, tap_channel;
	uint8_t frame_type;
	uint16_t tap_freq, tap_chan_flags;
	int rssi, noise, debug;
	size_t offset, frame_len, hdr_len;
	enum mt7902_rx_drop_reason drop_reason;
	bool is_mgmt, monitor_active, rcpi_valid, monitor_only;
	bool scanning, attached;

	rxd0 = rxd1 = rxd2 = rxd3 = rxd4 = rcpi_word = sec_mode = 0;
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
	 * Reject header-translated Ethernet frames: this net80211 path expects
	 * native 802.11 headers. HDR_TRANS_ERROR (RXD2 bit 25) is not a
	 * standalone RX error; mt76 only interprets it in conjunction with
	 * HDR_TRANS. Do not discard ordinary 802.11 frames when that bit is
	 * independently set by the device.
	 */
	if ((rxd1 & (1U << 28)) != 0) {
		drop_reason = MT7902_RX_DROP_HDR_TRANS;
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
	if ((rxd2 & ((1U << 13) | (1U << 23) | (1U << 24))) != 0) {
		drop_reason = MT7902_RX_DROP_RXD_ERROR;
		goto drop;
	}
	/* RXD4 payload format != 0 describes a deaggregated A-MSDU. */
	if ((rxd4 & 0x3) != 0) {
		drop_reason = MT7902_RX_DROP_PAYLOAD_FORMAT;
		goto drop;
	}

	offset = 24;
	if (rxd1 & (1U << 14))
		offset += 16;             /* RXD GROUP_4 */
	if (rxd1 & (1U << 11))
		offset += 16;             /* RXD GROUP_1 */
	if (rxd1 & (1U << 12))
		offset += 8;              /* RXD GROUP_2 */

	rcpi = 0;
	rcpi_valid = false;
	if (rxd1 & (1U << 13)) {
		if (offset > dma_len || dma_len - offset < 8) {
			drop_reason = MT7902_RX_DROP_GROUP_TRUNCATED;
			goto drop;
		}
		rcpi_word = le32dec(data + offset + 4);
		rcpi = rcpi_word & 0xff;  /* PRXV RCPI0 */
		rcpi_valid = true;
		offset += 8;              /* RXD GROUP_3 */
		if (rxd1 & (1U << 15)) {
			if (offset > dma_len || dma_len - offset < 72) {
				drop_reason = MT7902_RX_DROP_GROUP_TRUNCATED;
				goto drop;
			}
			offset += 24;         /* GROUP_5 prefix */
			rcpi_word = le32dec(data + offset);
			rcpi = rcpi_word & 0xff;
			rcpi_valid = true;
			offset += 48;         /* GROUP_5 RX vector */
		}
	}
	/* RXD2 HDR_OFFSET is measured in 16-bit words. */
	offset += 2 * ((rxd2 >> 14) & 0x3);
	if (offset >= dma_len) {
		drop_reason = MT7902_RX_DROP_BAD_OFFSET;
		goto drop;
	}
	frame_len = dma_len - offset;
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
	/*
	 * mt7921_mac_fill_rx() reports a nonzero hardware cipher with no
	 * CLM/CM exception as authenticated/decrypted data.  Tell net80211
	 * that the hardware removed IV/MIC bytes so its software CCMP path
	 * does not try to decrypt a second time.
	 */
	sec_mode = (rxd1 >> 16) & 0x1f;
	if (sec_mode != 0 && (rxd1 & ((1U << 23) | (1U << 24))) == 0)
		rxs.c_pktflags |= IEEE80211_RX_F_DECRYPTED |
		    IEEE80211_RX_F_IV_STRIP | IEEE80211_RX_F_MMIC_STRIP |
		    IEEE80211_RX_F_MIC_STRIP;
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
	for (count = 0; count < ring->ndesc; count++) {
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
	for (count = 0; count < ring->ndesc; count++) {
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
	mt7902_write(sc, MT7902_REG_WFDMA_INT_STA, 0xffffffff);
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
	return (0);
}

void
mt7902_irq_teardown(struct mt7902_softc *sc)
{
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
		mt7902_rx_event_poll(sc);
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
	mt7902_write(sc, MT7902_REG_WFDMA_INT_EN,
	    MT7902_INT_MASK | MT7902_INT_MCU_CMD);
}

void
mt7902_rx_task(void *arg __unused, int pending __unused)
{
	struct mt7902_softc *sc = arg;

	mt7902_rx_data_poll(sc);
}
