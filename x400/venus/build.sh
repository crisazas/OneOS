#!/bin/sh
# Credit: the source list and flags are adapted from OneiOS's runtime/venus/build-host.sh (Akshay CM / akshaynexus, Musicalista and
# contributors, https://github.com/Musicalista/OneiOS-A56; no license line in that file, and OneiOS has no license file: see x400/CREDITS.md).
# Builds virgl_test_server --venus for Android arm64 (bionic) with the NDK, without GL: the fork's vtest server and
# src/virglrenderer.c over the Venus core (src/venus/vkr_*); vrend/drm/proxy are stubs (vrend_stub.c).
# Inputs (git-ignored): out/virgl = virglrenderer 7e17c8e (Droid-VM's fork, MIT upstream) + x400/venus/vkr-dma-heap.patch +
# x400/venus/vtest-x400.patch (both ours, MIT); out/venus-src = the stub/ headers (see x400/CREDITS.md).
set -eu
X=$(cd "$(dirname "$0")" && pwd)
V=${VIRGL_TREE:-$X/../../out/virgl}; S=$X/../../out/venus-src; OUT=${1:-$X/../../out/venus-build}
NDKROOT=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
CC=$NDKROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
mkdir -p "$OUT"
cd "$V"
SRCS="src/venus/vkr_*.c src/virgl_context.c src/virgl_resource.c src/virgl_util.c src/iov.c src/gallium/auxiliary/util/u_hash_table.c src/gallium/auxiliary/cso_cache/cso_hash.c src/virgl_fence.c src/mesa/util/anon_file.c src/mesa/util/bitscan.c src/mesa/util/hash_table.c src/mesa/util/os_file.c src/mesa/util/os_misc.c src/mesa/util/ralloc.c src/mesa/util/u_cpu_detect.c src/mesa/util/u_debug.c src/mesa/util/u_math.c src/virglrenderer.c vtest/vtest_server.c vtest/vtest_renderer.c vtest/vtest_shm.c vtest/util.c"
INC="-I$X/stub -I$S/stub -I$V/prebuilt-intermediates -I$V/prebuilt-intermediates/venus -I$V/src -I$V/src/venus -I$V/src/drm -I$V/src/drm/drm-uapi -I$V/src/mesa -I$V/src/mesa/compat -I$V/src/mesa/pipe -I$V/src/mesa/util -I$V/src/gallium/include -I$V/src/gallium/auxiliary -I$V/vtest"
CF="-O2 -std=gnu17 -DHAVE_CONFIG_H -include $V/prebuilt-intermediates/config.h -DENABLE_VENUS=1 -DHAVE_MEMFD_CREATE=1 -D__USE_GNU -Wno-everything -fvisibility=default"
$CC $CF $INC -o "$OUT/virgl_test_server" $SRCS $X/vrend_stub.c -llog -ldl -lm -lc -Wl,--error-limit=0
ls -l "$OUT/virgl_test_server"
