# Native FreeBSD RTW88 port

This directory contains a separate native FreeBSD kernel-driver path for
the Realtek RTL8822BU USB adapter (USB ID 0bda:b812). It is developed against
the FreeBSD 15.1 kernel APIs and the in-tree native USB driver conventions.

## Implemented source components

- `if_rtw88_native.c`: newbus probe, attach and detach.
- `rtw88_native_usb.c`: FreeBSD USB transfer allocation, synchronous
  vendor-control requests, and one-shot synchronous bulk-OUT submission.
- `rtw88_native_reg.c`: checked 8-, 16- and 32-bit little-endian register
  access, including the RTL8822B USB security-section write at 0x04e0.
- `rtw88_native_fw.c`: FreeBSD firmware-framework acquisition, RTW88
  64-byte firmware-header/segment validation, and the USB firmware-page
  control-transfer primitive.
- `if_rtw88_native.h`: device state, interface constants and module APIs.

## USB transport

The driver binds only to Realtek `0bda:b812` on a vendor-specific
`ff/ff/ff` interface. At attach, it allocates five transfer objects from
the interface descriptors:

- one bulk-IN endpoint;
- three bulk-OUT endpoints selected using endpoint `ep_index` 0, 1 and 2;
- one interrupt-IN endpoint.

Endpoint addresses are not hard-coded. Bulk OUT has a one-buffer synchronous
submission helper that waits on a condition variable for the native USB
callback to report completion. The helper preserves the caller's buffer
until completion. It is a transport primitive only; no network packets or
firmware-DMA packets are submitted by attach.

Bulk RX and interrupt RX objects are allocated but not started. Their
callbacks are intentionally inert.

## Register access

The native control helper uses `usbd_do_request_flags()`, passes the device
mutex following the FreeBSD USB request locking contract, and verifies the
actual transfer length. Typed register helpers perform little-endian
conversion and return USB errors instead of silently substituting all-ones
values. The RTL8822B section operation writes the first data byte to register
0x04e0 after normal register accesses in the always-on and 0x1000-0x10ff
windows. The raw SYS_CFG1 probe intentionally bypasses this extra operation
so it remains a transport-only diagnostic.

## Firmware handling

The firmware API uses the FreeBSD `firmware_get()` / `firmware_put()`
framework and the image name `rtw88/rtw8822b_fw.bin`. On FreeBSD 15.1 this
resolves the binary below `/boot/firmware/` when that package/file is
installed. Firmware acquisition is a sleepable operation: it must be called
without the driver mutex held and only after the firmware filesystem is
available.

The parser verifies that the 64-byte RTW88 firmware header and the declared
DMEM, IMEM and optional EMEM payloads (including each 8-byte checksum trailer)
account for the exact image length. The USB page writer implements the RTW88
USB chunk sizes (196-byte blocks, followed by 8-byte and 1-byte tails) and
programs the firmware page field before writing a page.

**The full RTL8822B firmware boot sequence is not yet implemented.** In
particular, the 8822B uses the newer DDMA/firmware-reserved-TX-buffer path.
The USB page writer is only one low-level operation; it is not by itself a
complete firmware downloader and is not currently called automatically.

## Build location

The loadable module Makefile is `sys/modules/rtw88_native/Makefile`.
It does not modify `sys/conf/files` or require a kernel configuration
change. Build and run it only against a FreeBSD 15.1 source/build tree.

## Remaining implementation

1. Port the RTL8822B-specific firmware reserved-page packet format and
   DDMA sequence, including checksum status and CPU start/ready polling.
2. Port MAC/BB/RF power-on, efuse/board parsing, PHY tables and calibration.
3. Add the active bulk RX and interrupt event state machines.
4. Port TX descriptors, RX aggregation and hardware queue mapping.
5. Integrate device lifecycle and operations with native FreeBSD
   `net80211` and expose a `wlan(4)` interface.
6. Add suspend/resume, watchdog, recovery, and full detach synchronization.

## API references

- FreeBSD 15.1 Kernel Developer's Manual: `USBDI(9)`
  (`usbd_transfer_setup()`, `usbd_transfer_unsetup()`,
  `usbd_do_request_flags()`, and transfer callback conventions).
- FreeBSD 15.1 Kernel Developer's Manual: `FIRMWARE(9)`
  (`firmware_get()`, `firmware_put()`, and binary firmware loading).
- In-tree native FreeBSD driver references:
  `sys/dev/rtwn/usb/rtwn_usb_attach.c`,
  `sys/dev/rtwn/usb/rtwn_usb_ep.c`,
  `sys/dev/rtwn/usb/rtwn_usb_reg.c`.

This driver is not usable Wi-Fi yet. Do not call it functional until it builds
on FreeBSD, correctly boots firmware, registers with net80211, and passes
scan, association, TX/RX, suspend/resume and unplug tests.
