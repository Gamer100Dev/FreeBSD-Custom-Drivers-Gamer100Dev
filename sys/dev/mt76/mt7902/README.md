# Native FreeBSD MT7902 driver work

This directory contains a native FreeBSD PCI/MMIO/busdma implementation for
MediaTek MT7902 (`14c3:7902`). It has no LinuxKPI or mac80211 runtime
dependency. The sibling `rtw88_native` tree provides FreeBSD lifecycle and
net80211 patterns; Linux `mt76` is used as a hardware/protocol reference.

## Implemented

* PCI ID probe, BAR0 mapping, bus mastering, chip ID validation, and bounded
  direct, fixed-window, and remapped register access.
* Connac2 firmware container parsing and validation for the WM image and ROM
  patch image, with FreeBSD `firmware_get()`/`firmware_put()` ownership.
* Native busdma-backed WFDMA TX, firmware-download, MCU event, and ordinary RX
  rings. The MT7902-specific layout uses RX event ring 0, data RX ring 2,
  MCU-WM TX ring 15, and firmware-download TX ring 16.
* MSI/legacy interrupt setup, MCU event processing, sequence-matched response
  wakeups, PCIe MCU RX-wake handling, and native firmware download commands.
* Firmware startup and WFSYS recovery, with process-context `hw_start`
  retry and stop controls.
* Native net80211 STA/monitor registration, 2.4/5 GHz channel list, software
  scanning, channel-switch requests, and basic STA/BSS association records.
* A first-stage Connac2 normal-RXD decoder. It bounds-checks optional RXD
  groups, extracts channel/RSSI metadata, copies the 802.11 frame into an mbuf,
  and feeds FreeBSD `ieee80211_input_mimo_all()`. Beacon and probe-response
  frames can now reach the net80211 scan cache.
* RX descriptor rearming that preserves descriptor-to-buffer ownership across
  ring wrap, plus scan-end RX/drop counters and last-RXD diagnostics.
* A basic WFDMA TX queue for already-encapsulated 802.11 frames. WPA2 CCMP is
  configured for the net80211 software crypto path; hardware key offload is not
  advertised.
* An optional, separate `ng_ubt_mt7902` USB subclass. It validates the
  MT7902 79xx Bluetooth firmware section map, sends WMT patch-download
  commands over the USB control pipe, and then inherits HCI/ACL/SCO and
  Netgraph operation from FreeBSD `ng_ubt`.

The native Wi-Fi module can be built on FreeBSD with:

```sh
make -C sys/dev/mt76/mt7902 SRCTOP=/usr/src
```

The repository's FreeBSD CI job also builds this module against FreeBSD 15.1.
The Bluetooth module is built separately with:

```sh
make -C sys/dev/mt76/mt7902 -f Makefile.bt SRCTOP=/usr/src
```

Install `mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin` where FreeBSD's firmware
loader can resolve the `firmware_get()` name before attaching the Bluetooth
USB interface. The Bluetooth subclass recognizes `0e8d:7902` and common
`13d3` OEM IDs. Its probe performs the WMT firmware load before `ng_ubt`
attaches, so a missing firmware file or failed transfer prevents that USB
driver from attaching.

On a system where the device attaches, firmware startup runs automatically.
net80211 assigns the device the next free WLAN name (for example, `wlan1`
when another wireless interface already owns `wlan0`). If startup fails,
`dev.mt7902_native.0.hw_start=1` retries it in process context; setting it
to `0` stops IRQ and DMA.

## Current limitations and validation status

Firmware boot and net80211 registration have been observed on MT7902 hardware.
That confirms PCI/MMIO, firmware transfer, MCU response handling, and the
firmware-ready handshake; it does **not** yet establish that the radio can
reliably scan, associate, or pass traffic.

The RX implementation is intentionally a first usable stage, not a complete
port of every mt76 RX feature. It accepts normal 802.11 RX packets and safely
drops unsupported packet types, header-translated frames, and frames marked
with FCS/ICV or RXD error flags. Hardware-decrypted CCMP/TKIP/GCMP metadata and
stripped IV/MIC handling still need to be completed and checked against
net80211's crypto path. AMPDU reordering, A-MSDU handling, full PHY/rate
metadata, monitor-mode error-frame delivery, and the complete RX control/event
taxonomy are not implemented.

TX currently uses a basic fixed-rate legacy descriptor and polls descriptor
ownership for completion. It does not yet implement the complete mt76 rate
control, aggregation, TX status, retransmission, power-save, or queue-flow
control machinery. Basic station/BSS association payloads and software
encryption are present but are not yet validated end-to-end.

Scanning currently uses FreeBSD's software scanner and the normal channel-switch
and probe-request TX paths. Dedicated firmware hardware scan, scheduled scan,
scan cancellation offload, complete regulatory-domain handling, and 6 GHz
operation are not implemented. The advertised channel/mode set remains
conservative (2.4/5 GHz legacy modes); HT/VHT/HE capabilities are not advertised.

Runtime power management, suspend/resume, WoWLAN, thermal/SAR control, advanced
statistics, firmware recovery, and device reset sequencing still need work.
The Bluetooth WMT transfer is implemented, but firmware activation, subsequent
generic HCI setup, suspend/resume, and the Netgraph data path have not been
validated on MT7902 hardware.

A successful module build is necessary but not sufficient. The new RX path,
scan results, TX completion, association, encrypted traffic, teardown, and
Bluetooth operation all require hardware validation. Preserve the working
RTW88 interface while testing MT7902 as a separate VAP.

The firmware files and their redistribution terms are documented with the
firmware payloads elsewhere in the source tree. Keep all MT7902 driver code,
including future build glue, in this directory as requested.
