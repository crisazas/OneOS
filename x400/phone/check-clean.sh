echo "== montajes x400 (namespace global)"; grep -c x400 /proc/mounts
echo "== loops con x400"; n=0; for l in /sys/block/loop*/loop/backing_file; do read f < $l 2>/dev/null; case "$f" in *x400*) n=$((n+1));; esac; done; echo $n
echo "== procesos"; for p in /proc/[0-9]*; do c=$(cat $p/comm 2>/dev/null); case "$c" in x400vt|vulkaninfo|vkr-ring*) echo "$p $c";; esac; done
echo "== dmesg mali (400 lineas)"; dmesg | tail -n 400 | grep -c -i -E 'mali.*(reset|timeout|fault)|kbase.*(reset|hard)'
grep -E 'MemAvailable' /proc/meminfo
