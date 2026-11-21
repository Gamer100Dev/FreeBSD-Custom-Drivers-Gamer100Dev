# Native FreeBSD RTW88 port

This directory contains a separate native FreeBSD kernel-driver path for the
Realtek RTL8822BU USB adapter (USB ID `0bda:b812`). The implementation uses
FreeBSD newbus, USB USBDI, the kernel firmware framework, native mutexes and
condition variables. It does not use LinuxKPI or the Linuxulator.

## Implemented source components

- `if_rtw88_native.c`: newbus USB probe/attach/detach, endpoint setup, and
  lifetime management for synchronous I/O.
- `rtw88_native_usb.c`: descriptor-based allocation of four concurrent
  32 KiB bulk-IN transfers (matching upstream RTW88 USB RX queue depth), three
  bulk-OUT transfers and one interrupt-IN diagnostic transfer; synchronous
  vendor-control requests; completion-signalled synchronous bulk-OUT; callback
  and drain lifecycle for all asynchronous transfers.
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

After the module has attached, the device exposes an explicit sysctl:

```sh
sysctl dev.rtw88_native.0.hw_start=1
sysctl dev.rtw88_native.0.intr_count
sysctl dev.rtw88_native.0.intr_last_len
sysctl dev.rtw88_native.0.hw_start=0
```

Use the actual unit number shown by `dmesg` if it is not `0`. The start
handler serializes start/stop operations, calls firmware loading only in
sleepable process context, and starts the interrupt-IN capture after firmware
and base MAC setup. Setting it to `0` drains interrupt capture before power
down. The counters report raw interrupt completion only; C2H parsing and
event dispatch are not implemented.

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
6. Start continuous raw bulk-IN and interrupt-IN capture.

This routine is **not called by USB attach**. Firmware retrieval may access the
filesystem and therefore belongs in a sleepable device-start path after the
root filesystem is mounted. It is currently exposed through the device
`hw_start` sysctl as a controlled bring-up hook; the future native
`net80211` start method can call the same routine once that interface exists.

## Current hardware scope

The FIFO and queue layout is specifically the RTW88 8822B table for three
bulk-OUT pipes. The transfer setup only succeeds when the active USB
interface supplies the expected endpoints. It does not silently fall back to
a two- or four-pipe layout.

Firmware with an optional EMEM segment is parsed and passed through the same
DDMA segment/checksum path as the upstream RTW88 implementation. Actual
EMEM-bearing firmware must still be validated on hardware before claiming
that path works. The firmware sequence also does not yet perform the Bluetooth
LTE-coexistence register save/restore used by the upstream RTW88 implementation.

The eFuse reader decodes the packed map and extracts the USB variant's
permanent MAC address, channel plan, country code, crystal cap, thermal
meter, PA/LNA, board and RFE bytes. Full board/RFE selection, hardware-feature
report handling, RF calibration and power-table programming are not yet
implemented.

Bulk-IN batches are copied to a per-device latest-buffer snapshot and
walked using the 24-byte RTL8822B RX descriptor layout. The parser validates
packet length, driver-info length, shift and 8-byte aggregate alignment before
advancing. It counts normal data frames and identifies C2H records using the
RXD C2H bit, retaining the last command ID, sequence and bounded payload
metadata. It does not yet decode PHY status, perform C2H dispatch or inject
802.11 frames into net80211. The independent interrupt-IN endpoint remains
a raw diagnostic capture.

The MAC setup programs `RXFLTMAP0`, `RXFLTMAP1`, and `RXFLTMAP2` as
separate 16-bit registers, matching the upstream RTL8822B initialization
rather than writing the adjacent filter maps as one combined 32-bit value.
The diagnostic reports each map independently so their readback can be
compared against the programmed values.

## Not yet implemented

- Full RFE/board selection and PHY/BB/RF register-table programming.
- IQK/DPK calibration, antenna setup, channel control and regulatory
  handling.
- RX PHY-status decoding and full RX descriptor processing.
- C2H command-specific event dispatch/workqueue and normal 802.11 frame
  delivery into net80211.
- Normal TX packet descriptors, queue scheduling, aggregation, retries and
  TX status reporting.
- Native FreeBSD `net80211` attach/interface operations, scanning,
  authentication, association, key handling and power management.
- Suspend/resume, firmware recovery, watchdog and final hot-unplug tests.

## FreeBSD API references

The code is being developed against FreeBSD 15.1 and its in-tree native
wireless/USB conventions:

- `USBDI(9)`: `usbd_transfer_setup()`, `usbd_transfer_unsetup()`,
  `usbd_transfer_drain()`, `usbd_do_request_flags()`, USB transfer
  callbacks and external buffers.
- `FIRMWARE(9)`: `firmware_get()` and `firmware_put()`.
- Existing native USB driver patterns:
  `sys/dev/rtwn/usb/rtwn_usb_attach.c`,
  `sys/dev/rtwn/usb/rtwn_usb_ep.c`,
  `sys/dev/rtwn/usb/rtwn_usb_rx.c`,
  `sys/dev/rtwn/usb/rtwn_usb_tx.c` and
  `sys/dev/rtwn/usb/rtwn_usb_reg.c`.

The module Makefile is `sys/modules/rtw88_native/Makefile`. It builds as a
separate loadable module and does not modify `sys/conf/files`.

**Validation status (2026-11-22):** the native module builds successfully
on the user's FreeBSD 15.1 amd64 host, and the current diagnostic branch passes
the FreeBSD and FreeBSD 15.1 GitHub Actions build jobs. The RTL8822BU device
(`0bda:b812`) has been tested on physical hardware: firmware loads, passive
2.4-GHz scanning discovers two BSSs, and the bulk-IN receive path delivers
frames through the RX descriptor parser into net80211. One captured scan
reported 26 bulk-IN completions, 26 parsed descriptors, 26 data frames and
26 net80211 inputs, with zero USB RX errors, malformed records or input drops.

This validates the passive 2.4-GHz receive/scan path on the tested device.
It does **not** validate transmission, probe requests, authentication,
association, DHCP, sustained traffic, 5-GHz operation or power management.
TX and association remain unimplemented; keep this driver in the experimental
passive-scan stage until those paths are implemented and tested.


## RX hardware register snapshot

The diagnostic branch adds a read-only sysctl that samples the MAC/RXDMA
configuration and RTL8822B receive statistics through vendor-control reads.
Run it after a scan has completed, because the register snapshot temporarily
serializes against channel changes:

```sh
sysctl dev.rtw88_native.0.rx_hw_status
```

The output includes the MAC TRX enable register, TXDMA queue/arbiter map,
RXDMA mode and aggregation threshold, receive filters, RX path selection,
CCK/OFDM false-alarm counters, per-modulation CRC counters, OFDM and CCK CCA
counts, RFE selection/inversion and TR-switch readback, CCA thresholds, both
path IGI values, and the latest raw interrupt-IN report as hexadecimal bytes.
The RX path-select register is included because upstream RTL8822B false-alarm
accounting uses bit 28 of `0x0808` to determine whether CCK statistics are
active. The raw interrupt-IN payload is captured under the device mutex and copied
into the snapshot before formatting. It is a transport diagnostic only:
upstream RTW88 receives packet aggregates on bulk-IN and does not decode this
interrupt payload in its normal RX path. Therefore, the interrupt payload is
not evidence that 802.11 frames are reaching the host. The snapshot does not
change hardware configuration.

The native 2.4-GHz channel sequence follows upstream ordering: baseband/ADC,
MAC channel width and timing, RF synthesizer, RX DFIR, RX gain-index toggle,
CCA thresholds, then RFE routing.

The channel setup also uses only the shared base RFE control registers for
RFE option 3, matching upstream `rtw8822b_set_channel_rfe_ifem()`.
It does not write speculative `+0x200` copies of those controls.
