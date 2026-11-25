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
test_header_offset_bounds(void)
{
	struct mt7902_rxdesc_layout layout;

	assert(mt7902_rxdesc_get_layout(0, 3U << 14, 27, &layout) != 0);
	assert(mt7902_rxdesc_get_layout(0, 3U << 14, 32, &layout) == 0);
	assert(layout.frame_offset == 30);
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
	test_header_offset_bounds();
	test_descriptor_bit_semantics();
	puts("MT7902 RX descriptor layout tests: PASS");
	return (0);
}
