# FreeBSD Custom Drivers — Gamer100Dev

Native FreeBSD driver development, currently focused on the Realtek
RTL8822BU USB Wi-Fi adapter (USB ID `0bda:b812`).

## Native RTW88 driver

The default `main` branch contains the native implementation:

- Kernel driver: `sys/dev/rtw88_native/`
- Module build: `sys/modules/rtw88_native/`
- Linux RTW88 comparison source: `vendor/linux-master/`
- Current implementation notes: `docs/NATIVE-RTW88-STATUS.md`
- Boot and USB hotplug setup: `docs/NATIVE-RTW88-AUTOSTART.md`

The driver binds to the RTL8822BU through the FreeBSD USB PNP table and
provides a native USB transport, firmware download, RTL8822B MAC/PHY
initialization, RX parsing, TX descriptors, and net80211 station/monitor
interfaces. Hardware initialization is deferred out of the kernel USB attach
callback because firmware access requires the root filesystem to be ready.

## Build and install

On the FreeBSD machine, with a matching FreeBSD source tree:

```sh
tools/build-native-module.sh
tools/install-native-autostart.sh
```

The installer places `if_rtw88_native.ko` in `/boot/modules`, installs the
rc service and devd hotplug rule, and preserves `ifconfig_wlan0="WPA DHCP"`.
The service creates `wlan0` explicitly on the native `rtw88_nativeN` parent,
then applies the normal WPA/DHCP configuration. Keep network credentials in
`/etc/wpa_supplicant.conf`. Reboot after installing a replacement module.

The rc service loads the module after filesystems are mounted and starts
attached adapters before `netif`; devd handles USB insertion after boot. Once
installed, a manual `sysctl dev.rtw88_native.0.hw_start=1` should not be
required.

## Performance and scope

Ordinary data frames now use the firmware rate-adaptation path instead of
being forced to a fixed 1-Mbps rate. Management and EAPOL frames keep their
robust fixed rates.

This is not yet feature-equivalent to Linux RTW88. USB TX aggregation,
peer-negotiated HT/VHT rate masks, AMPDU, hardware crypto offload, and complete
TX rate/status feedback remain follow-up work. Build and runtime validation
must be performed on the target FreeBSD kernel and adapter; source changes
alone are not evidence of a successful build or measured performance gain.

## Upstream references

- FreeBSD RTW88 snapshot: `sys/contrib/dev/rtw88/`
- FreeBSD LinuxKPI source: `sys/compat/linuxkpi/common/`
- Linux RTW88 comparison source: `vendor/linux-master/`
- Imported source revisions: `upstream/IMPORT-MANIFEST.md`
