/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Shared MT7902 Bluetooth firmware download and validation helpers.
 */
#ifndef _MT7902_BT_FW_H_
#define _MT7902_BT_FW_H_

#include <stddef.h>
#include <stdint.h>

/*
 * MediaTek 79xx firmware protocol: the first chunk is always START,
 * including a section that fits in a single transfer. Only subsequent
 * chunks can be CONTINUE or END.
 */
static inline uint8_t
mt7902_bt_download_flag(int first_chunk, size_t remaining, size_t chunk)
{
	if (first_chunk)
		return (1); /* MTK_WMT_START */
	if (remaining == chunk)
		return (3); /* MTK_WMT_END */
	return (2); /* MTK_WMT_CONTINUE */
}

/*
 * Section payloads must start after the complete section-map table and fit
 * wholly inside the firmware object. Subtraction after the offset check
 * avoids overflow in offset + size.
 */
static inline int
mt7902_bt_fw_section_valid(size_t firmware_size, size_t maps_end,
    size_t offset, size_t size)
{
	return (offset >= maps_end && offset <= firmware_size &&
	    size <= firmware_size - offset);
}

/*
 * WMT events are vendor events (0xe4), with a five-byte WMT header after
 * the two-byte HCI event header. Validate the reported event length before
 * reading any fields beyond the fixed prefix.
 */
static inline int
mt7902_bt_wmt_event_valid(const uint8_t *event, size_t actual,
    uint8_t expected_op)
{
	if (event == NULL || actual < 7 || event[1] < 5 ||
	    (size_t)event[1] + 2 > actual)
		return (0);

	return (event[0] == 0xe4 && event[2] == 2 &&
	    event[3] == expected_op);
}

#endif /* _MT7902_BT_FW_H_ */
