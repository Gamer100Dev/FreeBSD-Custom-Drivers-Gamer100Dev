# Native RTW88 boot and USB hotplug

This integration is for the native FreeBSD driver in `sys/dev/rtw88_native/`
and the RTL8822BU USB adapter matched by USB ID `0bda:b812`.

## What it does

- Loads `if_rtw88_native.ko` from an rc service after `FILESYSTEMS`, so
  firmware loading is not attempted during early USB attach.
- Starts every already-attached adapter through its
  `dev.rtw88_native.N.hw_start` sysctl before the `netif` service runs.
- Uses `devd` to start hardware when an `rtw88_nativeN` device appears later.
- Creates `wlan0` explicitly on the `rtw88_nativeN` parent, then asks the
  normal FreeBSD `netif` scripts to apply its WPA/DHCP settings.

The kernel USB driver already binds on insertion through
`USB_PNP_HOST_INFO`; the rc/devd integration handles the deferred firmware
and radio initialization that intentionally does not run inside USB attach.

## Install on FreeBSD

Run from a clean checkout on the FreeBSD machine:

```sh
tools/build-native-module.sh
sh tools/install-native-autostart.sh
```

The installer copies the resulting module into `/boot/modules`, installs the
rc and devd files, enables `rtw88_native_enable`, and preserves an existing
`ifconfig_wlan0` setting. It restarts devd and starts the service immediately.
The service creates the VAP explicitly because the stock FreeBSD `wlans_*`
rc.conf parser does not recognize the underscore in `rtw88_nativeN`.

Keep this wireless setting in `/etc/rc.conf`:

```sh
ifconfig_wlan0="WPA DHCP"
```

Keep the matching network credentials in `/etc/wpa_supplicant.conf`. The
service creates `wlan0` on the real parent interface `rtw88_nativeN` and
then uses the normal FreeBSD WPA/DHCP network setup.

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
sysctl dev.rtw88_native.0.station_ra_mask
sysctl dev.rtw88_native.0.station_rate_id
ifconfig wlan0
```

The manual `hw_start=1` toggle should no longer be needed when this service
and devd rule are installed.

## Performance changes and remaining work

Ordinary data frames now leave the RTW88 descriptor's explicit-rate bit clear.
On STA association, the driver also sends the AP's negotiated legacy CCK/OFDM
rate mask to firmware using RTW88's RA_INFO H2C command. Management and EAPOL
frames retain robust fixed legacy rates.

This is not a complete Linux RTW88 port. USB TX aggregation, HT/VHT MCS masks
and negotiation, AMPDU, hardware crypto offload, and full TX-rate/status
feedback remain missing. These need separate implementation and on-device
validation; no throughput improvement is guaranteed by source changes alone.
