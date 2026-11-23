#!/bin/sh
# Builds everything of G5 into x400/camera/stage/ (what x400/oneos/deploy.sh pushes to the tablet):
#   x400-camd, fakesvc   Android (bionic arm64, NDK 28.2), run on the host side by stage/host.sh
#   x400-camsrc, x400-camtest   guest (glibc arm64) against the PC's rootfs, like x400/display/capture (x400/README.md, Build)
# Usage: x400/camera/build.sh [android|guest]     (default: both)
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
ROOT=$(cd ../.. && pwd)
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
R=${R:-$ROOT/out/rootfs-plasma}
STAGE=$HERE/stage
mkdir -p "$STAGE"
what=${1:-all}

if [ "$what" = all ] || [ "$what" = android ]; then
	BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
	$BIN/aarch64-linux-android30-clang -O2 -Wall -o "$STAGE/x400-camd" camd.c -lcamera2ndk -lmediandk -llog -lm
	$BIN/aarch64-linux-android31-clang -O2 -Wall -o "$STAGE/fakesvc" probe/fakesvc.c -lbinder_ndk
	$BIN/aarch64-linux-android30-clang++ -O2 -Wall -static-libstdc++ -o "$STAGE/sensormgr" probe/sensormgr.cpp -ldl
	echo "built x400-camd fakesvc sensormgr"
fi

if [ "$what" = all ] || [ "$what" = guest ]; then
	CFLAGS="-O2 -Wall --sysroot=$R -I$R/usr/include -I$R/usr/include/aarch64-linux-gnu -I$R/usr/include/pipewire-0.3 -I$R/usr/include/spa-0.2"
	LDFLAGS="-L$R/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$R/usr/lib/aarch64-linux-gnu -Wl,--unresolved-symbols=ignore-in-shared-libs"
	aarch64-linux-gnu-gcc $CFLAGS camsrc.c -o "$STAGE/x400-camsrc" $LDFLAGS -lpipewire-0.3
	aarch64-linux-gnu-gcc $CFLAGS camtest.c -o "$STAGE/x400-camtest" $LDFLAGS -lpipewire-0.3
	echo "built x400-camsrc x400-camtest"
fi
