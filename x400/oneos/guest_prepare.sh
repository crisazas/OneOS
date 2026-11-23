#!/system/bin/sh
# linux-x400: guest_prepare.sh for a Linux desktop guest, staged in place of OneOS's Android one (switcher/assets/guest_prepare.sh).
# oneos_switch.sh runs it at [5/6] in init's mount namespace, with One UI's framework already stopped, and returns to One UI
# (return_to_oneui.sh) if it exits non-zero. It attaches system.raw (an ext4 image of Ubuntu with KDE Plasma) read-only on a
# loop device, the way OneOS's does, and puts a persistent writable layer over it (see "Persistence" below), the result being $STAGE/rootfs.
# Everything else (the guest's /tmp, /run, /dev, /proc, /sys, the GPU server, the desktop) happens in guest_start.sh, inside the guest's
# own mount namespace, where One UI never sees it.
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
# Persistence: OneOS keeps the guest's state in $STAGE/data, bound over the guest's /data, because Android has a place for state apart from
# the system. Ubuntu mixes both in one tree, so here the whole root is an overlay: the image (read-only, never written, so a cut cannot
# corrupt it) is the lower layer and everything the guest changes (packages, /etc, /root, /home, /var) goes to the upper layer, which
# survives the session and a reboot. The kernel refuses f2fs (what /data is) as an overlay upper layer ("filesystem not supported"),
# so the upper layer is an ext4 file, $STAGE/persist.img, on a loop device. X400_PERSIST=0 gives the old read-only root with nothing kept.
# $STAGE/rootfs.id (written by deploy.sh: the image's sha256) is stored in the layer: a layer made over another image would hide that image's
# files, and is not used then; x400/oneos/persist-reset.sh starts a new one.
PERSIST="$STAGE/persist.img"; LOWER="$STAGE/lower"; PMNT="$STAGE/persist"
PERSIST_SIZE=${X400_PERSIST_SIZE:-8G}
persist_off() { echo "WARNING: persistent layer off ($1): the root is read-only and nothing is kept"; PERSIST_ON=0; }
attach_image() { # attaches system.raw read-only on a loop and mounts it on $1; leaves the loop in $LOOP
    for l in $($BB losetup -a | grep "system.raw" | cut -d: -f1); do
        $BB losetup -d "$l" 2>/dev/null || true
    done
    LOOP=$($BB losetup -f)
    $BB losetup -r "$LOOP" "$STAGE/system.raw" || { echo "ERROR: could not attach system.raw read-only ($LOOP)"; exit 3; }
    loop_ro "$LOOP" || echo "WARNING: could not confirm that $LOOP is read-only (sysfs)"
    echo "Using LOOP: $LOOP (read-only)"
    $BB mount -t ext4 -o ro "$LOOP" "$1" || { echo "ERROR: could not mount $LOOP on $1"; $BB losetup -d "$LOOP"; exit 3; }
}
if grep -q " $ROOTFS " /proc/mounts; then
    echo "Rootfs already mounted"
else
    PERSIST_ON=1
    [ "${X400_PERSIST:-1}" = 0 ] && persist_off "X400_PERSIST=0"
    if [ $PERSIST_ON = 1 ]; then
        mkdir -p "$LOWER" "$PMNT"
        attach_image "$LOWER"
        if [ ! -f "$PERSIST" ]; then
            # sparse: it takes space only as the guest writes
            $BB truncate -s "$PERSIST_SIZE" "$PERSIST" && /system/bin/mke2fs -t ext4 -F -q -L x400persist "$PERSIST" && echo "Persistent layer created: $PERSIST ($PERSIST_SIZE, sparse)" ||
                { rm -f "$PERSIST"; persist_off "could not create $PERSIST"; }
        fi
    fi
    if [ $PERSIST_ON = 1 ]; then
        chcon u:object_r:apex_data_file:s0 "$PERSIST" 2>/dev/null
        /system/bin/e2fsck -p "$PERSIST" > "$STAGE/persist.fsck.log" 2>&1; rc=$?
        # 0 clean, 1 errors fixed; 2 and up: it needs a reboot or could not be repaired
        [ $rc -le 1 ] || persist_off "e2fsck of $PERSIST returned $rc (see $STAGE/persist.fsck.log)"
    fi
    if [ $PERSIST_ON = 1 ]; then
        PL=$($BB losetup -f)
        $BB losetup "$PL" "$PERSIST" && $BB mount -t ext4 -o noatime "$PL" "$PMNT" || { [ -n "$PL" ] && $BB losetup -d "$PL" 2>/dev/null; persist_off "could not mount $PERSIST"; }
    fi
    if [ $PERSIST_ON = 1 ]; then
        mkdir -p "$PMNT/up" "$PMNT/work"
        # the layer belongs to the image it was made over
        WANT=$(cat "$STAGE/rootfs.id" 2>/dev/null)
        HAVE=$(cat "$PMNT/image.id" 2>/dev/null)
        if [ -n "$HAVE" ] && [ -n "$WANT" ] && [ "$HAVE" != "$WANT" ]; then
            persist_off "the layer was made over another image ($HAVE, now $WANT): run x400-persist-reset"
        fi
    fi
    if [ $PERSIST_ON = 1 ]; then
        [ -n "$WANT" ] && [ -z "$HAVE" ] && echo "$WANT" > "$PMNT/image.id"
        $BB mount -t overlay overlay -o "lowerdir=$LOWER,upperdir=$PMNT/up,workdir=$PMNT/work" "$ROOTFS" ||
            persist_off "the overlay was refused ($($BB dmesg | $BB tail -n 1 | cut -c1-120))"
    fi
    if [ $PERSIST_ON = 0 ]; then
        # unwind what the persistent path set up, then the plain read-only mount
        for m in "$PMNT" "$LOWER"; do grep -q " $m " /proc/mounts && $BB umount "$m" 2>/dev/null; done
        for l in $($BB losetup -a | grep -E "persist.img" | cut -d: -f1); do $BB losetup -d "$l" 2>/dev/null; done
        attach_image "$ROOTFS"
    else
        echo "Persistent layer: $(du -sk "$PMNT/up" | cut -f1) KB written so far, $(df -k "$PMNT" | tail -n 1 | tr -s ' ' | cut -d' ' -f4) KB free in it"
    fi
fi
[ -x "$ROOTFS/usr/bin/kwin_wayland" ] || { echo "ERROR: $ROOTFS has no kwin_wayland: not the Linux desktop image"; exit 3; }
echo "Linux guest image mounted at $ROOTFS ($(. "$ROOTFS/etc/os-release" 2>/dev/null; echo "$PRETTY_NAME"); $(grep -q " $ROOTFS overlay" /proc/mounts && echo "persistent overlay" || echo "read-only, nothing kept"))"
exit 0
