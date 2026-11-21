# LinuxKPI compatibility changes

Keep only the LinuxKPI files that are modified or needed to understand the driver integration. Record their original FreeBSD paths and base revision.

Current area of interest:
- `sys/compat/linuxkpi/common/src/linuxkpi_usb.c`

The local USB unlink guard experiment and any follow-up URB/control-message changes must be represented as small, reviewable patches.
