#!/bin/sh
# Builds everything the audio group ships and fills stage/ (x400/oneos/deploy.sh pushes stage/ to groups/audio/).
#   guest side (static or against the rootfs sysroot): x400-snd, x400-feed, x400-pwbridge (libpipewire 1.6.2, x400/README.md "Build")
#   Android side (NDK, build-android.sh): x400-audiod, x400-setvol, x400-aaudio, x400-accel
# stage/host.sh and stage/guest.sh are sources, not outputs.
set -e
cd "$(dirname "$0")"
R=${R:-../../out/rootfs-plasma}
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall -static x400-snd.c -o x400-snd -lm
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall -static x400-feed.c -o x400-feed -lm
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall --sysroot="$R" -I"$R/usr/include" -I"$R/usr/include/aarch64-linux-gnu" \
    -I"$R/usr/include/pipewire-0.3" -I"$R/usr/include/spa-0.2" x400-pwbridge.c -o x400-pwbridge \
    -L"$R/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$R/usr/lib/aarch64-linux-gnu" \
    -Wl,--unresolved-symbols=ignore-in-shared-libs -lpipewire-0.3
./build-android.sh
mkdir -p stage
cp x400-snd x400-feed x400-pwbridge stage/
cp out-android/x400-audiod out-android/x400-setvol out-android/x400-aaudio stage/
