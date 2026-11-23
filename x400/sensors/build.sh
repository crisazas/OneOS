#!/bin/sh
# Builds x400-sensord against out/rootfs-plasma and fills stage/ (what deploy.sh pushes). libdbus-1-dev's headers are taken from the .deb
# on the PC (nothing is installed in the rootfs: the guest only needs libdbus-1.so.3, already there).
set -eu
T=$(cd "$(dirname "$0")/../.." && pwd); R=$T/out/rootfs-plasma; S=$T/x400/sensors; W=$T/out/sensors-build
mkdir -p "$W" "$S/stage"
[ -f "$W/root/usr/include/dbus-1.0/dbus/dbus.h" ] || {
  [ -f "$W"/libdbus-1-dev_*.deb ] || { echo "put libdbus-1-dev_*_arm64.deb in $W (apt-get download libdbus-1-dev inside the rootfs chroot)"; exit 1; }
  dpkg-deb -x "$W"/libdbus-1-dev_*.deb "$W/root"; }
D=$W/root/usr
aarch64-linux-gnu-gcc -O2 -Wall --sysroot="$R" -I"$D/include/dbus-1.0" -I"$D/lib/aarch64-linux-gnu/dbus-1.0/include" "$S/x400-sensord.c" -o "$S/stage/x400-sensord" \
  -L"$R/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$R/usr/lib/aarch64-linux-gnu" -l:libdbus-1.so.3 -lm
cp "$S/x400-brightness" "$S/x400-rotate-hook" "$S/system-bus.conf" "$S/stage/"
chmod 755 "$S"/stage/x400-*
ls -l "$S/stage"
