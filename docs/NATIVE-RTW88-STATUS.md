# Native RTW88 implementation status

## Current state

**Stage 1 source added; not yet build- or hardware-verified.**

A separate native USB kernel module has been introduced at
`sys/dev/rtw88_native/` with its module Makefile at
`sys/modules/rtw88_native/Makefile`.

It targets Realtek USB ID `0bda:b812` and a vendor-specific
`ff/ff/ff` interface. At attach it allocates five native USB transfers
against the actual interface descriptors:

- one bulk-IN endpoint;
- three bulk-OUT endpoints, selected by `ep_index` 0, 1 and 2;
- one interrupt-IN endpoint.

The transfer objects are not started, so this stage does not submit packets.
The driver also performs one raw, read-only vendor control request for RTW88
`REG_SYS_CFG1` (0x00f0).

## API design

The implementation follows the FreeBSD 15.1 Kernel Developer's Manual,
`USBDI(9)`:

- `usbd_lookup_id_by_uaa()` and `USB_PNP_HOST_INFO()` for USB matching;
- `DRIVER_MODULE(..., uhub, ...)` for USB bus attachment;
- `usbd_transfer_setup()` and `usbd_transfer_unsetup()` for endpoint
  resource allocation/release;
- `usbd_do_request_flags()` for synchronous control requests.

Official API manual:
https://man.freebsd.org/cgi/man.cgi?query=usbd_do_request_flags&sektion=9&manpath=FreeBSD+15.1-RELEASE+and+Ports

The raw request format is taken from the vendored RTW88 USB code: vendor
request 0x05, register address in wValue, wIndex 0, and little-endian payload.
The read-only chip identification register `REG_SYS_CFG1` is defined by
the vendored RTW88 register header as 0x00f0. This raw probe deliberately
omits the RTL8822B-specific `rtw_usb_reg_sec()` side operation (a write to
0x04e0 for selected register ranges); the complete register-access layer
must implement and validate that sequence separately.

## Not implemented

- Active bulk RX, bulk TX and interrupt callbacks/state machines
- Firmware download and checksum/startup sequencing
- RTL8822B MAC/BB/RF configuration and calibration
- TX/RX descriptors, aggregation, queue mapping and error recovery
- Native net80211 glue and wlan interface registration
- Suspend/resume and active-transfer teardown/recovery

## Required validation

1. Build the module against the user's FreeBSD 15.1 source/build tree.
2. Load only this experimental module for the probe test; do not allow
   another driver to bind the same USB interface simultaneously.
3. Confirm dmesg reports attach, all five transfer allocations, and the
   SYS_CFG1 control-read result.
4. Record the SYS_CFG1 value, the transfer setup result and any
   `usbd_errstr()` failure.
5. Continue with active data transfers only after reviewing build/device logs.

No claim of functional wireless support is made at this stage.
