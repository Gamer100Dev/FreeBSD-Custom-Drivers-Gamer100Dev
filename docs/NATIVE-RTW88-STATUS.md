# Native RTW88 implementation status

Development branch: `main`

Target: FreeBSD 15.1-RELEASE-p2 amd64, Techkey AC1200 / RTL8822BU,
USB ID `0bda:b812`.

## Implemented in source

### USB and register layer

- Newbus host-driver binding to the vendor-specific Realtek interface.
- Explicit endpoint-number matching for the observed RTL8822BU interface:
  bulk-IN 0x84, bulk-OUT 0x05/0x06/0x08, and interrupt-IN 0x87.
- USB RXDMA burst-mode setup selected from the negotiated USB speed, plus
  RTL8822B v1 RX aggregation threshold/timeout programming.
- Four concurrent 32-KiB bulk-IN transfers (four independent DMA buffers),
  matching Linux RTW88's `RTW_USB_RXCB_NUM=4` receive-URB queue. Each transfer
  is resubmitted after completion; aggregate submit/completion/error counters
  distinguish an idle endpoint from a callback that repeatedly fails.
- RTL8822B firmware hardware-feature report request/decoder. It extracts
  board antenna count and NSS from the 13-byte C2H report before MAC/PHY
  initialisation, selecting RF-A/1T1R when firmware reports one antenna.
  If the report cannot be obtained, the driver conservatively initialises
  only RF-A rather than assuming both silicon RF paths are populated.
- Manual RTL8822B 2.4/5-GHz, 20-MHz channel programming for eFuse RFE option 3.
  The 5-GHz path uses separate baseband, RF synthesiser, antenna-routing and
  1R/2R CCA profiles; RF18 writes are verified and RF-B may fall back to
  RF-A/1T1R. Native net80211 advertises 2.4-GHz 11b/g and 5-GHz 11a/20-MHz
  channels. The 5-GHz channel set is passive-only pending regulatory, DFS and
  calibrated TX-power support.
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
- Management/control frames use QSEL 18 on the management/high-priority OUT
  endpoint (0x05), with 1-Mbps CCK on 2.4 GHz and 6-Mbps OFDM on 5 GHz. Group-addressed data uses
  QSEL_HIGH (17) on the high-priority endpoint. Unicast BE/BK map to their
  corresponding TIDs (0/1); unicast VI/VO map to QSEL_BEACON (16), matching
  Linux RTW88's three-pipe USB queue policy. Non-QoS unicast PAE/EAPOL frames
  are explicitly routed to BE/TID0: FreeBSD marks them M_EAPOL and promotes
  their WME category to VO, but the 802.11 frame itself is non-QoS. EAPOL
  retains the conservative 1-Mbps B rate profile.
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
- Descriptor-indexed allocation of four bulk-IN, three bulk-OUT and one
  interrupt-IN transfer.
- Completion-signalled synchronous bulk-OUT using FreeBSD USB callbacks,
  external transfer buffers, mutexes and condition variables.
- Synchronous vendor-control requests with actual-length checking and
  I/O-user accounting for detach.
- Continuous four-request bulk-IN receive and interrupt-IN report capture.
  Each RX transfer has a separate per-device 32-KiB buffer. All four RX
  transfers and the interrupt transfer have explicit stop/drain routines.
- Bounds-checked walking of the RTL8822B 24-byte RX descriptors, driver-info
  and shift lengths, 8-byte aggregate alignment, and C2H record identification.
- Read-only counters for aggregates, descriptors, data frames, C2H records
  and malformed/truncated RX records; last C2H ID/sequence/length are exposed
  through device sysctls.
- A write-triggered `rx_diag` sysctl reads back live CR, TXDMA/RXDMA,
  receive-filter, baseband/RF-control, WLRF1 BB/RF enable, RF-A/RF-B SIPI
  command ports, RX path selection, RX gain and both RF channel registers.
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
- Capture the first two packed 42-byte `rtw_txpwr_idx` records (RF A/B) from
  logical eFuse offset `0x10`.
- Program RFE-option-3 legacy TXAGC values on every channel change: 2.4-GHz
  CCK/OFDM and 5-GHz OFDM use upstream BB-PG rate deltas, eFuse bases and
  worldwide fallback limits. TXAGC words use checked USB register writes,
  matching Linux RTW88's 8822B write-only programming path; readback is not
  treated as authoritative because the tested adapter returns zero for these
  reads. USB completion is not proof of over-the-air transmit power.
- Route VO/VI to the NORMAL DMA map, BE/BK to LOW and MG/HI to HIGH for the
  three bulk-OUT endpoint layout.
- Configure the 8822B TX FIFO page table, reserved pages, beacon boundary,
  RX buffer boundary, USB auto-LLT and TX aggregation descriptor count.
- Initialise the H2C ring pointers and check that the ring is empty.
- Port the 8822B base EDCA, timing, beacon, RX-filter and WMAC register
  configuration.
- Start all four native bulk-IN transfers and the interrupt-IN transfer only
  after firmware and base MAC initialisation. Each completed 32-KiB USB batch
  is parsed and any decoded frames are queued to the net80211 input task.
- Add an explicit `rtw88_native_hw_start()` / `rtw88_native_hw_stop()`
  sequence, exposed by a serialized `dev.rtw88_native.N.hw_start` sysctl.
- Program the WLAN-owned RFE antenna-switch route before optional LTE/BT
  coexistence accesses. A failed optional coexistence operation is reported as
  a warning and no longer prevents net80211 registration or RX startup,
  matching Linux RTW88's best-effort coexistence callback semantics.
- Start the native interrupt-IN transfer after firmware/base MAC setup,
  capture raw reports in a bounded buffer, and expose count/last-length
  diagnostics through read-only sysctls.

## Current RF-path diagnostic response

The latest physical test showed RF-A reaching the requested RF18 value while
RF-B remained at `0x00400`, causing channel changes to return `EIO`. The
review identified a missing upstream initialisation step: RTW88 requests a
13-byte firmware hardware-feature report and uses its antenna-count field to
select the board's actual RF path count. The native driver now requests and
decodes that report before MAC/PHY setup. A reported one-antenna board is
configured as RF-A/1T1R, so RF-B is not loaded or selected for channel tuning.
If the report is missing or cannot be read, startup defaults to RF-A/1T1R.

An additional runtime safeguard remains: if a dual-path configuration is
reported but RF-B still fails RF18 readback, the channel path switches TX/RX
to RF-A only, applies the 1R CCA and TRSW settings, verifies RF-A again, and
continues channel tuning. The fallback logs the B-path SIPI command, RF control
and WLRF1 state. The same diagnostics exposed a more fundamental PHY initialisation error:
`rtw88_native_phy_write_mask32()` expects an unshifted field value, but the
WLRF1 enable mask (bits 24-26) and RXPSEL reset-release mask (bits 28-29) had
been passed as already-positioned values. The helper shifted them a second
time, resulting in WLRF1 remaining zero and RX path reset not being released.
The native sequence now passes field values `0x7` and `0x3` respectively and
checks both registers immediately after writing. This mirrors Linux RTW88's
`rtw8822b_phy_set_param()` use of `rtw_write32_set()` for those same enables.

The firmware-report parser, corrected WLRF1/RXPSEL masked writes, four
concurrent RX transfers and expanded RF diagnostics passed FreeBSD 15.1 CI in
[run 44937801781](https://github.com/Gamer100Dev/FreeBSD-Custom-Drivers-Gamer100Dev/actions/runs/44937801781).
A later change moved net80211 registration ahead of posting the RX transfers
and added an explicit build ID to the module; those follow-up revisions are
being checked by subsequent CI runs. Neither the corrected register readbacks
nor successful scan/RX behaviour has yet been confirmed on the physical
adapter.

## Not yet implemented

- Full RFE board selection, IQK/DPK, thermal tracking, antenna diversity and
  per-channel dynamic power tracking remain incomplete. The initial 5-GHz path
  is limited to RFE option 3 at 20 MHz; HT/VHT per-rate TXAGC, SAR and
  regulatory-domain-specific power integration, DFS CAC/radar handling and
  active 5-GHz scanning are not implemented. The legacy power path uses the
  upstream worldwide fallback table only and still requires physical RF
  validation.
  Runtime C2H event decoding remains incomplete; only the startup feature
  report is decoded.
- Per-chain RSSI/SNR/EVM reporting, C2H command-specific event
  decoding/dispatch, AMPDU reorder and full monitor/radiotap metadata.
  Basic RX rate/width decoding and RTL8822B PHY-status page 0/1 handling are
  implemented; the radio still advertises only 2.4-GHz 20-MHz operation.
- Negotiated rate control, TX aggregation, firmware TX-ACK report dispatch
  and verified over-the-air TX status. The current queue mapping selects the
  basic AC/TID and group queue, but does not implement aggregation.
- Hardware key-cache programming/offload, WPA3/GCMP, regulatory-domain
  integration and power-management operations. The current station path uses
  net80211 software WEP/TKIP/CCMP crypto and is not yet security-validated.
- Verified RX packet delivery and verified scan results on the physical
  adapter. The latest physical test predates the four-request bulk-IN queue
  and the WLRF1/RXPSEL masked-write correction.
- Suspend/resume, watchdog, recovery and full load/unload/device-unplug tests.

## Verifying the exact source revision on the target machine

`tools/build-native-module.sh` copies the checked-out repository's native
driver files into the selected FreeBSD source overlay before building. It now
prints the checked-out Git revision and stamps it into the loaded module, which
then logs `native driver build ID` during USB attach. This avoids confusing
a successful build of an older `/usr/src-rtw88-usb-test` overlay with a build of
the current GitHub branch.

After pulling this branch, the expected markers are:
- `native driver build ID: <revision>` matching the local Git checkout.
- `allocated native USB pipes: 4 bulk IN`.
- `BB/RF domain enabled: WLRF1=0x07000000` and no BB/RF enable failure.
- `RX diag: ... xfers=4 submits=4` immediately after RX starts, before any
  receive completions.

If the PHY readback checks fail, startup returns an error rather than
advertising a working radio with its BB/RF or RX path-select domain disabled.

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
  At that historical test point, one bulk-IN request was submitted and
  remained outstanding without a reported USB error; it did not complete with
  a payload. The current branch now posts four concurrent requests.
- The same run logged a firmware module lookup warning (`error 8`) followed
  by successful validation of the 161240-byte binary image (DMEM 11216,
  IMEM 149960, EMEM 0). FreeBSD's firmware loader tries the binary-file
  fallback after a linker module lookup fails; the subsequent validation
  line confirms the driver received and parsed the image bytes.


The endpoint descriptor dump showed a USB 2.0 High-Speed device with bulk-IN
0x84, bulk-OUT 0x05/0x06/0x08 and interrupt-IN 0x87. The reported RXDMA mode
was `0x1e` and aggregation register value `0x2005`; the user had not reported a
bulk-IN completion at that historical test point.

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
frames, successful USB bulk-OUT completions and errors. The `tx_raw_count` and
`tx_raw_reject_count` counters show whether a management frame reached the
callback and was accepted. The `tx_mgmt_count`, `tx_data_count`,
`tx_group_count`, `tx_eapol_count`, `tx_probe_req_count`, `tx_auth_req_count`,
`tx_assoc_req_count` and `tx_deauth_req_count` counters classify frames
entering the worker; `tx_last_qsel`, `tx_last_endpoint`,
`tx_last_frame_control` and `tx_last_frame_len` record the most recent
descriptor decision. RX subtype counters distinguish authentication responses,
association responses, deauthentication/disassociation, probe responses,
beacons and EAPOL frames; `rx_auth_to_self_count` confirms an authentication
frame addressed to this station actually reached the parser. A successful
USB completion is not a radio ACK. The initial rate is fixed at 1 Mbps;
negotiated rate control and TX-status C2H reports are not implemented.

The RX callback copies valid non-C2H frames to mbufs, removes the configured
appended FCS, adds basic channel/RSSI/TSF metadata and defers
`ieee80211_input_mimo_all()` to a taskqueue thread outside the USB transfer
mutex. The user's last runtime test predates the four-transfer RX queue and
WLRF1/RXPSEL corrections; active scanning still requires physical confirmation
that RX aggregates are delivered and parsed.

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
register state, including WLRF1 and both RF SIPI command ports. Re-run this
after rebuilding to verify that the BB/RF enable field and both RX path-select
reset bits now latch, and that one of the four bulk-IN requests completes.

Latest physical RX test: the user reported four active bulk-IN transfers,
171 submissions, 167 completions and zero USB errors. The scan cache contained
three 2.4-GHz networks, including `DIGI-G5HP_RPT` (channel 8, about -81 dBm)
on an earlier scan. Both RF-A and RF-B channel registers read back `0x00000c0a`
while tuned to channel 10. A subsequent wpa_supplicant run stayed in
`ASSOCIATING`, returned to `SCANNING`, and its later scan-results snapshot no
longer contained `DIGI-G5HP_RPT`; no authentication/association rejection or
EAPOL failure was captured because the supplicant was started in syslog mode
without debug output. DHCP therefore correctly failed with no carrier. This
proves USB RX and earlier scan reception, but not successful probe delivery,
association, or WPA handshake.

The TX queue-selection audit found that the FreeBSD implementation sent every
data frame as TID0/BE. Linux RTW88 selects QSEL_HIGH for group-addressed data
and uses the packet priority/TID for unicast data. The native path now mirrors
that classification using FreeBSD `M_WME_GETAC()` and `WME_AC_TO_TID()`, and
exposes frame-classification/QSEL diagnostics. EAPOL-tagged PAE traffic is
also assigned the conservative 1-Mbps B/management rate profile, following
FreeBSD net80211's M_EAPOL transmit guidance. Active probe TX has now produced 68 raw net80211 management-frame submissions
in the latest user test, all 68 completing on USB bulk-OUT with no driver TX
errors. The final observed frame-control was 0x00b0 (authentication request)
and length 30 bytes. This still does not prove an over-the-air acknowledgement.
The next revision adds per-subtype TX/RX counters so an authentication response
addressed to this station can be distinguished from an unanswered TX. The
latest wpa_supplicant log reports authentication timeout to BSSID
`1c:61:b4:98:43:de` (SSID `TP-Link_43DF`, 2412 MHz), rather than the requested
`DIGI-G5HP_RPT`; that network-selection mismatch must be resolved before
judging the intended AP's compatibility. The operation-20 `DELKEY` ioctl
warning is known FreeBSD supplicant key-cleanup noise and does not itself
identify the authentication timeout cause. End-to-end AP connection and WPA
handshake remain unverified.
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


## RX status parity with Linux RTW88

The native RX path now validates the descriptor rate against the RTW88
DESC_RATE_MAX range and enforces the 32-byte PHY driver-info size used by
RTL8822B. It maps legacy, HT and VHT rate codes into FreeBSD net80211 RX
metadata, decodes the RTL8822B CCK (page 0) and OFDM/HT/VHT (page 1) receive
power fields, and uses the page-1 receive subchannel to refine bandwidth.
Frames without valid PHY status no longer claim that RSSI/noise measurements
are present. Hardware-decrypted frames are marked with
IEEE80211_RX_F_DECRYPTED, the net80211 RX-status flag.

This is RX metadata parity only. HT/VHT association capabilities, hardware key-cache offload, firmware C2H event dispatch and AMPDU reordering remain unimplemented. The 5-GHz PHY path is passive 20-MHz only and has not yet been validated on the physical adapter.


## Association-state hardware programming

The STA VAP now wraps `iv_newstate`: before net80211 publishes
`IEEE80211_S_RUN`, the driver programs and verifies port-0 BSSID (0x0618),
AID (0x06a8, mask 0x7ff), and managed-linked network type (CR[17:16], mask
0x30000, value 2), matching the upstream RTL8822B RTW88 port-0 register map. On leaving RUN it clears link mode before
clearing AID/BSSID. A failed link-programming readback prevents the STA VAP
from publishing RUN.

The RX diagnostics add `rx_assoc_resp_to_self_count`,
`rx_last_assoc_status`, `rx_disassoc_to_self_count`,
`rx_disassoc_from_bss_count`, and `rx_last_disassoc_reason`, to distinguish
frames addressed to this station and disassociations sourced by its programmed
BSSID from the older all-BSS subtype totals.


The shutdown interlock distinguishes an actual hardware stop from an ordinary
channel/diagnostic register operation: link teardown waits for ordinary
hardware work to finish, but clears cached link state without waiting on the
stop operation that is itself stopping the VAP.


## Firmware H2C and Wi-Fi-only RF initialization

Hardware start sends the RTL8822B General Info (H2C 0x0d) and PHYDM Info
(H2C 0x11) packets after MAC/PHY setup and before net80211 exposes a station
VAP. These use QSEL 19. RTW88 specifies descriptor offset zero for H2C, so the
TX descriptor validator permits zero only for that queue. Firmware H2C counters
measure USB transfer completion, not firmware acceptance or over-air TX.

For supported RFE option 3 / 2T2R boards, startup configures the WLAN-only
LTE grant and antenna-switch path. Unsupported eFuse/RF combinations are
explicitly skipped; register I/O failures abort startup. PHY bring-up also
initializes EDCCA thresholds, and scan start temporarily applies RTW88's
maximum receive-coverage IGI before restoring the previous gain on scan end.
Physical connection and EAPOL operation still require hardware validation.
