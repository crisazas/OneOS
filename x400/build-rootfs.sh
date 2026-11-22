#!/bin/sh
# Builds out/rootfs (Ubuntu 24.04 arm64) and out/rootfs.ext4 on the PC. Needs sudo and qemu-user-static.
set -eu
cd "$(dirname "$0")/.."
OUT=$PWD/out; R=$OUT/rootfs
TAR=$OUT/ubuntu-base-24.04.4-base-arm64.tar.gz
PKGS="${PKGS:-weston vkmark mesa-vulkan-drivers libvulkan1 vulkan-tools kmscube mesa-utils libegl1 libgles2 libgbm1 libdrm-tests udev dbus-x11 xwayland iproute2 iputils-ping ca-certificates}"
sudo rm -rf "$R"; sudo mkdir -p "$R"
sudo tar -xpf "$TAR" -C "$R"
sudo cp /usr/bin/qemu-aarch64-static "$R/usr/bin/"
sudo cp /etc/resolv.conf "$R/etc/resolv.conf"
umnt() { for m in dev sys proc; do mountpoint -q "$R/$m" && { sudo mount --make-rslave "$R/$m"; sudo umount -R "$R/$m"; }; done; return 0; }
for m in proc sys dev; do sudo mount --rbind /$m "$R/$m"; done
trap umnt EXIT
sudo chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive sh -c "apt-get update && apt-get install -y --no-install-recommends $PKGS && apt-get clean && rm -rf /var/lib/apt/lists/*"

umnt
sudo rm -f "$R/usr/bin/qemu-aarch64-static" "$R/etc/resolv.conf"
sudo truncate -s 2G "$OUT/rootfs.ext4"
sudo mkfs.ext4 -q -F -L linuxroot -d "$R" "$OUT/rootfs.ext4"
sudo chown "$(id -u):$(id -g)" "$OUT/rootfs.ext4"
