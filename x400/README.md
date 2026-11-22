# x400: Ubuntu 26.04 + KDE Plasma 6 on the Galaxy Tab S10 Lite (SM-X400), next to One UI

A Linux desktop that runs as a chroot on One UI's own kernel (5.15), with One UI still running. Nothing is flashed.
Root is KernelSU/DFroot (lives in RAM: a reboot loses it). Everything the session mounts lives in a private mount namespace
(`busybox unshare -m`), so One UI never sees it and it disappears when the session ends.

See [CREDITS.md](CREDITS.md): this branch builds on OneOS, and on OneiOS (Akshay CM, Musicalista and contributors) for the display layer and the Venus build.

## How a frame reaches the panel
`kwin_wayland --virtual` (Plasma 6.6.7, llvmpipe) -> `zkde_screencast` -> PipeWire (+ wireplumber) -> `capture` -> a shared file
(`fb.h`) -> `X400Display` (a `SurfaceControl` layer made from `app_process`, above One UI). Touch: `/dev/input/event8` ->
`inputd` -> `org_kde_kwin_fake_input` (rotated: the panel is landscape 2112x1320, the touch reports portrait 1320x2112).
GPU for Vulkan apps: Mesa's Venus client in the chroot -> vtest socket -> `x400vt` (virglrenderer's Venus core, built with the NDK,
outside the chroot) -> Samsung's Vulkan on the Mali-G68.

## Build (on the PC; needs sudo, qemu-user-static, gcc-aarch64-linux-gnu, libvulkan-dev, the Android NDK 28.2 and SDK build-tools)
- `x400/build-rootfs-plasma.sh` -> `out/rootfs-plasma.ext4` (6 GB, Ubuntu 26.04.1 arm64 + Plasma + PipeWire + wireplumber).
  `capture` and `inputd` are compiled inside that chroot (they need libpipewire and libwayland): see the commands in `PLAN-linux-x400.md`.
- `x400/venus/build.sh` -> `out/venus-build/virgl_test_server` (inputs in `out/virgl`: virglrenderer `7e17c8e` + the dma-heap patch +
  `x400/venus/vtest-x400.patch`).
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
- `VN_DEBUG=vtest` is mandatory (without it Mesa tries the real DRM node first and finds no GPU) and `VN_PERF=no_fence_feedback` too (with
  fence feedback `vkWaitForFences` hangs; the cause is not found).
- vtest does not import dma-bufs (`has_dma_buf_import = false`): Wayland/dmabuf WSI clients on Venus fail ("importing the supplied dmabufs failed").
- KWin 6.6.7 only allows screencast with OpenGL compositing, so the compositor runs on llvmpipe; QPainter is refused.
- Without systemd, Plasma's services are started by hand: `kglobalacceld`, `kded6`, `kactivitymanagerd`, `kbuildsycoca6`, then `plasmashell`.
- The chroot shares the PID namespace with Android: `plasma.sh` kills only processes whose root is the chroot.
- The screen must be awake (`svc power stayon true`) or `screencap` is black; the panel is landscape.
