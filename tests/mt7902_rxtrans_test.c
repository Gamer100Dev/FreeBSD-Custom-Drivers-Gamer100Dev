#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../sys/dev/mt76/mt7902/mt7902_native_rxtrans.h"

static void
make_eth(uint8_t *eth, uint16_t type)
{
	static const uint8_t dst[6] = { 0x02, 0x10, 0x20, 0x30, 0x40, 0x50 };
	static const uint8_t src[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

	memcpy(eth, dst, sizeof(dst));
	memcpy(eth + 6, src, sizeof(src));
	eth[12] = (uint8_t)(type >> 8);
	eth[13] = (uint8_t)type;
	for (unsigned int i = MT7902_RXTRANS_ETH_HDR_LEN; i < 64; i++)
		eth[i] = (uint8_t)i;
}

static void
test_from_ds_qos_snap_restore(void)
{
	uint8_t eth[64], local[6] = { 2, 1, 2, 3, 4, 5 };
	uint8_t peer[6] = { 2, 6, 7, 8, 9, 10 };
	uint8_t bssid[6] = { 2, 11, 12, 13, 14, 15 };
	struct mt7902_rx_translated out;
	uint16_t fc = MT7902_RXTRANS_FC_FROMDS |
	    MT7902_RXTRANS_FC_TYPE_DATA | MT7902_RXTRANS_FC_SUBTYPE_QOS;

	make_eth(eth, 0x0800);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), fc, 0x1230,
	    5, 0, local, peer, bssid, &out) == 0);
	assert(out.header_len == 26 && out.llc_len == 8);
	assert(out.frame_len == 26 + 8 + 50);
	assert(out.header[0] == (uint8_t)fc &&
	    out.header[1] == (uint8_t)(fc >> 8));
	assert(memcmp(out.header + 4, local, 6) == 0);
	assert(memcmp(out.header + 10, peer, 6) == 0);
	assert(memcmp(out.header + 16, eth + 6, 6) == 0);
	assert(out.header[22] == 0x30 && out.header[23] == 0x12);
	assert(out.header[24] == 5 && out.header[25] == 0);
	assert(out.llc[0] == 0xaa && out.llc[1] == 0xaa &&
	    out.llc[2] == 3 && out.llc[3] == 0 &&
	    out.llc[4] == 0 && out.llc[5] == 0);
	assert(out.llc[6] == 0x08 && out.llc[7] == 0x00);
	assert(out.payload_offset == 14 && out.payload_len == 50);
	assert(out.qos_data && out.qos_control == 5);
}

static void
test_four_address_htc_restore(void)
{
	uint8_t eth[64], local[6] = { 2, 1, 1, 1, 1, 1 };
	uint8_t peer[6] = { 2, 2, 2, 2, 2, 2 };
	uint8_t bssid[6] = { 2, 3, 3, 3, 3, 3 };
	struct mt7902_rx_translated out;
	uint16_t fc = MT7902_RXTRANS_FC_TODS |
	    MT7902_RXTRANS_FC_FROMDS | MT7902_RXTRANS_FC_ORDER |
	    MT7902_RXTRANS_FC_TYPE_DATA | MT7902_RXTRANS_FC_SUBTYPE_QOS;

	make_eth(eth, 0x86dd);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), fc, 0xabcd,
	    0x1235, 0x78563412, local, peer, bssid, &out) == 0);
	assert(out.header_len == 36);
	assert(memcmp(out.header + 4, local, 6) == 0);
	assert(memcmp(out.header + 10, peer, 6) == 0);
	assert(memcmp(out.header + 16, eth, 6) == 0);
	assert(memcmp(out.header + 24, eth + 6, 6) == 0);
	assert(out.header[30] == 0x35 && out.header[31] == 0x12);
	assert(out.header[32] == 0x12 && out.header[33] == 0x34 &&
	    out.header[34] == 0x56 && out.header[35] == 0x78);
	assert(out.qos_control == 0x1235);
}

static void
test_bridge_tunnel_and_8023_length(void)
{
	uint8_t eth[64], local[6] = { 2, 1, 1, 1, 1, 1 };
	uint8_t peer[6] = { 2, 2, 2, 2, 2, 2 };
	uint8_t bssid[6] = { 2, 3, 3, 3, 3, 3 };
	struct mt7902_rx_translated out;
	uint16_t fc = MT7902_RXTRANS_FC_FROMDS |
	    MT7902_RXTRANS_FC_TYPE_DATA;

	make_eth(eth, MT7902_RXTRANS_ETH_P_AARP);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), fc, 0, 0, 0,
	    local, peer, bssid, &out) == 0);
	assert(out.llc_len == 8 && out.llc[5] == 0xf8);
	assert(out.llc[6] == 0x80 && out.llc[7] == 0xf3);
	assert(!out.qos_data && out.header_len == 24);

	make_eth(eth, 0x0050);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), fc, 0, 0, 0,
	    local, peer, bssid, &out) == 0);
	assert(out.llc_len == 0 && out.frame_len == 24 + 50);
}

static void
test_invalid_frames(void)
{
	uint8_t eth[64], local[6] = { 0 }, peer[6] = { 0 }, bssid[6] = { 0 };
	struct mt7902_rx_translated out;

	make_eth(eth, 0x0800);
	assert(mt7902_rxtrans_decode(eth, 13, 0x0008, 0, 0, 0,
	    local, peer, bssid, &out) != 0);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), 0x0000, 0, 0, 0,
	    local, peer, bssid, &out) != 0);
	assert(mt7902_rxtrans_decode(NULL, sizeof(eth), 0x0008, 0, 0, 0,
	    local, peer, bssid, &out) != 0);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), 0x0008, 0, 0, 0,
	    NULL, peer, bssid, &out) != 0);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), 0x0008, 0, 0, 0,
	    local, peer, NULL, &out) != 0);
	assert(mt7902_rxtrans_decode(eth, sizeof(eth), 0x0008, 0, 0, 0,
	    local, peer, bssid, NULL) != 0);
}

int
main(void)
{
	test_from_ds_qos_snap_restore();
	test_four_address_htc_restore();
	test_bridge_tunnel_and_8023_length();
	test_invalid_frames();
	puts("MT7902 translated RX reverse decoder tests: PASS");
	return (0);
}
