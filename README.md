# FreeBSD Custom Drivers — Gamer100Dev

A source workspace for FreeBSD driver development, starting with Realtek RTW88 USB support for the RTL88x2BU / RTL8822BU family.

## Working FreeBSD source tree

The actual FreeBSD code is stored in the normal source-tree layout at the repository root:

- **RTW88 driver:** `sys/contrib/dev/rtw88/`
- **RTW88 module build file:** `sys/modules/rtw88/Makefile`
- **LinuxKPI USB implementation:** `sys/compat/linuxkpi/common/src/linux_usb.c`
- **LinuxKPI USB header:** `sys/compat/linuxkpi/common/include/linux/usb.h`

These are real source files, including `.c`, `.h`, and Makefiles — not links or submodule pointers.

## Upstream comparison snapshots

- FreeBSD stable/15 source snapshot: `vendor/freebsd-stable-15/`
- Linux upstream RTW88 reference subset: `vendor/linux-master/`
- Exact revisions and imported scope: `upstream/IMPORT-MANIFEST.md`

Use the root-level `sys/` tree as the working FreeBSD source copy. Keep upstream reference snapshots unchanged and record local modifications in `patches/rtw88-usb/`.

## Current target

- OS: FreeBSD 15.1-RELEASE-p2, amd64
- Adapter: Techkey AC1200, USB ID `0bda:b812` (RTL88x2BU family)
- Driver: RTW88, built with `RTW88_USB=1`
- Firmware: `rtw88/rtw8822b_fw.bin`
- Existing local source tree: `/usr/src-rtw88-usb-test`
- Existing kernel object directory: `/usr/obj/rtw88-usb-test`

## Workflow

1. Make and review changes against the root-level `sys/` source.
2. Keep focused changes in `patches/rtw88-usb/` where practical.
3. Keep a known-good kernel available in `/boot/kernel`.
4. Build experimental kernels to a separate kernel directory.
5. Run builds in the foreground and preserve full output.
6. Do not mark a change fixed until it has built and been tested on the adapter.

See `docs/STATUS.md`, `docs/HARDWARE.md`, and `upstream/IMPORT-MANIFEST.md`.
