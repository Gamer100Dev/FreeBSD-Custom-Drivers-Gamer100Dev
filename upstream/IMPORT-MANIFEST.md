# Vendored source manifest

## Native FreeBSD implementation

- Wi-Fi: `sys/dev/mt76/mt7902/`
- Wi-Fi module entry point: `sys/modules/mt7902_native/`
- Optional Bluetooth USB subclass: `sys/dev/mt76/mt7902/ng_ubt_mt7902.c`
- The MT7902 Wi-Fi driver uses native FreeBSD PCI, busdma, firmware, taskqueue,
  mutex/condition-variable, and net80211 APIs. It does not depend on LinuxKPI
  or mac80211 at runtime.
- Linux source is a hardware-protocol and behavioural reference only.

## FreeBSD upstream baseline

- Upstream: https://github.com/freebsd/freebsd-src
- Branch: `stable/15`
- Snapshot commit: `55b89b7b529f1c37a749adadb11aba4f8e9e51ca`

## Linux MT76 / MT7902 comparison baseline

Canonical upstream: https://github.com/torvalds/linux
Driver directory: `drivers/net/wireless/mediatek/mt76/`

The audit baseline is pinned to Linux commit
`87b67aa8eb32cf89dab4c6df88d65e07fec46592` (2026-10-07), the latest
MT7921 PCI-path commit found during this update. Compare shared MT792x DMA
and Connac2 code at the same tree revision before importing behaviour.

Relevant files include `mt7921/pci.c`, `mt792x_dma.c`, `mt7921/mac.c`,
`mt792x_mac.c`, `mt7921/mcu.c`, `mt76_connac_mcu.c`, and
`mt76_connac_mac.c`.

The old `vendor/mt76/` snapshot at
`41d5d1dd63111870ef74a5b9645416d98979a145` is historical reference only.
It predates MT7902 upstream enablement and must not be the sole authority.

## Other vendored references

- RTW88: Linux upstream commit
  `c44104f3642da720164042623e4832836aefdd8e`, focused reference subset at
  `vendor/linux-master/drivers/net/wireless/realtek/rtw88/`.
- Firmware payloads are separate from driver source and retain their own
  license and checksum records under `firmware/mediatek/`.

## Validation status

A successful FreeBSD 15.1 build is necessary but not sufficient. It does not
prove over-the-air scan, association, throughput, suspend/resume, or Bluetooth
operation. The RX path remains a documented first-stage implementation and
must not be described as full mt76 feature parity.
