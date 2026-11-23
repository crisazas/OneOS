# linux-x400: where kwin_wayland's main thread waits, read from /proc only (no ptrace; gdb may be the tracer). Run as root on the host.
# Usage: wait-sample.sh [SECONDS] [INTERVAL]   (defaults 10 and 0.01)
N=${1:-10}; IV=${2:-0.01}
for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = kwin_wayland ] && { P=${p#/proc/}; break; }; done
[ -n "$P" ] || { echo "no kwin_wayland"; exit 1; }
T=/proc/$P/task/$P
echo "kwin_wayland pid $P, threads $(ls /proc/$P/task | wc -l), TracerPid $(awk '/TracerPid/ {print $2}' /proc/$P/status), cpus_allowed $(awk '/Cpus_allowed_list/ {print $2}' $T/status)"
echo "main thread schedstat before (run_ns wait_ns slices): $(cat $T/schedstat)"
echo "ctxt before: $(grep ctxt $T/status | tr '\n' ' ')"
S0=$(awk '{print $14+$15}' $T/stat)
OUT=/data/local/tmp/wait-sample.$$
i=0; n=$(( N * 200 ))
while [ $i -lt $n ]; do
  read -r l < $T/stat 2>/dev/null; l=${l#*) }; st=${l%% *}
  read -r w < $T/wchan 2>/dev/null
  read -r sc a0 rest < $T/syscall 2>/dev/null
  echo "$st ${w:-0} sys=$sc fd=$a0"
  i=$((i + 1)); sleep 0.005
done > $OUT
echo "main thread schedstat after: $(cat $T/schedstat)"
echo "ctxt after: $(grep ctxt $T/status | tr '\n' ' ')"
echo "main thread ticks in ${N}s: $(( $(awk '{print $14+$15}' $T/stat) - S0 ))"
echo "== samples: $(wc -l < $OUT); state + wchan histogram"
sort $OUT | uniq -c | sort -rn | head -n 12
echo "== fds the main thread blocks on (read=63 recvmsg=212 sendmsg=211 write=64 ioctl=29)"
for f in $(grep -E "sys=(63|64|29|211|212) " $OUT | awk '{print $4}' | sort | uniq -c | sort -rn | head -n 5 | awk '{print $1":"$2}'); do c=${f%%:*}; fd=${f#*fd=}; fd=$((fd)); echo "$c samples fd $fd -> $(readlink /proc/$P/fd/$fd)"; done
echo "== kernel stack of the main thread, 5 looks"
for k in 1 2 3 4 5; do echo "-- $(awk '{print $3}' $T/stat) $(cat $T/syscall 2>/dev/null | cut -d' ' -f1)"; cat $T/stack 2>/dev/null | head -n 6; sleep 0.2; done
echo "== all threads: ticks and state now (top 8)"
for t in /proc/$P/task/*; do echo "$(awk '{print $14+$15}' $t/stat) $(cat $t/comm) $(awk '{print $3}' $t/stat) $(cat $t/wchan)"; done | sort -rn | head -n 8
rm -f $OUT
