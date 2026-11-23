/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Gamer100Dev
 *
 * Native validators for MediaTek Connac2 patch and WM firmware containers.
 * Layout follows mt76/mt76_connac_mcu.h and mt76_connac_mcu.c.
 */
#include <sys/param.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/firmware.h>
#include <sys/systm.h>

#include "if_mt7902_native.h"

struct mt7902_fw_trailer {
	uint8_t chip_id;
	uint8_t eco_code;
	uint8_t n_region;
	uint8_t format_ver;
	uint8_t format_flag;
	uint8_t reserved[2];
	char fw_ver[10];
	char build_date[15];
	uint32_t crc;
} __packed;

struct mt7902_fw_region_disk {
	uint32_t decomp_crc;
	uint32_t decomp_len;
	uint32_t decomp_blk_sz;
	uint8_t reserved[4];
	uint32_t addr;
	uint32_t len;
	uint8_t feature_set;
	uint8_t type;
	uint8_t reserved1[14];
} __packed;

struct mt7902_patch_header {
	char build_date[16];
	char platform[4];
	uint32_t hw_sw_ver;
	uint32_t patch_ver;
	uint16_t checksum;
	uint16_t reserved;
	struct {
		uint32_t patch_ver;
		uint32_t subsys;
		uint32_t feature;
		uint32_t n_region;
		uint32_t crc;
		uint32_t reserved[11];
	} __packed desc;
} __packed;

struct mt7902_patch_section {
	uint32_t type;
	uint32_t offset;
	uint32_t size;
	union {
		uint32_t spec[13];
		struct {
			uint32_t addr;
			uint32_t len;
			uint32_t sec_key_idx;
			uint32_t align_len;
			uint32_t reserved[9];
		} info;
	};
} __packed;

CTASSERT(sizeof(struct mt7902_fw_trailer) == 36);
CTASSERT(sizeof(struct mt7902_fw_region_disk) == 40);
CTASSERT(sizeof(struct mt7902_patch_header) == 96);
CTASSERT(sizeof(struct mt7902_patch_section) == 64);

int
mt7902_fw_get(struct mt7902_softc *sc, const char *name,
    struct mt7902_fw_image *image)
{
	if (sc == NULL || name == NULL || image == NULL)
		return (EINVAL);

	bzero(image, sizeof(*image));
	image->fw = firmware_get(name);
	if (image->fw == NULL)
		return (ENOENT);
	device_printf(sc->dev, "loaded firmware image %s (%zu bytes)\n",
	    name, image->fw->datasize);
	return (0);
}

void
mt7902_fw_put(struct mt7902_fw_image *image)
{
	if (image == NULL || image->fw == NULL)
		return;
	firmware_put(image->fw, 0);
	bzero(image, sizeof(*image));
}

int
mt7902_fw_validate_wm(struct mt7902_fw_image *image)
{
	const struct mt7902_fw_trailer *trailer;
	const struct mt7902_fw_region_disk *region;
	const uint8_t *base;
	size_t body_end, body_offset;
	uint32_t len, addr;
	unsigned int i;

	if (image == NULL || image->fw == NULL || image->fw->data == NULL ||
	    image->fw->datasize < sizeof(*trailer))
		return (EINVAL);
	base = image->fw->data;
	trailer = (const void *)(base + image->fw->datasize - sizeof(*trailer));
	if (trailer->n_region == 0 ||
	    trailer->n_region > MT7902_FW_MAX_REGIONS)
		return (EINVAL);
	if (image->fw->datasize < sizeof(*trailer) +
	    trailer->n_region * sizeof(*region))
		return (EINVAL);

	body_end = image->fw->datasize - sizeof(*trailer) -
	    trailer->n_region * sizeof(*region);
	region = (const void *)(base + body_end);
	body_offset = 0;
	image->nregion = 0;
	image->override_addr = 0;
	for (i = 0; i < trailer->n_region; i++) {
		len = le32toh(region[i].len);
		addr = le32toh(region[i].addr);
		if (len == 0 || len > body_end - body_offset)
			goto invalid;
		image->region[i].data = base + body_offset;
		image->region[i].len = len;
		image->region[i].addr = addr;
		image->region[i].feature = region[i].feature_set;
		image->region[i].sec = 0;
		if (region[i].feature_set & 0x20) /* FW_FEATURE_OVERRIDE_ADDR */
			image->override_addr = addr;
		body_offset += len;
		image->nregion++;
	}
	/* Firmware may include padding between the region payloads and trailer.
	 * mt76 validates that region lengths fit, not that they consume every
	 * byte before the region table.
	 */
	return (0);

invalid:
	bzero(image->region, sizeof(image->region));
	image->nregion = 0;
	image->override_addr = 0;
	return (EINVAL);
}

int
mt7902_fw_validate_patch(struct mt7902_fw_image *image)
{
	const struct mt7902_patch_header *header;
	const struct mt7902_patch_section *section;
	const uint8_t *base;
	uint32_t nregion, offset, len, addr;
	unsigned int i;

	if (image == NULL || image->fw == NULL || image->fw->data == NULL ||
	    image->fw->datasize < sizeof(*header))
		return (EINVAL);
	base = image->fw->data;
	header = (const void *)base;
	nregion = be32toh(header->desc.n_region);
	if (nregion == 0 || nregion > MT7902_FW_MAX_REGIONS ||
	    nregion > (image->fw->datasize - sizeof(*header)) /
	    sizeof(*section))
		return (EINVAL);
	section = (const void *)(base + sizeof(*header));
	image->nregion = 0;
	image->override_addr = 0;
	for (i = 0; i < nregion; i++) {
		uint32_t type;

		type = be32toh(section[i].type);
		offset = be32toh(section[i].offset);
		len = be32toh(section[i].info.len);
		addr = be32toh(section[i].info.addr);
		if ((type & 0x0000ffff) != 0x2 || len == 0 ||
		    offset > image->fw->datasize ||
		    len > image->fw->datasize - offset)
			goto invalid;
		image->region[i].data = base + offset;
		image->region[i].len = len;
		image->region[i].addr = addr;
		image->region[i].feature = type;
		image->region[i].sec = be32toh(section[i].info.sec_key_idx);
		image->nregion++;
	}
	return (0);

invalid:
	bzero(image->region, sizeof(image->region));
	image->nregion = 0;
	return (EINVAL);
}
