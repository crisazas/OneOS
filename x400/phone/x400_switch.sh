#!/system/bin/sh
# linux-x400: the Linux session, in OneOS's shape. One UI keeps running; nothing is flashed.
# Every refusal comes before the first change to the phone. Way back: x400_return.sh (also run here when the session ends),
# or hold Volume Up + Volume Down for 3 s. Usage: sh x400_switch.sh [--seconds N] [--cmd 'command inside the chroot']
STAGE=/data/local/tmp/oneos-x400
BB=/data/adb/ksu/bin/busybox
S=$STAGE/session
SECS=0; INNER_CMD=
while [ $# -gt 0 ]; do case "$1" in --seconds) SECS=$2; shift 2;; --cmd) INNER_CMD=$2; shift 2;; *) echo "usage: x400_switch.sh [--seconds N] [--cmd CMD]"; exit 2;; esac; done

refuse() { echo "PREFLIGHT REFUSED: $*"; echo "Nothing on the phone was changed; One UI keeps running."; exit 2; }
[ "$(id -u 2>/dev/null)" = 0 ] || refuse "not running as root"
[ -x "$BB" ] || refuse "no busybox at $BB"
applets=$("$BB" --list 2>/dev/null); for a in mount umount losetup unshare chroot timeout sha256sum; do echo "$applets" | grep -qx "$a" || refuse "busybox lacks $a"; done
command -v getevent >/dev/null 2>&1 || refuse "no getevent: Volume Up + Volume Down could not end the session"
command -v screencap >/dev/null 2>&1 || refuse "no screencap"
[ -d "$STAGE" ] && [ ! -L "$STAGE" ] || refuse "$STAGE is missing"
[ "$(stat -c %u "$STAGE")" = 0 ] || refuse "$STAGE is not root's"
[ -f "$STAGE/rootfs.ext4" ] || refuse "the Linux image ($STAGE/rootfs.ext4) is not staged"
[ -f "$STAGE/stage.sha256" ] || refuse "no $STAGE/stage.sha256"
( cd "$STAGE" && "$BB" sha256sum -c stage.sha256 >/dev/null 2>&1 ) || refuse "a staged file does not match stage.sha256 (run deploy-phone.sh again)"
[ ! -e "$S/pid" ] || { kill -0 "$(cat "$S/pid")" 2>/dev/null && refuse "a session is already running (pid $(cat "$S/pid")); x400_return.sh ends it"; }
grep -q -F "$STAGE" /proc/1/mountinfo && refuse "something of the stage is still mounted in init's namespace; run x400_return.sh"
mem=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo); [ "$mem" -ge 1500 ] || refuse "MemAvailable is $mem MiB (need 1500)"
[ -e /dev/dma_heap/system ] || refuse "no /dev/dma_heap/system (the Venus server needs it)"
[ -e /system/lib64/libvulkan.so ] || refuse "no /system/lib64/libvulkan.so"
KDEVS=$(getevent -pl 2>/dev/null | awk '/^add device/ { dev = $NF } /KEY_VOLUME(UP|DOWN)/ { print dev }' | sort -u)
[ -n "$KDEVS" ] || refuse "no input device reports the volume keys"

# --- the session (from here the phone changes; x400_return.sh undoes it) ---
mkdir -p "$S/fb" "$STAGE/logs"; rm -f "$S/return.request" "$S/fb/fb.bin" "$S/.vu" "$S/.vd"
LOG=$STAGE/logs/session-$(date +%Y%m%d-%H%M%S).log
{
  echo "stayon=$(settings get global stay_on_while_plugged_in 2>/dev/null)" 
  echo "mounts=$(wc -l < /proc/1/mountinfo)"; echo "brightness=$(cat /sys/class/backlight/panel/brightness 2>/dev/null)"
  echo "selinux=$(getenforce 2>/dev/null)"; echo "memavail=$mem"
} > "$S/host.before"
echo "$$" > "$S/pid"
exec >> "$LOG" 2>&1
echo "x400 session start $(date): stage $STAGE, key devices: $KDEVS"
svc power stayon true; input keyevent KEYCODE_WAKEUP

for kd in $KDEVS; do getevent -lq "$kd" 2>/dev/null | sh "$STAGE/bin/x400_keywatch.sh" "$S" & done
SECS_ARG=${SECS:-0}
"$BB" unshare -m sh "$STAGE/bin/x400_session.sh" "$STAGE" "$SECS_ARG" "$INNER_CMD" > "$STAGE/logs/session-inner.log" 2>&1 &
SP=$!
n=0
while [ $n -lt 120 ]; do
  [ -f "$S/fb/fb.bin" ] && [ "$(od -A n -t u8 -j 32 -N 8 "$S/fb/fb.bin" 2>/dev/null | tr -d ' ')" -gt 0 ] 2>/dev/null && break
  kill -0 $SP 2>/dev/null || break
  sleep 1; n=$((n+1))
done
echo "first frame after ${n}s"
if [ -f "$S/fb/fb.bin" ]; then
  "$STAGE/bin/display.sh" "$STAGE/bin" "$S/fb/fb.bin" 2112 1320 > "$STAGE/logs/display.log" 2>&1 &
  echo $! > "$S/display.pid"
fi
[ -n "$SHOT" ] && { sleep 12; screencap -p "$SHOT"; echo "screenshot: $SHOT"; }
while [ ! -e "$S/return.request" ] && kill -0 $SP 2>/dev/null; do sleep 1; done
[ -e "$S/return.request" ] && echo "return requested (Volume Up + Volume Down, or x400_return.sh)"
exec sh "$STAGE/x400_return.sh"
