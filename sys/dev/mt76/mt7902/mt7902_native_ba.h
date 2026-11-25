/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Portable Connac2 Block Ack TLV encoders. The FreeBSD driver and host-side
 * tests share these exact byte serializers so firmware record sizes, offsets,
 * endianness and enable/disable semantics cannot silently diverge.
 */
#ifndef _MT7902_NATIVE_BA_H_
#define _MT7902_NATIVE_BA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT7902_BA_TID_COUNT       16U
#define MT7902_BA_WTBL_TAG        8U
#define MT7902_BA_STA_REC_TAG     6U
#define MT7902_BA_TYPE_ORIGINATOR 1U
#define MT7902_BA_TYPE_RECIPIENT  2U
#define MT7902_BA_WTBL_LEN        28U
#define MT7902_BA_STA_REC_LEN     12U

static inline void
mt7902_ba_put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void
mt7902_ba_zero(uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = 0;
}

/*
 * Encode WTBL_BA, 28 bytes. For an originator entry ba_en is the enable
 * boolean, while for a recipient entry the firmware uses the peer/TID reset
 * selector and the receive window. All reserved bytes are always zero.
 */
static inline int
mt7902_ba_encode_wtbl(uint8_t out[MT7902_BA_WTBL_LEN], uint8_t tid,
    uint16_t ssn, uint16_t winsize, const uint8_t peer[6],
    bool tx, bool enable)
{
	if (out == NULL || tid >= MT7902_BA_TID_COUNT ||
	    (!tx && peer == NULL))
		return (-1);
	mt7902_ba_zero(out, MT7902_BA_WTBL_LEN);
	mt7902_ba_put_le16(out + 0, MT7902_BA_WTBL_TAG);
	mt7902_ba_put_le16(out + 2, MT7902_BA_WTBL_LEN);
	out[4] = tid;
	out[5] = tx ? MT7902_BA_TYPE_ORIGINATOR :
	    MT7902_BA_TYPE_RECIPIENT;
	if (tx) {
		mt7902_ba_put_le16(out + 8, enable ? ssn : 0);
		out[10] = enable ? 1 : 0;
		mt7902_ba_put_le16(out + 12, enable ? winsize : 0);
	} else {
		mt7902_ba_put_le16(out + 12, enable ? winsize : 0);
		for (unsigned int i = 0; i < 6; i++)
			out[14 + i] = peer[i];
		out[20] = tid;
		out[21] = 0; /* RST_BA_MAC_TID_MATCH */
		out[22] = 1; /* reset BA session */
		out[23] = 0; /* band 0 */
	}
	return (0);
}

/*
 * Encode STA_REC_BA, 12 bytes. This port does not negotiate A-MSDU inside
 * A-MPDU, so amsdu is deliberately zero for both TX and RX sessions.
 */
static inline int
mt7902_ba_encode_sta_rec(uint8_t out[MT7902_BA_STA_REC_LEN], uint8_t tid,
    uint16_t ssn, uint16_t winsize, bool tx, bool enable)
{
	if (out == NULL || tid >= MT7902_BA_TID_COUNT)
		return (-1);
	mt7902_ba_zero(out, MT7902_BA_STA_REC_LEN);
	mt7902_ba_put_le16(out + 0, MT7902_BA_STA_REC_TAG);
	mt7902_ba_put_le16(out + 2, MT7902_BA_STA_REC_LEN);
	out[4] = tid;
	out[5] = tx ? MT7902_BA_TYPE_ORIGINATOR :
	    MT7902_BA_TYPE_RECIPIENT;
	out[6] = 0; /* A-MSDU in A-MPDU is not negotiated */
	out[7] = enable ? (uint8_t)(1U << tid) : 0;
	mt7902_ba_put_le16(out + 8, enable ? ssn : 0);
	mt7902_ba_put_le16(out + 10, enable ? winsize : 0);
	return (0);
}

#endif /* _MT7902_NATIVE_BA_H_ */
