/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Gamer100Dev
 * All rights reserved.
 *
 * Native RTL8822B transmit descriptor construction.
 *
 * Field positions follow the RTW88 tx.h / tx.c definitions. The chip's
 * descriptor allocation is 48 bytes; the defined TX descriptor occupies
 * the first 40 bytes and the final eight bytes are reserved padding.
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/systm.h>

#include "if_rtw88_native.h"

#define TXD_SIZE			RTW88_NATIVE_TX_DESC_SIZE
#define TXD_CHECKSUM_LENGTH		RTW88_NATIVE_TX_DESC_CHECKSUM_LEN

#define TXD_W0_PACKET_SIZE		0x0000ffffU
#define TXD_W0_OFFSET			0x00ff0000U
#define TXD_W0_BMC			(1U << 24)
#define TXD_W0_LAST_SEGMENT		(1U << 26)
#define TXD_W0_DISABLE_QSEL_SEQ		(1U << 31)

#define TXD_W1_MAC_ID			0x000000ffU
#define TXD_W1_QUEUE_SELECT		0x00001f00U
#define TXD_W1_RATE_ID			0x001f0000U
#define TXD_W1_SECURITY_TYPE		0x00c00000U
#define TXD_W1_PACKET_OFFSET		0x1f000000U
#define TXD_W1_MORE_DATA		(1U << 29)

#define TXD_W2_AMPDU_ENABLE		(1U << 12)
#define TXD_W2_SPECIAL_REPORT		(1U << 19)
#define TXD_W2_AMPDU_DENSITY		0x00700000U
#define TXD_W2_BT_NULL			(1U << 23)

#define TXD_W3_HW_SSN_SELECT		0x000000c0U
#define TXD_W3_USE_RATE			(1U << 8)
#define TXD_W3_DISABLE_RATE_FALLBACK	(1U << 10)
#define TXD_W3_USE_RTS			(1U << 12)
#define TXD_W3_NAV_USE_HEADER		(1U << 15)
#define TXD_W3_MAX_AGG_NUM		0x003e0000U

#define TXD_W4_DATA_RATE		0x0000007fU
#define TXD_W4_RATE_FALLBACK_LIMIT	0x00001f00U
#define TXD_W4_RTS_RATE			0x1f000000U

#define TXD_W5_DATA_SHORT_GI		(1U << 4)
#define TXD_W5_DATA_BANDWIDTH		0x00000060U
#define TXD_W5_DATA_LDPC		(1U << 7)
#define TXD_W5_DATA_STBC			0x00000300U
#define TXD_W5_RTS_SHORT_GI		(1U << 12)

#define TXD_W6_SOFTWARE_DEFINE		0x00000fffU
#define TXD_W7_CHECKSUM			0x0000ffffU
#define TXD_W7_DMA_AGGREGATE_COUNT	0xff000000U
#define TXD_W8_HARDWARE_SEQUENCE	(1U << 15)
#define TXD_W9_SOFTWARE_SEQUENCE	0x00fff000U
#define TXD_W9_TIM_ENABLE		(1U << 7)
#define TXD_W9_TIM_OFFSET		0x0000007fU

#define TXD_PREP(value, shift, mask)	(((uint32_t)(value) << (shift)) & (mask))

static void
rtw88_native_txdesc_word(uint8_t *desc, unsigned int word, uint32_t value)
{
	uint32_t le_value;

	le_value = htole32(value);
	memcpy(desc + (word * sizeof(le_value)), &le_value, sizeof(le_value));
}

static uint16_t
rtw88_native_txdesc_checksum(const uint8_t *desc)
{
	uint16_t checksum, value;
	unsigned int i;

	checksum = 0;
	for (i = 0; i < TXD_CHECKSUM_LENGTH / sizeof(value); i++) {
		memcpy(&value, desc + (i * sizeof(value)), sizeof(value));
		checksum ^= le16toh(value);
	}
	return (checksum);
}

int
rtw88_native_txdesc_build(uint8_t *desc, size_t desc_size,
    const struct rtw88_native_txdesc_params *p)
{
	uint32_t w[10];
	uint16_t checksum;

	if (desc == NULL || p == NULL || desc_size < TXD_SIZE)
		return (EINVAL);
	if (p->queue_select > 0x1f || p->rate_id > 0x1f ||
	    p->security_type > 0x03 || p->packet_offset > 0x1f ||
	    p->ampdu_density > 0x07 || p->hw_ssn_select > 0x03 ||
	    p->ampdu_factor > 0x1f || p->data_rate > 0x7f ||
	    p->data_rate_fallback_limit > 0x1f || p->rts_rate > 0x1f ||
	    p->bandwidth > 0x03 || p->stbc > 0x03 ||
	    p->software_define > 0x0fff || p->software_sequence > 0x0fff ||
	    p->tim_offset > 0x7f)
		return (EINVAL);

	bzero(desc, desc_size);
	bzero(w, sizeof(w));

	w[0] = TXD_PREP(p->packet_size, 0, TXD_W0_PACKET_SIZE) |
	    TXD_PREP(p->descriptor_offset, 16, TXD_W0_OFFSET);
	if (p->bmc)
		w[0] |= TXD_W0_BMC;
	if (p->last_segment)
		w[0] |= TXD_W0_LAST_SEGMENT;
	if (p->disable_qsel_sequence)
		w[0] |= TXD_W0_DISABLE_QSEL_SEQ;

	w[1] = TXD_PREP(p->mac_id, 0, TXD_W1_MAC_ID) |
	    TXD_PREP(p->queue_select, 8, TXD_W1_QUEUE_SELECT) |
	    TXD_PREP(p->rate_id, 16, TXD_W1_RATE_ID) |
	    TXD_PREP(p->security_type, 22, TXD_W1_SECURITY_TYPE) |
	    TXD_PREP(p->packet_offset, 24, TXD_W1_PACKET_OFFSET);
	if (p->more_data)
		w[1] |= TXD_W1_MORE_DATA;

	if (p->ampdu_enable)
		w[2] |= TXD_W2_AMPDU_ENABLE;
	if (p->special_report)
		w[2] |= TXD_W2_SPECIAL_REPORT;
	w[2] |= TXD_PREP(p->ampdu_density, 20, TXD_W2_AMPDU_DENSITY);
	if (p->bluetooth_null)
		w[2] |= TXD_W2_BT_NULL;

	w[3] = TXD_PREP(p->hw_ssn_select, 6, TXD_W3_HW_SSN_SELECT) |
	    TXD_PREP(p->ampdu_factor, 17, TXD_W3_MAX_AGG_NUM);
	if (p->use_hardware_rate)
		w[3] |= TXD_W3_USE_RATE;
	if (p->disable_rate_fallback)
		w[3] |= TXD_W3_DISABLE_RATE_FALLBACK;
	if (p->use_rts)
		w[3] |= TXD_W3_USE_RTS;
	if (p->nav_use_header)
		w[3] |= TXD_W3_NAV_USE_HEADER;

	w[4] = TXD_PREP(p->data_rate, 0, TXD_W4_DATA_RATE) |
	    TXD_PREP(p->data_rate_fallback_limit, 8,
	    TXD_W4_RATE_FALLBACK_LIMIT) |
	    TXD_PREP(p->rts_rate, 24, TXD_W4_RTS_RATE);

	w[5] = TXD_PREP(p->bandwidth, 5, TXD_W5_DATA_BANDWIDTH) |
	    TXD_PREP(p->stbc, 8, TXD_W5_DATA_STBC);
	if (p->data_short_gi)
		w[5] |= TXD_W5_DATA_SHORT_GI;
	if (p->data_ldpc)
		w[5] |= TXD_W5_DATA_LDPC;
	if (p->rts_short_gi)
		w[5] |= TXD_W5_RTS_SHORT_GI;

	w[6] = TXD_PREP(p->software_define, 0, TXD_W6_SOFTWARE_DEFINE);
	w[7] = TXD_PREP(p->dma_aggregate_count, 24,
	    TXD_W7_DMA_AGGREGATE_COUNT);
	if (p->enable_hardware_sequence)
		w[8] |= TXD_W8_HARDWARE_SEQUENCE;
	w[9] = TXD_PREP(p->software_sequence, 12, TXD_W9_SOFTWARE_SEQUENCE);
	if (p->tim_offset != 0)
		w[9] |= TXD_W9_TIM_ENABLE |
		    TXD_PREP(p->tim_offset, 0, TXD_W9_TIM_OFFSET);

	/* The checksum field itself is zero while the first 32 bytes are XORed. */
	for (unsigned int i = 0; i < nitems(w); i++)
		rtw88_native_txdesc_word(desc, i, w[i]);
	checksum = rtw88_native_txdesc_checksum(desc);
	w[7] |= checksum;
	rtw88_native_txdesc_word(desc, 7, w[7]);

	return (0);
}
