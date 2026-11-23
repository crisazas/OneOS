#!/bin/sh
# Runs in the guest as root, in the foreground, at the start of session-inner.sh (nothing else may start before it ends).
# Makes the user the desktop runs as (X400_USER, default x400, uid 12000) and exits 0; any other exit code makes session-inner.sh run as root, as before.
# Idempotent: the root is persistent, so what an earlier session made is kept. It needs a writable root (the persistent overlay), not the read-only image.
U=${X400_USER:-x400}
[ "$U" = root ] && exit 1
H=/home/$U
touch /etc/.x400-writable 2> /dev/null && rm -f /etc/.x400-writable || { echo "user: the root is read-only, no user made"; exit 1; }
command -v setpriv > /dev/null || { echo "user: no setpriv"; exit 1; }
# The kernel is Android's, so ids mean what they mean there: uid 1000 is "system", which is not what a desktop user should be, so the user is 12000 (an unused app uid);
# 3003 (inet) is the group Android's kernel asks for to open network sockets, 1023 (media_rw) the one One UI's files carry (/mnt/oneui). The guest needs the numbers, the names are for ls.
for g in "x400 12000" "inet 3003" "media_rw 1023"; do
    set -- $g; getent group "$2" > /dev/null || groupadd -g "$2" "$1"
done
if ! id "$U" > /dev/null 2>&1; then
    useradd -m -u 12000 -g 12000 -s /bin/bash -G sudo,audio,video,render,plugdev,netdev,bluetooth,3003,1023 "$U" || { echo "user: useradd failed"; exit 1; }
fi
# kwin_wayland carries a file capability (CAP_SYS_NICE): for a user that is not root, glibc then runs it in secure-execution mode and ignores LD_LIBRARY_PATH and LD_PRELOAD,
# so KWin would not find the Mesa 25 for Zink (/tmp/mesa25) nor the fd-leak shim. The capability is dropped from the copy in the persistent layer (the image is untouched);
# KWin only used it to ask for real-time scheduling, which fails in this guest anyway.
for b in /usr/bin/kwin_wayland; do
    python3 -c 'import os,sys; os.removexattr(sys.argv[1], "security.capability")' "$b" 2> /dev/null
done
# The lock screen's greeter is a Qt Quick app and, like falkon and haruna, cannot import Zink's dma-bufs (the Wayland connection dies with "importing the supplied dmabufs
# failed", kscreenlocker goes to its emergency mode and the screen stays black): it draws with llvmpipe. The real binary stays beside the wrapper, in the persistent layer.
G=/usr/lib/aarch64-linux-gnu/libexec/kscreenlocker_greet
if [ -x "$G" ] && [ ! -e "$G.real" ]; then
    mv "$G" "$G.real" && printf '#!/bin/sh\nexport MESA_LOADER_DRIVER_OVERRIDE=llvmpipe GALLIUM_DRIVER=llvmpipe\nexec %s.real "$@"\n' "$G" > "$G" && chmod 755 "$G"
fi
# sudo without a password: root is one command away, it is just not what everything runs as
printf '%s ALL=(ALL) NOPASSWD:ALL\n' "$U" > /etc/sudoers.d/x400-user && chmod 440 /etc/sudoers.d/x400-user
# sudo looks its own host name up; this guest's (Android's) is not in /etc/hosts
h=$(hostname 2> /dev/null); [ -n "$h" ] && ! grep -q -w "$h" /etc/hosts 2> /dev/null && echo "127.0.0.1 $h" >> /etc/hosts
# Once: what the desktop kept as root (Plasma's settings, Dolphin's places, the shortcuts) is copied to the user. Nothing is moved or removed from /root.
if [ ! -e "$H/.x400-migrated" ]; then
    for d in .config .local/share; do
        [ -d "/root/$d" ] || continue
        mkdir -p "$H/$d" && cp -a "/root/$d/." "$H/$d/"
    done
    [ -d /root/Desktop ] && mkdir -p "$H/Desktop" && cp -a /root/Desktop/. "$H/Desktop/"
    [ -f "$H/.local/share/user-places.xbel" ] && sed -i "s#file:///root#file://$H#g" "$H/.local/share/user-places.xbel"
    : > "$H/.x400-migrated"
fi
# The user's password (the lock screen asks for it; sudo stays without one). Only its hash travels, in an untracked file (x400/user/stage/password.hash, made by
# `openssl passwd -6`); without the file the account stays locked for passwords, as before.
[ -s /tmp/x400/user/password.hash ] && usermod -p "$(cat /tmp/x400/user/password.hash)" "$U" && echo "user: password set"
# Lock screen defaults (Settings > Screen Locking, kept in the user's own ~/.config, overrides these): no automatic lock, so that a locked tablet is always the user's doing
mkdir -p /etc/xdg && printf '[Daemon]\nAutolock=false\nLockOnResume=false\n' > /etc/xdg/kscreenlockerrc
chown -R "$U:$U" "$H"
echo "user: $U ($(id -u "$U")) ready, home $H"
