# MediaTek MT7902 firmware

Files here are from the `mediatek/` directory of the Linux firmware project,
retrieved from its upstream repository. The two Wi-Fi images are requested by
the upstream mt76 MT7921-family driver for MT7902. The native FreeBSD port
does not load the Wi-Fi images yet. The Bluetooth image is retained
for the separate MT7902 USB Bluetooth function. The optional native
`ng_ubt_mt7902` subclass implements the MediaTek 79xx WMT patch-download
bootstrap before the inherited FreeBSD `ng_ubt` driver attaches. Bluetooth
firmware activation and end-to-end operation still require hardware validation.

Redistribution terms are in `LICENCE.mediatek`. Linux-firmware's WHENCE
metadata marks these files as redistributable for MediaTek chipset devices.
Keep this license with redistributed copies.

SHA-256:

| File | SHA-256 |
| --- | --- |
| `WIFI_RAM_CODE_MT7902_1.bin` | `b5958ac72c71fb8405e080f52d378d02b484812b9f1010c8843727056bbfc998` |
| `WIFI_MT7902_patch_mcu_1_1_hdr.bin` | `d73ba9e982f781221a2b9f10c42031f20a9dce046929fc0d55c791a417efe30a` |
| `BT_RAM_CODE_MT7902_1_1_hdr.bin` | `4f53b5e02fbd933172e18caf952bda410a877eaad00561781528cb4aff58dc38` |

Install the Wi-Fi and Bluetooth files under `/boot/firmware/mediatek` with
`tools/install-mt7902-firmware.sh`.
