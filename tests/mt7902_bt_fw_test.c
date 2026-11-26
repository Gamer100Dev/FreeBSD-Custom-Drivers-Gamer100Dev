#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../sys/dev/mt76/mt7902/mt7902_bt_fw.h"

static void
test_chunk_sequence(void)
{
	/* A one-transfer section still starts with START, not END. */
	assert(mt7902_bt_download_flag(1, 128, 128) == 1);

	/* First packet of a multi-packet section. */
	assert(mt7902_bt_download_flag(1, 600, 250) == 1);

	/* Middle and final packets. */
	assert(mt7902_bt_download_flag(0, 350, 250) == 2);
	assert(mt7902_bt_download_flag(0, 100, 100) == 3);
}

static void
test_section_bounds(void)
{
	assert(mt7902_bt_fw_section_valid(4096, 256, 256, 3840));
	assert(!mt7902_bt_fw_section_valid(4096, 256, 255, 1));
	assert(!mt7902_bt_fw_section_valid(4096, 256, 4097, 1));
	assert(!mt7902_bt_fw_section_valid(4096, 256, 4000, 97));
	assert(!mt7902_bt_fw_section_valid(4096, 256, SIZE_MAX, 2));
	assert(mt7902_bt_fw_section_valid(4096, 256, 512, 0));
}

static void
test_wmt_event_validation(void)
{
	const uint8_t valid[] = { 0xe4, 5, 2, 0x01, 0, 0, 2 };
	const uint8_t wrong_event[] = { 0x0e, 5, 2, 0x01, 0, 0, 2 };
	const uint8_t wrong_direction[] = { 0xe4, 5, 1, 0x01, 0, 0, 2 };
	const uint8_t wrong_opcode[] = { 0xe4, 5, 2, 0x02, 0, 0, 2 };
	const uint8_t truncated_length[] = { 0xe4, 8, 2, 0x01, 0, 0, 2 };
	const uint8_t short_header[] = { 0xe4, 4, 2, 0x01, 0, 0 };

	assert(mt7902_bt_wmt_event_valid(valid, sizeof(valid), 0x01));
	assert(!mt7902_bt_wmt_event_valid(wrong_event,
	    sizeof(wrong_event), 0x01));
	assert(!mt7902_bt_wmt_event_valid(wrong_direction,
	    sizeof(wrong_direction), 0x01));
	assert(!mt7902_bt_wmt_event_valid(wrong_opcode,
	    sizeof(wrong_opcode), 0x01));
	assert(!mt7902_bt_wmt_event_valid(truncated_length,
	    sizeof(truncated_length), 0x01));
	assert(!mt7902_bt_wmt_event_valid(short_header,
	    sizeof(short_header), 0x01));
	assert(!mt7902_bt_wmt_event_valid(valid, 6, 0x01));
	assert(!mt7902_bt_wmt_event_valid(NULL, sizeof(valid), 0x01));
}

int
main(void)
{
	test_chunk_sequence();
	test_section_bounds();
	test_wmt_event_validation();
	return (0);
}
