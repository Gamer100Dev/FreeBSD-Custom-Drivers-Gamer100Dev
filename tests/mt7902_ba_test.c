#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../sys/dev/mt76/mt7902/mt7902_native_ba.h"

static uint16_t
get_le16(const uint8_t *p)
{
	return ((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void
test_tx_originator_enable_disable(void)
{
	uint8_t ba[MT7902_BA_WTBL_LEN];
	uint8_t peer[6] = { 0x02, 1, 2, 3, 4, 5 };
	uint8_t sta[MT7902_BA_STA_REC_LEN];

	assert(mt7902_ba_encode_wtbl(ba, 5, 0x123, 32, peer, true,
	    true) == 0);
	assert(get_le16(ba) == MT7902_BA_WTBL_TAG);
	assert(get_le16(ba + 2) == MT7902_BA_WTBL_LEN);
	assert(ba[4] == 5 && ba[5] == MT7902_BA_TYPE_ORIGINATOR);
	assert(get_le16(ba + 8) == 0x123);
	assert(ba[10] == 1 && get_le16(ba + 12) == 32);
	assert(ba[14] == 0 && ba[20] == 0 && ba[27] == 0);

	assert(mt7902_ba_encode_wtbl(ba, 5, 0x123, 32, peer, true,
	    false) == 0);
	assert(get_le16(ba + 8) == 0 && ba[10] == 0);
	assert(get_le16(ba + 12) == 0);

	assert(mt7902_ba_encode_sta_rec(sta, 5, 0x123, 32, true,
	    true) == 0);
	assert(get_le16(sta) == MT7902_BA_STA_REC_TAG);
	assert(get_le16(sta + 2) == MT7902_BA_STA_REC_LEN);
	assert(sta[4] == 5 && sta[5] == MT7902_BA_TYPE_ORIGINATOR);
	assert(sta[6] == 0 && sta[7] == (1U << 5));
	assert(get_le16(sta + 8) == 0x123 && get_le16(sta + 10) == 32);

	assert(mt7902_ba_encode_sta_rec(sta, 5, 0x123, 32, true,
	    false) == 0);
	assert(sta[7] == 0 && get_le16(sta + 8) == 0);
	assert(get_le16(sta + 10) == 0);
}

static void
test_rx_recipient_enable_disable(void)
{
	uint8_t ba[MT7902_BA_WTBL_LEN];
	uint8_t peer[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t sta[MT7902_BA_STA_REC_LEN];

	assert(mt7902_ba_encode_wtbl(ba, 3, 0x456, 48, peer, false,
	    true) == 0);
	assert(ba[4] == 3 && ba[5] == MT7902_BA_TYPE_RECIPIENT);
	assert(get_le16(ba + 12) == 48);
	assert(memcmp(ba + 14, peer, sizeof(peer)) == 0);
	assert(ba[20] == 3 && ba[21] == 0 && ba[22] == 1 && ba[23] == 0);

	assert(mt7902_ba_encode_wtbl(ba, 3, 0, 0, peer, false,
	    false) == 0);
	assert(ba[5] == MT7902_BA_TYPE_RECIPIENT);
	assert(get_le16(ba + 12) == 0);
	assert(memcmp(ba + 14, peer, sizeof(peer)) == 0);
	assert(ba[20] == 3 && ba[22] == 1);

	assert(mt7902_ba_encode_sta_rec(sta, 3, 0x456, 48, false,
	    true) == 0);
	assert(sta[4] == 3 && sta[5] == MT7902_BA_TYPE_RECIPIENT);
	assert(sta[6] == 0 && sta[7] == (1U << 3));
	assert(get_le16(sta + 8) == 0x456 && get_le16(sta + 10) == 48);
}

static void
test_invalid_tid_and_output(void)
{
	uint8_t ba[MT7902_BA_WTBL_LEN];
	uint8_t sta[MT7902_BA_STA_REC_LEN];
	uint8_t peer[6] = { 0 };

	assert(mt7902_ba_encode_wtbl(ba, 16, 0, 1, peer, true,
	    true) != 0);
	assert(mt7902_ba_encode_wtbl(ba, 0, 0, 1, NULL, false,
	    true) != 0);
	assert(mt7902_ba_encode_wtbl(NULL, 0, 0, 1, peer, true,
	    true) != 0);
	assert(mt7902_ba_encode_sta_rec(sta, 16, 0, 1, true,
	    true) != 0);
	assert(mt7902_ba_encode_sta_rec(NULL, 0, 0, 1, true,
	    true) != 0);
}

int
main(void)
{
	test_tx_originator_enable_disable();
	test_rx_recipient_enable_disable();
	test_invalid_tid_and_output();
	puts("MT7902 Connac2 Block Ack encoder tests: PASS");
	return (0);
}
