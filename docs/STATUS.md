# Development status

## Current known state

The modified kernel has previously built successfully with:

```sh
cd /usr/src-rtw88-usb-test || exit 1
env MAKEOBJDIRPREFIX=/usr/obj/rtw88-usb-test \
    make -j2 buildkernel KERNCONF=GENERIC RTW88_USB=1
```

The experimental kernel was installed under `/boot/kernel.rtw88-usb-sync` and booted successfully.

The adapter enumerates as a SuperSpeed USB device. The driver loads `rtw88/rtw8822b_fw.bin` and reports firmware version 30.20.0, H2C version 14. Initialization still fails intermittently/consistently at USB transfers with `-32` (`EPIPE`), including a control-register security write at register `0x4e0` and a separate reserved-page bulk write.

## Existing local experiment

A reserved-page synchronous bulk-write experiment has been applied locally to:

- `sys/contrib/dev/rtw88/usb.c`
- `sys/compat/linuxkpi/common/src/linuxkpi_usb.c`

The local patch and full source files have not yet been imported into this repository. Do not assume this experiment is validated or present here until those exact files/patches are committed.

## Next investigation

1. Record `compat.linuxkpi.skb.mem_limit` from both `sysctl` and `kenv`.
2. Compare FreeBSD LinuxKPI control-message and URB handling with upstream Linux semantics.
3. Trace the `-EPIPE` control transfer at endpoint zero separately from the reserved-page bulk transfer.
4. Verify endpoint mapping against the adapter's three bulk OUT endpoints (0x05, 0x06, 0x08).
5. Rebuild in the foreground, boot the alternate kernel and record complete dmesg output.

## Test record format

For each test, record date, source commit, kernel config/options, exact command, exit status, adapter enumeration, relevant dmesg and wlan interface state.
