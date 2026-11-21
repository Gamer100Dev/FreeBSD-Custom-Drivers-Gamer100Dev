# Native RTW88 implementation status

## Current state

**Native transport and firmware primitives implemented in source; not yet built or hardware-tested.**

Development is on `work/native-rtw88-usb`, separate from `main`.
The module is at `sys/modules/rtw88_native/`; source is at
`sys/dev/rtw88_native/`.

## Implemented in source

- Newbus USB attachment matching Realtek `0bda:b812`.
- Descriptor-based allocation of one bulk-IN, three bulk-OUT and one
  interrupt-IN transfer object using `usbd_transfer_setup()`.
- Correct transfer release through `usbd_transfer_unsetup()`.
- Serialized native vendor-control requests using `usbd_do_request_flags()`
  with exact actual-length checking.
- Typed little-endian 8/16/32-bit register read/write helpers.
- RTL8822B-specific register section write to address `0x04e0` for the
  documented register ranges, kept as a raw control request to avoid
  recursion.
- A synchronous one-shot bulk-OUT transport helper using native USB transfer
  callbacks and condition variables. It is not currently called by attach.
- Firmware-framework acquisition for `rtw88/rtw8822b_fw.bin`, a parser for
  the 64-byte RTW88 header and DMEM/IMEM/optional EMEM segments, and a
  low-level firmware-page writer using the RTW88 USB chunk sizes.

## Important limitations

- These changes have **not** been compiled against the user's FreeBSD 15.1
  source tree and have **not** been loaded on the physical adapter.
- Firmware acquisition is implemented but is not yet called automatically.
  `firmware_get()` must run in a sleepable context, without the driver mutex,
  after `/boot/firmware` is available.
- The page writer does not perform the full RTL8822B firmware download.
  RTL8822B uses a newer firmware-reserved-TX-buffer and DDMA sequence.
- Bulk RX and interrupt RX callbacks remain inactive.
- No RTL8822B MAC/BB/RF startup, efuse parsing, calibration, native
  `net80211` interface, or working wlan device exists yet.

## FreeBSD API basis

The code is being implemented against FreeBSD 15.1 `USBDI(9)` and
`FIRMWARE(9)`, with the in-tree `rtwn_usb` transport used as a native USB
pattern reference. See:

- https://man.freebsd.org/cgi/man.cgi?query=usbd_do_request_flags&sektion=9&manpath=FreeBSD+15.1-RELEASE+and+Ports
- https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD+15.1-RELEASE&query=firmware&sektion=9

## Next implementation work

1. RTL8822B firmware-reserved-page TX descriptors and DDMA load/checksum/
   ready sequence.
2. MAC/BB/RF bring-up, efuse/board configuration and PHY calibration.
3. Active USB RX/event loops and normal TX descriptor path.
4. Native `net80211` integration and wlan lifecycle.
5. Recovery, suspend/resume and safe hot-unplug teardown.

The branch remains a development draft until a FreeBSD 15.1 build and
physical-device validation have been completed.
