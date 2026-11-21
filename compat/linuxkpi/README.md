# LinuxKPI USB compatibility sources

The actual FreeBSD stable/15 LinuxKPI USB compatibility implementation is vendored at:

- `vendor/freebsd-stable-15/sys/compat/linuxkpi/common/src/linux_usb.c`
- `vendor/freebsd-stable-15/sys/compat/linuxkpi/common/include/linux/usb.h`

The FreeBSD upstream filename is `linux_usb.c`. Local experimental changes must be added as focused patches against this exact base revision, not assumed to be present in the imported upstream snapshot.

See `upstream/IMPORT-MANIFEST.md`.
