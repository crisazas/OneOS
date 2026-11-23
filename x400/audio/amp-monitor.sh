#!/bin/sh
# Awinic readings while a stream plays: te (voice-coil temperature the DSP estimates from the IV sense, deg C) and
# dsp_re (load resistance estimate), both channels, one line per second. Runs on the Android host as root.
#   amp-monitor.sh <label> <secs> [command...]   (command runs in the background during the window)
name=$1; secs=$2; shift 2
[ $# -gt 0 ] && "$@" >/dev/null 2>&1 &
i=0
while [ $i -lt "$secs" ]; do
    l=""
    for d in 2-0034 2-0035; do
        te=$(cat /sys/bus/i2c/devices/$d/cur_temp | sed 's/te: //; s/ .*//')
        re=$(cat /sys/bus/i2c/devices/$d/dsp_re | sed 's/read dsp_re failed!/-/')
        l="$l $d te=$te re=$re"
    done
    echo "$name t=$i$l"
    i=$((i + 1)); sleep 1
done
wait
