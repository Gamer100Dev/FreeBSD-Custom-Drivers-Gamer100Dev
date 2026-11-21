# Native RTW88 implementation status

## Current state

**Stage 1 source added; not yet build- or hardware-verified.**

A separate native USB kernel module has been introduced at
`sys/dev/rtw88_native/` with its module Makefile at
`sys/modules/rtw88_native/Makefile`.

It targets only Realtek USB ID `0bda:b812` with vendor-specific interface
class/subclass/protocol `ff/ff/ff`. On attach it performs one read-only
vendor control request for `REG_SYS_CFG1` (0x00f0). It does not modify
register state.

## Why this is staged

FreeBSD's USB API documents synchronous control requests and asynchronous
transfer setup/lifecycle separately. We first need to validate the native
control-pipe path before introducing bulk transfer callbacks or firmware
download. The existing LinuxKPI-based driver remains untouched as a
comparison/reference and fallback.

## Not implemented

- USB endpoint discovery and bulk/interrupt transfers
- Firmware download and checksum/startup sequencing
- RTL8822B MAC/BB/RF configuration and calibration
- TX/RX descriptors, aggregation, queue mapping and error recovery
- Native net80211 glue and wlan interface registration
- Suspend/resume and full detach/teardown for active transfers

## Required validation

- Build the module against the user's FreeBSD 15.1 source/build tree.
- Load only this experimental module for the probe test; avoid simultaneously
  binding another driver to the same USB interface.
- Confirm dmesg reports attach and a successful SYS_CFG1 read.
- Record the exact register value and any `usbd_errstr()` failure.
- Continue only after reviewing build and device logs.

No claim of functional wireless support is made at this stage.
