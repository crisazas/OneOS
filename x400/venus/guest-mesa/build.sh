#!/bin/sh
# linux-x400: builds the guest's libvulkan_virtio.so (Mesa 26.0.8, Ubuntu 26.04's own version) with the dma-buf import added.
# Needs sudo, qemu-user-static and the PC's network. Work dir: out/mesabuild (an Ubuntu 26.04 arm64 chroot under qemu).
# Output: out/mesa-guest/libvulkan_virtio.so, which the session binds over the rootfs's one (x400/oneos/guest_start.sh).
set -eu
cd "$(dirname "$0")/../../.."; ROOT=$PWD; W=$ROOT/out/mesabuild; P=$ROOT/x400/venus/guest-mesa/vtest-import-fd.patch; P2=$ROOT/x400/venus/guest-mesa/vtest-sync-pool.patch
P3=$ROOT/x400/venus/guest-mesa/vtest-image-modifier.patch
if [ ! -d "$W/usr" ]; then
  mkdir -p "$W"; sudo tar -xzf "$ROOT/out/ubuntu-base-26.04.1-base-arm64.tar.gz" -C "$W"
  sudo cp /usr/bin/qemu-aarch64-static "$W/usr/bin/"; sudo cp /etc/resolv.conf "$W/etc/resolv.conf"
  sudo sed -i 's/^Types: deb$/Types: deb deb-src/' "$W/etc/apt/sources.list.d/ubuntu.sources"
fi
for m in proc sys dev; do mountpoint -q "$W/$m" || sudo mount --bind /$m "$W/$m"; done
sudo chroot "$W" /bin/bash -c 'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq; apt-get install -y -qq dpkg-dev meson ninja-build gcc g++ pkg-config python3-mako python3-yaml python3-packaging python3-ply flex bison glslang-tools libdrm-dev libwayland-dev wayland-protocols libexpat1-dev zlib1g-dev libzstd-dev libx11-dev libxcb1-dev libvulkan-dev libffi-dev libelf-dev libx11-xcb-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev libxshmfence-dev libxcb-xfixes0-dev libxcb-randr0-dev libxcb-shm0-dev'
# tar inside qemu fails on a syscall it lacks: the source is unpacked here, on the host
sudo sh -c "cd $W/root && [ -d mesa-26.0.8 ] || { chroot $W /bin/bash -c 'cd /root && apt-get source --download-only mesa=26.0.8-1ubuntu0.3'; tar -xf mesa_26.0.8.orig.tar.xz; patch -d mesa-26.0.8 -p1 < $P; patch -d mesa-26.0.8 -p1 < $P2; }"
sudo chroot "$W" /bin/bash -c 'cd /root/mesa-26.0.8 && { [ -d /root/b ] || meson setup /root/b -Dvulkan-drivers=virtio -Dgallium-drivers= -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dgbm=disabled -Dplatforms=wayland -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release -Dvulkan-layers= -Dtools= -Dlmsensors=disabled; } && ninja -C /root/b src/virtio/vulkan/libvulkan_virtio.so'
# vtest-image-modifier.patch (vn_image.c): exportable OPTIMAL images are made with the LINEAR DRM modifier, the only way Samsung's Mali takes them. Applied to an
# already unpacked tree too, once (the dry run with -R tells if it is in).
sudo sh -c "cd $W/root/mesa-26.0.8 && { patch -p1 -R --dry-run -s < $P3 > /dev/null 2>&1 && echo 'vtest-image-modifier.patch: already applied' || patch -p1 < $P3; }"
sudo chroot "$W" /bin/bash -c 'ninja -C /root/b src/virtio/vulkan/libvulkan_virtio.so'
mkdir -p "$ROOT/out/mesa-guest"; sudo cp "$W/root/b/src/virtio/vulkan/libvulkan_virtio.so" "$ROOT/out/mesa-guest/"; sudo chown "$(id -u):$(id -g)" "$ROOT/out/mesa-guest/libvulkan_virtio.so"
ls -l "$ROOT/out/mesa-guest/libvulkan_virtio.so"
