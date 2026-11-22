#!/bin/sh
# Makes out/virgl: Droid-VM's virglrenderer at 7e17c8e plus this branch's two patches (x400/venus/*.patch).
# Needs a checkout of that fork (VIRGL_SRC, default ~/virglrenderer or a path you give). No patch from other projects is used.
set -eu
X=$(cd "$(dirname "$0")" && pwd); OUT=${1:-$X/../../out/virgl}
SRC=${VIRGL_SRC:-$HOME/virglrenderer}
[ -d "$SRC/.git" ] || [ -f "$SRC/.git" ] || { echo "VIRGL_SRC=$SRC is not a git checkout of https://github.com/Droid-VM/virglrenderer" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
git -C "$SRC" archive 7e17c8e | tar -x -C "$OUT"
cd "$OUT"
patch -p1 -s < "$X/vkr-dma-heap.patch"
patch -p1 -s < "$X/vtest-x400.patch"
echo "prepared $OUT"
