#!/bin/sh
# Overlay and build the native RTW88 module in a FreeBSD source tree.
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
OBJDIR="$(make -V .OBJDIR)"
if [ -z "$OBJDIR" ]; then
    echo "Could not determine the kernel-module object directory." >&2
    exit 1
fi

make clean
make

KMOD="$OBJDIR/if_rtw88_native.ko"
if [ ! -f "$KMOD" ]; then
    echo "Build completed but the module was not found at: $KMOD" >&2
    exit 1
fi
echo "Build succeeded: $KMOD"
