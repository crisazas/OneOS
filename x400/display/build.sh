#!/bin/sh
# Builds out/display/{x400-display.dex,libx400display.so} (NDK 28.2, javac, d8). Output stays out of git.
set -eu
D=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$D/../../out/display}; mkdir -p "$OUT"
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
D8=$(ls -d $HOME/Android/Sdk/build-tools/*/d8 | tail -1)
$CC -shared -fPIC -O2 -Wall -o "$OUT/libx400display.so" "$D/jni.c" -landroid -lnativewindow -llog
rm -rf "$OUT/cls"; mkdir -p "$OUT/cls"
javac --release 8 -d "$OUT/cls" "$D/X400Display.java" 2>&1 | grep -v '^warning\|^Note' || true
"$D8" --output "$OUT" "$OUT"/cls/*.class
mv "$OUT/classes.dex" "$OUT/x400-display.dex"
ls -l "$OUT"/x400-display.dex "$OUT"/libx400display.so
