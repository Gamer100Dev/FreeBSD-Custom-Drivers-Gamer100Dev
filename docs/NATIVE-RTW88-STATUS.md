# Native RTW88 implementation status

Development branch: `work/native-rtw88-usb`

Target: FreeBSD 15.1-RELEASE-p2 amd64, Techkey AC1200 / RTL8822BU,
USB ID `0bda:b812`.

## Implemented in source (not build-tested)

### USB and register layer

- Newbus host-driver binding to the vendor-specific Realtek interface.
- Descriptor-indexed allocation of one bulk-IN, three bulk-OUT and one
  interrupt-IN transfer.
- Completion-signalled synchronous bulk-OUT using FreeBSD USB callbacks,
  external transfer buffers, mutexes and condition variables.
- Synchronous vendor-control requests with actual-length checking and
  I/O-user accounting for detach.
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

The current boot path supports the regular DMEM+IMEM image. An image with an
optional EMEM segment is rejected with `ENOTSUP` pending validation. The
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
- Add an explicit `rtw88_native_hw_start()` / `rtw88_native_hw_stop()`
  sequence. It is not invoked automatically from USB attach.

## Not yet implemented

- Full RFE board selection, HW-feature C2H report, PHY/BB/RF register tables,
  IQK/DPK, antenna and channel initialisation.
- Active RX and interrupt/C2H state machines.
- Normal TX data path, aggregation, per-frame queue mapping, completion and
  status reporting.
- Native `net80211` interface and lifecycle integration.
- Regulatory, scan/auth/association, key and power-management operations.
- Suspend/resume, watchdog, recovery and full load/unload/device-unplug tests.

## Validation

**The source has not been compiled against FreeBSD 15.1 and has not been
loaded on the physical adapter.** No claim of successful firmware boot,
hardware initialisation, scanning or Wi-Fi connectivity is made.

The next local verification batch should run a foreground module build
against `/usr/src-rtw88-usb-test`, then review any compiler diagnostics
before attempting a device load. Hardware testing remains deferred until
more of the driver is implemented.

## FreeBSD API basis

- FreeBSD 15.1 `USBDI(9)` for transfer allocation/lifetime, synchronous
  control requests, callbacks and buffer handling.
- FreeBSD 15.1 `FIRMWARE(9)` for filesystem-backed firmware acquisition.
- In-tree native USB wireless patterns under `sys/dev/rtwn/usb/`.
- Vendored upstream RTW88 8822B source under `sys/contrib/dev/rtw88/`
  for chip-specific register values and hardware sequencing.
