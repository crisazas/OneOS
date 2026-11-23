#!/bin/sh
# Audio group, guest side. Run once PipeWire and WirePlumber are up (G0's session-inner.sh: `sh /tmp/x400/audio/guest.sh &`).
# x400-pwbridge creates the sink "Altavoces" and the source "Microfono" and connects them to x400-audiod's sockets in /tmp.
G=/tmp/x400/audio
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
n=0
while [ $n -lt 60 ] && ! [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; do n=$((n + 1)); sleep 1; done
[ -S "$XDG_RUNTIME_DIR/pipewire-0" ] || { echo "no pipewire socket in $XDG_RUNTIME_DIR" >&2; exit 1; }
sleep 2
exec $G/x400-pwbridge --dir /tmp > /tmp/x400-pwbridge.log 2>&1
