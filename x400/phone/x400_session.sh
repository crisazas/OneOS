#!/system/bin/sh
# linux-x400: inside `busybox unshare -m` (started by x400_switch.sh). Mounts the image and the helpers, starts the Venus
# server outside the chroot, runs session-inner.sh inside it. Usage: x400_session.sh STAGE SECONDS [COMMAND]
STAGE=$1; SECS=$2; INNER_CMD=$3
BB=/data/adb/ksu/bin/busybox
R=$STAGE/root; S=$STAGE/session
mkdir -p "$R"
"$BB" mount --make-rprivate / || exit 1
"$BB" mount -o loop,ro -t ext4 "$STAGE/rootfs.ext4" "$R" || exit 1
"$BB" mount -t tmpfs tmpfs "$R/tmp"; "$BB" mount -t tmpfs tmpfs "$R/run"
"$BB" mount --bind /dev "$R/dev"; "$BB" mount -t proc proc "$R/proc"; "$BB" mount -t sysfs sysfs "$R/sys"
mkdir -p "$R/tmp/fb"; "$BB" mount --bind "$S/fb" "$R/tmp/fb"
for f in capture inputd session-inner.sh; do : > "$R/tmp/$f"; "$BB" mount --bind "$STAGE/bin/$f" "$R/tmp/$f"; done
VKR_DMA_HEAP_NODE=/dev/dma_heap/system ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so VIRGL_LOG_LEVEL=info \
  "$STAGE/bin/x400vt" --venus --no-fork --socket-path="$R/tmp/.virgl_test" > "$STAGE/logs/venus.log" 2>&1 &
echo $! > "$S/venus.pid"; sleep 2
LIM=; [ "$SECS" -gt 0 ] && LIM="$BB timeout $((SECS + 30))"
$LIM "$BB" chroot "$R" /usr/bin/env -i PATH=/usr/bin:/bin RUNSECS="$SECS" GPUTEST="$INNER_CMD" /usr/bin/dbus-run-session -- sh /tmp/session-inner.sh
