# linux-x400: runs on the tablet as root. Starts the Linux session (plasma.sh, own mount namespace) and, once the first
# frame exists, the display helper that shows it on a layer above One UI. Usage: sh run-desktop.sh [seconds]
T=/data/local/tmp; BB=/data/adb/ksu/bin/busybox
SECS=${1:-90}
rm -f $T/fbdir/fb.bin
svc power stayon true; input keyevent KEYCODE_WAKEUP
$BB unshare -m sh $T/plasma.sh $SECS > $T/plasma-run.log 2>&1 &
PP=$!
n=0; while [ $n -lt 90 ]; do
  [ -f $T/fbdir/fb.bin ] && [ "$(od -A n -t u8 -j 32 -N 8 $T/fbdir/fb.bin 2>/dev/null | tr -d ' ')" -gt 0 ] 2>/dev/null && break
  sleep 1; n=$((n+1))
done
echo "first frame after ${n}s"
$T/display.sh $T $T/fbdir/fb.bin 2112 1320 > $T/display-run.log 2>&1 &
DP=$!
sleep 12
screencap -p $T/desktop.png
echo "screencap taken"
if [ -n "$TAP" ]; then
  E=/dev/input/event8; set -- $TAP
  sendevent $E 3 47 0; sendevent $E 3 57 321; sendevent $E 3 53 $1; sendevent $E 3 54 $2; sendevent $E 1 330 1; sendevent $E 0 0 0
  sleep 0.15
  sendevent $E 3 57 -1; sendevent $E 1 330 0; sendevent $E 0 0 0
  sleep 4; screencap -p $T/desktop-tap.png; echo "tap at $1,$2 done"
fi
wait $PP
kill -TERM $DP 2>/dev/null; sleep 2; kill -9 $DP 2>/dev/null
svc power stayon false
echo "== display log"; cat $T/display-run.log | tail -n 6
