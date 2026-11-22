# linux-x400: runs on the tablet as root, inside `busybox unshare -m` (run-plasma.sh). Usage: sh plasma.sh [seconds]
BB=/data/adb/ksu/bin/busybox
T=/data/local/tmp; R=$T/x400root; mkdir -p $R $T/fbdir
$BB mount --make-rprivate / || exit 1
$BB mount -o loop,ro -t ext4 $T/x400-plasma.ext4 $R || exit 1
$BB mount -t tmpfs tmpfs $R/tmp; $BB mount -t tmpfs tmpfs $R/run
$BB mount --bind /dev $R/dev; $BB mount -t proc proc $R/proc; $BB mount -t sysfs sysfs $R/sys
mkdir -p $R/tmp/fb; $BB mount --bind $T/fbdir $R/tmp/fb
touch $R/tmp/capture; $BB mount --bind $T/capture $R/tmp/capture
touch $R/tmp/inputd; $BB mount --bind $T/inputd $R/tmp/inputd
touch $R/tmp/session-inner.sh; $BB mount --bind $T/session-inner.sh $R/tmp/session-inner.sh
VKR_DMA_HEAP=/dev/dma_heap/system ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so VIRGL_LOG_LEVEL=info $T/x400vt --venus --no-fork --socket-path=$R/tmp/.virgl_test > $T/x400vt.log 2>&1 &
SP=$!; sleep 2
$BB timeout $((${1:-60} + 60)) $BB chroot $R /usr/bin/env -i PATH=/usr/bin:/bin RUNSECS=${1:-60} GPUTEST="$GPUTEST" /usr/bin/dbus-run-session -- sh /tmp/session-inner.sh 2>&1 | cut -c1-200
# the chroot shares the PID namespace: kill every process whose root is the chroot, then the Venus server
for sig in TERM KILL; do
  for p in /proc/[0-9]*; do r=$(readlink $p/root 2>/dev/null); [ "$r" = "$R" ] && kill -$sig ${p#/proc/} 2>/dev/null; done
  sleep 2
done
kill $SP 2>/dev/null; sleep 1; kill -9 $SP 2>/dev/null
