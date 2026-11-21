# Hardware and test environment

- OS: FreeBSD 15.1-RELEASE-p2
- Architecture: amd64
- Host CPU: 4 logical CPUs
- RAM: 8 GB
- Wireless adapter: Techkey AC1200 USB Wi-Fi
- USB VID:PID: `0bda:b812`
- Chip family: RTL88x2BU / RTL8822BU
- USB link: SuperSpeed (5.0 Gbit/s)
- Firmware file: `/boot/firmware/rtw88/rtw8822b_fw.bin`
- Firmware package previously installed: `wifi-firmware-rtw88-kmod-rtw8822b-20260410`
- Ethernet interface: `em0`
- WLAN interface observed: `wlan0`, parent `rtw881`

USB bulk endpoints observed in the adapter descriptor:

| Endpoint | Direction | Type | Max packet |
|---|---|---|---:|
| 0x84 | IN | Bulk | 1024 |
| 0x05 | OUT | Bulk | 1024 |
| 0x06 | OUT | Bulk | 1024 |
| 0x87 | IN | Interrupt | 64 |
| 0x08 | OUT | Bulk | 1024 |

These details are a test snapshot, not a universal description of every RTL88x2BU device.
