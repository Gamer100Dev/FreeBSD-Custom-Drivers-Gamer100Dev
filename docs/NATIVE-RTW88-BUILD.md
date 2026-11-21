# Native RTL8822BU module build helper

This helper builds the current experimental driver against a FreeBSD source
tree. It overlays the driver source into the tree (without changing other
files), then invokes the in-tree kernel-module build system.

On the target FreeBSD system, ensure the source tree matches the running
kernel release as closely as possible:

```sh
sudo SRCROOT=/usr/src sh tools/build-native-module.sh
```

The output module is at `$SRCROOT/sys/modules/rtw88_native/if_rtw88_native.ko`.
This is a compile-only helper; it does not install or load the module, and
it does not imply hardware support or successful attach.
