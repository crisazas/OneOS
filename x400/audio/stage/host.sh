#!/bin/sh
# Audio group, host side. guest_start.sh runs it (outside the chroot, as root) at session start, after the guest's
# /tmp is mounted; it stays alive as a small keeper until the guest is unmounted.
#  - OneOS stops Android's audioserver while /data/local/tmp/oneos/data/guest_audio.flag exists (the loop in
#    oneos_switch.sh also suppresses zygote and surfaceflinger, so it must NOT be killed): remove the flag instead.
#    Cleaner: start oneos_switch.sh with ONEOS_GUEST_AUDIO=0, which never creates it (see RESULT.md).
#  - start audioserver (Samsung's HAL is already running) and x400-audiod, which owns the speaker/microphone and
#    serves /tmp/x400-audio-{out,in}.sock of the guest. X400_MAXAMP=<percent> caps the speaker level in audiod.
#  - With One UI's framework stopped nothing gives audio_policy a volume range and index, and the speaker stays silent
#    (every curve "invalid", track gain -inf). x400-setvol sets STREAM_MUSIC 0..15, index X400_STREAM_INDEX (default 5), on
#    the speaker. It must be redone every time audioserver gets a new pid (a restart loses it): watched every 2 s.
#  - NEVER run `dumpsys media.audio_flinger` / `dumpsys audio` here or anywhere while audioserver is up: without
#    system_server AudioFlinger::dump dereferences a null UidInfo and audioserver dies (tombstone, 11:20:57).
G=$(cd "$(dirname "$0")" && pwd)
STAGE=/data/local/tmp/oneos
R=$STAGE/rootfs
FLAG=$STAGE/data/guest_audio.flag
LOG=$STAGE/logs
IDX=${X400_STREAM_INDEX:-5}
mkdir -p "$LOG" 2>/dev/null
say() { echo "$(date +%T) host.sh: $*"; }

policy_up() { service check media.audio_policy 2>/dev/null | grep -q ": found"; }

wait_policy() {
    n=0
    while [ $n -lt 100 ] && ! policy_up; do n=$((n + 1)); sleep 0.2; done
    policy_up
}

ensure_audioserver() {
    [ -f "$FLAG" ] && { rm -f "$FLAG"; say "removed guest_audio.flag"; }
    if [ "$(getprop init.svc.audioserver)" != running ]; then
        setprop ctl.start audioserver
        wait_policy
        say "audioserver: $(getprop init.svc.audioserver), media.audio_policy $(service check media.audio_policy 2>/dev/null | cut -d: -f2)"
    fi
}

# applied for the audioserver pid in $APPLIED; a new pid (crash, restart) means the range and index are gone again
APPLIED=""
apply_volume() {
    pid=$(pidof audioserver)
    [ -n "$pid" ] && [ "$pid" != "$APPLIED" ] || return
    wait_policy || { say "media.audio_policy not registered after 20 s, will retry"; return; }
    if "$G/x400-setvol" "$IDX" 15 2>&1 | sed "s/^/x400-setvol: /"; then
        APPLIED=$pid
        say "volume applied to audioserver $pid (STREAM_MUSIC index $IDX/15)"
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
    X400_STREAM_INDEX=$IDX "$G/x400-audiod" --dir "$d" --log "$LOG/x400-audiod.log" >/dev/null 2>&1 &
    say "x400-audiod started ($!), log $LOG/x400-audiod.log"
}

ensure_audioserver
apply_volume
start_audiod
while grep -q " $R " /proc/mounts 2>/dev/null; do
    sleep 2
    ensure_audioserver
    apply_volume
    pidof x400-audiod >/dev/null || start_audiod
done
say "guest unmounted, stopping x400-audiod"
pkill x400-audiod
