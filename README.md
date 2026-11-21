# FreeBSD Custom Drivers — Gamer100Dev

A focused workspace for FreeBSD driver development, starting with USB support for Realtek RTL88x2BU / RTL8822BU through the RTW88 driver and LinuxKPI compatibility layer.

## Actual source code

This repository now vendors the complete FreeBSD stable/15 RTW88 driver directory and its module Makefile, plus the LinuxKPI USB implementation and header. It also includes a focused snapshot of Linux upstream RTW88 code for comparison.

- Primary FreeBSD source: `vendor/freebsd-stable-15/sys/contrib/dev/rtw88/`
- FreeBSD module integration: `vendor/freebsd-stable-15/sys/modules/rtw88/Makefile`
- LinuxKPI USB layer: `vendor/freebsd-stable-15/sys/compat/linuxkpi/common/`
- Linux reference: `vendor/linux-master/drivers/net/wireless/realtek/rtw88/`

See [the import manifest](upstream/IMPORT-MANIFEST.md) for upstream commit IDs and scope.

## Current target

- OS: FreeBSD 15.1-RELEASE-p2, amd64
- Adapter: Techkey AC1200, USB ID `0bda:b812` (RTL88x2BU family)
- Driver: RTW88, built with `RTW88_USB=1`
- Firmware: `rtw88/rtw8822b_fw.bin`
- Local working tree: `/usr/src-rtw88-usb-test`
- Kernel object directory: `/usr/obj/rtw88-usb-test`

## Workflow

1. Keep vendored upstream snapshots unchanged unless deliberately refreshing them.
2. Put local modifications in `patches/rtw88-usb/` and document the base revision.
3. Keep a known-good kernel available in `/boot/kernel`.
4. Build and install experimental kernels to a separate kernel directory.
5. Run builds in the foreground and preserve full output.
6. Do not call a change fixed until it has built and been tested on the adapter.

See [docs/STATUS.md](docs/STATUS.md), [docs/HARDWARE.md](docs/HARDWARE.md), and [upstream/SOURCES.md](upstream/SOURCES.md).
