# Native FreeBSD MT7902 driver

This directory contains a native FreeBSD PCI/MMIO/busdma implementation for
MediaTek MT7902 (`14c3:7902`). It has no LinuxKPI or mac80211 runtime
dependency. The sibling `rtw88_native` driver is the FreeBSD lifecycle and
net80211 reference; Linux `mt76` is used as the hardware protocol reference.

## Implemented

* PCI ID probe, BAR0 mapping, PCI bus mastering, chip ID validation, and
  bounded direct, fixed-window, and remapped register access.
* Connac2 firmware container parsing and validation for the WM image and ROM
  patch image, with FreeBSD `firmware_get()`/`firmware_put()` ownership.
* Native busdma-backed WFDMA TX, firmware-download, MCU event, and ordinary RX
  rings. The MT7902-specific layout uses RX event ring 0, data RX ring 2,
  four access-category data TX rings 0-3, MCU-WM TX ring 15, and
  firmware-download TX ring 16.
* MSI/legacy interrupt setup, MCU event processing, serialized
  response-bearing MCU transactions (the firmware exposes one response
  mailbox), sequence-matched response wakeups, PCIe MCU RX-wake handling, and native firmware download commands.
  RX ring 0 is shared by MCU events and TX-status notifications; the receive
  worker dispatches by RXD packet type, so short 16-byte TXS headers are not
  mistaken for truncated MCU responses. Data ring 2 is polled after either
  ring-0 or ring-2 receive interrupts to handle coalesced notifications.
* Firmware startup and WFSYS recovery, with process-context `hw_start`
  retry/stop controls. WFDMA shutdown waits for TX/RX idle; if the engines do
  not quiesce, PCI bus mastering is disabled before ring memory is released.
* Native net80211 STA/monitor registration, 2.4/5 GHz legacy plus firmware-
  gated 802.11n/ac channel modes, software scanning, acknowledged channel
  changes, and station/BSS association. HT/VHT interface capabilities, NSS,
  MCS maps and channel widths are derived from firmware capability data.
  Association sends Connac2 BSS basic/QBSS and SET_BSS_ABORT /
  SET_BSS_CONNECTED commands, plus per-peer STA_REC BASIC/HT/VHT/PHY/RA/STATE
  and WTBL GENERIC/RX/HT/VHT/HDR_TRANS records. Legacy and HT MCS masks and
  VHT RX/TX maps are taken from the negotiated FreeBSD node. Disconnect removes
  the station and resets its WTBL entry. AP BSSID/AID/DTIM/QoS state is cached
  so teardown still identifies the peer when net80211 has already dropped the
  BSS node. HE and 6 GHz are intentionally disabled for this Wi-Fi 5 port.
* Negotiated legacy CCK/OFDM rate masks and AP basic rates are passed to the
  firmware. Fixed-rate management/multicast TX chooses the lowest AP basic
  rate when associated, with 1 Mbps CCK / 6 Mbps OFDM defaults for
  unassociated operation.
* A first-stage Connac2 normal-RXD decoder. It bounds-checks optional RXD
  groups, validates full native 802.11 header lengths, extracts channel/RCPI
  metadata, copies the frame into an mbuf, and feeds FreeBSD
  `ieee80211_input_mimo_all()`. Beacon and probe-response frames can reach
  the net80211 scan cache. The parser passes hardware-decryption and IV/MIC
  strip metadata to net80211.
* RX descriptor rearming that rotates completed buffers into the reserved
  producer slot, preserving descriptor-to-buffer ownership across ring wrap.
  Scan-end RX/drop counters and last-RXD diagnostics are available. A monitor
  VAP requests control and failed-FCS frames, receives FCS/TKIP-MIC error flags,
  and gets ICV-failed frames through radiotap only; ICV errors never enter the
  normal station input path.
* Native FreeBSD radiotap attachment for monitor/BPF use. RX reports
  channel frequency/flags, dBm signal/noise and failed-FCS state; ICV-failed
  frames are tapped only to monitor listeners. TX reports channel metadata.
  TSFT and PHY rate fields are deliberately omitted until the RX descriptor's
  TSF/rate information is decoded.
* Four Connac2 access-category TX rings mapped from FreeBSD WME ACs, plus a
  native WME update callback that programs per-AC EDCA contention parameters
  through CE `SET_EDCA_PARMS`. The 32-byte hardware TXP carries the payload
  DMA pointer and 12-bit frame length separately from the 32-byte TXWI.
  Descriptor ownership is polled in process context; DMA sync state is kept
  across timeouts so an outstanding device DMA is not reused prematurely.
* Native FreeBSD PCI `device_suspend`/`device_resume` methods. The driver
  suspends net80211 VAPs, stops WFDMA, reloads firmware on resume, and then
  restores the VAP state with `ieee80211_resume_all()`.
* An optional, separate `ng_ubt_mt7902` USB subclass. It validates the
  MT7902 79xx Bluetooth firmware section map, sends WMT patch-download
  commands over the USB control pipe, and inherits HCI/ACL/SCO and Netgraph
  operation from FreeBSD `ng_ubt`.

## Build

Build the Wi-Fi module on FreeBSD with:

```sh
make -C sys/dev/mt76/mt7902 SRCTOP=/usr/src
```

The standalone module entry point is also supported:

```sh
make -C sys/modules/mt7902_native SRCTOP=/usr/src
```

The repository's FreeBSD CI job builds both Wi-Fi entry points and the optional
Bluetooth module against FreeBSD 15.1.
The Bluetooth module is built separately:

```sh
make -C sys/dev/mt76/mt7902 -f Makefile.bt SRCTOP=/usr/src
```

Install `mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin` where FreeBSD's firmware
loader can resolve the `firmware_get()` name before attaching the Bluetooth
USB interface. The Bluetooth subclass recognizes `0e8d:7902` and common
`13d3` OEM IDs. Its probe performs the WMT firmware load before `ng_ubt`
attaches, so a missing firmware file or failed transfer prevents that USB
driver from attaching.

Wi-Fi firmware startup runs automatically when the PCI device attaches.
net80211 assigns the next free WLAN name (for example, `wlan1` when another
wireless interface already owns `wlan0`). If startup fails, use:

```sh
sysctl dev.mt7902_native.0.hw_start=1
```

Setting `hw_start=0` stops the IRQ and DMA engines. The sysctl is intended
for recovery/diagnostics; normal operation starts the firmware during attach.

## Remaining limitations and validation

Firmware boot and net80211 registration have been observed on MT7902 hardware.
That confirms PCI/MMIO, firmware transfer, MCU response handling, and the
firmware-ready handshake. The recent code also builds in FreeBSD 15.1 CI.
Neither result proves that scanning, association, traffic, suspend/resume, or
Bluetooth operation works reliably over the air/on the device.

The RX implementation is a first-stage port, not a complete replacement for
all mt76 RX features. It accepts normal native-header 802.11 frames and
safely drops unsupported packet types, header-translated frames, A-MSDU
payload formats, and unsupported RXD error conditions. ICV-failed frames are
sent only to radiotap monitor listeners; they are never sent to the station
input path. Protected traffic needs end-to-end validation with the FreeBSD
crypto path. AMPDU reordering, A-MSDU deaggregation, complete PHY/rate/TSFT
metadata, and the full RX control/event taxonomy remain outstanding.

TX uses basic legacy TXWI/TXP descriptors and four hardware queues with
FreeBSD WME/EDCA programming. The driver keeps each packet/node pending until
the firmware's Connac2 TX_FREE notification returns its MSDU token, then
completes the net80211 packet with the reported success/failure status.
Outstanding packets are failed and drained only after WFDMA is stopped.
TXS notification records are parsed for per-record WCID/PID/rate, ACK/RTS/queue
timeouts, failure/retry counts, and transmitted MPDU bytes. TXS is currently
diagnostic accounting rather than a rate-control input. Full mt76 rate control,
AMPDU aggregation, hardware key offload, power-save buffering, and stronger
queue backpressure are not implemented. Unicast data rate selection and all
TX behavior still require over-the-air validation.

Scanning currently uses FreeBSD's software scanner and normal channel-switch
and probe-request TX paths. The channel-switch command waits for firmware
acknowledgement; ring-0 TXS notifications are recognized, and ring 2 is polled
on shared ring-0 interrupts as well as its own data interrupt. Hardware
beacon reception and scan results still require confirmation on the device.
Dedicated firmware hardware scan, scheduled scan, scan cancellation offload,
and complete regulatory-domain handling remain outstanding. Firmware-gated
HT/VHT mode/channel capabilities and peer MCS negotiation are now connected
to net80211, but end-to-end 802.11n/ac traffic still requires hardware testing.
HE and 6 GHz remain deliberately disabled.

The PCI suspend/resume hooks are implemented but have not been tested on
physical MT7902 hardware. Runtime power management, WoWLAN, thermal/SAR
control, advanced statistics, autonomous firmware recovery, and complete
device reset sequencing still need work.

The Bluetooth WMT patch transfer is implemented, but firmware activation,
subsequent generic HCI setup, suspend/resume, and the Netgraph data path have
not been validated on MT7902 hardware.

A successful module build is necessary but not sufficient. Validate scan
results, authentication/association, DHCP and sustained traffic, protected
traffic, monitor capture, recovery after suspend/resume, disconnect/reconnect,
module unload, and Bluetooth separately on hardware. Preserve the working
RTW88 interface while testing MT7902 as a separate VAP.

## Continuous integration

The FreeBSD 15.1 workflow compile-checks the in-tree Wi-Fi Makefile, the
standalone `sys/modules/mt7902_native` entry point, and the optional
`ng_ubt_mt7902` Bluetooth module. These are compile checks only: CI does not
load the modules or validate RF scanning, association, suspend/resume, or
Bluetooth operation on physical hardware.

## Explicit Linux parity differences still open

The following are known port deltas, not claims of upstream equivalence:

| Area | Linux MT7902 reference at `87b67aa8` | Current FreeBSD implementation |
|---|---|---|
| Data TX ring | One 2048-descriptor band-0 data ring; logical AC queues share it | Four physical AC rings, 256 descriptors each |
| TX token table | 8192 tokens | 1024 pending slots (4 × 256) |
| RX buffer size | 2048 bytes | 4096 bytes |
| RX data ring | 1536 descriptors | 1536 descriptors |
| RX event ring | MT7902-specific 512 descriptors | 512 descriptors |
| MCU-WM queue | Hardware queue 15; no MCU-WA RX ring | Queue 15; no MCU-WA RX ring |
| Firmware-download queue | Hardware queue 16 | Queue 16 |

The TX ring/token layout and RX buffer size should be reconciled against the
pinned Linux implementation before calling the DMA port upstream-equivalent.
The current four-ring layout is retained as a FreeBSD implementation choice
until the descriptor/queue mapping is changed and validated on the device.

Other outstanding feature differences include the first-stage RX decoder
(no translated-frame reverse path, A-MSDU deaggregation or AMPDU reorder), no
complete firmware Block Ack / aggregation integration, no full mt76 rate-
control feedback, incomplete SAR/regulatory handling, and no validated runtime
power management. HT/VHT capabilities are configured from firmware and peer
negotiation data, but that is not yet proof of full Wi-Fi 5 throughput or
aggregation support.
