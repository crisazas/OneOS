#!/bin/sh
# Builds out/rootfs-plasma.ext4 (Ubuntu 26.04.1 arm64 + KDE Plasma 6 + Mesa/Zink + Venus client). Needs sudo and qemu-user-static.
set -eu
cd "$(dirname "$0")/.."
OUT=$PWD/out; R=$OUT/rootfs-plasma
TAR=$OUT/ubuntu-base-26.04.1-base-arm64.tar.gz
IMG=$OUT/rootfs-plasma.ext4; SIZE=${SIZE:-6G}
PKGS="${PKGS:-plasma-desktop kwin-wayland dbus-x11 dbus-user-session mesa-vulkan-drivers libgl1-mesa-dri libegl1 libgles2 libgbm1 vulkan-tools mesa-utils libinput-tools xwayland konsole dolphin kate fonts-noto-core pipewire libpipewire-0.3-dev libwayland-dev plasma-wayland-protocols libwayland-bin kscreen iproute2 iputils-ping ca-certificates locales sudo vkmark}"
umnt() { for m in dev sys proc; do mountpoint -q "$R/$m" && { sudo mount --make-rslave "$R/$m"; sudo umount -R "$R/$m"; }; done; return 0; }
trap umnt EXIT
umnt; sudo rm -rf "$R"; sudo mkdir -p "$R"
sudo tar -xpf "$TAR" -C "$R"
sudo cp /usr/bin/qemu-aarch64-static "$R/usr/bin/"
sudo cp /etc/resolv.conf "$R/etc/resolv.conf"
for m in proc sys dev; do sudo mount --rbind /$m "$R/$m"; done
sudo chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive sh -c "apt-get update && apt-get install -y --no-install-recommends $PKGS && apt-get clean && rm -rf /var/lib/apt/lists/*"
umnt
sudo rm -f "$R/usr/bin/qemu-aarch64-static" "$R/etc/resolv.conf"
sudo rm -f "$IMG"; sudo truncate -s "$SIZE" "$IMG"
sudo mkfs.ext4 -q -F -L linuxroot -d "$R" "$IMG"
sudo chown "$(id -u):$(id -g)" "$IMG"
