#!/bin/sh
# Bionic builds for the Android host (NDK). Output goes to ./out-android/ (not versioned).
set -e
cd "$(dirname "$0")"
NDK=${NDK:-/home/console/Android/Sdk/ndk/28.2.13676358}
CC=$(echo "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android34-clang)
mkdir -p out-android
for t in x400-aaudio; do
    "$CC" -O2 -Wall "$t.c" -o "out-android/$t" -laaudio -lm
done
