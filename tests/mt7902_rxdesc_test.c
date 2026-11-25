#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "../sys/dev/mt76/mt7902/mt7902_native_rxdesc.h"

static void
test_base_layout(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(0, 0, 80, &layout) == 0);
	assert(layout.group4_offset == 0);
	assert(layout.group2_offset == 0);
	assert(layout.group3_offset == 0);
	assert(layout.group5_rcpi_offset == 0);
	assert(layout.frame_offset == 24);
}

static void
test_all_optional_groups(void)
{
	struct mt7902_rxdesc_layout layout;
	uint32_t groups;

	groups = MT7902_RXD1_GROUP_4 | MT7902_RXD1_GROUP_1 |
	    MT7902_RXD1_GROUP_2 | MT7902_RXD1_GROUP_3 |
	    MT7902_RXD1_GROUP_5;
	assert(mt7902_rxdesc_get_layout(groups, 2U << 14, 160,
	    &layout) == 0);
	assert(layout.group4_offset == 24);
	assert(layout.group2_offset == 56);
	assert(layout.group3_offset == 64);
	assert(layout.group5_rcpi_offset == 96);
	assert(layout.frame_offset == 148);
}

static void
test_truncated_groups(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_4, 0, 39,
	    &layout) != 0);
	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_3, 0, 31,
	    &layout) != 0);
	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_3 |
	    MT7902_RXD1_GROUP_5, 0, 103, &layout) != 0);
	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_5, 0, 160,
	    &layout) != 0);
}

static void
test_group2_offset_and_truncation(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_2, 0, 40,
	    &layout) == 0);
	assert(layout.group2_offset == 24);
	assert(layout.frame_offset == 32);
	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_2, 0, 31,
	    &layout) != 0);
}

static void
test_wifi5_prxv_decode(void)
{
	struct mt7902_rx_phy phy;
	uint32_t word;

	word = 7U | (1U << 7) | (2U << 12) | (1U << 15) |
	    MT7902_PRXV_LDPC | (MT7902_PHY_VHT << 24);
	assert(mt7902_rxdesc_decode_phy(word, &phy) == 0);
	assert(phy.rate_idx == 7 && phy.nss == 2);
	assert(phy.mode == MT7902_PHY_VHT && phy.bandwidth == 2);
	assert(phy.gi == 1 && phy.ldpc && phy.stbc == 0);

	word = 9U | (3U << 7) | (1U << 22) |
	    (MT7902_PHY_HT << 24);
	assert(mt7902_rxdesc_decode_phy(word, &phy) == 0);
	assert(phy.rate_idx == 9 && phy.nss == 2);
	assert(phy.mode == MT7902_PHY_HT && phy.bandwidth == 0);
	assert(phy.stbc == 1);

	word = (3U << 12) | (MT7902_PHY_VHT << 24);
	assert(mt7902_rxdesc_decode_phy(word, &phy) != 0);
	word = (10U << 0) | (MT7902_PHY_VHT << 24);
	assert(mt7902_rxdesc_decode_phy(word, &phy) != 0);
	word = (5U << 24);
	assert(mt7902_rxdesc_decode_phy(word, &phy) != 0);
	assert(mt7902_rxdesc_decode_phy(0, NULL) != 0);
}

static void
test_amsdu_descriptor_metadata(void)
{
	struct mt7902_rx_amsdu amsdu;

	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_NONE, 1, 512, 26, &amsdu) == 0);
	assert(!amsdu.present && !amsdu.more && amsdu.frame_len == 512);

	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_AMSDU_FIRST, 1, 512, 26, &amsdu) == 0);
	assert(amsdu.present && amsdu.more && amsdu.frame_len == 510);
	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_AMSDU_MIDDLE, 1, 512, 26, &amsdu) == 0);
	assert(amsdu.present && amsdu.more && amsdu.frame_len == 510);
	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_AMSDU_LAST, 1, 512, 26, &amsdu) == 0);
	assert(amsdu.present && !amsdu.more && amsdu.frame_len == 510);

	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_AMSDU_FIRST, 0, 512, 26, &amsdu) != 0);
	assert(mt7902_rxdesc_decode_amsdu(
	    MT7902_RXD4_PAYLOAD_AMSDU_LAST, 1, 27, 26, &amsdu) != 0);
	assert(mt7902_rxdesc_decode_amsdu(0, 1, 25, 26, &amsdu) != 0);
	assert(mt7902_rxdesc_decode_amsdu(0, 1, 64, 26, NULL) != 0);
}

static void
test_group4_offset_and_truncation(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_4, 0, 64,
	    &layout) == 0);
	assert(layout.group4_offset == 24);
	assert(layout.frame_offset == 40);
	assert(mt7902_rxdesc_get_layout(MT7902_RXD1_GROUP_4, 0, 39,
	    &layout) != 0);
}

static void
test_header_offset_bounds(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(0, 3U << 14, 27, &layout) != 0);
	assert(mt7902_rxdesc_get_layout(0, 3U << 14, 32, &layout) == 0);
	assert(layout.frame_offset == 30);
}

static void
test_rxd2_error_semantics_and_observed_scan_descriptor(void)
{
	/*
	 * Captured from the user's scan log:
	 * RXD0=10000117 RXD1=80003bff RXD2=42000c4f
	 * RXD3=02022423 RXD4=c0000000.
	 *
	 * Bit 25 is HDR_TRANS_ERROR, but HDR_TRANS (bit 13) is clear. Linux
	 * mt76 treats this as a native frame, not an RX drop.
	 */
	uint32_t rxd2 = 0x42000c4fU;

	assert((rxd2 & MT7902_RXD2_HDR_TRANS) == 0);
	assert((rxd2 & MT7902_RXD2_HDR_TRANS_ERR) != 0);
	assert(!mt7902_rxdesc_has_fatal_error(rxd2));
	assert(!mt7902_rxdesc_hdr_trans_failed(rxd2));

	assert(mt7902_rxdesc_has_fatal_error(MT7902_RXD2_AMSDU_ERR));
	assert(mt7902_rxdesc_has_fatal_error(MT7902_RXD2_MAX_LEN_ERROR));
	assert(mt7902_rxdesc_hdr_trans_failed(
	    MT7902_RXD2_HDR_TRANS | MT7902_RXD2_HDR_TRANS_ERR));
	assert(!mt7902_rxdesc_hdr_trans_failed(MT7902_RXD2_HDR_TRANS));
}

static void
test_descriptor_bit_semantics(void)
{
	assert(MT7902_RXD1_BAND_IDX == (1U << 28));
	assert(MT7902_RXD2_HDR_TRANS == (1U << 13));
	assert(MT7902_RXD2_AMSDU_ERR == (1U << 23));
	assert(MT7902_RXD2_MAX_LEN_ERROR == (1U << 24));
	assert(MT7902_RXD2_HDR_TRANS_ERR == (1U << 25));
}

int
main(void)
{
	test_base_layout();
	test_all_optional_groups();
	test_truncated_groups();
	test_group2_offset_and_truncation();
	test_group4_offset_and_truncation();
	test_wifi5_prxv_decode();
	test_amsdu_descriptor_metadata();
	test_header_offset_bounds();
	test_descriptor_bit_semantics();
	test_rxd2_error_semantics_and_observed_scan_descriptor();
	puts("MT7902 RX descriptor layout tests: PASS");
	return (0);
}
