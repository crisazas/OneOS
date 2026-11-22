BB=/data/adb/ksu/bin/busybox
n=0
while [ $(grep -c x400root /proc/self/mountinfo) -gt 0 ] && [ $n -lt 400 ]; do
  n=$((n+1))
  line=$(grep x400root /proc/self/mountinfo | sort -n -r | head -1)
  path=$(echo "$line" | awk '{print $5}')
  $BB umount "$path" 2>/dev/null || { $BB umount -l "$path" 2>/dev/null || { echo "NO SE PUDO: $line" | cut -c1-200; break; }; }
done
echo "intentos=$n restantes=$(grep -c x400root /proc/self/mountinfo)"
for l in /sys/block/loop*/loop/backing_file; do read f < $l 2>/dev/null; case "$f" in *x400*) b=$(echo $l | sed 's#.*/\(loop[0-9]*\)/.*#\1#'); $BB losetup -d /dev/block/$b 2>/dev/null;; esac; done
c=0; for l in /sys/block/loop*/loop/backing_file; do read f < $l 2>/dev/null; case "$f" in *x400*) c=$((c+1));; esac; done
echo "loops con x400=$c  montajes totales init=$(wc -l < /proc/1/mountinfo)"
