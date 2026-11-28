# Native MT7902 boot setup

The MT7902 PCI driver is installed at `/boot/modules/if_mt7902_native.ko` and
started by `/usr/local/etc/rc.d/mt7902_native` after `FILESYSTEMS` and before
`netif`. The service waits for the PCI device's `hw_start` sysctl, ensures
firmware startup, and creates `wlan1` on `mt7902_nativeN` when that name is
free. If `wlan1` already belongs to another device, the service leaves it
alone.

Install or refresh the module and enable the service with:

```sh
tools/build-native-module.sh
sh tools/install-mt7902-autostart.sh
```

The installer enables `mt7902_native_enable` and adds
`ifconfig_wlan1="WPA DHCP"` only if no `ifconfig_wlan1` setting exists. Keep
the matching network credentials in `/etc/wpa_supplicant.conf`. The service
configures only `wlan1`; it does not restart other interfaces.

Useful controls:

```sh
service mt7902_native status
service mt7902_native start
sysctl dev.mt7902_native.0.hw_start
ifconfig wlan1
```
