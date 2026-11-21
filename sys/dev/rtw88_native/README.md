# Native FreeBSD RTW88 port

This directory starts a separate native FreeBSD driver path for the Realtek
RTL8822BU USB adapter (USB ID 0bda:b812).

## First milestone: native USB control transport

`if_rtw88_native.c` uses FreeBSD's USB host-driver interfaces directly:

- `struct usb_attach_arg` and `usbd_lookup_id_by_uaa()` for device matching;
- `DRIVER_MODULE(..., uhub, ...)` for USB bus attachment;
- `usbd_do_request_flags()` for synchronous vendor control transfers;
- a device-private FreeBSD mutex to serialize control requests.

It performs one read-only request to RTW88 `REG_SYS_CFG1` (0x00f0) at
attachment. The request format (vendor request 0x05, register in wValue,
zero wIndex, little-endian data) is taken from the vendored RTW88 USB
implementation. The driver logs whether the control transfer succeeds.

The module deliberately does **not** register a `wlan(4)` interface and does
not yet download firmware, set up bulk RX/TX, initialise the RTL8822B MAC/PHY,
or implement net80211 callbacks. It is a native transport bring-up stage,
not a usable Wi-Fi driver yet.

## Build location

The module Makefile is `sys/modules/rtw88_native/Makefile`. It is intended
to be built as a loadable kernel module against a FreeBSD 15 source/build
tree. It does not modify `sys/conf/files` or require a kernel configuration
change.

## Implementation order

1. Validate this native USB module on FreeBSD 15.1.
2. Add endpoint discovery and native asynchronous bulk/interrupt transfers
   using `usb_config`, `usbd_transfer_setup()`, callbacks and orderly
   `usbd_transfer_drain()/unsetup()`.
3. Port register access, firmware block download and RTL8822B startup.
4. Port the RTW88 packet descriptors, RX aggregation, TX queues and USB
   queue mapping.
5. Integrate with FreeBSD `net80211` and expose a `wlan(4)` interface.
6. Add suspend/resume, error recovery, teardown and documented tunables.

## API references used

- FreeBSD 15.1 Kernel Developer's Manual, `USBDI(9)`.
- FreeBSD source `sys/dev/rtwn/usb/rtwn_usb_reg.c` and
  `sys/dev/rtwn/usb/rtwn_usb_attach.c` for native USB driver conventions.
- FreeBSD `rtwn(4)` uses the native `net80211` stack; this driver will
  follow that model once the transport and chipset initialisation are ready.

Do not describe this stage as working Wi-Fi until it builds, attaches to the
target hardware, completes register access, downloads firmware, and passes
scan/association/TX/RX testing.
