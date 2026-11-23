# linux-x400: fps and where every part of the session spends its time, idle and under load (read-only on /proc, no ptrace).
# Run as root on the host while a session is up. Usage: load-sample.sh [SECONDS per phase, default 12]
# Phases: idle | shm client (es2gears on llvmpipe, wl_shm buffers) | zink client (es2gears on Zink/Venus).
D=${1:-12}
BB=/data/adb/ksu/bin/busybox
K=$(pidof kwin_wayland | cut -d' ' -f1); [ -n "$K" ] || { echo "no kwin_wayland"; exit 1; }
R=/proc/$K/root
FB=/proc/$K/root/tmp/fb/fb.bin
tr '\0' '\n' < /proc/$K/environ | sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z_0-9]*\)=\(.*\)\$/export \1='\2'/" | grep '^export ' > $R/tmp/kwin.env
pid_of() { for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = "$1" ] && { echo ${p#/proc/}; return; }; done; }
tid_of() { for t in /proc/$1/task/*; do [ "$(cat $t/comm 2>/dev/null)" = "$2" ] && { echo ${t##*/}; return; }; done; }
VP=$(pid_of x400vt); CP=$(pid_of capture); PP=$(pid_of plasmashell)
DP=""; for p in /proc/[0-9]*; do tr '\0' ' ' < $p/cmdline 2>/dev/null | grep -q 'x400-display' && { DP=${p#/proc/}; break; }; done
RING=$(tid_of $VP vkr-ring-1)
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
fbseq() { od -An -t u8 -j 32 -N 8 $FB | tr -d ' '; }
snap() { for p in $K $VP $CP $PP $DP $CL; do [ -n "$p" ] || continue; for t in /proc/$p/task/*; do
  echo "$p ${t##*/} $(cat $t/comm | tr ' ' _) $(sed 's/.*) //' $t/stat | awk '{print $12+$13}')"; done; done; }
sample_states() {
  end=$(( $(date +%s) + D )); n=0
  while :; do
    for x in "kwin $K $K" "ring $VP $RING" "capture $CP $CP"; do set -- $x
      [ -n "$3" ] || continue
      st=$(sed 's/.*) //' /proc/$2/task/$3/stat | cut -d' ' -f1); w=$(cat /proc/$2/task/$3/wchan 2>/dev/null)
      echo "$1 $st ${w:-0}"
    done
    n=$((n + 1)); [ $((n % 20)) = 0 ] && [ "$(date +%s)" -ge "$end" ] && break
  done >> /data/local/tmp/ls-states.tmp
}
phase() { # $1 name, $2 mode: idle | shm | zink
  NAME=$1; MODE=$2; CL=""
  if [ "$MODE" != idle ]; then
    cat > $R/tmp/load-inner.sh <<EOF
. /tmp/kwin.env
export PATH=/usr/bin:/bin XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-x400 HOME=/tmp
if [ "$MODE" = shm ]; then unset MESA_LOADER_DRIVER_OVERRIDE VK_INSTANCE_LAYERS; export GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1; fi
exec es2gears_wayland
EOF
    $BB nsenter -t $K -m $BB chroot $R /bin/sh /tmp/load-inner.sh > $R/tmp/gears.$MODE.out 2>&1 < /dev/null &
    CL=$!; sleep 4
    kill -0 $CL 2>/dev/null || { echo "== $NAME: the client died:"; tail -n 3 $R/tmp/gears.$MODE.out; return; }
  else sleep 2; fi
  rm -f /data/local/tmp/ls-states.tmp /data/local/tmp/ls-s0 /data/local/tmp/ls-s1
  snap > /data/local/tmp/ls-s0; q0=$(fbseq); t0=$(date +%s); r0=$(cat /proc/$K/task/$K/schedstat); v0=$(cat /proc/$VP/task/$RING/schedstat)
  sample_states
  q1=$(fbseq); t1=$(date +%s); r1=$(cat /proc/$K/task/$K/schedstat); v1=$(cat /proc/$VP/task/$RING/schedstat); snap > /data/local/tmp/ls-s1
  dt=$((t1 - t0)); echo; echo "=== $NAME: ${dt}s, capture seq $q0 -> $q1 = $(( (q1 - q0) * 100 / dt )) centi-fps"
  [ -n "$CL" ] && { echo "client fps (its own count):"; grep -i fps $R/tmp/gears.$MODE.out | tail -n 2; }
  echo "$r0 $r1" | awk -v dt=$dt '{ printf "kwin main: on CPU %.0f ms, waiting for a CPU %.0f ms, of %d ms\n", ($4-$1)/1e6, ($5-$2)/1e6, dt*1000 }'
  echo "$v0 $v1" | awk '{ printf "Venus ring thread: on CPU %.0f ms, waiting for a CPU %.0f ms\n", ($4-$1)/1e6, ($5-$2)/1e6 }'
  echo "ticks per thread over the phase (top 9, 1 tick = $((1000 / HZ)) ms):"
  awk 'NR==FNR { a[$1" "$2]=$4; next } { d=$4-a[$1" "$2]; if (d > 0) print d, $1, $2, $3 }' /data/local/tmp/ls-s0 /data/local/tmp/ls-s1 | sort -rn | head -n 9
  echo "states (samples, who, state, wchan):"
  sort /data/local/tmp/ls-states.tmp | uniq -c | sort -rn | head -n 10; rm -f /data/local/tmp/ls-states.tmp
  [ -n "$CL" ] && { kill $CL 2>/dev/null; sleep 1; kill -9 $CL 2>/dev/null; wait $CL 2>/dev/null; }
}
echo "kwin $K (tracer $(awk '/TracerPid/ {print $2}' /proc/$K/status)), x400vt $VP (ring tid $RING), capture $CP, plasmashell $PP, display $DP"
echo "cpu freq (kHz): $(for c in 0 1 2 3 4 5 6 7; do printf '%s ' "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)"; done)"
phase "idle" idle
phase "shm client (es2gears, llvmpipe)" shm
phase "zink client (es2gears, Zink on Venus)" zink
echo; echo "kwin alive: $(pidof kwin_wayland)"
