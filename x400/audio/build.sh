#!/bin/sh
# Static arm64 build of x400-snd (runs on the Android host and in the guest).
set -e
cd "$(dirname "$0")"
${CC:-aarch64-linux-gnu-gcc} -O2 -Wall -static x400-snd.c -o x400-snd -lm
