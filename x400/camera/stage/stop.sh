#!/bin/sh
# Stops the camera group's host side (host.sh and its daemons). Run as root on the tablet.
for p in $(pgrep -f "groups/camera/host.sh" 2>/dev/null); do [ "$p" != "$$" ] && kill "$p" 2>/dev/null; done
for n in x400-camd sensormgr sensorservice fakesvc; do for p in $(pidof "$n" 2>/dev/null); do kill "$p" 2>/dev/null; done; done
rm -f /data/local/tmp/oneos/rootfs/tmp/x400-cam.sock
echo "camera group stopped"
