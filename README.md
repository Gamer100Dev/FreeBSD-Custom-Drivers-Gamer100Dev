# FreeBSD Custom Drivers — Gamer100Dev

A focused workspace for FreeBSD driver experiments, starting with USB support for Realtek RTL88x2BU / RTL8822BU through the RTW88 driver and LinuxKPI compatibility layer.

## Current target

- OS: FreeBSD 15.1-RELEASE-p2, amd64
- Adapter: Techkey AC1200, USB ID `0bda:b812` (RTL88x2BU family)
- Driver: RTW88, built with `RTW88_USB=1`
- Firmware: `rtw88/rtw8822b_fw.bin`
- Working tree used for the current experiment: `/usr/src-rtw88-usb-test`
- Kernel object directory: `/usr/obj/rtw88-usb-test`

This repository is an integration/workbench repository, not a replacement for the complete FreeBSD source tree. It will track focused source imports, patches, build instructions, test logs and upstream references.

## Repository layout

- `drivers/rtw88/` — imported RTW88 driver source and local notes
- `compat/linuxkpi/` — only the LinuxKPI files/changes needed by these drivers
- `patches/rtw88-usb/` — reviewable, ordered patches
- `firmware/` — firmware provenance and installation notes (firmware binaries are not committed by default)
- `tools/` — foreground build, install and test helpers
- `docs/` — hardware, architecture, status and test records
- `upstream/` — source revision and license/provenance tracking

## Safety / workflow

1. Keep a known-good kernel available in `/boot/kernel`.
2. Build and install experimental kernels to a separate kernel directory.
3. Run builds in the foreground and preserve the full output.
4. Make one logical change per commit.
5. Do not call a change fixed until it has built and been tested on the adapter.
6. Keep firmware provenance and each imported file's upstream revision documented.

See [docs/STATUS.md](docs/STATUS.md), [docs/HARDWARE.md](docs/HARDWARE.md) and [upstream/SOURCES.md](upstream/SOURCES.md).
