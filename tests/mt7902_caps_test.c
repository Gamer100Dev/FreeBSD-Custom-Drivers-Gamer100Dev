#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../sys/dev/mt76/mt7902/mt7902_native_caps.h"

static void put_tlv(uint8_t *p, uint32_t type, const uint8_t *payload,
    uint32_t len)
{
	p[0] = type; p[1] = type >> 8; p[2] = type >> 16; p[3] = type >> 24;
	p[4] = len; p[5] = len >> 8; p[6] = len >> 16; p[7] = len >> 24;
	memcpy(p + 8, payload, len);
}

static void test_parse_phy_and_optional_tlvs(void)
{
	uint8_t data[64] = { 0 };
	uint8_t phy[12] = { 1, 1, 1, 2, 1, 0, 1, 1, 0, 0, 3, 1 };
	uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	uint8_t sixg = 1, chip[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct mt7902_nic_caps caps;

	data[0] = 4;
	put_tlv(data + 4, MT7902_NIC_CAP_PHY, phy, sizeof(phy));
	put_tlv(data + 24, MT7902_NIC_CAP_MAC_ADDR, mac, sizeof(mac));
	put_tlv(data + 38, MT7902_NIC_CAP_6G, &sixg, sizeof(sixg));
	put_tlv(data + 47, MT7902_NIC_CAP_CHIP_CAP, chip, sizeof(chip));
	assert(mt7902_nic_caps_parse(data, 63, &caps) == 0);
	assert(caps.has_phy && caps.phy.ht == 1 && caps.phy.vht == 1);
	assert(caps.phy.he == 1 && caps.phy.nss == 1);
	assert(caps.has_2ghz && caps.has_5ghz && caps.has_6ghz);
	assert(caps.has_macaddr && caps.macaddr[5] == 0x55);
	assert(caps.has_chip_cap &&
	    caps.chip_cap == UINT64_C(0x0807060504030201));
}

static void test_unknown_tlv_is_skipped(void)
{
	uint8_t data[36] = { 0 };
	uint8_t unknown[4] = { 1, 2, 3, 4 };
	uint8_t phy[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
	struct mt7902_nic_caps caps;

	data[0] = 2;
	put_tlv(data + 4, 0x99, unknown, sizeof(unknown));
	put_tlv(data + 16, MT7902_NIC_CAP_PHY, phy, sizeof(phy));
	assert(mt7902_nic_caps_parse(data, sizeof(data), &caps) == 0);
	assert(caps.has_phy && !caps.has_macaddr);
}

static void test_reject_malformed_responses(void)
{
	uint8_t data[24] = { 0 }, short_phy[11] = { 0 };
	struct mt7902_nic_caps caps;

	assert(mt7902_nic_caps_parse(data, 3, &caps) != 0);
	data[0] = 1;
	put_tlv(data + 4, MT7902_NIC_CAP_PHY, short_phy, sizeof(short_phy));
	assert(mt7902_nic_caps_parse(data, sizeof(data), &caps) != 0);
	data[0] = 2;
	assert(mt7902_nic_caps_parse(data, sizeof(data), &caps) != 0);
}

int main(void)
{
	test_parse_phy_and_optional_tlvs();
	test_unknown_tlv_is_skipped();
	test_reject_malformed_responses();
	puts("MT7902 NIC capability TLV tests: PASS");
	return (0);
}
