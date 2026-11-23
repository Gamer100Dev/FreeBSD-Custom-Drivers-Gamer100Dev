#!/bin/sh
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST=${DESTDIR:-}/boot/firmware/mediatek
SRC=$REPO/firmware/mediatek

install -d "$DEST"
for image in \
	WIFI_RAM_CODE_MT7902_1.bin \
	WIFI_MT7902_patch_mcu_1_1_hdr.bin \
	BT_RAM_CODE_MT7902_1_1_hdr.bin; do
	install -m 644 "$SRC/$image" "$DEST/$image"
done
install -m 644 "$SRC/LICENCE.mediatek" "$DEST/LICENCE.mediatek"
printf 'Installed MT7902 firmware in %s\n' "$DEST"
