#!/bin/sh
# Independent speaker check: Awinic's algorithm runs on the A-Box DSP, senses coil
# current/voltage (IV sense) and logs the diaphragm excursion once per second:
#   awinic_get_big_data: get left big data[max_excursion max_temp over_excursion_cnt ...] = [...]
# The log is a ring that keeps old lines, so only lines stamped inside this window (firmware
# clock == kernel uptime) are printed. Runs on the Android host as root.
#
#   amp-excursion.sh <phase-name> <secs> [command...]   (command runs during the window)
L=/sys/kernel/debug/abox/runtime/log
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
name=$1; secs=$2; shift 2
t0=$(cut -d' ' -f1 /proc/uptime)
[ $# -gt 0 ] && "$@" >/dev/null 2>&1 &
sleep "$secs"
wait
t1=$(cut -d' ' -f1 /proc/uptime)
strings $L | grep 'awinic_get_big_data' | awk -v n="$name" -v a="$t0" -v b="$t1" '
    { t = $0; sub(/^\[ */, "", t); sub(/\].*/, "", t); t += 0
      if (t >= a && t <= b) { s = $0; sub(/.*big data\[[^]]*\] = \[/, "", s); sub(/\].*/, "", s); printf "%s t=%.1f %s %s\n", n, t, ($0 ~ /left/ ? "L" : "R"), s } }'
