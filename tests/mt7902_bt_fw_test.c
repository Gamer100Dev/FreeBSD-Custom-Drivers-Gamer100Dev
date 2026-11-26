#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../sys/dev/mt76/mt7902/mt7902_bt_fw.h"

int
main(void)
{
	/* A one-transfer section still starts with START, not END. */
	assert(mt7902_bt_download_flag(1, 128, 128) == 1);

	/* First packet of a multi-packet section. */
	assert(mt7902_bt_download_flag(1, 600, 250) == 1);

	/* Middle and final packets. */
	assert(mt7902_bt_download_flag(0, 350, 250) == 2);
	assert(mt7902_bt_download_flag(0, 100, 100) == 3);

	return (0);
}
