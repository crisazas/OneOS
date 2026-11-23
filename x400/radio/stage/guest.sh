#!/bin/sh
# Runs in the guest as root. G0: `sh /tmp/x400/radio/guest.sh &` from session-inner.sh after G2's guest.sh, PATH=/usr/bin:/usr/sbin:/bin:/sbin.
# Both radios stay OFF unless a flag file exists, in /tmp/x400/radio/ (the bind of the stage: from host.sh, `touch` on the Android side
# in /data/local/tmp/oneos/x400/groups/radio/) or in /run/x400-radio/ (by hand in the guest):
#   enable-wifi   NetworkManager + wpa_supplicant on wlan0   (risk W2/W3)
#   enable-bt     btbridge + btattach + bluetoothd           (risk B3 onwards; run `btbridge.py --probe` by hand first)
# `guest.sh stop` ends what this script started, by pid file (never by name: Android has its own wpa_supplicant); run it before returning to One UI.
G=${X400_RADIO_G:-/tmp/x400/radio}
RUN=/run/x400-radio
LOG=/tmp/x400-radio.log
log() { echo "$(date +%T) radio: $*" >> "$LOG"; }
on() { [ -f $G/enable-$1 ] || [ -f $RUN/enable-$1 ]; }
stop_pid() { [ -f "$RUN/$1.pid" ] && kill "$2" "$(cat "$RUN/$1.pid")" 2>/dev/null; }

if [ "$1" = stop ]; then
  log "stop"
  stop_pid bluetoothd -TERM
  stop_pid btattach -TERM
  sleep 1
  stop_pid btbridge -USR2
  sleep 1
  stop_pid btbridge -TERM
  stop_pid nm -TERM
  sleep 2
  stop_pid wpa -TERM
  [ -f $RUN/nm.pid ] && ip addr flush dev wlan0 2>/dev/null
  rm -f $RUN/*.pid $RUN/btbridge.pty
  exit 0
fi

mkdir -p $RUN && chmod 700 $RUN
# host.sh puts the networks it extracted in the guest's /run/x400-radio before the guest runs; keep them (mkdir -p does)

# 1. The rootfs is read-only. /etc and /var/lib get an overlay whose upper layer is in /run (tmpfs): everything the packages and NetworkManager/bluez write
#    there lives in RAM and vanishes with the session. Must come first: a later overlay would cover earlier mounts.
rw_overlay() {
  mountpoint -q "$1" && return 0
  mkdir -p "$RUN/ov$1/up" "$RUN/ov$1/work" && mount -t overlay overlay -o "lowerdir=$1,upperdir=$RUN/ov$1/up,workdir=$RUN/ov$1/work" "$1" 2>>"$LOG" || log "overlay on $1 failed"
}
rw_overlay /etc
rw_overlay /var/lib
mkdir -p /etc/NetworkManager/system-connections /var/lib/NetworkManager /var/lib/bluetooth /run/NetworkManager /run/wpa_supplicant /run/dbus /run/udev/data
chmod 700 /etc/NetworkManager/system-connections /var/lib/bluetooth
[ -e /etc/resolv.conf ] || ln -s /run/NetworkManager/resolv.conf /etc/resolv.conf

# G2's guest.sh starts the system bus (x400/sensors/system-bus.conf, /run/dbus/system_bus_socket): wait for it up to 10 s, never create a second one,
# unless it never shows up (G2's config is then used, so the bus is the same kind).
wait_bus() {
  n=0
  while [ $n -lt 100 ] && ! [ -S /run/dbus/system_bus_socket ]; do n=$((n + 1)); sleep 0.1; done
  [ -S /run/dbus/system_bus_socket ] && return 0
  log "no system bus after 10 s: starting one with G2's config"
  dbus-daemon --config-file=/tmp/x400/sensors/system-bus.conf --nofork --nopidfile >> /tmp/sysbus-radio.log 2>&1 &
  sleep 1
  [ -S /run/dbus/system_bus_socket ]
}

if on wifi; then
  log "wifi: enabled"
  wait_bus || { log "wifi: no system bus, giving up"; exit 1; }
  export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
  # NetworkManager keeps a link without a udev database entry "unmanaged (platform-init)"; there is no systemd-udevd in the guest, so the entry that
  # marks wlan0 initialized is written here (nothing touches /dev; the entry vanishes with /run)
  idx=$(cat /sys/class/net/wlan0/ifindex 2>/dev/null)
  [ -n "$idx" ] && printf 'I:%s\nE:INTERFACE=wlan0\nG:x400\n' "$(( $(date +%s) * 1000000 ))" > /run/udev/data/n$idx
  for f in $RUN/*.nmconnection; do [ -f "$f" ] && install -m 600 "$f" /etc/NetworkManager/system-connections/; done
  install -m 644 $G/conf/NetworkManager.conf /run/NetworkManager/x400.conf
  wpa_supplicant -u -O /run/wpa_supplicant >> /tmp/wpa_supplicant.log 2>&1 &
  echo $! > $RUN/wpa.pid
  n=0; while [ $n -lt 30 ] && ! [ -S /run/dbus/system_bus_socket ]; do n=$((n + 1)); sleep 0.1; done
  sleep 1
  NetworkManager --no-daemon --config=/run/NetworkManager/x400.conf --config-dir=/nonexistent --pid-file=$RUN/nm-own.pid >> /tmp/NetworkManager.log 2>&1 &
  echo $! > $RUN/nm.pid
  log "wifi: wpa_supplicant $(cat $RUN/wpa.pid), NetworkManager $(cat $RUN/nm.pid)"
fi

if on bt; then
  log "bt: enabled"
  wait_bus || { log "bt: no system bus, giving up"; exit 1; }
  export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
  python3 $G/btbridge.py --pty-file $RUN/btbridge.pty --trace /tmp/btbridge.trace >> /tmp/btbridge.log 2>&1 &
  echo $! > $RUN/btbridge.pid
  n=0; while [ $n -lt 30 ] && ! [ -s $RUN/btbridge.pty ]; do n=$((n + 1)); sleep 0.1; done
  PTY=$(cat $RUN/btbridge.pty 2>/dev/null)
  [ -n "$PTY" ] || { log "bt: the bridge made no pty"; stop_pid btbridge -TERM; exit 1; }
  kill -USR1 "$(cat $RUN/btbridge.pid)"
  sleep 1
  btattach -B "$PTY" -P h4 >> /tmp/btattach.log 2>&1 &
  echo $! > $RUN/btattach.pid
  install -m 644 $G/conf/bluetooth-main.conf /run/x400-bluetooth-main.conf
  /usr/libexec/bluetooth/bluetoothd -n -f /run/x400-bluetooth-main.conf >> /tmp/bluetoothd.log 2>&1 &
  echo $! > $RUN/bluetoothd.pid
  log "bt: bridge $(cat $RUN/btbridge.pid) on $PTY, btattach $(cat $RUN/btattach.pid), bluetoothd $(cat $RUN/bluetoothd.pid)"
fi
on wifi || on bt || log "both radios off: only the writable places are ready"
exit 0
