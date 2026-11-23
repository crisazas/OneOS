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
  **90 fps is not reached**: with that client KWin hands about 51-62 frames/s to the screencast and `kwin_wayland` is at ~58 % of a core, so the limit is not the CPU. Why is not known yet.
- **Not measured:** a long session (hours), heat, battery, more than two simultaneous GPU clients, and the picture with anything other than `es2gears` and the desktop itself.
  The panel was looked at by eye (orientation, colours, no cuts or artefacts); no automated image check was made.

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
  `capture` and `inputd` are compiled inside that chroot (they need libpipewire and libwayland): see the commands in `PLAN-linux-x400.md`.
- `x400/venus/build.sh` -> `out/venus-build/virgl_test_server` (`x400/venus/prepare-virgl.sh` makes `out/virgl` from a checkout of Droid-VM's virglrenderer at `7e17c8e` plus `x400/venus/vkr-dma-heap.patch` and
  `x400/venus/vtest-x400.patch`, both ours).
- `x400/display/build.sh` -> `out/display/{x400-display.dex,libx400display.so}`.

## Run (OneOS's shape: preflight, session, way back)
`x400/phone/deploy.sh` (PC, adb) stages everything in `/data/local/tmp/oneos-x400` (separate from OneOS's own `/data/local/tmp/oneos`;
deleting it removes it all) and pins the files in `stage.sha256`; the session refuses to start if one changed. The image gets
`chcon u:object_r:apex_data_file:s0` (the loop's kernel thread cannot read `shell_data_file`). `--no-image` re-stages only the scripts.

On the tablet, as root:
- `sh /data/local/tmp/oneos-x400/x400_switch.sh [--seconds N] [--cmd 'command inside the chroot']`: preflight first (root, busybox applets,
  staged sums, no session already running, nothing of the stage mounted in init's namespace, MemAvailable >= 1500 MiB, `/dev/dma_heap/system`,
  Samsung's `libvulkan.so`, a volume-key device); then the session in its own mount namespace, the display layer, and a watcher.
  With `SHOT=/path.png` it also takes a `screencap` 12 s after the first frame. `--seconds 0` runs until the way back.
- Way back: `sh /data/local/tmp/oneos-x400/x400_return.sh`, or hold **Volume Up + Volume Down for 3 s** (`x400_keywatch.sh` only reads the key
  devices, never grabs them). The return removes the layer, kills every process whose root is the chroot (it shares Android's PID namespace;
  `inputd` dying gives the touchscreen back), stops the Venus server, restores the stay-on setting, and checks and writes
  `return.outcome`: no chroot process, no stage mount in init, no loop device, init's mount count as before. It is safe to run twice or with no session.
- One UI keeps running the whole time; the side keys (event0..event4) are never opened for input.

## Facts that cost time
- `VN_DEBUG=vtest` is mandatory (without it Mesa tries the real DRM node first and finds no GPU) and so is `VN_PERF=no_fence_feedback,no_semaphore_feedback,no_query_feedback` (with
  fence feedback `vkWaitForFences` hangs; the cause is not found).
- Stock vtest does not import dma-bufs (`has_dma_buf_import = false`): `VCMD_RESOURCE_IMPORT_FD` (id 64) and a patched guest `libvulkan_virtio` add it.
- The host-visible memory comes from `/dev/dma_heap/system-uncached` by default (`X400_DMA_HEAP` overrides it): with the cached `system` heap the CPU read-back of a texture varied between runs
  (and the desktop showed black bars and glitches), with `system-uncached` it was stable; it also makes KWin's read-back slower.
- The tablet's kernel (5.15) has no `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE`; there is no implicit dma-buf synchronization.
- KWin 6.6.7 only allows screencast with OpenGL compositing, so QPainter is refused (llvmpipe is the fallback when Zink does not come up).
- A swapchain on the layer needs `preTransform` IDENTITY (the surface reports ROTATE_90 as current), and a surface used with `ANativeWindow_lock` cannot get a Vulkan one afterwards.
- With the Linux session up the USB connection to the PC dropped twice (once during a `simpleperf report` on the tablet); returning with Vol+ and Vol- brings it back. The cause is not known.
- Without systemd, Plasma's services are started by hand: `kglobalacceld`, `kded6`, `kactivitymanagerd`, `kbuildsycoca6`, then `plasmashell`.
- The chroot shares the PID namespace with Android: `plasma.sh` kills only processes whose root is the chroot.
- The screen must be awake (`svc power stayon true`) or `screencap` is black; the panel is landscape.
