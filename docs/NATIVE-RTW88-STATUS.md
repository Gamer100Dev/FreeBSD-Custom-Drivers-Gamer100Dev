# Native RTW88 implementation status

## Current state

**Stage 1 source added; not yet build- or hardware-verified.**

A separate native USB kernel module has been introduced at
`sys/dev/rtw88_native/` with its module Makefile at
`sys/modules/rtw88_native/Makefile`.

It targets Realtek USB ID `0bda:b812` with vendor-specific interface
class/subclass/protocol `ff/ff/ff`. On attach it performs one read-only vendor
raw, read-only control request for `REG_SYS_CFG1` (0x00f0). It does not
change device register state.

## API design

The implementation follows the FreeBSD 15.1 Kernel Developer's Manual,
`USBDI(9)`, for its USB host-driver interfaces:

- `usbd_lookup_id_by_uaa()` and `USB_PNP_HOST_INFO()` for USB device matching;
- `DRIVER_MODULE(..., uhub, ...)` for USB bus attachment;
- `usbd_do_request_flags()` for synchronous control requests;
- a device-private mutex to serialize register requests.

Official API manual:
https://man.freebsd.org/cgi/man.cgi?query=usbd_do_request_flags&sektion=9&manpath=FreeBSD+15.1-RELEASE+and+Ports

The raw request format is taken from the vendored RTW88 USB code: vendor
request 0x05, register address in wValue, wIndex 0, and little-endian payload.
The read-only chip identification register `REG_SYS_CFG1` is defined by the
vendored RTW88 register header as 0x00f0. This raw probe deliberately omits
the RTL8822B-specific `rtw_usb_reg_sec()` side operation (a write to 0x04e0
for selected register ranges); the complete chip register-access layer must
implement and validate that sequence separately.

The choice to retain an explicit FreeBSD `net80211` integration stage is
consistent with existing native FreeBSD wireless drivers such as `rtwn(4)`.
The native FreeBSD driver handbook is also a reference for driver/module
structure:
https://docs.freebsd.org/en/books/arch-handbook/driverbasics/

## Not implemented

- USB endpoint discovery and bulk/interrupt transfers
- Firmware download and checksum/startup sequencing
- RTL8822B MAC/BB/RF configuration and calibration
- TX/RX descriptors, aggregation, queue mapping and error recovery
- Native net80211 glue and wlan interface registration
- Suspend/resume and full transfer teardown/recovery

## Required validation

1. Build the module against the user's FreeBSD 15.1 source/build tree.
2. Load only this experimental module for the probe test; do not allow
   another driver to bind the same USB interface simultaneously.
3. Confirm dmesg reports attachment and a successful SYS_CFG1 read.
4. Record the SYS_CFG1 value and any `usbd_errstr()` failure.
5. Continue with endpoint discovery only after reviewing build/device logs.

No claim of functional wireless support is made at this stage.
