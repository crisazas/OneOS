# linux-x400: runs inside the chroot (as root). Starts PipeWire, KWin (virtual), Plasma and the capture to /tmp/fb/fb.bin.
# The desktop and its apps (PipeWire, KWin, Plasma, Dolphin...) run as the user X400_USER (default x400, uid 12000, made by x400/user/stage/setup.sh); what needs root stays root:
# the hardware groups (/dev/input, the radio, the sockets of the host side), inputd and capture. X400_USER=root, or a setup that fails, runs everything as root as before.
U=${X400_USER:-x400}
if [ "$U" != root ] && [ -f /tmp/x400/user/setup.sh ] && PATH=/usr/bin:/usr/sbin:/bin:/sbin sh /tmp/x400/user/setup.sh > /tmp/x400-user.log 2>&1; then
  UH=/home/$U; UIDN=$(id -u $U)
else
  U=root; UH=/root; UIDN=0
fi
# the Venus server's sockets are the root's; connecting to a socket needs write permission (the GPU probe below runs as the user)
[ "$U" != root ] && chmod 666 /tmp/.virgl_test* 2>/dev/null
as_user() { if [ "$U" = root ]; then "$@"; else setpriv --reuid=$U --regid=$U --init-groups "$@"; fi; }
export HOME=$UH USER=$U LOGNAME=$U XDG_RUNTIME_DIR=/run/user/$UIDN LANG=C.UTF-8 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true
# the keyboard cover's layout (KWin falls back to the environment when kxkbrc names none); X400_KBD_LAYOUT=us for an English one
export XKB_DEFAULT_LAYOUT=${X400_KBD_LAYOUT:-es}
export XDG_DATA_DIRS=/usr/share XDG_CONFIG_DIRS=/etc/xdg KWIN_WAYLAND_NO_PERMISSION_CHECKS=1
# GPU for the desktop's OpenGL: Zink (OpenGL over Vulkan) on Venus, from the Mesa 25.0.7 bound at /tmp/mesa25 (Zink from Mesa 25.2 demands
# robustness2 nullDescriptor, which Samsung's Mali driver does not offer). A probe decides: if Zink does not come up the desktop runs
# on llvmpipe as before, never without a picture. X400_GPU=sw forces llvmpipe, X400_GPU=zink forces Zink (no probe).
M=/tmp/mesa25/usr/lib/aarch64-linux-gnu
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json VN_DEBUG=vtest VN_PERF=no_fence_feedback,no_semaphore_feedback,no_query_feedback
zink_env() {
  printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s/libEGL_mesa.so.0"}}\n' "$M" > /tmp/50_mesa.json
  export LD_LIBRARY_PATH=$M LIBGL_DRIVERS_PATH=$M/dri __EGL_VENDOR_LIBRARY_FILENAMES=/tmp/50_mesa.json GBM_BACKENDS_PATH=$M/gbm
  export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink __GLX_VENDOR_LIBRARY_NAME=mesa
  # Zink 25.0 matches the Vulkan device by the DRM render node ids; this layer (x400/venus/layer) supplies 226:128
  export VK_LAYER_PATH=/tmp/mesa25/x400layer VK_INSTANCE_LAYERS=VK_LAYER_x400_drmid
  unset LIBGL_ALWAYS_SOFTWARE EGL_PLATFORM
}
sw_env() {
  unset VK_LAYER_PATH VK_INSTANCE_LAYERS LD_LIBRARY_PATH LIBGL_DRIVERS_PATH __EGL_VENDOR_LIBRARY_FILENAMES GBM_BACKENDS_PATH __GLX_VENDOR_LIBRARY_NAME
  export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=llvmpipe GALLIUM_DRIVER=llvmpipe
}
GPU_USED=sw
case "${X400_GPU:-auto}" in
  sw) sw_env ;;
  zink) zink_env; GPU_USED=zink ;;
  *)
    zink_env
    # the Venus server must be answering (guest_start.sh starts it before this script); a probe that hangs is cut at 25 s
    # KWin's virtual backend makes its EGL display from a GBM device on the DRM render node, so the probe is eglinfo's GBM section
    as_user timeout 40 eglinfo -B > /tmp/eglprobe.log 2>&1
    if awk '/^GBM platform:/ {g=1; next} /platform:/ {g=0} g' /tmp/eglprobe.log | grep -q -i 'renderer.*zink'; then
      GPU_USED=zink
    else
      sw_env
    fi ;;
esac
echo "x400: desktop GPU = $GPU_USED" > /tmp/gpu-choice.txt
if [ "$GPU_USED" = zink ]; then
  # what the guest's Vulkan offers for dma-buf: GBM only allocates real buffers if Zink reports dmabuf import/export
  as_user timeout 40 vulkaninfo > /tmp/vulkaninfo.log 2>&1
  { echo "== vulkan dma-buf extensions"; grep -o -E 'VK_(KHR_external_memory_fd|EXT_external_memory_dma_buf|EXT_queue_family_foreign|EXT_image_drm_format_modifier|KHR_external_semaphore_fd)' /tmp/vulkaninfo.log | sort -u; echo "== end"; } >> /tmp/gpu-choice.txt
fi
cat /tmp/eglprobe.log >> /tmp/gpu-choice.txt 2>/dev/null
export KWIN_COMPOSE=${KWIN_COMPOSE:-O2}
export QT_QUICK_BACKEND=software KDE_SESSION_VERSION=6 KDE_APPLICATIONS_AS_SCOPE=0
L=/usr/lib/aarch64-linux-gnu/libexec
mkdir -p $XDG_RUNTIME_DIR && chown $U:$U $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
rm -f $XDG_RUNTIME_DIR/wayland-x400* /tmp/fb/fb.bin
if [ "$U" != root ]; then
  # dbus-run-session (guest_start.sh) made root's session bus: the user gets its own
  as_user dbus-daemon --session --address=unix:path=$XDG_RUNTIME_DIR/bus --fork --nopidfile
  export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
fi
as_user pipewire > /tmp/pw.log 2>&1 &
sleep 2
# the PulseAudio socket for apps that only speak that (vlc): pipewire-pulse, when the image has it
command -v pipewire-pulse > /dev/null && { as_user pipewire-pulse > /tmp/pw-pulse.log 2>&1 & }
as_user wireplumber > /tmp/wp.log 2>&1 &
sleep 2
# Hardware groups (x400/HARDWARE.md): each ships /tmp/x400/<group>/guest.sh (bound by guest_start.sh); a failing one never stops the desktop.
# (as root, HOME=/root; they find the user's PipeWire in XDG_RUNTIME_DIR, and X400_HOME is where the user's files are)
grp() { [ -f /tmp/x400/$1/guest.sh ] && { HOME=/root USER=root LOGNAME=root X400_HOME=$UH PATH=/usr/bin:/usr/sbin:/bin:/sbin sh /tmp/x400/$1/guest.sh > /tmp/x400-$1.log 2>&1 & }; }
grp audio
grp camera
# With Zink, KWin runs under gdb in batch mode: if it dies, the backtrace ends /tmp/kwin.log (guest_start.sh copies it out).
KWIN=kwin_wayland
# Zink 25.0.7 leaks one dma-buf fd per barrier on a client's buffer (the kernel has no DMA_BUF_IOCTL_EXPORT_SYNC_FILE): this shim closes it (x400/venus/zink-syncfd-leak.c)
SHIM=/tmp/mesa25/x400layer/libx400_syncfd.so; [ -f $SHIM ] && [ -z "$X400_NOSHIM" ] || SHIM=
if [ "$GPU_USED" = zink ] && command -v gdb >/dev/null; then
  KWIN="gdb -q -batch -ex 'handle SIGPIPE SIGUSR1 SIGUSR2 SIG34 SIG35 nostop noprint' ${SHIM:+-ex 'set environment LD_PRELOAD $SHIM'} -ex run -ex 'info registers pc' -ex 'thread apply all bt 25' --args kwin_wayland"
elif [ "$GPU_USED" = zink ] && [ -n "$SHIM" ]; then
  KWIN="env LD_PRELOAD=$SHIM kwin_wayland"
fi
export QT_LOGGING_RULES="kwin_screencast.debug=true;kwin_core.debug=true;kwin_scene_opengl.debug=true;kwin_wayland_backend.debug=true" PIPEWIRE_DEBUG=${PWDBG:-2}
# the file /tmp/x400-kwin-perf (put there by the stage, since the session starts with an empty environment) makes KWin write "kwin perf statistics <output>.csv" in /tmp:
# per frame, the target and the real pageflip time and the render start and end
[ -f /tmp/x400-kwin-perf ] && { export KWIN_LOG_PERFORMANCE_DATA=1; cd /tmp; }
as_user sh -c "exec $KWIN --virtual --width 2112 --height 1320 --socket wayland-x400 > /tmp/kwin.log 2>&1" &
sleep 6
export WAYLAND_DISPLAY=wayland-x400 QT_QPA_PLATFORM=wayland
# KWin's virtual output has one mode, 60 Hz. Add a 120 Hz mode and make it the current one before Plasma and the capture start (X400_HZ=60 keeps 60).
# 120 although the panel does 90: the helper presents with mailbox, so it never waits for the panel, and a faster virtual output gives KWin a shorter frame interval.
# Measured on the SM-X400 with one wl-client, capture restarted at each mode (the stream's max framerate is fixed when it connects), mean of 3 runs: frames shown 53 fps at 90 Hz, 57 at 100, 67 at 120.
HZ=${X400_HZ:-120}
if [ "$HZ" != 60 ] && command -v kscreen-doctor >/dev/null; then
  n=0; while [ $n -lt 10 ]; do
    as_user kscreen-doctor -o 2>/dev/null | grep -a -q "2112x1320@$HZ" || as_user kscreen-doctor output.1.addCustomMode.2112.1320.${HZ}000.full > /dev/null 2>&1
    as_user kscreen-doctor output.1.mode.2112x1320@$HZ > /tmp/hz.log 2>&1
    as_user kscreen-doctor -o 2>/dev/null | grep -a -E 'Modes' | grep -a -q "2112x1320@$HZ[.0-9]*\S*\*" && break
    n=$((n + 1)); sleep 1
  done
  echo "x400: output mode $(as_user kscreen-doctor -o 2>/dev/null | grep -a Modes | grep -a -o '[0-9x]*@[0-9.]*[^ ]*[*]' | head -1) after $n retries" >> /tmp/gpu-choice.txt
fi
# sensors first: it owns the guest's system bus; radio (NetworkManager, BlueZ) waits for it, and both must be on the bus before plasmashell loads its applets
grp sensors
grp radio
# the desktop's shortcuts (x400/shortcuts): before plasmashell, which reads ~/Desktop and Dolphin's places when it starts
grp shortcuts
as_user $L/kglobalacceld > /tmp/kga.log 2>&1 &
as_user kded6 > /tmp/kded.log 2>&1 &
sleep 3
as_user $L/kactivitymanagerd > /tmp/kam.log 2>&1 &
sleep 2
as_user kbuildsycoca6 > /tmp/syc.log 2>&1
as_user plasmashell > /tmp/plasma.log 2>&1 &
sleep 4
/tmp/inputd --touch /dev/input/event8 --pen /dev/input/event10 --grab > /tmp/inputd.log 2>&1 &
grp input
sleep 8
PIPEWIRE_DEBUG=${PWDBG:-2} /tmp/capture /tmp/fb/fb.bin > /tmp/capture.log 2>&1 &
# RUNSECS=0 (OneOS session): stay until the way back ends this mount namespace; the desktop's processes are watched, and
# the script ends (and with it guest_start.sh, then OneOS's watcher returns to One UI) only if KWin dies.
if [ "${RUNSECS:-60}" = 0 ]; then
  while kill -0 $(pidof kwin_wayland | cut -d' ' -f1) 2>/dev/null; do sleep 5; done
  echo "== kwin_wayland ended: the session ends"; tail -n 20 /tmp/kwin.log
  exit 1
fi
sleep "${RUNSECS:-60}"
echo "== pw-link"; pw-link -l 2>&1 | head -12; echo "== nodos"; pw-dump 2>/dev/null | grep -E '"node.name"|"media.class"|"node.description"|"state"' | head -20
echo "== procesos"; ps -eo pid,etime,rss,comm | grep -E 'kwin|plasma|kded|kactivity|kglobal|pipewire|capture' | grep -v grep
echo "== kwin.log"; grep -v -E 'intel_device|locale|UTF-8|^$' /tmp/kwin.log | tail -n 8
echo "== plasma.log"; grep -v -E 'intel_device|locale|UTF-8|^$|libEGL|MESA' /tmp/plasma.log | head -8
if [ -n "$GPUTEST" ]; then
  echo "== GPU test: $GPUTEST"
  VN_PERF=no_fence_feedback timeout 40 sh -c "$GPUTEST" > /tmp/gpu.log 2>&1; echo "rc=$?"; tail -n 14 /tmp/gpu.log | cut -c1-170
fi
if [ -f /tmp/fb/load.sh ]; then
  echo "== load: $(head -n 3 /tmp/fb/load.sh | tr '\n' ';')"
  WAYLAND_DISPLAY=wayland-x400 QT_QPA_PLATFORM=wayland sh /tmp/fb/load.sh > /tmp/load.log 2>&1 &
  sleep 8
fi
echo "== cpu over 10 s"; sh /tmp/cpu-sample.sh 10
echo "== capture (seq of the shared frame: $(od -A n -t u8 -j 32 -N 8 /tmp/fb/fb.bin | tr -d " "))"; grep -E "frames" /tmp/capture.log | tail -n 3
echo "== inputd.log"; cat /tmp/inputd.log
echo "== capture.log"; tail -n 8 /tmp/capture.log; echo "== kwin screencast"; grep -i -E "screencast|stream|format|dmabuf" /tmp/kwin.log | head -20
