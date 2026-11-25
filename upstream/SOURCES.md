# Upstream sources and provenance

Keep exact commit IDs/tags for every imported snapshot and audit baseline.

| Component | Canonical upstream | Purpose |
|---|---|---|
| FreeBSD | https://github.com/freebsd/freebsd-src | Native kernel APIs, busdma, PCI, taskqueue and net80211 |
| Linux MT76 | https://github.com/torvalds/linux/tree/master/drivers/net/wireless/mediatek/mt76 | MT7902 hardware protocol and behavioural reference |
| Linux RTW88 | https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtw88 | Separate RTW88 comparison reference |
| FreeBSD net80211 | https://github.com/freebsd/freebsd-src/tree/stable/15/sys/net80211 | Native wireless stack contract |

## MT7902 Linux reference

Audit baseline: Linux commit `87b67aa8eb32cf89dab4c6df88d65e07fec46592`
(2026-10-07; latest MT7921 PCI-path commit found during this update).

Relevant files at that revision:

- `drivers/net/wireless/mediatek/mt76/mt7921/pci.c`
- `drivers/net/wireless/mediatek/mt76/mt792x_dma.c`
- `drivers/net/wireless/mediatek/mt76/mt7921/mac.c`
- `drivers/net/wireless/mediatek/mt76/mt792x_mac.c`
- `drivers/net/wireless/mediatek/mt76/mt7921/mcu.c`
- `drivers/net/wireless/mediatek/mt76/mt76_connac_mcu.c`
- `drivers/net/wireless/mediatek/mt76/mt76_connac_mac.c`

MT7902 has chip-specific queue and RX-ring exceptions. Do not copy generic
MT7921 defaults over those exceptions. Recheck upstream commit history before
every substantive port update.

## Porting rules

- Linux is the hardware protocol reference, not a runtime dependency.
- Translate Linux PCI/DMA/interrupt/workqueue/firmware/netdevice/mac80211
  mechanics into native FreeBSD PCI, busdma, taskqueue, firmware, mutex/CV,
  and net80211 mechanisms.
- Preserve wire formats, register programming order, queue IDs, descriptor
  ownership, firmware command semantics, and recovery behaviour unless a
  documented FreeBSD or hardware-specific reason requires a deviation.
- Record deliberate deviations in the MT7902 README with the Linux
  function/register/field and the FreeBSD replacement.
- Keep unsupported features explicitly disabled rather than advertising
  capabilities without a working data path.
- Preserve copyright and license notices when porting code.
- Do not import LinuxKPI/mac80211 runtime dependencies into the native driver.
- Do not commit generated kernel build products or proprietary/unlicensed
  firmware blobs.
- A build passing is not equivalent to hardware validation.
