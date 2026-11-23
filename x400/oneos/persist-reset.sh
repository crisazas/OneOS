#!/bin/sh
# linux-x400: starts the guest's persistent layer from scratch (x400/oneos/guest_prepare.sh): everything the Linux session saved since
# (installed packages, /etc, /root, /home, /var) is gone and the next session shows the image as it came.
# It does not delete the layer: it moves it to persist.img.old-<epoch>, so nothing is lost until you remove that file yourself.
# Usage, on the tablet as root and with One UI up (no session running):  sh /data/local/tmp/oneos/persist-reset.sh
STAGE=/data/local/tmp/oneos
[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
grep -q " $STAGE/rootfs " /proc/mounts && { echo "a session is mounted ($STAGE/rootfs): return to One UI first" >&2; exit 1; }
grep -q " $STAGE/persist " /proc/mounts && { echo "$STAGE/persist is still mounted: return to One UI first" >&2; exit 1; }
[ -f "$STAGE/persist.img" ] || { echo "no $STAGE/persist.img: nothing to reset"; exit 0; }
T=$(date +%s)
mv "$STAGE/persist.img" "$STAGE/persist.img.old-$T" && echo "moved to $STAGE/persist.img.old-$T ($(du -k "$STAGE/persist.img.old-$T" | cut -f1) KB on disk); the next session creates a new one"
