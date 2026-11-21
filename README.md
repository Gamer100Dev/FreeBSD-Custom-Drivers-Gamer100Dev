# FreeBSD Custom Drivers — Gamer100Dev

A source workspace for FreeBSD driver development, starting with native USB support for the Realtek RTL88x2BU / RTL8822BU adapter.

## Native FreeBSD implementation

The native implementation is being developed separately from the existing LinuxKPI-based RTW88 port:

- **Native USB module:** `sys/dev/rtw88_native/`
- **Native module Makefile:** `sys/modules/rtw88_native/Makefile`
- **Implementation status:** `docs/NATIVE-RTW88-STATUS.md`

The first milestone binds only USB ID `0bda:b812` and probes RTW88 `REG_SYS_CFG1` through FreeBSD's native USB control-transfer API. It is not yet a usable Wi-Fi driver and does not create a `wlan(4)` interface.

## Existing upstream source

- FreeBSD RTW88 snapshot: `sys/contrib/dev/rtw88/`
- FreeBSD LinuxKPI USB source: `sys/compat/linuxkpi/common/`
- Linux RTW88 comparison snapshot: `vendor/linux-master/`
- Exact source revisions: `upstream/IMPORT-MANIFEST.md`

The existing LinuxKPI implementation is retained unchanged as a reference and fallback while the native port is developed.

## Current hardware target

- OS: FreeBSD 15.1-RELEASE-p2, amd64
- Adapter: Techkey AC1200, USB ID `0bda:b812`
- Chip family: Realtek RTL8822BU
- Firmware: `rtw88/rtw8822b_fw.bin`

Do not mark the native driver functional until it builds on FreeBSD, attaches to the adapter, downloads firmware, creates a wlan interface, and passes scan/association/TX/RX tests.
