#!/bin/sh
# Builds inputd (x400/display/bin/inputd, where deploy.sh picks it up) and keys, pentest (out/input/) on the PC against out/rootfs-plasma.
set -eu
T=$(cd "$(dirname "$0")/../.." && pwd); R=$T/out/rootfs-plasma; O=$T/out/input; mkdir -p "$O" "$T/x400/display/bin"
WS="qemu-aarch64-static -L $R $R/usr/bin/wayland-scanner"; P=$R/usr/share/plasma-wayland-protocols
$WS client-header $P/fake-input.xml "$O/fake-input-client-protocol.h"; $WS private-code $P/fake-input.xml "$O/fake-input-protocol.c"
aarch64-linux-gnu-gcc -O2 -Wall --sysroot=$R -I"$O" -I$R/usr/include -I$R/usr/include/aarch64-linux-gnu "$T/x400/display/inputd.c" "$O/fake-input-protocol.c" \
  -o "$T/x400/display/bin/inputd" -L$R/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$R/usr/lib/aarch64-linux-gnu -lwayland-client -lm
for x in keys pentest; do aarch64-linux-gnu-gcc -O2 -Wall --sysroot=$R "$T/x400/input/$x.c" -o "$O/$x"; done
ls -l "$T/x400/display/bin/inputd" "$O/keys" "$O/pentest"
