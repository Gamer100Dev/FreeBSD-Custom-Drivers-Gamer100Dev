# FreeBSD Custom Drivers — Gamer100Dev

A source workspace for native FreeBSD driver development, starting with the
Realtek RTL8822BU USB Wi-Fi adapter (USB ID `0bda:b812`).

## Native FreeBSD RTW88 implementation

Development is on `work/native-rtw88-usb`, separate from `main`:

- **Native driver source:** `sys/dev/rtw88_native/`
- **Loadable module:** `sys/modules/rtw88_native/`
- **Implementation status:** `docs/NATIVE-RTW88-STATUS.md`

The native code now contains USB/control-transfer primitives, checked
register access, RTL8822B DDMA firmware-download logic, USB-specific power
sequencing, physical/logical eFuse decoding, permanent MAC/board-field
extraction, 3-endpoint FIFO/page allocation, H2C ring setup and base MAC
configuration. The start sequence is explicit and is not called from USB
attach. Active RX/TX networking and native `net80211` integration remain
unimplemented.

**It has not been compiled or hardware-tested yet.** This repository does not
claim functional Wi-Fi until the FreeBSD 15.1 build and adapter tests pass.

## Upstream source

- FreeBSD RTW88 snapshot: `sys/contrib/dev/rtw88/`
- FreeBSD LinuxKPI USB source: `sys/compat/linuxkpi/common/`
- Linux RTW88 comparison subset: `vendor/linux-master/`
- Source revisions and import scope: `upstream/IMPORT-MANIFEST.md`

The original LinuxKPI implementation remains as a reference/fallback. The
native driver code in `sys/dev/rtw88_native/` uses FreeBSD kernel APIs.
