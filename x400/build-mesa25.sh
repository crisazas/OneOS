#!/bin/sh
# linux-x400: adds Mesa 25.0.7 from Debian 13 (trixie) to the Plasma rootfs under /opt/mesa25, next to Ubuntu's own Mesa 26.
# Why: Zink needs VK_EXT_robustness2 nullDescriptor from Mesa 25.2.0 on (commit 0a2c3953, "zink: require nullDescriptor feature",
# 2025-06-26), and Samsung's Vulkan driver (the host side of Venus) does not offer it. 25.0.7 does not ask for it.
# Packages are the ones Debian ships, checked against the SHA256 of its Packages index. Output: out/mesa25/root (no root needed, the
# Plasma image is not touched: it is read-only on the tablet). x400/oneos/deploy.sh puts it in the stage; guest_start.sh binds it at
# /tmp/mesa25 in the guest, and session-inner.sh picks it with LD_LIBRARY_PATH and the loader variables. Ubuntu's Mesa stays as it is.
set -eu
cd "$(dirname "$0")/.."
OUT=$PWD/out/mesa25; DEST=$OUT/root
MIRROR=https://deb.debian.org/debian
PKGS="mesa-libgallium libllvm19 libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 libz3-4 libxml2 libicu76"
mkdir -p "$OUT"; cd "$OUT"
[ -s Packages ] || curl -fsSL "$MIRROR/dists/trixie/main/binary-arm64/Packages.xz" | xz -dc > Packages
python3 - "$MIRROR" $PKGS <<'PY'
import sys,hashlib,os,urllib.request
mirror=sys.argv[1]; want=sys.argv[2:]
pk={}
for blk in open('Packages').read().split('\n\n'):
    d={}
    for l in blk.split('\n'):
        if ': ' in l and not l.startswith(' '):
            k,v=l.split(': ',1); d[k]=v
    if 'Package' in d: pk[d['Package']]=d
for n in want:
    d=pk[n]; f=d['Filename'].split('/')[-1]
    if not os.path.exists(f): urllib.request.urlretrieve(mirror+'/'+d['Filename'],f)
    if hashlib.sha256(open(f,'rb').read()).hexdigest()!=d['SHA256']: sys.exit('bad sha256: '+f)
    print('ok',f)
PY
rm -rf "$DEST"; mkdir -p "$DEST"
for d in *.deb; do dpkg-deb -x "$d" "$DEST"; done
du -sh "$DEST" | cut -f1
