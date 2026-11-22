#!/bin/sh
# Install the native RTW88 module and its boot/hotplug integration.
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
MODULE="${MODULE:-$REPO_ROOT/build-native/if_rtw88_native.ko}"

if [ "$(uname -s)" != "FreeBSD" ]; then
	echo "This installer must run on FreeBSD." >&2
	exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "Run this installer as root." >&2
	exit 1
fi
if [ ! -s "$MODULE" ]; then
	echo "Missing $MODULE; run tools/build-native-module.sh first." >&2
	exit 1
fi

install -d /boot/modules /usr/local/etc/rc.d /usr/local/etc/devd
install -m 0555 "$MODULE" /boot/modules/if_rtw88_native.ko
install -m 0555 "$REPO_ROOT/etc/rc.d/rtw88_native" /usr/local/etc/rc.d/rtw88_native
install -m 0444 "$REPO_ROOT/etc/devd/rtw88_native.conf" /usr/local/etc/devd/rtw88_native.conf

sysrc rtw88_native_enable="YES"
sysrc wlans_rtw88_native0="wlan0"
if [ -z "$(sysrc -n ifconfig_wlan0 2>/dev/null || true)" ]; then
	sysrc ifconfig_wlan0="WPA DHCP"
fi

/etc/rc.d/devd restart
/usr/local/etc/rc.d/rtw88_native start
/etc/rc.d/netif quietstart wlan0 || true

echo "Installed if_rtw88_native and boot/USB-hotplug autostart."
echo "The next boot will load the newly installed module before netif."
