#!/bin/sh
# Runs in the guest (G0's session-inner.sh: `sh /tmp/x400/sensors/guest.sh &` after KWin is up). Needs WAYLAND_DISPLAY and the session bus in the environment.
G=/tmp/x400/sensors
mkdir -p /run/dbus
if ! [ -S /run/dbus/system_bus_socket ]; then
  dbus-daemon --config-file=$G/system-bus.conf --nofork --nopidfile > /tmp/sysbus.log 2>&1 &
  sleep 1
fi
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
[ -x /usr/libexec/upowerd ] && /usr/libexec/upowerd > /tmp/upowerd.log 2>&1 &
# --rotate stays off until the display helper and inputd follow a rotation (G6); X400_ROTATE=1 turns it on
if [ "${X400_ROTATE:-0}" = 1 ]; then
  exec $G/x400-sensord --rotate --hook $G/x400-rotate-hook > /tmp/sensord.log 2>&1
else
  exec $G/x400-sensord > /tmp/sensord.log 2>&1
fi
