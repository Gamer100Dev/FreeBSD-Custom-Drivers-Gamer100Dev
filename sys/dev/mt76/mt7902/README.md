# Native FreeBSD MT7902 driver work

This directory contains a native FreeBSD PCI/MMIO/busdma implementation in
progress for MediaTek MT7902 (`14c3:7902`). It has no LinuxKPI or mac80211
dependency. The sibling `rtw88_native` tree was used for FreeBSD lifecycle and
net80211 patterns; the mt76/Linux source is protocol reference material.

## Implemented in this directory

* PCI ID probe, BAR0 mapping, bus mastering, chip ID validation, and bounded
  direct, fixed-window, and remapped register access.
* Firmware container parsing and validation for the Connac2 WM image and ROM
  patch image, with FreeBSD `firmware_get()`/`firmware_put()` ownership.
* Native busdma descriptor and payload ring allocation for WFDMA TX, firmware
  download, MCU event, and data queues.
* MSI/legacy interrupt setup, event-ring servicing, sequence-matched MCU
  completion wakeups, and Connac2 firmware download/patch command framing.
* Automatic firmware/WFDMA startup from PCI attach, with process-context
  `hw_start` retry and stop controls if startup fails.
* A native net80211 STA/monitor registration, 2.4/5 GHz channel list, software
  scan hooks, channel switch MCU requests, and basic STA/BSS association state
  updates. Active scan probe requests use the normal net80211 software path.
* WPA2 CCMP through net80211 software crypto. The driver does not advertise
  hardware key offload; keys stay in the host crypto path.
* A basic WFDMA TX queue for already-encapsulated 802.11 frames.
* An optional, separate `ng_ubt_mt7902` USB subclass. It validates the
  MT7902 79xx Bluetooth firmware section map, sends WMT patch-download
  commands over the USB control pipe, and then inherits HCI/ACL/SCO and
  Netgraph operation from FreeBSD `ng_ubt`.

Build the Wi-Fi module from this directory on FreeBSD:

```sh
make -C sys/dev/mt76/mt7902 SRCTOP=/usr/src
make -C sys/dev/mt76/mt7902 -f Makefile.bt SRCTOP=/usr/src
```

Install `mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin` where FreeBSD's firmware
loader can resolve the `firmware_get()` name before attaching the Bluetooth
USB interface. The Bluetooth subclass recognizes `0e8d:7902` and common
`13d3` OEM IDs. Its probe performs the WMT firmware load before `ng_ubt`
attaches, so a missing firmware file or failed transfer prevents that USB
driver from attaching.

On a system where the device attaches, firmware startup runs automatically.
net80211 assigns the device the next free WLAN name (wlan1 when wlan0 already
exists). If startup fails, `dev.mt7902_native.0.hw_start=1` retries it in
process context; setting it to `0` stops IRQ and DMA. Current hardware attempts
still time out waiting for the MCU patch-semaphore response, so wlan1 is not
registered yet.

## Current limits

The Wi-Fi data RX task intentionally remains a no-op, so received beacons,
management frames, and data frames are not delivered to net80211. That prevents
scan results and a complete station association from working in practice even
though the registration and command paths are present. TX completion currently
waits on descriptor ownership and has not been confirmed against the hardware.
Association records and channel-switch payloads are a minimal port and need
validation against MT7902 firmware behavior.

Bluetooth WMT firmware transfer is implemented, but firmware activation,
subsequent generic HCI setup, suspend/resume, and the Netgraph data path have
not been validated on MT7902 hardware. The inherited `ng_ubt` driver provides
the generic HCI transport after the subclass probe succeeds.

Both modules have compile validation only. Firmware startup, register
mappings, DMA ownership, scan/association exchanges, Bluetooth WMT responses,
and teardown still require hardware validation. A successful build does not
establish working radio operation.

The firmware files and their redistribution terms are documented with the
firmware payloads elsewhere in the source tree. Keep all MT7902 driver code,
including future build glue, in this directory as requested.
