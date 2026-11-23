#!/system/bin/sh
# linux-x400: guest_start.sh for a Linux desktop guest, staged in place of OneOS's Android one (switcher/assets/guest_start.sh).
# oneos_switch.sh copies it to $STAGE/data/guest_start.sh and runs it at [6/6] as
#     $BB unshare -m $BB sh "$STAGE/data/guest_start.sh" > "$STAGE/data/takeover.log"
# with One UI's framework and surfaceflinger stopped. Everything started here lives in this mount namespace, which
# oneos_switch.sh records (guest_mntns) and return_to_oneui.sh ends as a whole.
#
# What the session watcher of oneos_switch.sh checks, and how this script meets it:
#  - a process called surfaceflinger, once "SurfaceFlinger PID:" is in takeover.log: One UI's own surfaceflinger binary is
#    started here as a plain child (as OneOS's Android guest starts its own); init does not know it, so the session's
#    `ctl.stop surfaceflinger` loop leaves it alone;
#  - the takeover process alive in its namespace: this script stays in the foreground until the desktop ends;
#  - "Home activated" in /data/local/tmp/ui_activator.log within ONEOS_RETURN_BLACK: written once the first frame is shown.
# The way back is OneOS's (Volume Up + Volume Down for 3 s, the app, adb), never this script's.
STAGE=/data/local/tmp/oneos
R=$STAGE/rootfs
D=$STAGE/data/x400
BB=""
for c in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ -x "$c" ] && { BB=$c; break; }
done
[ -n "$BB" ] || { echo "[x400] FATAL: no busybox. Not starting the guest."; exit 1; }
say() { echo "[x400] $(date +%T) $*"; }
say "Linux guest starting in mount namespace $(readlink /proc/$$/ns/mnt)"
# Anything of an earlier session that is still alive (a second desktop, an inputd that holds the touchscreen) is ended first:
# two desktops saturated the CPU and the memory, and a left-over inputd with EVIOCGRAB took the touchscreen from One UI.
# Only processes whose root is a chroot of ours are touched.
OLD=0
for sig in TERM KILL; do
    for p in /proc/[0-9]*; do
        r=$(readlink "$p/root" 2>/dev/null)
        case "$r" in /data/local/tmp/x400root|/data/local/tmp/x400root/*) kill -$sig "${p#/proc/}" 2>/dev/null && OLD=$((OLD + 1));; esac
    done
    [ "$OLD" -gt 0 ] && sleep 2
done
[ "$OLD" -gt 0 ] && say "ended $OLD process(es) of an earlier x400 session"
$BB mount --make-rprivate / || { say "FATAL: could not make the mounts private"; exit 1; }
[ -x "$R/usr/bin/kwin_wayland" ] || { say "FATAL: $R is not the Linux image (guest_prepare.sh mounts it)"; exit 1; }

# The guest's own writable places, in this namespace only.
mkdir -p "$D/fb" "$D/logs"
$BB mount -t tmpfs -o mode=1777 tmpfs "$R/tmp"
$BB mount -t tmpfs -o mode=755 tmpfs "$R/run"
$BB mount --bind /dev "$R/dev"
$BB mount -t devpts -o newinstance,ptmxmode=0666,mode=620 devpts "$R/dev/pts" 2>/dev/null || $BB mount --bind /dev/pts "$R/dev/pts"
$BB mount -t proc proc "$R/proc"
$BB mount -t sysfs sysfs "$R/sys"
# The frame file (11 MB rewritten per frame) lives in RAM: on f2fs every frame dirtied pages the kernel then wrote to flash.
$BB mount -t tmpfs -o mode=1777,size=32m tmpfs "$D/fb" || say "tmpfs for the frame file failed: it stays on $D/fb (f2fs)"
mkdir -p "$R/tmp/fb"; $BB mount --bind "$D/fb" "$R/tmp/fb"
# $STAGE/x400/kwin-perf present: KWin logs its per-frame timing (session-inner.sh looks for /tmp/x400-kwin-perf)
[ -e "$STAGE/x400/kwin-perf" ] && : > "$R/tmp/x400-kwin-perf"
if [ -d "$STAGE/x400/mesa25" ]; then
    mkdir -p "$R/tmp/mesa25"; $BB mount --bind "$STAGE/x400/mesa25" "$R/tmp/mesa25" && say "Mesa 25.0.7 for Zink bound at /tmp/mesa25"
fi
# libvulkan_virtio with the dma-buf import (x400/venus/guest-mesa), over the rootfs's own: the image is read-only, so a bind mount
if [ -f "$STAGE/x400/libvulkan_virtio.so" ]; then
    VL=$(ls "$R"/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so 2>/dev/null)
    [ -n "$VL" ] && $BB mount --bind "$STAGE/x400/libvulkan_virtio.so" "$VL" && say "libvulkan_virtio with dma-buf import bound over $VL"
fi
for f in capture inputd keys session-inner.sh cpu-sample.sh; do
    [ -f "$STAGE/x400/$f" ] || continue
    : > "$R/tmp/$f"; $BB mount --bind "$STAGE/x400/$f" "$R/tmp/$f"
done
# Each hardware group (x400/HARDWARE.md) ships what it needs in $STAGE/x400/groups/<name>/: executables and config for the guest are
# bound under /tmp/x400/<name>; host.sh (if there) runs here, outside the chroot, as root, before the desktop starts.
mkdir -p "$R/tmp/x400"
for g in "$STAGE"/x400/groups/*/; do
    [ -d "$g" ] || continue; gn=$(basename "$g")
    mkdir -p "$R/tmp/x400/$gn"; $BB mount --bind "$g" "$R/tmp/x400/$gn" && say "group $gn bound at /tmp/x400/$gn"
    [ -f "$g/host.sh" ] && { sh "$g/host.sh" > "$D/logs/group-$gn.log" 2>&1 & say "group $gn host.sh started ($!)"; }
done
# One UI's own shared storage (DCIM, Download, Documents, ...: Android's /data/media/0), at /mnt/oneui in the guest, read and write. OneOS shares only six
# folders and only on request (ONEOS_SHARE_FILES, guest_start.sh of switcher/assets) because in its run 25 the guest's cleanup of a user it could not prepare
# deleted the photos: a deletion from Linux here is a real deletion of the phone's files. Nothing in this repo runs chown/chmod/rm -r over /mnt/oneui.
# It is the pass-through view (no FUSE, so no per-app rules, thumbnails or Android/data protection); the files belong to u0_aNNN:media_rw, and root sees them.
# X400_SHARE=ro mounts it read-only, X400_SHARE=0 does not mount it. The bind is made in this private namespace and goes with the session.
case "${X400_SHARE:-rw}" in
    0) say "One UI's storage: not shared (X400_SHARE=0)" ;;
    *)
        SHR=/mnt/pass_through/0/emulated/0
        if [ -d "$SHR/" ] && mkdir -p "$R/mnt/oneui" && $BB mount --bind "$SHR" "$R/mnt/oneui" && $BB mount --make-private "$R/mnt/oneui"; then
            if [ "${X400_SHARE:-rw}" = ro ]; then
                $BB mount -o remount,bind,ro "$R/mnt/oneui" "$R/mnt/oneui" && say "One UI's storage at /mnt/oneui, read only" || say "WARNING: could not make /mnt/oneui read only"
            else
                say "One UI's storage at /mnt/oneui, READ AND WRITE (X400_SHARE=ro for read only)"
            fi
        else
            say "One UI's storage: not reachable at $SHR; /mnt/oneui not mounted"
        fi ;;
esac
say "guest filesystems ready: $(grep -c " $R" /proc/mounts) mounts under $R"

# The display. OneOS stopped One UI's surfaceflinger and its watcher keeps init's copy stopped; the desktop's layer needs a
# surfaceflinger, so One UI's binary runs here as this guest's own (the composer first, as OneOS's Android guest does).
COMPOSER_SVC=""
for c in vendor.hwcomposer-3 vendor.qti.hardware.display.composer; do
    [ -n "$(getprop init.svc.$c)" ] && { COMPOSER_SVC=$c; break; }
done
[ -n "$COMPOSER_SVC" ] || COMPOSER_SVC=$(getprop | $BB sed -n 's/^\[init\.svc\.\([^]]*\)\]: \[running\]$/\1/p' | $BB grep -m1 -E 'hwcomposer|display\.composer|graphics\.composer')
if [ -n "$COMPOSER_SVC" ]; then
    setprop ctl.stop "$COMPOSER_SVC"
    n=0; while [ $n -lt 30 ] && [ "$(getprop init.svc.$COMPOSER_SVC)" != stopped ]; do n=$((n + 1)); sleep 0.1; done
    setprop ctl.start "$COMPOSER_SVC"
    n=0; while [ $n -lt 30 ] && ! service check android.hardware.graphics.composer3.IComposer/default 2>/dev/null | grep -q ': found'; do n=$((n + 1)); sleep 0.1; done
    say "composer $COMPOSER_SVC restarted ($(getprop init.svc.$COMPOSER_SVC))"
fi
# The composer's rc has "onrestart restart surfaceflinger": any surfaceflinger now is init's, which the session keeps stopped.
setprop ctl.stop surfaceflinger
n=0; while [ $n -lt 20 ] && pidof surfaceflinger >/dev/null; do kill -9 $(pidof surfaceflinger) 2>/dev/null; n=$((n + 1)); sleep 0.1; done
/system/bin/surfaceflinger > "$D/logs/surfaceflinger.log" 2>&1 &
SF_PID=$!
echo "[Pivoted Root] SurfaceFlinger PID: $SF_PID"
n=0; while [ $n -lt 50 ] && ! service check SurfaceFlinger 2>/dev/null | grep -q ': found'; do n=$((n + 1)); sleep 0.1; done
say "surfaceflinger $SF_PID up after $((n))00 ms ($(service check SurfaceFlinger 2>/dev/null | tail -c 20))"
# No framework is left to power the panel on: ask surfaceflinger (the display helper does, with --power).

# The GPU: virglrenderer's Venus server, outside the chroot, on Samsung's Vulkan, its socket inside the guest's /tmp.
VKR_DMA_HEAP_NODE=${X400_DMA_HEAP:-/dev/dma_heap/system-uncached} VKR_TRACE=${X400_VKR_TRACE:-} ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so VIRGL_LOG_LEVEL=info \
    "$STAGE/x400/x400vt" --venus --no-fork --multi-clients --socket-path="$R/tmp/.virgl_test" > "$D/logs/venus.log" 2>&1 &
say "Venus server $!"
sleep 1

# A frame file left by an earlier session has a high seq and would pass the wait below at once, before this desktop has drawn
# anything (the display helper then found no file and left: the desktop ran and nothing showed it).
rm -f "$D/fb/fb.bin"

# The desktop, inside the chroot (KWin virtual, Plasma, the capture into $D/fb/fb.bin, the touch daemon).
$BB chroot "$R" /usr/bin/env -i PATH=/usr/bin:/bin RUNSECS=0 /usr/bin/dbus-run-session -- sh /tmp/session-inner.sh > "$D/logs/session-inner.log" 2>&1 &
DESK=$!
say "desktop session $DESK"

# The first frame, then the layer that shows it, then OneOS's "home" mark for its watcher.
n=0
while [ $n -lt 120 ]; do
    [ -f "$D/fb/fb.bin" ] && [ "$(od -A n -t u8 -j 32 -N 8 "$D/fb/fb.bin" 2>/dev/null | tr -d ' ')" -gt 0 ] 2>/dev/null && break
    kill -0 $DESK 2>/dev/null || { say "the desktop session ended before its first frame"; break; }
    sleep 1; n=$((n + 1))
done
say "first frame after ${n}s"
"$STAGE/x400/display.sh" "$STAGE/x400" "$D/fb/fb.bin" 2112 1320 --power > "$D/logs/display.log" 2>&1 &
say "display helper $!"
sleep 3
# What the desktop chose for its GPU (session-inner.sh probes Zink and falls back to llvmpipe), copied out of the guest's /tmp.
cp "$R/tmp/gpu-choice.txt" "$D/logs/gpu-choice.txt" 2>/dev/null
say "$(head -n 1 "$D/logs/gpu-choice.txt" 2>/dev/null || echo 'desktop GPU: not reported')"
echo "[UI Activator] Home activated! (linux-x400 desktop)" >> /data/local/tmp/ui_activator.log

# The way back starts by creating oneos_rec_stop (return_to_oneui.sh, [1/4]) and then kills everything of the guest with SIGKILL. The radio group's
# processes must close /dev/scsc_h4_0 and give Wi-Fi back first, or One UI finds Bluetooth held by a dead relay: stop them on that signal.
rm -f "$STAGE/data/oneos_rec_stop" 2>/dev/null
( n=0; while [ ! -e "$STAGE/data/oneos_rec_stop" ] && kill -0 $DESK 2>/dev/null; do sleep 0.5; done
  if [ -e "$STAGE/data/oneos_rec_stop" ]; then
      say "the way back has started: stopping the radio group"
      $BB chroot "$R" /usr/bin/env PATH=/usr/bin:/usr/sbin:/bin:/sbin sh /tmp/x400/radio/guest.sh stop > "$D/logs/group-radio-stop.log" 2>&1
      [ -f "$STAGE/x400/groups/camera/stop.sh" ] && sh "$STAGE/x400/groups/camera/stop.sh" >> "$D/logs/group-radio-stop.log" 2>&1
      say "radio group stopped"
  fi ) &
# Stay in the foreground for the session's watcher; the way back ends this namespace.
while kill -0 $DESK 2>/dev/null; do sleep 5; done
for f in kwin.log; do cp "$R/tmp/$f" "$D/logs/$f" 2>/dev/null; done
say "desktop session ended"
