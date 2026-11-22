#!/system/bin/sh
# linux-x400: the way back. Ends everything the session started and checks that the phone is as it was. Safe to run at any
# time, twice, or with no session. Writes $STAGE/return.outcome. Usage: sh x400_return.sh
STAGE=/data/local/tmp/oneos-x400
S=$STAGE/session; R=$STAGE/root
OUT=$STAGE/return.outcome; : > "$OUT"
say() { echo "return: $*"; echo "$*" >> "$OUT"; }
touch "$S/return.request" 2>/dev/null
# 1. the display layer first (it removes its own layer on SIGTERM), then the keys' readers
[ -f "$S/display.pid" ] && kill -TERM "$(cat "$S/display.pid")" 2>/dev/null
for p in /proc/[0-9]*; do
  c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
  case "$c" in *x400-display.dex*|*X400Display*) kill -TERM "${p#/proc/}" 2>/dev/null;; *x400_keywatch.sh*) kill "${p#/proc/}" 2>/dev/null;; esac
done
sleep 2
# 2. every process of the chroot (it shares Android's PID namespace), TERM then KILL; inputd's death gives the touchscreen back
for sig in TERM KILL; do
  for p in /proc/[0-9]*; do r=$(readlink "$p/root" 2>/dev/null); [ "$r" = "$R" ] && kill -$sig "${p#/proc/}" 2>/dev/null; done
  sleep 2
done
# 3. the Venus server and the session's own shell
# (the switch execs into this script and keeps its pid: never signal ourselves)
for f in venus.pid display.pid pid; do
  [ -f "$S/$f" ] || continue
  q=$(cat "$S/$f"); rm -f "$S/$f"
  [ "$q" = "$$" ] && continue
  kill -TERM "$q" 2>/dev/null; sleep 1; kill -KILL "$q" 2>/dev/null
done
# 4. what the phone must look like again
left=0; for p in /proc/[0-9]*; do r=$(readlink "$p/root" 2>/dev/null); [ "$r" = "$R" ] && left=$((left+1)); done
say "chroot processes left: $left"
say "stage mounts in init's namespace: $(grep -c -F "$STAGE" /proc/1/mountinfo)"
loops=0; for l in /sys/block/loop*/loop/backing_file; do read f < "$l" 2>/dev/null; case "$f" in *oneos-x400*) loops=$((loops+1));; esac; done
say "loop devices on the stage: $loops"
svc power stayon false 2>/dev/null; settings put global stay_on_while_plugged_in "$(sed -n 's/^stayon=//p' "$S/host.before" 2>/dev/null | head -n 1)" 2>/dev/null
say "stay-on setting: $(settings get global stay_on_while_plugged_in 2>/dev/null)"
tr_n=0; for p in /proc/[0-9]*; do r=$(readlink "$p/root" 2>/dev/null); [ "$r" = "$R" ] && ls -l "$p/fd" 2>/dev/null | grep -q 'input/event8' && tr_n=$((tr_n+1)); done
say "chroot readers of the touchscreen: $tr_n"
b=$(sed -n 's/^mounts=//p' "$S/host.before" 2>/dev/null | head -n 1); say "init mounts: $(wc -l < /proc/1/mountinfo) (before: ${b:-?})"
say "MemAvailable: $(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo) MiB"
if [ "$left" = 0 ] && [ "$(grep -c -F "$STAGE" /proc/1/mountinfo)" = 0 ] && [ "$loops" = 0 ]; then say "OK: One UI is as it was"; else say "FAILED: see above"; exit 1; fi
