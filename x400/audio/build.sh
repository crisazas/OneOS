#!/bin/sh
# Builds everything of the audio group that runs on the PC-built guest side, plus the static x400-snd.
#   x400-snd, x400-feed  static arm64 (host and guest)
#   x400-pwbridge  against the rootfs sysroot (libpipewire 1.6.2), as x400/README.md "Build"
# Android-side binaries (x400-audiod, x400-aaudio, x400-accel): build-android.sh
set -e
cd "$(dirname "$0")"
R=${R:-../../out/rootfs-plasma}
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall -static x400-snd.c -o x400-snd -lm
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall -static x400-feed.c -o x400-feed -lm
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall --sysroot="$R" -I"$R/usr/include" -I"$R/usr/include/aarch64-linux-gnu" \
    -I"$R/usr/include/pipewire-0.3" -I"$R/usr/include/spa-0.2" x400-pwbridge.c -o x400-pwbridge \
    -L"$R/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$R/usr/lib/aarch64-linux-gnu" \
    -Wl,--unresolved-symbols=ignore-in-shared-libs -lpipewire-0.3
