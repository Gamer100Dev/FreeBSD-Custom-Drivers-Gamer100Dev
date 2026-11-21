# Native FreeBSD RTW88 port

This directory contains a separate native FreeBSD kernel-driver path for the
Realtek RTL8822BU USB adapter (USB ID `0bda:b812`). The implementation uses
FreeBSD newbus, USB USBDI, the kernel firmware framework, native mutexes and
condition variables. It does not use LinuxKPI or the Linuxulator.

## Implemented source components

- `if_rtw88_native.c`: newbus USB probe/attach/detach, endpoint setup, and
  lifetime management for synchronous I/O.
- `rtw88_native_usb.c`: descriptor-based allocation of one bulk-IN,
  three bulk-OUT and one interrupt-IN transfer; synchronous vendor-control
  requests; completion-signalled synchronous bulk-OUT.
- `rtw88_native_reg.c`: checked 8/16/32-bit little-endian register access
  and the RTL8822B USB security-section write to register `0x04e0`.
- `rtw88_native_fw.c`: FreeBSD firmware acquisition and RTW88 image parsing,
  firmware TX descriptor construction, reserved-page USB transmission,
  DDMA segment copy/checksum handling and MCU READY polling.
- `rtw88_native_mac.c`: USB-applicable RTL8822B power sequences, MAC system
  configuration, 3-endpoint TX DMA mapping, TX FIFO/page-table setup, H2C
  ring initialisation and base MAC/WMAC register configuration.
- `rtw88_native_efuse.c`: native physical eFuse reads, packed logical-map
  decoding, and RTL8822BU MAC/board-field extraction.
- `if_rtw88_native.h`: hardware definitions, per-device state and API.

## Explicit hardware-start entry point

`rtw88_native_hw_start()` sequences the current native stages:

1. RTL8822B USB power-on and MAC system configuration.
2. Acquire and validate `rtw88/rtw8822b_fw.bin` through FreeBSD
   `firmware_get()`.
3. Send DMEM/IMEM through the USB beacon/high-priority queue and RTL8822B
   DDMA engine; verify section checksums and firmware READY bits.
4. Read and decode the physical/logical eFuse map; extract the permanent
   MAC address and board configuration bytes.
5. Configure the 3-OUT-endpoint FIFO/page allocation, H2C ring and base
   8822B MAC/WMAC registers.

This routine is **not called by USB attach**. Firmware retrieval may access the
filesystem and therefore belongs in a sleepable device-start path after the
root filesystem is mounted. It is exposed for the future native `net80211`
start method, which has not been implemented yet.

## Current hardware scope

The FIFO and queue layout is specifically the RTW88 8822B table for three
bulk-OUT pipes. The transfer setup only succeeds when the active USB
interface supplies the expected endpoints. It does not silently fall back to
a two- or four-pipe layout.

Firmware with an optional EMEM segment is parsed but the current DDMA start
routine rejects it with `ENOTSUP` until its checksum/address behaviour is
validated. The firmware sequence also does not yet perform the Bluetooth
LTE-coexistence register save/restore used by the upstream RTW88 implementation.

The eFuse reader decodes the packed map and extracts the USB variant's
permanent MAC address, channel plan, country code, crystal cap, thermal
meter, PA/LNA, board and RFE bytes. Full board/RFE selection, hardware-feature
report handling, RF calibration and power-table programming are not yet
implemented.

## Not yet implemented

- Full RFE/board selection and PHY/BB/RF register-table programming.
- IQK/DPK calibration, antenna setup, channel control and regulatory
  handling.
- Active bulk-IN RX and interrupt/C2H event state machines.
- Normal TX packet descriptors, queue scheduling, aggregation, retries and
  TX status reporting.
- Native FreeBSD `net80211` attach/interface operations, scanning,
  authentication, association, key handling and power management.
- Suspend/resume, firmware recovery, watchdog and final hot-unplug tests.

## FreeBSD API references

The code is being developed against FreeBSD 15.1 and its in-tree native
wireless/USB conventions:

- `USBDI(9)`: `usbd_transfer_setup()`, `usbd_transfer_unsetup()`,
  `usbd_do_request_flags()`, USB transfer callbacks and external buffers.
- `FIRMWARE(9)`: `firmware_get()` and `firmware_put()`.
- Existing native USB driver patterns:
  `sys/dev/rtwn/usb/rtwn_usb_attach.c`,
  `sys/dev/rtwn/usb/rtwn_usb_ep.c`,
  `sys/dev/rtwn/usb/rtwn_usb_rx.c`,
  `sys/dev/rtwn/usb/rtwn_usb_tx.c` and
  `sys/dev/rtwn/usb/rtwn_usb_reg.c`.

The module Makefile is `sys/modules/rtw88_native/Makefile`. It builds as a
separate loadable module and does not modify `sys/conf/files`.

**Validation status:** source implementation only. It has not yet been
compiled against the user's FreeBSD 15.1 tree and has not been run on the
physical RTL8822BU device. Do not treat it as a working Wi-Fi driver until
build, firmware, RX/TX and `net80211` validation passes.
