# linux-x400: does the frame file (fb.bin, on f2fs) cost flash writes and CPU? Read-only on /proc; run as root with a session up.
# Usage: [SHM=1] fbio-sample.sh [SECONDS, default 10]. Starts es2gears as load for the second half: on Zink, or with SHM=1 on llvmpipe
# (wl_shm buffers, no Venus client: a second Zink client has crashed x400vt twice).
D=${1:-10}
BB=/data/adb/ksu/bin/busybox
K=$(pidof kwin_wayland | cut -d' ' -f1); R=/proc/$K/root
FB=/proc/$K/root/tmp/fb/fb.bin
DEV=$(awk '$5 ~ /\/tmp\/fb$/ {print $3}' /proc/$K/mountinfo | head -n 1)
DISP=""; for p in /proc/[0-9]*; do case "$(tr '\0' ' ' < $p/cmdline 2>/dev/null)" in *X400Display*) DISP=${p#/proc/}; break;; esac; done
echo "device $DEV, display helper pid $DISP, dirty: $(for f in dirty_ratio dirty_background_ratio dirty_expire_centisecs dirty_writeback_centisecs; do printf '%s=%s ' $f $(cat /proc/sys/vm/$f); done)"
vm() { awk '/^(nr_dirty|nr_written|pgpgout|nr_dirtied) / {printf "%s=%s ", $1, $2} END {print ""}' /proc/vmstat; }
dsall() { awk -v M="${DEV%:*}" -v m="${DEV#*:}" '$1 == M && $2 == m {print "writes=" $8, "sectors_written=" $10}' /proc/diskstats; }
cpu() { for p in $K $DISP; do [ -n "$p" ] || continue; for t in /proc/$p/task/*; do echo "$p ${t##*/} $(cat $t/comm | tr ' ' _) $(sed 's/.*) //' $t/stat | awk '{print $12+$13}')"; done; done; }
run() { # $1 label
  cpu > /data/local/tmp/fio-c0; c0t=$(date +%s); vm > /data/local/tmp/fio-v0; dsall > /data/local/tmp/fio-d0; q0=$(od -An -t u8 -j 32 -N 8 $FB | tr -d ' '); t0=$(date +%s)
  sleep $D
  q1=$(od -An -t u8 -j 32 -N 8 $FB | tr -d ' '); t1=$(date +%s)
  cpu > /data/local/tmp/fio-c1; c1t=$(date +%s); vm > /data/local/tmp/fio-v1; dsall > /data/local/tmp/fio-d1
  echo; echo "=== $1: $((t1 - t0)) s between the two frame counts, frames $q0 -> $q1 = $(( (q1 - q0) / (t1 - t0) )) fps"
  echo "vmstat before: $(cat /data/local/tmp/fio-v0)"; echo "vmstat after:  $(cat /data/local/tmp/fio-v1)"
  echo "block device before: $(cat /data/local/tmp/fio-d0)"; echo "block device after:  $(cat /data/local/tmp/fio-d1)"
  echo "cpu snapshots $((c1t - c0t)) s apart; ticks, pid, tid, name:"; awk 'NR==FNR { a[$1" "$2]=$4; next } { d=$4-a[$1" "$2]; if (d > 0) print d, $1, $2, $3 }' /data/local/tmp/fio-c0 /data/local/tmp/fio-c1 | sort -rn | head -n 8
}
run "idle"
cat > $R/tmp/fio-inner.sh <<EOF
SHM=$SHM
. /tmp/kwin.env
export PATH=/usr/bin:/bin XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-x400 HOME=/tmp
if [ "$SHM" = 1 ]; then unset MESA_LOADER_DRIVER_OVERRIDE VK_INSTANCE_LAYERS; export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1; fi
exec es2gears_wayland
EOF
[ -f $R/tmp/kwin.env ] || { tr '\0' '\n' < /proc/$K/environ | sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z_0-9]*\)=\(.*\)\$/export \1='\2'/" | grep '^export ' > $R/tmp/kwin.env; }
$BB nsenter -t $K -m $BB chroot $R /bin/sh /tmp/fio-inner.sh > /data/local/tmp/fio-gears.out 2>&1 < /dev/null &
G=$!; sleep 4
run "es2gears ($([ "$SHM" = 1 ] && echo llvmpipe/wl_shm || echo Zink))"
kill $G 2>/dev/null; sleep 1; kill -9 $G 2>/dev/null
echo; echo "display.log: $(tail -n 3 /data/local/tmp/oneos/data/x400/logs/display.log)"
echo "capture.log: $(tail -n 3 $R/tmp/capture.log)"
echo "kwin alive: $(pidof kwin_wayland)"
