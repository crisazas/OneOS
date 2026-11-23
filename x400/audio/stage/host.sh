#!/bin/sh
# Audio group, host side. guest_start.sh runs it (outside the chroot, as root) at session start, after the guest's
# /tmp is mounted; it stays alive as a small keeper until the guest is unmounted.
#  - OneOS stops Android's audioserver while /data/local/tmp/oneos/data/guest_audio.flag exists (the loop in
#    oneos_switch.sh also suppresses zygote and surfaceflinger, so it must NOT be killed): remove the flag instead.
#    Cleaner: start oneos_switch.sh with ONEOS_GUEST_AUDIO=0, which never creates it (see RESULT.md).
#  - start audioserver (Samsung's HAL is already running) and x400-audiod, which owns the speaker/microphone and
#    serves /tmp/x400-audio-{out,in}.sock of the guest. X400_MAXAMP=<percent> caps the speaker level in audiod.
G=$(cd "$(dirname "$0")" && pwd)
STAGE=/data/local/tmp/oneos
R=$STAGE/rootfs
FLAG=$STAGE/data/guest_audio.flag
LOG=$STAGE/logs
mkdir -p "$LOG" 2>/dev/null
say() { echo "$(date +%T) host.sh: $*"; }

ensure_audioserver() {
    [ -f "$FLAG" ] && { rm -f "$FLAG"; say "removed guest_audio.flag"; }
    if [ "$(getprop init.svc.audioserver)" != running ]; then
        setprop ctl.start audioserver
        n=0
        while [ $n -lt 50 ] && ! service check media.audio_policy 2>/dev/null | grep -q ": found"; do n=$((n + 1)); sleep 0.2; done
        say "audioserver: $(getprop init.svc.audioserver), media.audio_policy $(service check media.audio_policy 2>/dev/null | cut -d: -f2)"
    fi
}

# the guest's /tmp: $R/tmp in guest_start.sh's mount namespace; from any other one it is the read-only mount point
sockdir() {
    if touch "$R/tmp/.x400-audio-w" 2>/dev/null; then rm -f "$R/tmp/.x400-audio-w"; echo "$R/tmp"; return; fi
    k=$(pidof kwin_wayland); [ -n "$k" ] && echo "/proc/$k/root/tmp"
}

start_audiod() {
    pidof x400-audiod >/dev/null && return
    d=$(sockdir); [ -n "$d" ] || { say "no writable guest /tmp yet"; return; }
    "$G/x400-audiod" --dir "$d" --log "$LOG/x400-audiod.log" >/dev/null 2>&1 &
    say "x400-audiod started ($!), log $LOG/x400-audiod.log"
}

ensure_audioserver
start_audiod
while grep -q " $R " /proc/mounts 2>/dev/null; do
    sleep 2
    ensure_audioserver
    pidof x400-audiod >/dev/null || start_audiod
done
say "guest unmounted, stopping x400-audiod"
pkill x400-audiod
