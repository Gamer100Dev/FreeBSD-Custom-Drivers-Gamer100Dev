/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Connac2 RX header-translation reverse decoder. Converts the firmware's
 * Ethernet-style RX payload plus RXD group-4 metadata back to the native
 * 802.11 frame format required by FreeBSD net80211.
 */
#ifndef _MT7902_NATIVE_RXTRANS_H_
#define _MT7902_NATIVE_RXTRANS_H_

#include <stddef.h>
#include <stdint.h>

#define MT7902_RXTRANS_ETH_HDR_LEN 14U
#define MT7902_RXTRANS_HDR_MAX     36U
#define MT7902_RXTRANS_LLC_MAX     8U
#define MT7902_RXTRANS_FC_TYPE_MASK 0x000cU
#define MT7902_RXTRANS_FC_TYPE_DATA 0x0008U
#define MT7902_RXTRANS_FC_SUBTYPE_QOS 0x0080U
#define MT7902_RXTRANS_FC_TODS      0x0100U
#define MT7902_RXTRANS_FC_FROMDS    0x0200U
#define MT7902_RXTRANS_FC_ORDER     0x8000U
#define MT7902_RXTRANS_ETH_P_802_3_MIN 0x0600U
#define MT7902_RXTRANS_ETH_P_AARP   0x80f3U
#define MT7902_RXTRANS_ETH_P_IPX    0x8137U

struct mt7902_rx_translated {
	uint8_t header[MT7902_RXTRANS_HDR_MAX];
	uint8_t llc[MT7902_RXTRANS_LLC_MAX];
	size_t header_len;
	size_t llc_len;
	size_t payload_offset;
	size_t payload_len;
	size_t frame_len;
	uint16_t frame_control;
	uint16_t qos_control;
	uint8_t qos_data;
};

/*
 * Restore a translated unicast Ethernet payload to an 802.11 data MPDU.
 * Addresses follow the Connac2 reverse_frag0_hdr_trans mapping:
 * local address -> addr1, peer transmitter -> addr2, and addr3/addr4
 * reconstructed from DS direction and the Ethernet source/destination.
 *
 * For EtherType frames, restore RFC1042 SNAP; AARP/IPX use the bridge-tunnel
 * OUI. IEEE 802.3 length frames keep their LLC payload without a SNAP prefix.
 */
static inline int
mt7902_rxtrans_decode(const uint8_t *eth, size_t eth_len, uint16_t fc,
    uint16_t seq_ctrl, uint16_t qos_ctrl, uint32_t ht_control,
    const uint8_t local[6], const uint8_t peer[6], const uint8_t bssid[6],
    struct mt7902_rx_translated *out)
{
	uint16_t eth_type, ds;
	size_t offset, llc_len, payload_len;
	unsigned int i;
	int qos_data, addr4, ordered;

	if (eth == NULL || local == NULL || peer == NULL || bssid == NULL ||
	    out == NULL || eth_len < MT7902_RXTRANS_ETH_HDR_LEN)
		return (-1);
	if ((fc & MT7902_RXTRANS_FC_TYPE_MASK) !=
	    MT7902_RXTRANS_FC_TYPE_DATA)
		return (-1);

	qos_data = (fc & MT7902_RXTRANS_FC_SUBTYPE_QOS) != 0;
	ds = fc & (MT7902_RXTRANS_FC_TODS | MT7902_RXTRANS_FC_FROMDS);
	addr4 = ds == (MT7902_RXTRANS_FC_TODS | MT7902_RXTRANS_FC_FROMDS);
	ordered = (fc & MT7902_RXTRANS_FC_ORDER) != 0;

	for (i = 0; i < MT7902_RXTRANS_HDR_MAX; i++)
		out->header[i] = 0;
	for (i = 0; i < MT7902_RXTRANS_LLC_MAX; i++)
		out->llc[i] = 0;
	out->frame_control = fc;
	out->qos_control = qos_data ? qos_ctrl : 0;
	out->qos_data = (uint8_t)qos_data;

	out->header[0] = (uint8_t)fc;
	out->header[1] = (uint8_t)(fc >> 8);
	/* duration/id is zero for a data MPDU rebuilt from an Ethernet RXD. */
	for (i = 0; i < 6; i++) {
		out->header[4 + i] = local[i];
		out->header[10 + i] = peer[i];
	}
	switch (ds) {
	case 0:
		for (i = 0; i < 6; i++)
			out->header[16 + i] = bssid[i];
		break;
	case MT7902_RXTRANS_FC_FROMDS:
		for (i = 0; i < 6; i++)
			out->header[16 + i] = eth[6 + i];
		break;
	case MT7902_RXTRANS_FC_TODS:
		for (i = 0; i < 6; i++)
			out->header[16 + i] = eth[i];
		break;
	default:
		for (i = 0; i < 6; i++) {
			out->header[16 + i] = eth[i];
			out->header[24 + i] = eth[6 + i];
		}
		break;
	}
	out->header[22] = (uint8_t)seq_ctrl;
	out->header[23] = (uint8_t)(seq_ctrl >> 8);
	offset = 24;
	if (addr4)
		offset += 6;
	if (qos_data) {
		out->header[offset] = (uint8_t)qos_ctrl;
		out->header[offset + 1] = (uint8_t)(qos_ctrl >> 8);
		offset += 2;
	}
	if (ordered) {
		out->header[offset] = (uint8_t)ht_control;
		out->header[offset + 1] = (uint8_t)(ht_control >> 8);
		out->header[offset + 2] = (uint8_t)(ht_control >> 16);
		out->header[offset + 3] = (uint8_t)(ht_control >> 24);
		offset += 4;
	}
	if (offset > MT7902_RXTRANS_HDR_MAX)
		return (-1);
	out->header_len = offset;

	eth_type = ((uint16_t)eth[12] << 8) | eth[13];
	llc_len = 0;
	if (eth_type >= MT7902_RXTRANS_ETH_P_802_3_MIN) {
		out->llc[0] = 0xaa;
		out->llc[1] = 0xaa;
		out->llc[2] = 0x03;
		if (eth_type == MT7902_RXTRANS_ETH_P_AARP ||
		    eth_type == MT7902_RXTRANS_ETH_P_IPX)
			out->llc[5] = 0xf8;
		out->llc[6] = eth[12];
		out->llc[7] = eth[13];
		llc_len = MT7902_RXTRANS_LLC_MAX;
	}
	payload_len = eth_len - MT7902_RXTRANS_ETH_HDR_LEN;
	out->llc_len = llc_len;
	out->payload_offset = MT7902_RXTRANS_ETH_HDR_LEN;
	out->payload_len = payload_len;
	out->frame_len = out->header_len + llc_len + payload_len;
	return (0);
}

#endif /* _MT7902_NATIVE_RXTRANS_H_ */
