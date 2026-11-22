#!/system/bin/sh
# linux-x400: guest_prepare.sh for a Linux desktop guest, staged in place of OneOS's Android one (switcher/assets/guest_prepare.sh).
# oneos_switch.sh runs it at [5/6] in init's mount namespace, with One UI's framework already stopped, and returns to One UI
# (return_to_oneui.sh) if it exits non-zero. It only attaches system.raw (an ext4 image of Ubuntu with KDE Plasma) read-only on a
# loop device and mounts it at $STAGE/rootfs, the way OneOS's does. Everything else (the guest's /tmp, /run, /dev, /proc, /sys,
# the GPU server, the desktop) happens in guest_start.sh, inside the guest's own mount namespace, where One UI never sees it.
# Credit: the attach and mount follow OneOS's guest_prepare.sh (OneOS, by Fahad Ali Javed and contributors; started from
# WeWaffles' OneLineage); see CREDITS.md.
STAGE="/data/local/tmp/oneos"
ROOTFS="$STAGE/rootfs"
if [ -z "$(readlink /proc/$$/ns/mnt 2>/dev/null)" ] || [ "$(readlink /proc/$$/ns/mnt 2>/dev/null)" != "$(readlink /proc/1/ns/mnt 2>/dev/null)" ]; then
    echo "ERROR: not in init's mount namespace; not starting the guest"
    exit 4
fi
BB=""
for c in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ -x "$c" ] && { BB=$c; break; }
done
[ -n "$BB" ] && [ -x "$BB" ] || { echo "ERROR: no busybox at the root manager's paths; not starting the guest"; exit 4; }
[ -f "$STAGE/system.raw" ] || { echo "ERROR: no $STAGE/system.raw (the Linux image)"; exit 3; }
# The loop's kernel thread cannot read a file labelled shell_data_file (SELinux), which /data/local/tmp gives:
# without this label the ext4 mount fails with an I/O error (measured on the SM-X400).
chcon u:object_r:apex_data_file:s0 "$STAGE/system.raw" 2>/dev/null
loop_ro() {
    d=$(stat -L -c '%t %T' "$1" 2>/dev/null) || return 1
    set -- $d
    [ "$(cat "/sys/dev/block/$(printf '%d:%d' "0x$1" "0x$2")/ro" 2>/dev/null)" = 1 ]
}
mkdir -p "$ROOTFS" "$STAGE/data"
if grep -q " $ROOTFS " /proc/mounts; then
    echo "Rootfs already mounted"
else
    for l in $($BB losetup -a | grep "system.raw" | cut -d: -f1); do
        $BB losetup -d "$l" 2>/dev/null || true
    done
    LOOP=$($BB losetup -f)
    $BB losetup -r "$LOOP" "$STAGE/system.raw" || { echo "ERROR: could not attach system.raw read-only ($LOOP)"; exit 3; }
    loop_ro "$LOOP" || echo "WARNING: could not confirm that $LOOP is read-only (sysfs)"
    echo "Using LOOP: $LOOP (read-only)"
    $BB mount -t ext4 -o ro "$LOOP" "$ROOTFS" || { echo "ERROR: could not mount $LOOP on $ROOTFS"; $BB losetup -d "$LOOP"; exit 3; }
fi
[ -x "$ROOTFS/usr/bin/kwin_wayland" ] || { echo "ERROR: $ROOTFS has no kwin_wayland: not the Linux desktop image"; exit 3; }
echo "Linux guest image mounted at $ROOTFS ($(. "$ROOTFS/etc/os-release" 2>/dev/null; echo "$PRETTY_NAME"))"
exit 0
