# linux-x400: runs inside the chroot (as root). Starts PipeWire, KWin (virtual), Plasma and the capture to /tmp/fb/fb.bin.
export HOME=/root XDG_RUNTIME_DIR=/run/user/0 LANG=C.UTF-8 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true
export XDG_DATA_DIRS=/usr/share XDG_CONFIG_DIRS=/etc/xdg KWIN_WAYLAND_NO_PERMISSION_CHECKS=1
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=llvmpipe GALLIUM_DRIVER=llvmpipe KWIN_COMPOSE=${KWIN_COMPOSE:-O2}
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json VN_DEBUG=vtest VN_PERF=no_fence_feedback
export QT_QUICK_BACKEND=software KDE_SESSION_VERSION=6 KDE_APPLICATIONS_AS_SCOPE=0
L=/usr/lib/aarch64-linux-gnu/libexec
mkdir -p /run/user/0 && chmod 700 /run/user/0
rm -f /run/user/0/wayland-x400* /tmp/fb/fb.bin
pipewire > /tmp/pw.log 2>&1 &
sleep 2
wireplumber > /tmp/wp.log 2>&1 &
sleep 2
QT_LOGGING_RULES="kwin_screencast.debug=true" PIPEWIRE_DEBUG=${PWDBG:-2} kwin_wayland --virtual --no-lockscreen --width 2112 --height 1320 --socket wayland-x400 > /tmp/kwin.log 2>&1 &
sleep 6
export WAYLAND_DISPLAY=wayland-x400 QT_QPA_PLATFORM=wayland
$L/kglobalacceld > /tmp/kga.log 2>&1 &
kded6 > /tmp/kded.log 2>&1 &
sleep 3
$L/kactivitymanagerd > /tmp/kam.log 2>&1 &
sleep 2
kbuildsycoca6 > /tmp/syc.log 2>&1
plasmashell > /tmp/plasma.log 2>&1 &
sleep 4
/tmp/inputd --touch /dev/input/event8 --pen /dev/input/event10 --grab > /tmp/inputd.log 2>&1 &
sleep 8
PIPEWIRE_DEBUG=${PWDBG:-2} /tmp/capture /tmp/fb/fb.bin > /tmp/capture.log 2>&1 &
sleep "${RUNSECS:-60}"
echo "== pw-link"; pw-link -l 2>&1 | head -12; echo "== nodos"; pw-dump 2>/dev/null | grep -E '"node.name"|"media.class"|"node.description"|"state"' | head -20
echo "== procesos"; ps -eo pid,etime,rss,comm | grep -E 'kwin|plasma|kded|kactivity|kglobal|pipewire|capture' | grep -v grep
echo "== kwin.log"; grep -v -E 'intel_device|locale|UTF-8|^$' /tmp/kwin.log | tail -n 8
echo "== plasma.log"; grep -v -E 'intel_device|locale|UTF-8|^$|libEGL|MESA' /tmp/plasma.log | head -8
if [ -n "$GPUTEST" ]; then
  echo "== GPU test: $GPUTEST"
  VN_PERF=no_fence_feedback timeout 40 sh -c "$GPUTEST" > /tmp/gpu.log 2>&1; echo "rc=$?"; tail -n 14 /tmp/gpu.log | cut -c1-170
fi
echo "== cpu (kwin, plasmashell, capture, inputd)"; top -b -n 1 2>/dev/null | head -n 14 | tail -n 8
echo "== inputd.log"; cat /tmp/inputd.log
echo "== capture.log"; tail -n 8 /tmp/capture.log; echo "== kwin screencast"; grep -i -E "screencast|stream|format|dmabuf" /tmp/kwin.log | head -20
