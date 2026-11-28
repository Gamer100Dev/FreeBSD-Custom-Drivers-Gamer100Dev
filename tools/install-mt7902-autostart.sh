#!/bin/sh
# Install the native MT7902 module and enable its boot-time VAP setup.
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
MODULE="${MODULE:-$REPO_ROOT/build-native/if_mt7902_native.ko}"

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

install -d /boot/modules /usr/local/etc/rc.d
install -m 0555 "$MODULE" /boot/modules/if_mt7902_native.ko
install -m 0555 "$REPO_ROOT/etc/rc.d/mt7902_native" \
	/usr/local/etc/rc.d/mt7902_native

sysrc mt7902_native_enable="YES"
if [ -z "$(sysrc -n ifconfig_wlan1 2>/dev/null || true)" ]; then
	sysrc ifconfig_wlan1="WPA DHCP"
fi

/usr/local/etc/rc.d/mt7902_native start
/etc/rc.d/netif quietstart wlan1 || true

echo "Installed if_mt7902_native and enabled boot-time wlan1 setup."
echo "The next boot will load the module before netif and configure wlan1."
