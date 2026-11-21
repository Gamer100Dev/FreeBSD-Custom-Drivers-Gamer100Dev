# Upstream sources and provenance

This file records source projects to compare against and import from. Keep exact commit IDs/tags for every imported snapshot.

| Component | Canonical upstream | Purpose |
|---|---|---|
| FreeBSD | https://github.com/freebsd/freebsd-src | Base kernel, USB stack and LinuxKPI compatibility layer |
| Linux RTW88 | https://github.com/torvalds/linux/tree/master/drivers/net/wireless/realtek/rtw88 | Reference USB transport, chip initialization and firmware flow |
| Linux USB API docs | https://docs.kernel.org/driver-api/usb/usb.html | URB and USB transfer semantics |
| FreeBSD RTW88 manual | https://man.freebsd.org/cgi/man.cgi?query=rtw88&sektion=4 | FreeBSD driver status, firmware and known limitations |

## Import rules

- Prefer source from the canonical upstream repositories.
- Record repository URL, commit SHA/tag, import date and license for each vendored directory.
- Preserve copyright and license headers.
- Do not copy an entire FreeBSD source tree into this repository. Track the exact FreeBSD base revision and carry focused patches instead.
- Do not commit proprietary or unlicensed firmware blobs. Record the package name, version, source and installation path.
- The Linux RTW88 driver is a reference implementation; it is not itself a drop-in FreeBSD driver.
