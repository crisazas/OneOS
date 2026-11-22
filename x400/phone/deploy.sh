#!/bin/sh
# linux-x400: puts the Linux session on the tablet (adb, root). Everything lives in /data/local/tmp/oneos-x400, separate from
# OneOS's own stage (/data/local/tmp/oneos). Deleting that folder removes it all. Usage: x400/phone/deploy.sh [--no-image]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STAGE=/data/local/tmp/oneos-x400
IMG=$ROOT/out/rootfs-plasma.ext4
NOIMG=0; [ "$1" = "--no-image" ] && NOIMG=1
for f in "$ROOT/out/display/x400-display.dex" "$ROOT/out/display/libx400display.so" "$ROOT/x400/display/bin/capture" "$ROOT/x400/display/bin/inputd" "$ROOT/out/venus-build/virgl_test_server"; do
  [ -f "$f" ] || { echo "missing $f (build it first: x400/README.md)" >&2; exit 1; }
done
[ "$NOIMG" = 1 ] || [ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin"
cp "$ROOT/out/display/x400-display.dex" "$ROOT/out/display/libx400display.so" "$TMP/bin/"
cp "$ROOT/x400/display/bin/capture" "$ROOT/x400/display/bin/inputd" "$TMP/bin/"
cp "$ROOT/out/venus-build/virgl_test_server" "$TMP/bin/x400vt"
cp "$ROOT/x400/display/display.sh" "$ROOT/x400/phone/session-inner.sh" "$ROOT/x400/phone/x400_keywatch.sh" "$ROOT/x400/phone/x400_session.sh" "$TMP/bin/"
cp "$ROOT/x400/phone/x400_switch.sh" "$ROOT/x400/phone/x400_return.sh" "$TMP/"
adb shell "su -c 'mkdir -p $STAGE/bin $STAGE/logs $STAGE/session && chmod 700 $STAGE && chown 0:0 $STAGE'"
adb push "$TMP/bin/." /data/local/tmp/x400-incoming/ >/dev/null 2>&1 || { adb shell 'mkdir -p /data/local/tmp/x400-incoming'; adb push "$TMP/bin/." /data/local/tmp/x400-incoming/ >/dev/null; }
adb push "$TMP/x400_switch.sh" "$TMP/x400_return.sh" /data/local/tmp/x400-incoming/ >/dev/null
adb shell "su -c 'cp /data/local/tmp/x400-incoming/x400_switch.sh /data/local/tmp/x400-incoming/x400_return.sh $STAGE/ && cp /data/local/tmp/x400-incoming/* $STAGE/bin/ && rm -f $STAGE/bin/x400_switch.sh $STAGE/bin/x400_return.sh && chmod 755 $STAGE/*.sh $STAGE/bin/* && chown -R 0:0 $STAGE/bin $STAGE/*.sh && rm -rf /data/local/tmp/x400-incoming'"
if [ "$NOIMG" = 0 ]; then
  have=$(adb shell "su -c '[ -f $STAGE/rootfs.ext4 ] && stat -c %s $STAGE/rootfs.ext4'" | tr -d '\r')
  if [ "$have" != "$(stat -c %s "$IMG")" ]; then
    adb push "$IMG" /data/local/tmp/x400-rootfs-incoming.ext4 >/dev/null
    adb shell "su -c 'mv /data/local/tmp/x400-rootfs-incoming.ext4 $STAGE/rootfs.ext4 && chown 0:0 $STAGE/rootfs.ext4 && chmod 600 $STAGE/rootfs.ext4'"
  fi
  adb shell "su -c 'chcon u:object_r:apex_data_file:s0 $STAGE/rootfs.ext4'"
fi
# the pinned sums: the session refuses to start if a staged file changed
adb shell "su -c 'cd $STAGE && sha256sum x400_switch.sh x400_return.sh bin/* > stage.sha256 && wc -l < stage.sha256'"
echo "staged in $STAGE; start with: adb shell su -c 'sh $STAGE/x400_switch.sh --seconds 120'"
