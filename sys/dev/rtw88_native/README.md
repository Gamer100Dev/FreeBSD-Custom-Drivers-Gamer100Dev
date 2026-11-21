# Native FreeBSD RTW88 port

This directory starts a separate native FreeBSD driver path for the Realtek
RTL8822BU USB adapter (USB ID 0bda:b812).

## First milestone: native USB transport allocation

`if_rtw88_native.c` uses FreeBSD's USB host-driver interfaces directly:

- `usbd_lookup_id_by_uaa()` / `USB_PNP_HOST_INFO()` for adapter matching;
- `DRIVER_MODULE(..., uhub, ...)` for newbus USB attachment;
- `usbd_transfer_setup()` to allocate one bulk-IN, three bulk-OUT and one
  interrupt-IN transfer from the active interface descriptors;
- `usbd_transfer_unsetup()` during detach and attach failure;
- `usbd_do_request_flags()` for a synchronous vendor control-register read.

Endpoint addresses are selected by endpoint type and direction using
`UE_ADDR_ANY` and `ep_index`; the implementation does not hard-code the
adapter's observed endpoint numbers. The transfer objects are currently
allocated but **never started**. The callback is intentionally inert until
firmware, MAC and RX/TX state machines exist.

At attach, the driver also performs one raw, read-only request to RTW88
`REG_SYS_CFG1` (0x00f0). This tests the native control pipe, not full chip
initialisation. The raw helper intentionally does not implement the RTL8822B
section-specific side operation (`rtw_usb_reg_sec()` writes register 0x04e0
for selected register ranges), so it must not be treated as the final chipset
register-access implementation.

## Build location

The module Makefile is `sys/modules/rtw88_native/Makefile`. It is intended
to be built as a loadable module against a FreeBSD 15 source/build tree. It
does not modify `sys/conf/files` or require a kernel configuration change.

## Implementation order

1. Build and validate the native transport module on FreeBSD 15.1.
2. Verify all five endpoint matches against the actual descriptor set.
3. Implement proper RTL8822B USB control-register semantics, including
   section handling and checked register read/write paths.
4. Port firmware block download and checksum/startup sequencing.
5. Port the RTW88 TX/RX descriptors, aggregation, queue mapping and recovery.
6. Integrate with FreeBSD `net80211` and expose a `wlan(4)` interface.
7. Add suspend/resume, active-transfer teardown and error recovery.

## API references used

- FreeBSD 15.1 Kernel Developer's Manual, `USBDI(9)`.
- In-tree FreeBSD native USB wireless driver patterns:
  `sys/dev/rtwn/usb/rtwn_usb_attach.c`,
  `sys/dev/rtwn/usb/rtwn_usb_ep.c` and
  `sys/dev/rtwn/usb/rtwn_usb_reg.c`.
- FreeBSD `rtwn(4)` integrates with the native `net80211` stack; this
  driver will follow that model once transport and chipset startup are ready.

Do not describe this stage as working Wi-Fi until it builds, attaches to the
target hardware, downloads firmware, and passes scan/association/TX/RX tests.
