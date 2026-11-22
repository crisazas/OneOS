# linux-x400: CPU used by each session process over N seconds (inside the chroot, so /proc is the host's). Usage: cpu-sample.sh N
N=${1:-10}
snap() { for p in /proc/[0-9]*; do c=$(cat $p/comm 2>/dev/null); case "$c" in kwin_wayland|plasmashell|capture|pipewire|wireplumber|inputd|kded6|konsole|vkmark|glxgears|llvmpipe*|llvmpipe-*) echo "$c ${p#/proc/} $(awk '{print $14+$15}' $p/stat 2>/dev/null)";; esac; done; }
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
snap > /tmp/cpu0; sleep "$N"; snap > /tmp/cpu1
awk -v n="$N" -v hz="$HZ" 'NR==FNR { a[$2]=$3; next } ($2 in a) { d=($3-a[$2]); printf "%-14s pid %-6s %6.1f %% of one core\n", $1, $2, d*100/hz/n }' /tmp/cpu0 /tmp/cpu1
echo "== where the processes may run"
for c in kwin_wayland plasmashell konsole capture; do
  for p in /proc/[0-9]*; do
    [ "$(cat $p/comm 2>/dev/null)" = "$c" ] || continue
    echo "$c ${p#/proc/}: cpus_allowed $(awk '/Cpus_allowed_list/ {print $2}' $p/status) cpuset $(cat $p/cpuset 2>/dev/null) oom $(cat $p/oom_score_adj 2>/dev/null) threads $(ls $p/task | wc -l)"
    break
  done
done
echo "== cpu frequencies (kHz): $(for c in 0 1 2 3 4 5 6 7; do printf '%s ' "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)"; done)"
echo "== governor cpu0/cpu4/cpu7: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) $(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null) $(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor 2>/dev/null)"
echo "== /dev/pts in the chroot: $(ls /dev/pts 2>&1 | head -n 3 | tr '\n' ' ') | ptmx: $(ls -l /dev/ptmx 2>&1 | cut -c1-40)"
echo "== thread CPU of kwin_wayland (top 6 by ticks)"
for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = kwin_wayland ] && for t in $p/task/*; do echo "$(awk '{print $14+$15}' $t/stat) $(cat $t/comm)"; done; done | sort -rn | head -n 6
