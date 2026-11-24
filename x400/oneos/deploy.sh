#!/bin/sh
# linux-x400: stages the Linux desktop guest for OneOS on the tablet. OneOS's own deploy (scripts/deploy-phone.sh) puts its session
# scripts in /data/local/tmp/oneos and the image as system.raw; then this replaces the two guest scripts with the Linux ones
# (x400/oneos/guest_prepare.sh, guest_start.sh) and puts the desktop's helpers in $STAGE/x400. OneOS's scripts are not changed.
# Usage: x400/oneos/deploy.sh [--scripts-only]      (needs adb and root on the tablet; ANDROID_SERIAL picks the device)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=/data/local/tmp/oneos
IMG=$ROOT/out/rootfs-plasma.ext4
SERIAL="${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}"
export ANDROID_SERIAL="$SERIAL"
adb_() { adb -s "$SERIAL" "$@"; }
su_() { adb_ shell "su -c '$*'"; }
for f in "$ROOT/out/display/x400-display.dex" "$ROOT/out/display/libx400display.so" "$ROOT/x400/display/bin/capture" \
         "$ROOT/x400/display/bin/inputd" "$ROOT/out/venus-build/virgl_test_server"; do
    [ -f "$f" ] || { echo "missing $f (build it first: x400/README.md)" >&2; exit 1; }
done
# The same copy OneOS's scripts/deploy-phone.sh makes of its session scripts and helpers (switcher/assets/*.sh and its HELPERS list,
# same owner and modes), and the image as system.raw. Not through deploy-phone.sh itself: its "plain umount" lint refuses
# OneOS 0.5.34's own guest_prepare.sh:5423-5439 (not used here: the Linux guest_prepare.sh replaces it) and oneos_switch.sh:55
# (inside a string written to the logcat wrapper, not run), so it stops before pushing anything.
HELPERS="OneOSBootStatus.dex oneos_boot_icon.png zygote_exec OneOSEdmStub.dex oneos_btaudio OneOSOverlayDimen.dex"
OI=/data/local/tmp/oneos_incoming
[ "$(su_ id -u | tr -d '\r')" = 0 ] || { echo "no root on the tablet" >&2; exit 1; }
su_ "grep -q \" $STAGE/rootfs \" /proc/mounts && echo mounted" | grep -q mounted && { echo "a session is mounted ($STAGE/rootfs); return to One UI first" >&2; exit 1; }
adb_ shell "rm -rf $OI && mkdir -p $OI"
adb_ push "$ROOT"/switcher/assets/*.sh $(for h in $HELPERS; do printf "%s " "$ROOT/switcher/assets/$h"; done) \
         "$ROOT/x400/oneos/overlays/services_patched.jar" "$ROOT/x400/oneos/overlays/guards.txt" "$OI/" >/dev/null
su_ "mkdir -p $STAGE && chown 0:0 $STAGE && chmod 755 $STAGE && cp -f $OI/* $STAGE/ && chmod 755 $STAGE/*.sh $STAGE/zygote_exec $STAGE/oneos_btaudio && \
     chmod 644 $STAGE/OneOSBootStatus.dex $STAGE/oneos_boot_icon.png $STAGE/OneOSEdmStub.dex $STAGE/OneOSOverlayDimen.dex $STAGE/services_patched.jar $STAGE/guards.txt && \
     chown -R 0:0 $STAGE && rm -rf $OI"
if [ "$1" != --scripts-only ]; then
    [ -f "$IMG" ] || { echo "missing $IMG (x400/build-rootfs-plasma.sh)" >&2; exit 1; }
    want=$(stat -c %s "$IMG")
    have=$(su_ "stat -c %s $STAGE/system.raw 2>/dev/null" | tr -d '\r')
    # the size alone is not enough: a rebuilt image of the same size (rootfs-extras.sh keeps SIZE) is a different image
    [ "$have" = "$want" ] && [ "$(su_ "cat $STAGE/rootfs.id 2>/dev/null" | tr -d '\r')" != "$(sha256sum "$IMG" | cut -d' ' -f1)" ] && have=changed
    if [ "$have" != "$want" ]; then
        adb_ push "$IMG" /data/local/tmp/x400-system.raw.tmp >/dev/null
        su_ "mv -f /data/local/tmp/x400-system.raw.tmp $STAGE/system.raw && chown 0:0 $STAGE/system.raw && chmod 644 $STAGE/system.raw"
    fi
    got=$(su_ "sha256sum $STAGE/system.raw" | cut -d' ' -f1 | tr -d '\r')
    [ "$got" = "$(sha256sum "$IMG" | cut -d' ' -f1)" ] || { echo "system.raw checksum mismatch on the tablet" >&2; exit 1; }
    su_ "echo $got > $STAGE/rootfs.id && chmod 644 $STAGE/rootfs.id"
    echo "system.raw verified ($got)"
fi
INC=/data/local/tmp/x400_incoming
KEYS=""; for k in keys pend x400_kwin_input.so; do [ -f "$ROOT/out/input/$k" ] && KEYS="$KEYS $k"; done
adb_ shell "mkdir -p $INC"
adb_ push "$ROOT/x400/oneos/guest_prepare.sh" "$ROOT/x400/oneos/guest_start.sh" "$ROOT/x400/oneos/persist-reset.sh" "$INC/" >/dev/null
adb_ push "$ROOT/out/display/x400-display.dex" "$ROOT/out/display/libx400display.so" "$ROOT/x400/display/display.sh" \
         "$ROOT/x400/display/bin/capture" "$ROOT/x400/display/bin/inputd" $(for k in $KEYS; do echo "$ROOT/out/input/$k"; done) "$ROOT/x400/phone/session-inner.sh" \
         "$ROOT/x400/phone/cpu-sample.sh" "$INC/" >/dev/null
# the server with VCMD_RESOURCE_IMPORT_FD (x400/venus/vtest-import.patch) when it was built, else the plain one
VT=$ROOT/out/venus-build-import/virgl_test_server; [ -f "$VT" ] || VT=$ROOT/out/venus-build/virgl_test_server
# X400_VT=<path> uses another server binary (for instance the one built with the VKR_TRACE prints)
[ -n "${X400_VT:-}" ] && VT=$X400_VT
adb_ push "$VT" "$INC/x400vt" >/dev/null
# the guest's libvulkan_virtio with the dma-buf import (x400/venus/guest-mesa/build.sh), if built
if [ -f "$ROOT/out/mesa-guest/libvulkan_virtio.so" ]; then
    adb_ push "$ROOT/out/mesa-guest/libvulkan_virtio.so" "$INC/libvulkan_virtio.so" >/dev/null
    su_ "mkdir -p $STAGE/x400 && cp -f $INC/libvulkan_virtio.so $STAGE/x400/libvulkan_virtio.so && chmod 644 $STAGE/x400/libvulkan_virtio.so && chcon u:object_r:apex_data_file:s0 $STAGE/x400/libvulkan_virtio.so"
fi
su_ "cp -f $INC/guest_prepare.sh $INC/guest_start.sh $INC/persist-reset.sh $STAGE/ && chmod 755 $STAGE/guest_prepare.sh $STAGE/guest_start.sh $STAGE/persist-reset.sh && \
     mkdir -p $STAGE/x400 && cd $INC && cp -f x400-display.dex libx400display.so display.sh capture inputd $KEYS session-inner.sh cpu-sample.sh x400vt $STAGE/x400/ && \
     chmod 755 $STAGE/x400/* && chown -R 0:0 $STAGE/x400 $STAGE/guest_prepare.sh $STAGE/guest_start.sh $STAGE/persist-reset.sh && rm -rf $INC"
# what each hardware group ships (x400/HARDWARE.md): x400/<group>/stage/ goes to $STAGE/x400/groups/<group>/ as it is
for gd in "$ROOT"/x400/*/stage; do
    [ -d "$gd" ] || continue; gn=$(basename "$(dirname "$gd")")
    tar -C "$gd" -cf /tmp/x400-group-$gn.tar . && adb_ push /tmp/x400-group-$gn.tar /data/local/tmp/x400-group-$gn.tar >/dev/null
    su_ "rm -rf $STAGE/x400/groups/$gn && mkdir -p $STAGE/x400/groups/$gn && /data/adb/ksu/bin/busybox tar -C $STAGE/x400/groups/$gn -xf /data/local/tmp/x400-group-$gn.tar && chown -R 0:0 $STAGE/x400/groups/$gn && chmod -R a+rX $STAGE/x400/groups/$gn && rm -f /data/local/tmp/x400-group-$gn.tar"
    rm -f /tmp/x400-group-$gn.tar; echo "group $gn staged"
done
if [ -d "$ROOT/out/mesa25/root" ]; then
    # one tar: faster than pushing 9000 small files, and the stage's copy is replaced as a whole
    tar -C "$ROOT/out/mesa25/root" -cf /tmp/x400-mesa25.tar .
    adb_ push /tmp/x400-mesa25.tar /data/local/tmp/x400-mesa25.tar >/dev/null
    su_ "rm -rf $STAGE/x400/mesa25 && mkdir -p $STAGE/x400/mesa25 && /data/adb/ksu/bin/busybox tar -C $STAGE/x400/mesa25 -xf /data/local/tmp/x400-mesa25.tar && chown -R 0:0 $STAGE/x400/mesa25 && rm -f /data/local/tmp/x400-mesa25.tar"
    rm -f /tmp/x400-mesa25.tar
fi
su_ "ls -la $STAGE/x400; sha256sum $STAGE/guest_prepare.sh $STAGE/guest_start.sh"
echo "staged the Linux guest in $STAGE; start a session with:"
echo "  adb shell su -c 'ONEOS_RETURN_MODE=inplace sh $STAGE/oneos_switch.sh'"
