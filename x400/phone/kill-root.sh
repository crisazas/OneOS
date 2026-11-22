# mata solo procesos cuya raiz es x400root (el chroot comparte el espacio de PIDs con Android)
for sig in TERM KILL; do
  for p in /proc/[0-9]*; do r=$(readlink $p/root 2>/dev/null); case "$r" in *x400root*) kill -$sig ${p#/proc/} 2>/dev/null;; esac; done
  sleep 2
done
n=0; for p in /proc/[0-9]*; do r=$(readlink $p/root 2>/dev/null); case "$r" in *x400root*) n=$((n+1));; esac; done
echo "procesos x400root restantes: $n; MemAvailable $(grep MemAvailable /proc/meminfo | awk '{print int($2/1024)}') MiB"
