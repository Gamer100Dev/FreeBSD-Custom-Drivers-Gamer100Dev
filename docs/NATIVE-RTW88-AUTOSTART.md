# Native RTW88 boot and USB hotplug

This integration is for the native FreeBSD driver in `sys/dev/rtw88_native/`
and the RTL8822BU USB adapter matched by USB ID `0bda:b812`.

## What it does

- Loads `if_rtw88_native.ko` from an rc service after `FILESYSTEMS`, so
  firmware loading is not attempted during early USB attach.
- Starts every already-attached adapter through its
  `dev.rtw88_native.N.hw_start` sysctl before the `netif` service runs.
- Uses `devd` to start hardware when an `rtw88_nativeN` device appears later.
- On hotplug, asks the normal FreeBSD `netif` scripts to create/configure
  `wlan0` from the wireless settings in `/etc/rc.conf`.

The kernel USB driver already binds on insertion through
`USB_PNP_HOST_INFO`; the rc/devd integration handles the deferred firmware
and radio initialization that intentionally does not run inside USB attach.

## Install on FreeBSD

Run from a clean checkout on the FreeBSD machine:

```sh
tools/build-native-module.sh
tools/install-native-autostart.sh
```

The installer copies the resulting module into `/boot/modules`, installs the
rc and devd files, enables `rtw88_native_enable`, sets the VAP parent
`wlans_rtw88_native0="wlan0"`, and preserves an existing
`ifconfig_wlan0` setting. It restarts devd and starts the service immediately.

The default wireless interface configuration is:

```sh
wlans_rtw88_native0="wlan0"
ifconfig_wlan0="WPA DHCP"
```

Keep the matching network credentials in `/etc/wpa_supplicant.conf`. The
parent interface name is `rtw88_native0`; an old or mistyped setting such as
`wlans_rtw881` does not create a VAP on this driver.

Reboot once after installing a new `.ko`, so the kernel loads the newly
installed binary. After that, the service starts the radio automatically
when the adapter is present at boot, and devd starts it when the adapter is
plugged in later.

## Controls and diagnostics

```sh
service rtw88_native status
service rtw88_native start
service rtw88_native stop
sysctl dev.rtw88_native.0.hw_start
ifconfig wlan0
```

The manual `hw_start=1` toggle should no longer be needed when this service
and devd rule are installed.

## Performance changes and remaining work

Ordinary data frames now leave the RTW88 descriptor's explicit-rate bit clear
and allow firmware rate adaptation, instead of forcing every 2.4-GHz data
frame to 1 Mbps with fallback disabled. Management and EAPOL frames retain
robust fixed legacy rates.

This is a first performance correction, not a complete Linux RTW88 port.
USB TX aggregation, peer-negotiated HT/VHT rate masks, AMPDU/aggregation,
hardware crypto offload, and full TX-rate/status feedback are still missing.
Those need separate implementation and on-device validation; do not assume
this change alone guarantees a particular throughput.
