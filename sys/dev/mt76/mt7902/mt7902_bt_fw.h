/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Shared MT7902 Bluetooth firmware download sequencing helpers.
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

#endif /* _MT7902_BT_FW_H_ */
