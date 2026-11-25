/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Portable Connac2 RX descriptor layout helpers.  This header is shared by
 * the kernel RX path and the host-side parser tests so the tests exercise the
 * exact offset calculation used by the driver.
 */
#ifndef _MT7902_NATIVE_RXDESC_H_
#define _MT7902_NATIVE_RXDESC_H_

#define MT7902_RXD_BASE_SIZE       24U
#define MT7902_RXD1_GROUP_1        (1U << 11)
#define MT7902_RXD1_GROUP_2        (1U << 12)
#define MT7902_RXD1_GROUP_3        (1U << 13)
#define MT7902_RXD1_GROUP_4        (1U << 14)
#define MT7902_RXD1_GROUP_5        (1U << 15)
#define MT7902_RXD1_BAND_IDX       (1U << 28)
#define MT7902_RXD2_HDR_TRANS      (1U << 13)
#define MT7902_RXD2_HDR_OFFSET     (3U << 14)
#define MT7902_RXD2_NON_AMPDU      (1U << 30)
#define MT7902_RXD4_PAYLOAD_FORMAT (3U << 0)
#define MT7902_RXD4_PAYLOAD_NONE   0U
#define MT7902_RXD4_PAYLOAD_AMSDU_FIRST 1U
#define MT7902_RXD4_PAYLOAD_AMSDU_MIDDLE 2U
#define MT7902_RXD4_PAYLOAD_AMSDU_LAST 3U
#define MT7902_RXD2_AMSDU_ERR      (1U << 23)
#define MT7902_RXD2_MAX_LEN_ERROR  (1U << 24)
#define MT7902_RXD2_HDR_TRANS_ERR  (1U << 25)

#define MT7902_PRXV_RATE_MASK       0x0000007fU
#define MT7902_PRXV_NSTS_MASK       0x00000380U
#define MT7902_PRXV_LDPC             (1U << 11)
#define MT7902_PRXV_BW_MASK         0x00007000U
#define MT7902_PRXV_GI_MASK         0x00018000U
#define MT7902_PRXV_STBC_MASK       0x00c00000U
#define MT7902_PRXV_MODE_MASK       0x0f000000U

#define MT7902_PHY_CCK              0U
#define MT7902_PHY_OFDM             1U
#define MT7902_PHY_HT               2U
#define MT7902_PHY_HT_GF            3U
#define MT7902_PHY_VHT              4U

struct mt7902_rx_phy {
	uint8_t rate_idx;
	uint8_t nss;
	uint8_t mode;
	uint8_t bandwidth;
	uint8_t gi;
	uint8_t stbc;
	uint8_t ldpc;
};

/*
 * Decode the Connac2 P-RXV first dword. This intentionally accepts only
 * legacy/HT/VHT and 20/40/80 MHz: HE and wider channels are outside this
 * FreeBSD Wi-Fi 5 implementation.
 */
static inline int
mt7902_rxdesc_decode_phy(uint32_t rxv0, struct mt7902_rx_phy *phy)
{
	if (phy == NULL)
		return (-1);
	phy->rate_idx = rxv0 & MT7902_PRXV_RATE_MASK;
	phy->nss = ((rxv0 & MT7902_PRXV_NSTS_MASK) >> 7) + 1;
	phy->ldpc = (rxv0 & MT7902_PRXV_LDPC) != 0;
	phy->bandwidth = (rxv0 & MT7902_PRXV_BW_MASK) >> 12;
	phy->gi = (rxv0 & MT7902_PRXV_GI_MASK) >> 15;
	phy->stbc = (rxv0 & MT7902_PRXV_STBC_MASK) >> 22;
	phy->mode = (rxv0 & MT7902_PRXV_MODE_MASK) >> 24;
	if (phy->mode > MT7902_PHY_VHT || phy->bandwidth > 2)
		return (-1);
	if ((phy->mode == MT7902_PHY_CCK && phy->rate_idx > 3) ||
	    (phy->mode == MT7902_PHY_OFDM && phy->rate_idx > 7) ||
	    ((phy->mode == MT7902_PHY_HT ||
	    phy->mode == MT7902_PHY_HT_GF) && phy->rate_idx > 31) ||
	    (phy->mode == MT7902_PHY_VHT && phy->rate_idx > 9))
		return (-1);
	if (phy->stbc != 0 && phy->nss > 1)
		phy->nss >>= 1;
	return (0);
}

struct mt7902_rxdesc_layout {
	size_t group2_offset;
	size_t group3_offset;
	size_t group5_rcpi_offset;
	size_t frame_offset;
};

/*
 * The optional Connac2 groups follow GROUP_4, GROUP_1, GROUP_2, GROUP_3,
 * then GROUP_5 (which is present only with GROUP_3).  GROUP_5 is 72 bytes:
 * a 24-byte prefix followed by 48 bytes of RX vector data.  The returned
 * frame_offset includes the RXD2 two-byte-word header offset.
 *
 * Returns zero for a complete, internally consistent descriptor and -1 for
 * a truncated/malformed descriptor.  The caller remains responsible for
 * checking the packet type, error flags, and actual 802.11 header length.
 */
static inline int
mt7902_rxdesc_get_layout(uint32_t rxd1, uint32_t rxd2, size_t dma_len,
    struct mt7902_rxdesc_layout *layout)
{
	size_t offset, hdr_offset;

	if (layout == NULL || dma_len < MT7902_RXD_BASE_SIZE)
		return (-1);

	layout->group2_offset = 0;
	layout->group3_offset = 0;
	layout->group5_rcpi_offset = 0;
	offset = MT7902_RXD_BASE_SIZE;

	if ((rxd1 & MT7902_RXD1_GROUP_4) != 0) {
		if (dma_len - offset < 16)
			return (-1);
		offset += 16;
	}
	if ((rxd1 & MT7902_RXD1_GROUP_1) != 0) {
		if (dma_len - offset < 16)
			return (-1);
		offset += 16;
	}
	if ((rxd1 & MT7902_RXD1_GROUP_2) != 0) {
		if (dma_len - offset < 8)
			return (-1);
		layout->group2_offset = offset;
		offset += 8;
	}
	if ((rxd1 & MT7902_RXD1_GROUP_5) != 0 &&
	    (rxd1 & MT7902_RXD1_GROUP_3) == 0)
		return (-1);
	if ((rxd1 & MT7902_RXD1_GROUP_3) != 0) {
		if (dma_len - offset < 8)
			return (-1);
		layout->group3_offset = offset;
		offset += 8;
		if ((rxd1 & MT7902_RXD1_GROUP_5) != 0) {
			if (dma_len - offset < 72)
				return (-1);
			layout->group5_rcpi_offset = offset + 24;
			offset += 72;
		}
	}

	hdr_offset = 2U * ((rxd2 & MT7902_RXD2_HDR_OFFSET) >> 14);
	if (offset > dma_len || hdr_offset > dma_len - offset)
		return (-1);
	offset += hdr_offset;
	if (offset >= dma_len)
		return (-1);
	layout->frame_offset = offset;
	return (0);
}

#endif /* _MT7902_NATIVE_RXDESC_H_ */
