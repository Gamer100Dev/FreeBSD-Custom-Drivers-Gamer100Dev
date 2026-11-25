/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Connac2 NIC-capability TLV parser shared by the kernel driver and tests.
 */
#ifndef _MT7902_NATIVE_CAPS_H_
#define _MT7902_NATIVE_CAPS_H_

#define MT7902_NIC_CAP_MAC_ADDR 7U
#define MT7902_NIC_CAP_PHY      8U
#define MT7902_NIC_CAP_6G       24U
#define MT7902_NIC_CAP_CHIP_CAP 32U
#define MT7902_NIC_PHY_CAP_LEN  12U

struct mt7902_nic_phy_caps {
	uint8_t ht, vht, has_5g, max_bw, nss, dbdc;
	uint8_t tx_ldpc, rx_ldpc, tx_stbc, rx_stbc, hw_path, he;
};

struct mt7902_nic_caps {
	struct mt7902_nic_phy_caps phy;
	uint8_t macaddr[6];
	uint64_t chip_cap;
	uint8_t has_2ghz, has_5ghz, has_6ghz;
	uint8_t has_phy, has_macaddr, has_chip_cap;
};

/*
 * Parse the MT792x/Connac2 GET_NIC_CAPAB response. Each element has an
 * eight-byte little-endian {type,len} header. Unknown elements are skipped;
 * malformed lengths and a missing PHY capability are fatal.
 */
static inline int
mt7902_nic_caps_parse(const uint8_t *data, size_t data_len,
    struct mt7902_nic_caps *caps)
{
	size_t offset, tlv_len;
	uint32_t type;
	uint16_t count;
	unsigned int i, j;

	if (data == NULL || caps == NULL || data_len < 4)
		return (-1);
	for (j = 0; j < sizeof(*caps); j++)
		((uint8_t *)caps)[j] = 0;
	count = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
	offset = 4;

	for (i = 0; i < count; i++) {
		if (offset > data_len || data_len - offset < 8)
			return (-1);
		type = (uint32_t)data[offset] |
		    ((uint32_t)data[offset + 1] << 8) |
		    ((uint32_t)data[offset + 2] << 16) |
		    ((uint32_t)data[offset + 3] << 24);
		tlv_len = (size_t)data[offset + 4] |
		    ((size_t)data[offset + 5] << 8) |
		    ((size_t)data[offset + 6] << 16) |
		    ((size_t)data[offset + 7] << 24);
		offset += 8;
		if (tlv_len > data_len - offset)
			return (-1);

		switch (type) {
		case MT7902_NIC_CAP_PHY:
			if (tlv_len < MT7902_NIC_PHY_CAP_LEN)
				return (-1);
			caps->phy.ht = data[offset];
			caps->phy.vht = data[offset + 1];
			caps->phy.has_5g = data[offset + 2];
			caps->phy.max_bw = data[offset + 3];
			caps->phy.nss = data[offset + 4];
			caps->phy.dbdc = data[offset + 5];
			caps->phy.tx_ldpc = data[offset + 6];
			caps->phy.rx_ldpc = data[offset + 7];
			caps->phy.tx_stbc = data[offset + 8];
			caps->phy.rx_stbc = data[offset + 9];
			caps->phy.hw_path = data[offset + 10];
			caps->phy.he = data[offset + 11];
			caps->has_2ghz = (caps->phy.hw_path & 0x01) != 0;
			caps->has_5ghz = (caps->phy.hw_path & 0x02) != 0;
			caps->has_phy = 1;
			break;
		case MT7902_NIC_CAP_MAC_ADDR:
			if (tlv_len < sizeof(caps->macaddr))
				return (-1);
			for (j = 0; j < sizeof(caps->macaddr); j++)
				caps->macaddr[j] = data[offset + j];
			caps->has_macaddr = 1;
			break;
		case MT7902_NIC_CAP_6G:
			if (tlv_len < 1)
				return (-1);
			caps->has_6ghz = data[offset] != 0;
			break;
		case MT7902_NIC_CAP_CHIP_CAP:
			if (tlv_len < 8)
				return (-1);
			caps->chip_cap = (uint64_t)data[offset] |
			    ((uint64_t)data[offset + 1] << 8) |
			    ((uint64_t)data[offset + 2] << 16) |
			    ((uint64_t)data[offset + 3] << 24) |
			    ((uint64_t)data[offset + 4] << 32) |
			    ((uint64_t)data[offset + 5] << 40) |
			    ((uint64_t)data[offset + 6] << 48) |
			    ((uint64_t)data[offset + 7] << 56);
			caps->has_chip_cap = 1;
			break;
		default:
			break;
		}
		offset += tlv_len;
	}
	if (!caps->has_phy || caps->phy.nss == 0)
		return (-1);
	return (0);
}

#endif /* _MT7902_NATIVE_CAPS_H_ */
