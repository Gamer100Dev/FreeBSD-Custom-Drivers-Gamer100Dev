# Vendored source manifest

## Working source tree

The FreeBSD stable/15 source has been copied into native repository-root paths so it can be browsed and edited directly:

- `sys/contrib/dev/rtw88/` — complete RTW88 driver directory
- `sys/modules/rtw88/Makefile` — driver module build integration
- `sys/compat/linuxkpi/common/src/linux_usb.c` — LinuxKPI USB implementation
- `sys/compat/linuxkpi/common/include/linux/usb.h` — LinuxKPI USB API header

The full FreeBSD snapshot is also retained under `vendor/freebsd-stable-15/`.

## FreeBSD upstream baseline

- Upstream: https://github.com/freebsd/freebsd-src
- Branch: `stable/15`
- Snapshot commit: `55b89b7b529f1c37a749adadb11aba4f8e9e51ca`

## Linux comparison baseline

### MT76 / MT7902 reference

- Upstream: https://github.com/openwrt/mt76
- Clone: `vendor/mt76/` (upstream Linux driver source; reference only)
- Clone revision: `41d5d1dd63111870ef74a5b9645416d98979a145`
- Native FreeBSD implementation: `sys/dev/mt76/mt7902/`
- FreeBSD module: `sys/modules/mt7902_native/`
- Status: PCI resource-layer foundation only; firmware boot, WFDMA, net80211,
  Wi-Fi operation, and Bluetooth vendor firmware initialization are pending.

### RTW88 comparison baseline

- Upstream: https://github.com/torvalds/linux
- Snapshot commit: `c44104f3642da720164042623e4832836aefdd8e`
- Imported path: `vendor/linux-master/drivers/net/wireless/realtek/rtw88/`
- This is a focused RTW88 reference subset for USB, RTL8822B/RTL8822BU, and shared core code, not the complete Linux directory.

## Important status

- Source code is physically committed to this repository; the native paths above contain real `.c`, `.h`, and Makefile content.
- Original source headers and copyright notices are retained.
- RTW88 firmware is not vendored; the MT7902 firmware payloads are separately
  included under `firmware/mediatek/` with their license and checksums.
- Your previous experimental changes to RTW88 USB and LinuxKPI USB are not yet ported into this clean upstream baseline.
- The import is not a working-driver claim. Build and hardware validation are still required.
- Refresh snapshots only with an explicit upstream commit SHA and a reviewed diff.
