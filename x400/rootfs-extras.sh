#!/bin/sh
# Adds the packages every hardware group asks for (x400/PACKAGES and x400/*/PACKAGES, one name per line, # comments) to the existing
# out/rootfs-plasma tree and rebuilds out/rootfs-plasma.ext4 from it, without rebuilding the whole system (build-rootfs-plasma.sh does that).
# Needs sudo and qemu-user-static. Usage: x400/rootfs-extras.sh [--no-image]
set -eu
cd "$(dirname "$0")/.."
OUT=$PWD/out; R=$OUT/rootfs-plasma; IMG=$OUT/rootfs-plasma.ext4; SIZE=${SIZE:-6G}
# x400/NOT-IN-IMAGE lists names that a group asks for but that must not reach the guest (build-time only, or replaced by another design)
PKGS=$(cat x400/PACKAGES x400/*/PACKAGES 2>/dev/null | sed 's/#.*//' | tr -s ' \t\n' '\n' | grep -v '^$' | sort -u | grep -v -x -F -f x400/NOT-IN-IMAGE 2>/dev/null | tr '\n' ' ')
[ -n "$PKGS" ] || { echo "no packages listed"; exit 1; }
umnt() { for m in dev sys proc; do mountpoint -q "$R/$m" && { sudo mount --make-rslave "$R/$m"; sudo umount -R "$R/$m"; }; done; return 0; }
trap umnt EXIT
sudo cp /usr/bin/qemu-aarch64-static "$R/usr/bin/"; sudo cp /etc/resolv.conf "$R/etc/resolv.conf"
for m in proc sys dev; do mountpoint -q "$R/$m" || sudo mount --rbind /$m "$R/$m"; done
# nothing may start a daemon inside the chroot while installing (it shares the host's /proc, /sys and network)
printf '#!/bin/sh\nexit 101\n' | sudo tee "$R/usr/sbin/policy-rc.d" >/dev/null; sudo chmod 755 "$R/usr/sbin/policy-rc.d"
sudo chroot "$R" /usr/bin/env DEBIAN_FRONTEND=noninteractive sh -c "apt-get update -qq && apt-get install -y --no-install-recommends $PKGS && apt-get clean && rm -rf /var/lib/apt/lists/*"
sudo rm -f "$R/usr/sbin/policy-rc.d"
# config files the groups want inside the (read-only) image: x400/<group>/image-conf/<path under /etc/> and the keyboard's kwinrc
for d in x400/*/image-conf; do [ -d "$d" ] || continue
  sudo mkdir -p "$R/etc/wireplumber/wireplumber.conf.d"; sudo cp "$d"/*.conf "$R/etc/wireplumber/wireplumber.conf.d/"
done
[ -f x400/input/kwinrc-keyboard ] && { sudo mkdir -p "$R/etc/xdg"; grep -v '^#' x400/input/kwinrc-keyboard | sudo tee "$R/etc/xdg/kwinrc" >/dev/null; }
umnt
sudo rm -f "$R/usr/bin/qemu-aarch64-static" "$R/etc/resolv.conf"
echo "installed: $PKGS"
[ "${1:-}" = --no-image ] && exit 0
sudo rm -f "$IMG"; sudo truncate -s "$SIZE" "$IMG"
sudo mkfs.ext4 -q -F -L linuxroot -d "$R" "$IMG"
sudo chown "$(id -u):$(id -g)" "$IMG"
ls -l "$IMG"
