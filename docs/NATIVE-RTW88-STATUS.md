# Native RTW88 implementation status

Development branch: `work/native-rtw88-net80211`

Target: FreeBSD 15.1-RELEASE-p2 amd64, Techkey AC1200 / RTL8822BU,
USB ID `0bda:b812`.

## Implemented in source

### USB and register layer

- Newbus host-driver binding to the vendor-specific Realtek interface.
- Explicit endpoint-number matching for the observed RTL8822BU interface:
  bulk-IN 0x84, bulk-OUT 0x05/0x06/0x08, and interrupt-IN 0x87.
- USB RXDMA burst-mode setup selected from the negotiated USB speed, plus
  RTL8822B v1 RX aggregation threshold/timeout programming.
- Read-only RX transfer submit/error counters so an idle bulk-IN endpoint can
  be distinguished from a callback that repeatedly fails.
- Manual RTL8822B 2.4-GHz/20-MHz channel programming for eFuse RFE option 3,
  exposed as `dev.rtw88_native.N.channel` for channels 1-13.
- Native net80211 radio registration after successful hardware start, with
  station/monitor VAP creation and 2.4-GHz channel advertisement.
- Active and passive software scan callbacks: active scans use the
  FreeBSD net80211 probe-request generator and route those frames through
  the driver's management-frame transmit callback.
- Native `ic_raw_xmit` and `ic_transmit` hooks for net80211 management and
  data frames. Frames are queued to a sleepable task and sent using the
  RTL8822B 48-byte TX descriptor and the USB queue-to-endpoint map.
- The raw transmit callback accepts the normal stack-generated BPF fields
  used by probe requests and station management frames (priority, legacy
  management rate, retry count, power and group-frame NOACK). Unsupported
  radiotap/BPF features and hardware-crypto requests remain rejected.
- Management/control frames use QSEL 18 on the high-priority OUT endpoint
  (0x05), with a conservative 1-Mbps rate. Data frames currently use TID0/BE
  on the low-priority OUT endpoint (0x08), also at a conservative 1-Mbps rate.
- USB transmit completion is returned to net80211 through
  `ieee80211_tx_complete()`; separate queue, completion and error counters
  are exposed. Completion currently means USB bulk-OUT completion, not a
  confirmed over-the-air MAC acknowledgement.
- WEP/TKIP/CCMP software crypto is registered with net80211. RX descriptors
  that report hardware decryption are marked for net80211's hardware-decrypted
  receive path; SWDEC frames are left for software decryption.
- Bounds-checked RX descriptor handling now copies normal 802.11 MPDUs into
  mbufs, removes the configured appended FCS, adds channel/RSSI/TSF metadata
  and queues them to deferred net80211 input outside the USB transfer mutex.
- Read-only counters for frames queued to net80211 and frames dropped before
  delivery.
- Descriptor-indexed allocation of one bulk-IN, three bulk-OUT and one
  interrupt-IN transfer.
- Completion-signalled synchronous bulk-OUT using FreeBSD USB callbacks,
  external transfer buffers, mutexes and condition variables.
- Synchronous vendor-control requests with actual-length checking and
  I/O-user accounting for detach.
- Continuous bulk-IN receive and interrupt-IN report capture. Both external
  buffers are per-device state, and both transfers have explicit stop/drain
  routines for safe teardown.
- Bounds-checked walking of the RTL8822B 24-byte RX descriptors, driver-info
  and shift lengths, 8-byte aggregate alignment, and C2H record identification.
- Read-only counters for aggregates, descriptors, data frames, C2H records
  and malformed/truncated RX records; last C2H ID/sequence/length are exposed
  through device sysctls.
- A write-triggered `rx_diag` sysctl reads back live CR, TXDMA/RXDMA,
  receive-filter, baseband/RF-control, RX-gain and both RF channel registers.
  It serializes the register snapshot against the scanner's channel callback
  and logs both the receive-transfer counters and each register value.
- Checked little-endian 8/16/32-bit register access.
- RTL8822B USB security-section side operation at `0x04e0` for the
  register ranges used by upstream RTW88.

### Firmware path

- Acquire `rtw88/rtw8822b_fw.bin` with FreeBSD `firmware_get()`.
- Validate the 64-byte RTW88 firmware header and segment/trailer sizes.
- Build the 48-byte firmware TX descriptor and its first-32-byte checksum.
- Send firmware data through the BEACON/high-priority USB bulk-OUT path.
- Run RTL8822B DDMA chunk submission, checksum status handling and
  DMEM/IMEM segment-completion bits.
- Release FWDL enable, start the WLAN MCU and poll the documented READY bit
  mask.

The boot path handles DMEM, IMEM and optional EMEM segments using the
upstream RTW88 DDMA/checksum sequence. An EMEM-bearing image still needs
hardware validation. The
upstream LTE-coexistence save/restore operation is also not yet ported.

### Power, eFuse and base MAC configuration

- Port the USB-applicable commands from the 8822B card-dispose/card-emulation/
  active power transition tables.
- Port the pre-system and post-power system-configuration register steps.
- Read the 1-KiB physical eFuse map through the native register path.
- Decode the packed one-/two-byte eFuse headers into the 768-byte logical
  map, respecting the 96-byte protected tail.
- Extract the RTL8822BU MAC address, channel plan, country code, crystal cap,
  thermal meter, PA/LNA, board and RFE fields.
- Route VO/VI to the NORMAL DMA map, BE/BK to LOW and MG/HI to HIGH for the
  three bulk-OUT endpoint layout.
- Configure the 8822B TX FIFO page table, reserved pages, beacon boundary,
  RX buffer boundary, USB auto-LLT and TX aggregation descriptor count.
- Initialise the H2C ring pointers and check that the ring is empty.
- Port the 8822B base EDCA, timing, beacon, RX-filter and WMAC register
  configuration.
- Start the native bulk-IN and interrupt-IN transfers only after firmware
  and base MAC initialisation. Completed data is retained as raw USB batches
  for the future descriptor/C2H parsers.
- Add an explicit `rtw88_native_hw_start()` / `rtw88_native_hw_stop()`
  sequence, exposed by a serialized `dev.rtw88_native.N.hw_start` sysctl.
- Start the native interrupt-IN transfer after firmware/base MAC setup,
  capture raw reports in a bounded buffer, and expose count/last-length
  diagnostics through read-only sysctls.

## Not yet implemented

- Full RFE board selection, HW-feature C2H report, IQK/DPK, thermal tracking,
  antenna diversity and TX-power calibration; 5-GHz and non-20-MHz channel
  programming are not implemented.
- Full RX PHY-status/rate/chain interpretation, C2H command-specific event
  decoding/dispatch, AMPDU reorder and full monitor/radiotap metadata.
- Negotiated rate control, QoS/TID-specific queue mapping, TX aggregation,
  firmware TX-ACK report dispatch and verified over-the-air TX status.
- Hardware key-cache programming/offload, WPA3/GCMP, regulatory-domain
  integration and power-management operations. The current station path uses
  net80211 software WEP/TKIP/CCMP crypto and is not yet security-validated.
- Verified RX packet delivery and verified scan results on the physical
  adapter; the current RX DMA request had not yet completed during the user's
  latest test.
- Suspend/resume, watchdog, recovery and full load/unload/device-unplug tests.

## Validation

User-reported local FreeBSD testing on the RTL8822BU adapter confirms:
- `kldload ./build/if_rtw88_native.ko` returned without an error.
- The USB device bound as `rtw88_native0`; the SYS_CFG1 vendor-control read
  succeeded.
- `hw_start=1` reported completion of power, firmware, eFuse, FIFO, MAC
  and PHY table setup, including two RF paths.
- The interrupt-IN endpoint completed repeatedly (769 reports observed,
  last report length 60 bytes).
- On the RXDMA revision, the user reported `rx_started=1`,
  `rx_submit_count=1`, `rx_count=0`, `rx_error_count=0` and
  `rx_error_code=0`, with 46 interrupt-IN reports at the time of sampling.
  This means one bulk-IN request was submitted and remained outstanding
  without a reported USB error; it did not complete with a payload.
- The same run logged a firmware module lookup warning (`error 8`) followed
  by successful validation of the 161240-byte binary image (DMEM 11216,
  IMEM 149960, EMEM 0). FreeBSD's firmware loader tries the binary-file
  fallback after a linker module lookup fails; the subsequent validation
  line confirms the driver received and parsed the image bytes.


The endpoint descriptor dump showed a USB 2.0 High-Speed device with bulk-IN
0x84, bulk-OUT 0x05/0x06/0x08 and interrupt-IN 0x87. The reported RXDMA mode
was `0x1e` and aggregation register value `0x2005`; the user has not yet
reported a bulk-IN completion after this configuration.

The current branch registers a net80211 radio after successful hardware
startup. Create a station VAP, bring it up, scan, and inspect results with:

```sh
ifconfig wlan create wlandev rtw88_native0 wlanmode sta
ifconfig wlan0 up
ifconfig wlan0 scan
sleep 15
ifconfig wlan0 list scan
```

The net80211 software scanner now retains the requested active/passive flag.
Active scans generate probe requests via `ic_raw_xmit`; passive scans only
listen. The channel callback programs channels 1-13 as the scanner visits them.

The default net80211 station state machine also sends authentication and
association management frames through `ic_raw_xmit`. Ordinary 802.11 data
frames use `ic_transmit`. Both callbacks enqueue mbufs with their node
references to a sleepable TX task; the task builds an RTL8822B descriptor and
uses the correct management or data USB endpoint. FreeBSD's ordinary probe
and station-management helpers populate BPF parameters even for kernel-
generated frames; the old callback mistakenly rejected these non-zero
defaults. The current source accepts the standard priority/rate/retry/power
fields and group-frame NOACK flag, but continues to reject crypto offload and
other unsupported overrides. The driver registers software WEP/TKIP/CCMP
crypto, but key handling and WPA exchange have not been validated on hardware.

TX counters distinguish raw net80211 callback entries/rejections, queued
frames, successful USB bulk-OUT completions and errors. The new
`tx_raw_count` and `tx_raw_reject_count` counters make it possible to tell
whether a scan probe reached the driver and whether its parameters were
accepted. A successful USB completion is not a radio ACK. The initial rate
is fixed at 1 Mbps; negotiated rate control, QoS queue selection and
TX-status C2H reports are not implemented.

The RX callback copies valid non-C2H frames to mbufs, removes the configured
appended FCS, adds basic channel/RSSI/TSF metadata and defers
`ieee80211_input_mimo_all()` to a taskqueue thread outside the USB transfer
mutex. The user's latest runtime test still showed no completed bulk-IN
transfer, so an active scan may send probes and channel-hop but will not
populate results until RX aggregates are actually delivered.

The user's most recent local test built and loaded the preceding source
revision successfully. Hardware startup completed and the net80211 scanner
visited channels 1-13, but `ifconfig wlan0 list scan` returned no results.
At that point `tx_queued_count`, `tx_complete_count`, `tx_error_count`,
`rx_count`, `rx_desc_count`, `rx_data_count` and `rx_input_count` were
all zero. This was consistent with ordinary probe-request parameters being
rejected before queueing; the current source adds support for those defaults
and exposes raw-callback attempt/reject counters. The corrected revision still
needs a fresh local compile and a repeat hardware test. The current branch also
adds `sysctl dev.rtw88_native.0.rx_diag=1` to read back the live RX/MAC/RF
register state; this is the next diagnostic step because the RX bulk-IN request
remains outstanding without a completion.

Active probe TX and the net80211 authentication/association management-frame
submission path are implemented in source, but neither over-the-air probe
delivery nor a successful AP connection/WPA exchange has been demonstrated.
The GitHub Actions workflow builds inside a FreeBSD 15.1 amd64 VM against the
matching `releng/15.1` source. Do not consider the driver functional until
the corrected revision builds and both TX and RX are verified on the adapter.

## FreeBSD API basis

- FreeBSD 15.1 `USBDI(9)` for transfer allocation/lifetime, synchronous
  control requests, callbacks, drain semantics and buffer handling.
- FreeBSD 15.1 `FIRMWARE(9)` for filesystem-backed firmware acquisition.
- In-tree native USB wireless patterns under `sys/dev/rtwn/usb/`.
- Vendored upstream RTW88 8822B source under `sys/contrib/dev/rtw88/`
  for chip-specific register values and hardware sequencing.
