#!/bin/sh
# linux-x400 camera group, host side. oneos/guest_start.sh runs this as root, outside the chroot, in the background, at session start
# (the group's stage/ directory is this one). It keeps four Android-side daemons up, in this order, because One UI's system_server is
# stopped and the camera HAL needs services that only it registers (details in x400/camera/RESULT.md):
#   fakesvc       permission_checker, activity, appops, sensor_privacy, batterystats, ... stand-ins for what cameraserver and sensorservice call
#   sensorservice Android's own /system/bin/sensorservice (nobody starts it with One UI stopped; the camera HAL enables motion sensors through it)
#   sensormgr     registers android.frameworks.sensorservice@1.0::ISensorManager/default (the provider blocks forever without it)
#   x400-camd     the camera server: /data/local/tmp/oneos/rootfs/tmp/x400-cam.sock, opens a camera only while a client is connected
# If a real system_server appears (One UI came back) everything is stopped and this script ends. stop.sh ends it by hand.
D=$(cd "$(dirname "$0")" && pwd)
LOG=/data/local/tmp/oneos/data/x400/logs
mkdir -p "$LOG"
say() { echo "[camera] $(date +%T) $*"; }

# pidof, not pkill -x: a binder thread pool renames the main thread (comm), so sensorservice is not found by name
stop_all() { for n in x400-camd sensormgr sensorservice fakesvc; do for p in $(pidof "$n" 2>/dev/null); do kill "$p" 2>/dev/null; done; done; }

if [ -n "$(pidof system_server)" ]; then say "system_server is running: its services are real, not starting anything"; exit 0; fi
# an earlier host.sh of this group (a second session start) is replaced
me=$$
for p in $(pgrep -f "groups/camera/host.sh" 2>/dev/null); do [ "$p" != "$me" ] && kill "$p" 2>/dev/null; done
stop_all
sleep 1
rm -f /data/local/tmp/oneos/rootfs/tmp/x400-cam.sock
say "starting from $D"

up() { [ -n "$(pidof "$1")" ]; }
registered() { service list 2>/dev/null | grep -q -E "[0-9][[:space:]]+$1:"; }
launch() { n=$1; shift; "$@" >> "$LOG/camera-$n.log" 2>&1 & say "$n started ($!)"; }

while :; do
    if [ -n "$(pidof system_server)" ]; then say "system_server appeared: stopping"; stop_all; exit 0; fi
    up fakesvc || launch fakesvc "$D/fakesvc" 0
    if registered package_native; then
        up sensorservice || launch sensorservice /system/bin/sensorservice
    fi
    if registered sensorservice; then
        up sensormgr || launch sensormgr "$D/sensormgr"
        up x400-camd || launch x400-camd "$D/x400-camd" serve
    fi
    sleep 2
done
