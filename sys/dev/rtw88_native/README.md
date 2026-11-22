# Native FreeBSD RTW88 RTL8822BU driver

Experimental native FreeBSD driver for the Realtek RTL8822BU USB Wi-Fi adapter
(USB ID `0bda:b812`). The native path uses FreeBSD newbus, USBDI, firmware
loading, net80211, and kernel synchronization primitives; it is separate from
LinuxKPI and the Linuxulator. Work is focused on FreeBSD 15.1 amd64.

## Current implementation

- USB probe/attach and descriptor-based transfer allocation: four bulk-IN,
  three bulk-OUT, one interrupt-IN.
- Checked USB register access, RTL8822B power/MAC setup, firmware acquisition
  and validation, DDMA segment download and checksum/READY checks.
- eFuse map decoding, board/MAC extraction and packed per-chain RTL8822B TX-power indices.
- RFE-option-3 legacy CCK/OFDM TXAGC programming for 2.4/5 GHz on every channel change, using upstream BB-PG offsets and worldwide power-limit fallback with register readback.
- PHY/BB/RF table setup, channel tuning and diagnostic register readback.
- Native net80211 station/monitor VAP, 2.4-GHz channels and restricted
  5-GHz channels, scanning callbacks and deferred RX input.
- RX aggregate/descriptor parsing and basic RX metadata; TX queueing and
  descriptor construction for management/data frames.
- Counters/sysctls for RX, TX, association subtype and hardware diagnostics.

This is still experimental. USB transfer completion is not an over-the-air
acknowledgement. End-to-end WPA association, key handling, DHCP/IP, regulatory
compliance, calibration and suspend/resume are not yet fully validated.
Full HT/VHT per-rate TX power programming, thermal/SAR power tracking and
regulatory-domain-specific power integration are not yet implemented. The
legacy TX power path currently uses the RFE-3 worldwide fallback limits and
has not been verified on the physical adapter. Hardware key-cache offload,
negotiated rate control, aggregation, full C2H
dispatch and TX ACK status are not complete. Do not infer functionality from
successful module load alone.

## Build

On the FreeBSD host, use a source tree matching the running kernel. Clone the
repository and build the module using the repository helper:

```sh
git clone https://github.com/Gamer100Dev/FreeBSD-Custom-Drivers-Gamer100Dev.git
cd FreeBSD-Custom-Drivers-Gamer100Dev
sudo SRCROOT=/usr/src sh tools/build-native-module.sh
```

The helper overlays the native module files in the selected FreeBSD source tree
and builds the module. Its output is under
`$SRCROOT/sys/modules/rtw88_native/if_rtw88_native.ko`. It prints the
checkout revision/build ID so the loaded module can be matched to the source.

Ensure the firmware is installed in the FreeBSD firmware search path as
`rtw88/rtw8822b_fw.bin`. The firmware loader's fallback file path should
resolve the file before starting the hardware.

## Load and start

```sh
sudo kldload /usr/src/sys/modules/rtw88_native/if_rtw88_native.ko
dmesg | tail -50
```

Use the unit number reported by attach (examples below use unit 0). Start the
radio using the driver's process-context startup sysctl:

```sh
sudo sysctl dev.rtw88_native.0.hw_start=1
```

The driver must be started before creating the station VAP. Create and bring up
the WLAN interface, then scan:

```sh
sudo ifconfig wlan create wlandev rtw88_native0 wlanmode sta
sudo ifconfig wlan0 up
sudo ifconfig wlan0 scan
sleep 15
ifconfig wlan0 list scan
```

If the adapter attaches under a different device unit or the VAP is assigned a
different interface name, use those actual names. Inspect counters and RF/RX
diagnostics under `dev.rtw88_native.0` when scan or association fails.

## Associate and obtain an IP address

The driver provides the 802.11 interface; FreeBSD's normal networking stack
configures IPv4/IPv6 on that interface. IP addresses are not programmed into
the USB Wi-Fi driver itself.

For WPA/WPA2-Personal, configure a supplicant profile (replace SSID and
password with your own):

```sh
sudo wpa_passphrase 'YOUR_SSID' 'YOUR_PASSWORD' | sudo tee /etc/wpa_supplicant.conf
sudo wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
```

Once the station is associated, obtain an IPv4 lease:

```sh
sudo dhclient wlan0
ifconfig wlan0
```

For a static IPv4 configuration, use the normal FreeBSD interface command,
replacing the address and subnet as appropriate:

```sh
sudo ifconfig wlan0 inet 192.168.1.50/24
```

These networking commands are standard FreeBSD behaviour, not a driver-specific
IP feature. DHCP will not succeed unless the Wi-Fi association and data path
are functioning.

## Troubleshooting and diagnostics

- `dmesg`: attach/start errors, firmware messages and driver build ID.
- `ifconfig wlan0 list scan`: net80211 scan-cache contents.
- `sysctl dev.rtw88_native.0.rx_diag=1`: readback of receive/MAC/RF registers
  and receive-transfer statistics (when hardware is started).
- `sysctl dev.rtw88_native.0`: driver counters; inspect TX rejection/errors,
  receive parsing/input counts, association response status and RFK state.
- Stop hardware before unloading: `sudo sysctl dev.rtw88_native.0.hw_start=0`,
  then `sudo kldunload if_rtw88_native`.

A scan-cache result, USB bulk completion, or RX/TX counter alone does not prove
association or WPA completion. Verify the supplicant's authentication,
association and key-exchange messages, and confirm that DHCP receives a lease.

## Reference implementation and API basis

Chip-specific sequences are based on the vendored RTW88 Linux source for the
RTL8822B. USB/module lifecycle and networking glue are based on FreeBSD
USBDI(9), FIRMWARE(9), net80211 and in-tree native USB wireless driver
patterns (including rtwn). See the repository status notes and build
documentation for the known incomplete features and test history.
