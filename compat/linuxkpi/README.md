# LinuxKPI USB compatibility

The working FreeBSD source files are at the normal source-tree paths:

- `sys/compat/linuxkpi/common/src/linux_usb.c`
- `sys/compat/linuxkpi/common/include/linux/usb.h`

The matching upstream stable/15 snapshots are kept under `vendor/freebsd-stable-15/`.

The imported files are upstream base code. Your previous experimental LinuxKPI changes have not yet been applied to this clean baseline.
