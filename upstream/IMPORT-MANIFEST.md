# Vendored source manifest

This repository now contains actual source snapshots, not just pointers.

## FreeBSD primary implementation

- Upstream: https://github.com/freebsd/freebsd-src
- Branch: `stable/15`
- Snapshot commit: `55b89b7b529f1c37a749adadb11aba4f8e9e51ca`
- Imported paths:
  - `vendor/freebsd-stable-15/sys/contrib/dev/rtw88/` — complete RTW88 directory snapshot
  - `vendor/freebsd-stable-15/sys/modules/rtw88/Makefile`
  - `vendor/freebsd-stable-15/sys/compat/linuxkpi/common/src/linux_usb.c`
  - `vendor/freebsd-stable-15/sys/compat/linuxkpi/common/include/linux/usb.h`

## Linux reference implementation

- Upstream: https://github.com/torvalds/linux
- Snapshot commit: `c44104f3642da720164042623e4832836aefdd8e`
- Imported path: `vendor/linux-master/drivers/net/wireless/realtek/rtw88/`
- This is a focused reference subset for USB, RTL8822B/RTL8822BU and shared RTW88 core paths, not the complete Linux directory.

## Notes

- Original source headers and copyright notices are retained.
- The FreeBSD stable/15 tree is the primary build target. Linux files are comparison/reference material and must not be compiled directly into FreeBSD.
- Firmware binaries are not vendored; see `firmware/README.md`.
- The imported FreeBSD `linux_usb.c` is the upstream filename. In the local experiment notes, this file was sometimes referred to as `linuxkpi_usb.c`; do not assume those are separate upstream files.
- Refresh snapshots only with an explicit upstream commit SHA and a reviewed diff.
