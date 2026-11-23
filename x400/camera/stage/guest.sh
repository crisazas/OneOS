#!/bin/sh
# linux-x400 camera group, guest side. session-inner.sh runs it in the background once PipeWire and WirePlumber are up:
#   sh /tmp/x400/camera/guest.sh &
# x400-camsrc publishes the Video/Source nodes x400-camera-front and x400-camera-back; it only asks x400-camd (host side, over
# /tmp/x400-cam.sock) to open a camera while a client is streaming from the node. Restarted if it dies.
D=$(dirname "$0")
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
n=0
while [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ] && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
while :; do
    "$D/x400-camsrc" both >> /tmp/x400-camsrc.log 2>&1
    sleep 2
done
