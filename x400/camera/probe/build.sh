#!/bin/sh
# Cross-compile the probes for the tablet (bionic, arm64).
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cd "$(dirname "$0")"
# camprobe: NDK camera2, read-only. API 29 is enough.
$BIN/aarch64-linux-android29-clang -O2 -Wall -o camprobe camprobe.c -lcamera2ndk -lmediandk
# fakesvc: libbinder_ndk services; ABinderProcess_* and AServiceManager_addService need API 29+.
$BIN/aarch64-linux-android31-clang -O2 -Wall -o fakesvc fakesvc.c -lbinder_ndk
echo built camprobe fakesvc
