#!/bin/sh
# Build the native RTW88 module against a matching FreeBSD source tree.
set -eu

SRCROOT="${SRCROOT:-/usr/src}"
REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

if [ "$(uname -s)" != "FreeBSD" ]; then
    echo "This script must run on FreeBSD, not $(uname -s)." >&2
    exit 1
fi
if [ ! -d "$SRCROOT/sys/modules" ] || [ ! -d "$SRCROOT/sys/dev" ]; then
    echo "FreeBSD source tree not found at $SRCROOT (set SRCROOT)." >&2
    exit 1
fi

install -d "$SRCROOT/sys/dev/rtw88_native" "$SRCROOT/sys/modules/rtw88_native"
cp -R "$REPO_ROOT/sys/dev/rtw88_native/." "$SRCROOT/sys/dev/rtw88_native/"
cp -R "$REPO_ROOT/sys/modules/rtw88_native/." "$SRCROOT/sys/modules/rtw88_native/"
cd "$SRCROOT/sys/modules/rtw88_native"
make clean
make
test -f if_rtw88_native.ko
echo "Build succeeded: $SRCROOT/sys/modules/rtw88_native/if_rtw88_native.ko"
