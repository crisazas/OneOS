BB=/data/adb/ksu/bin/busybox; ENVX=${ENVX:-env}
T=/data/local/tmp; R=$T/x400root; mkdir -p $R
$BB mount --make-rprivate / || exit 1
$BB mount -o loop,ro -t ext4 $T/x400-rootfs.ext4 $R || exit 1
$BB mount -t tmpfs tmpfs $R/tmp; $BB mount -t tmpfs tmpfs $R/run
$BB mount --bind /dev $R/dev; $BB mount -t proc proc $R/proc; $BB mount -t sysfs sysfs $R/sys
VKR_DMA_HEAP_NODE=${HEAP:-/dev/dma_heap/system} ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so X400_VTEST_TRACE=1 VIRGL_LOG_LEVEL=info $T/x400vt --venus --no-fork --socket-path=$R/tmp/.virgl_test > $T/x400vt.log 2>&1 &
SP=$!; sleep 2
touch $R/tmp/vk-offscreen; $BB mount --bind $T/vk-offscreen $R/tmp/vk-offscreen
$BB timeout 40 $BB chroot $R /usr/bin/env PATH=/usr/bin:/bin VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json VN_DEBUG=all VN_PERF=no_fence_feedback HOME=/tmp XDG_RUNTIME_DIR=/run $ENVX /tmp/vk-offscreen ${NF:-1} > $T/x400cli.log 2>&1
echo "rc=$?"; grep -v -E 'hit|miss|skip|cache' $T/x400cli.log | tail -n 14 | cut -c1-170
kill $SP 2>/dev/null; sleep 1; kill -9 $SP 2>/dev/null
