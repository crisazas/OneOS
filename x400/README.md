# x400: Ubuntu 26.04 + KDE Plasma 6 on the Galaxy Tab S10 Lite (SM-X400), next to One UI

A Linux desktop that runs as a chroot on One UI's own kernel (5.15), with One UI still running. Nothing is flashed.
Root is KernelSU/DFroot (lives in RAM: a reboot loses it). Everything the session mounts lives in a private mount namespace
(`busybox unshare -m`), so One UI never sees it and it disappears when the session ends.

See [CREDITS.md](CREDITS.md): this branch builds on OneOS, and on OneiOS (Akshay CM, Musicalista and contributors) for the display layer and the Venus build.

## Status
The desktop is drawn by the tablet's GPU, not by llvmpipe. Measured on the tablet (2026-11-22/23); "not measured" means exactly that.
- **GPU in the composition:** `kwin_wayland --virtual` (Plasma 6.6.7) renders with Zink (Mesa 25.0.7, Debian's) on Venus on the Mali-G68
  (`OpenGL core 3.1`, renderer `zink Vulkan 1.3 (Virtio-GPU Venus (Mali-G68))`). Mesa 26's Zink needs `nullDescriptor`, which Samsung's Vulkan does not offer, hence 25.0.7.
- **No CPU copy of the frame:** `capture` asks KWin for linear BGRA dma-bufs; the display helper imports each one as a `VkImage` and blits it into the layer's swapchain
  (Vulkan, present mode mailbox). With one `es2gears` on Zink as the load, KWin used ~263 ticks per 5 s with that stream against ~410 with shared memory, and the helper's `acquire` took 0.14 ms (mean).
- **120 Hz mode:** KWin's virtual output has one 60 Hz mode; `session-inner.sh` adds a 120 Hz one with `kscreen-doctor` and makes it current (`X400_HZ=60` keeps 60). It is above the panel's 90 Hz on purpose: it gave the most frames (see the comment in the script).
- **Frame rate:** the frames that reach the layer went from ~7 fps (shared memory, `capture` copying into a file) to ~41 fps at 60 Hz and ~51 fps at 90 Hz with one `es2gears` on Zink.
  With `wl-client` (`x400/test/wl-client.c`, draws almost nothing, so it measures the chain and not a client), `capture` restarted at each mode, mean of 3 runs, frames shown by the helper:
  **63 fps at 90 Hz and 81 at 120 Hz** (80.6-81.1 across the runs); without `vtest-sync-pool.patch` it was 53 and 67. **90 fps is not reached.**
  Later, with the hardware groups running and a rebuilt `capture` (3 interleaved runs, the real helper, `capture` restarted at each mode): **69 fps shown at 90 Hz and 89 at 120 Hz** (69.3-69.5 and 88.4-89.3); the gain over 63 and 81 is not isolated.
  At 120 Hz the helper shows ~89 fps, just under the panel's 90 Hz; at 90 Hz `capture` gets ~66 fps (KWin's 11 ms frame interval is the likely reason; not isolated). Only means were taken: how even the intervals between frames are was not measured.
- **What limits it (measured):** KWin's main thread. `simpleperf` on it (120 Hz, before the pool): ~60 % of a core busy, kernel 28 %, Zink 19 %, KWin's own code 17 %, Venus 6 %;
  `ScreenCastStream::record` is ~30 % and the scene paint ~47 % of its CPU. Each `vkGetFenceFdKHR`/`vkGetSemaphoreFdKHR` did a `VCMD_SYNC_CREATE` round trip to `x400vt` (~330/s) and the main thread waited on the socket ~200 ms
  per second. `x400/venus/guest-mesa/vtest-sync-pool.patch` removes it (the fd is an `eventfd` that is already signaled; nothing waited on that sync): `SYNC_CREATE` and `SYNC_UNREF` went to 0/s and the wait to ~1 ms/s.
  Tried and discarded: `KWIN_NO_TIMER_QUERY=1` (KWin's render time drops 3.6 to 2.3 ms, but `capture` gets 81 fps instead of 85). Without the end-of-frame `EGLNativeFence` in `ItemRendererOpenGL::endFrame` (a one-instruction binary patch of `libkwin`, test only):
  `capture` 87.4 instead of 84.9 fps and ~190 fewer `SUBMIT_CMD2`/s, but that fence is what tells clients using `wp_linux_drm_syncobj` when a buffer is free, and no such client was tried, so it is not shipped.
  Also a KWin with `maxPendingFrameCount` 2 (a binary patch of the constructor) presented fewer frames, ~60 instead of ~87 at 90 Hz (the virtual backend's `present()` replaces the frame it holds, which would explain it; not checked).
  The stream's `max framerate` is fixed when `capture` connects, so a mode change needs `capture` restarted (an earlier sweep that did not was wrong).
- **Not known yet:** what is left of the cost in KWin's main thread (Zink and the kernel), whether `x400vt`'s `SUBMIT_CMD2` (~460-780/s after the pool, up to 10 ms) is the next limit, and why KWin presents ~23 % of the 120 Hz vblanks late
  even alone. The 120 Hz mode is above the panel's 90 Hz on purpose (mailbox presents without waiting): it gave the most frames at every point measured.
- **Not measured:** a long session (hours), heat, battery, more than two simultaneous GPU clients, and the picture with anything other than `es2gears` and the desktop itself.
  The panel was looked at by eye (orientation, colours, no cuts or artefacts); no automated image check was made.

## The tablet's hardware in the Linux session
Everything below runs in the same session, started by `oneos_switch.sh` with nothing done by hand; each group has its `x400/<group>/RESULT.md` with the commands and the output.
Wiring and the list of what no person has tested yet: [HARDWARE.md](HARDWARE.md).
- **Input:** touch, S Pen (as a mouse: no pressure or tilt, KWin's `fake_input` has no tablet requests), side keys (volume, power = backlight) without grabbing them, on-screen keyboard (`plasma-keyboard`).
- **Sensors and power:** accelerometer and light through `net.hadess.SensorProxy`, battery through UPower, brightness (`x400-brightness`), auto-rotation (display, `capture` and `inputd` follow KWin's output rotation).
- **Audio:** speaker and microphone as PipeWire nodes "Altavoces" and "Microfono" over Android's `audioserver` (AAudio). Never run `dumpsys media.audio_flinger` there: it crashes `audioserver` without `system_server`.
- **Radios:** Wi-Fi (NetworkManager with Ubuntu's `wpa_supplicant`, the network saved in One UI) and Bluetooth (`hci0` over `/dev/scsc_h4_0` through a relay); both are stopped when the way back starts.
- **Cameras:** front and rear as PipeWire `Video/Source` nodes (`x400-camera-front`, `x400-camera-back`), opened only while a client streams.
- **If root is lost after a reboot:** KernelSU manager -> Superuser -> show system apps -> `com.android.shell` -> Superuser on. A kernel oops from opening an ABOX capture PCM that Android's HAL holds caused one reboot: see HARDWARE.md.

## How a frame reaches the panel
`kwin_wayland --virtual` (Plasma 6.6.7, Zink on Venus; llvmpipe only if the Zink probe fails or `X400_GPU=sw`) -> `zkde_screencast` -> PipeWire (+ wireplumber) -> `capture` ->
(linear BGRA dma-buf, its descriptor over `FILE.sock`; shared memory copied into a file `fb.h` if the helper is not connected or KWin cannot give a dma-buf) ->
`X400Display` (a `SurfaceControl` layer made from `app_process`, above One UI; Vulkan swapchain, or `ANativeWindow_lock` for shared memory).
Touch: `/dev/input/event8` -> `inputd` -> `org_kde_kwin_fake_input` (rotated: the panel is landscape 2112x1320, the touch reports portrait 1320x2112).
The GPU: Mesa's Venus client in the chroot -> vtest socket -> `x400vt` (virglrenderer's Venus core, built with the NDK, outside the chroot) -> Samsung's Vulkan on the Mali-G68.
Pieces that make Zink work on it: `x400/venus/layer` (a Vulkan layer that gives Zink the DRM render node ids and works around Samsung applying `vkCmdSetDepthCompareOp` with the
depth test off), `x400/venus/vtest-import.patch` + `x400/venus/guest-mesa/` (a dma-buf import command for vtest), `x400/venus/vtest-sync-lock.patch` (a lock; `x400vt` died with two Zink clients
without it), `x400/venus/zink-syncfd-leak.c` (an `LD_PRELOAD` shim: Zink 25.0.7 leaked one dma-buf fd per barrier because this kernel has no `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`).

## Build (on the PC; needs sudo, qemu-user-static, gcc-aarch64-linux-gnu, libvulkan-dev, the Android NDK 28.2 and SDK build-tools)
- `x400/build-rootfs-plasma.sh` -> `out/rootfs-plasma.ext4` (6 GB, Ubuntu 26.04.1 arm64 + Plasma + PipeWire + wireplumber).
  `capture` and `inputd` are built on the PC against that rootfs (`R=out/rootfs-plasma`; the PC has no `wayland-scanner`, so the rootfs's runs under qemu). Checked on 2026-11-23:
  `WS="qemu-aarch64-static -L $R $R/usr/bin/wayland-scanner"; P=$R/usr/share/plasma-wayland-protocols`; for `x` in `zkde-screencast-unstable-v1` and `fake-input`:
  `$WS client-header $P/$x.xml $x-client-protocol.h; $WS private-code $P/$x.xml $x-protocol.c`; then
  `aarch64-linux-gnu-gcc -O2 -Wall --sysroot=$R -I. -Ix400/display -I$R/usr/include -I$R/usr/include/aarch64-linux-gnu -I$R/usr/include/pipewire-0.3 -I$R/usr/include/spa-0.2 x400/display/capture.c zkde-screencast-unstable-v1-protocol.c -o capture -L$R/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$R/usr/lib/aarch64-linux-gnu -Wl,--unresolved-symbols=ignore-in-shared-libs -lpipewire-0.3 -lwayland-client`
  (`inputd`: the same with `inputd.c fake-input-protocol.c` and `-lwayland-client -lm`). The results go in `x400/display/bin/`, which `x400/oneos/deploy.sh` pushes.
  **Compiled binaries are not in git** (`.gitignore`: `x400/display/bin/`, `out/`, each group's binaries in `stage/`): `deploy.sh` pushes whatever is on the PC's disk, so build first (`capture` and `inputd` with the commands above, `x400/display/build.sh` for the helper, `x400/<group>/build.sh` for each group)
  and again after changing a `.c`. A stale `capture` is not caught: on 2026-11-23 one built before the rotation commit (`8b2196d`) was deployed, `capture` died at start (`no PipeWire node from KWin`) and the helper got no frames;
  rebuilt, it connected (`strings x400/display/bin/capture | grep "output transform"` tells the two apart).
- `x400/venus/build.sh` -> `out/venus-build/virgl_test_server` (`x400/venus/prepare-virgl.sh` makes `out/virgl` from a checkout of Droid-VM's virglrenderer at `7e17c8e` plus `x400/venus/vkr-dma-heap.patch` and
  `x400/venus/vtest-x400.patch`, both ours).
- `x400/display/build.sh` -> `out/display/{x400-display.dex,libx400display.so}`.

## Run (OneOS's flow: this branch only replaces the guest)
OneOS's own scripts (`switcher/assets/`, `oneos_switch.sh`) do the preflight, stop One UI's framework, start the session and bring One UI back; `x400/oneos/` supplies the Linux guest.
- `x400/oneos/deploy.sh [--scripts-only]` (PC, adb, root on the tablet): stages OneOS's scripts and `system.raw` (the rootfs image, checked by sha256) in `/data/local/tmp/oneos`,
  replaces `guest_prepare.sh` and `guest_start.sh` with `x400/oneos/`'s, and puts the desktop's pieces (`capture`, `inputd`, `x400vt`, the display helper, `session-inner.sh`, Mesa 25.0.7) in `/data/local/tmp/oneos/x400`.
  It refuses to run while a session is mounted.
- Start, on the tablet as root: `cd /data/local/tmp/oneos && ONEOS_RETURN_MODE=inplace sh oneos_switch.sh`. The log is where you redirect it; the desktop's is `/data/local/tmp/oneos/data/takeover.log`.
  `guest_prepare.sh` attaches `system.raw` read-only on a loop device and mounts it; `guest_start.sh` mounts a 32 MB tmpfs for `fb.bin`, starts `x400vt`, then `session-inner.sh` (KWin, Plasma), `capture` and the display helper.
- Way back: hold **Volume Up + Volume Down for 3 s**, or `setprop oneos.guest.powerctl app:x400`. OneOS restores One UI in place and checks it (`auto_return.log`).
  If the guest cannot be prepared, the switch returns to One UI by itself (it did once with `losetup` not finding `system.raw`; the same command worked a minute later).
- A reboot loses root (DFroot) and the session; the stage under `/data/local/tmp/oneos` survives it.

## Facts that cost time
- `VN_DEBUG=vtest` is mandatory (without it Mesa tries the real DRM node first and finds no GPU) and so is `VN_PERF=no_fence_feedback,no_semaphore_feedback,no_query_feedback` (with
  fence feedback `vkWaitForFences` hangs; the cause is not found).
- Stock vtest does not import dma-bufs (`has_dma_buf_import = false`): `VCMD_RESOURCE_IMPORT_FD` (id 64) and a patched guest `libvulkan_virtio` add it.
- The host-visible memory comes from `/dev/dma_heap/system-uncached` by default (`X400_DMA_HEAP` overrides it): with the cached `system` heap the CPU read-back of a texture varied between runs
  (and the desktop showed black bars and glitches), with `system-uncached` it was stable; it also makes KWin's read-back slower.
- The tablet's kernel (5.15) has no `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE`; there is no implicit dma-buf synchronization.
- KWin 6.6.7 only allows screencast with OpenGL compositing, so QPainter is refused (llvmpipe is the fallback when Zink does not come up).
- A swapchain on the layer needs `preTransform` IDENTITY (the surface reports ROTATE_90 as current), and a surface used with `ANativeWindow_lock` cannot get a Vulkan one afterwards.
- With the Linux session up the USB connection to the PC dropped three times; returning with Vol+ and Vol- brought it back twice. The third ended in a reboot: the kernel log shows `hard LOCKUP on cpu 1`
  fifteen seconds after a `WARNING` in `rb_commit` under `__uprobe_trace_func` in `kwin_wayland`, i.e. tracefs uprobes set by us on KWin. Do not use uprobes on a live session. The causes of the first two (one during a `simpleperf report`) are not known.
- Without systemd, Plasma's services are started by hand: `kglobalacceld`, `kded6`, `kactivitymanagerd`, `kbuildsycoca6`, then `plasmashell`.
- The chroot shares the PID namespace with Android: `plasma.sh` kills only processes whose root is the chroot.
- The screen must be awake (`svc power stayon true`) or `screencap` is black; the panel is landscape.
