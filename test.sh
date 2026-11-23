#!/bin/sh

set -eu

MROOT="/compat/linux"
SYS="$MROOT/sys"
LSYS="$SYS/.sys"

normalize_pci_id()
{
    id=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')

    vendor=${id%%:*}
    device=${id#*:}

    # Normalize e.g. 8086:412 -> 8086:0412
    printf '%s:%04x\n' "$vendor" "$((16#$device))"
}

mkdir -p "$SYS" "$LSYS"

# Mount linsysfs underneath /compat/linux/sys/.sys.
# FreeBSD does not necessarily provide the Linux "mountpoint" command,
# so use mount(8) to test whether it is already mounted.
if ! mount | grep -Fq "linsysfs on $LSYS"; then
    mount -t linsysfs linsysfs "$LSYS"
fi

mkdir -p "$SYS/class/drm"
mkdir -p "$SYS/dev/char"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT INT TERM

echo "Scanning linsysfs DRM devices..."

# Build PCI_ID -> linsysfs DRM card mapping.
for card_dir in "$LSYS"/class/drm/card*; do
    [ -d "$card_dir" ] || continue

    uevent="$card_dir/device/uevent"
    [ -f "$uevent" ] || continue

    pci_id=$(
        awk -F= '
            /^PCI_ID=/ {
                print $2
                exit
            }
        ' "$uevent"
    )

    [ -n "$pci_id" ] || continue

    pci_id=$(normalize_pci_id "$pci_id")

    printf '%s\n' "$card_dir" > "$TMPDIR/$pci_id"

    echo "linsysfs: $pci_id -> $card_dir"
done

echo
echo "Reconstructing Linux DRM topology..."

# Use FreeBSD's DRM information to reconstruct the Linux DRM sysfs layout.
sysctl dev.drm 2>/dev/null |
while IFS= read -r line; do
    case "$line" in
        dev.drm.*.PCI_ID:\ *)
            idx=$(
                printf '%s\n' "$line" |
                sed -n 's/^dev\.drm\.\([0-9][0-9]*\)\.PCI_ID:.*/\1/p'
            )

            pci_id=$(
                printf '%s\n' "$line" |
                sed 's/^dev\.drm\.[0-9][0-9]*\.PCI_ID:[[:space:]]*//'
            )

            [ -n "$idx" ] || continue
            [ -n "$pci_id" ] || continue

            pci_id=$(normalize_pci_id "$pci_id")

            card_file="$TMPDIR/$pci_id"

            if [ ! -f "$card_file" ]; then
                echo "WARNING: no linsysfs DRM card for PCI ID $pci_id"
                continue
            fi

            card_dir=$(cat "$card_file")

            # Convert absolute path:
            # /compat/linux/sys/.sys/class/drm/card0
            #
            # into:
            # .sys/class/drm/card0
            rel_card=${card_dir#"$SYS/"}

            echo "DRM: dev.drm.$idx -> $pci_id -> $rel_card"

            # /compat/linux/sys/class/drm/cardN
            ln -sfn \
                "../../$rel_card" \
                "$SYS/class/drm/card$idx"

            # /compat/linux/sys/dev/char/226:N
            mkdir -p "$SYS/dev/char/226:$idx"

            # /sys/dev/char/226:N/device
            ln -sfn \
                "../../../$rel_card/device" \
                "$SYS/dev/char/226:$idx/device"

            # Linux DRM uevent
            cat > "$SYS/dev/char/226:$idx/uevent" <<EOF
MAJOR=226
MINOR=$idx
DEVNAME=dri/card$idx
DEVTYPE=dri_minor
EOF
            ;;
    esac
done

echo
echo "Re-exporting linsysfs class entries..."

# Re-export everything under .sys/class except DRM.
for path in "$LSYS"/class/*; do
    [ -e "$path" ] || continue

    basename=$(basename "$path")

    [ "$basename" = "drm" ] && continue

    ln -sfn \
        ".sys/class/$basename" \
        "$SYS/class/$basename"
done

echo "Re-exporting linsysfs top-level entries..."

# Re-export everything else from .sys.
for path in "$LSYS"/*; do
    [ -e "$path" ] || continue

    basename=$(basename "$path")

    [ "$basename" = "class" ] && continue

    ln -sfn \
        ".sys/$basename" \
        "$SYS/$basename"
done

echo
echo "LSU linsysfs + DRM workaround applied to $MROOT"
echo

echo "DRM cards:"
ls -la "$SYS/class/drm/" 2>/dev/null || true

echo
echo "DRM character devices:"
find "$SYS/dev/char" -maxdepth 1 -type d -name '226:*' -print 2>/dev/null || true
