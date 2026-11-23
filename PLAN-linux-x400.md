# Plan: full Linux on the SM-X400 on top of One UI's kernel

Folder: `~/linux-x400` (clean clone of `fahadalijaved/OneOS`, `bc10b21` = 0.5.34). Nothing is written to `~/SM-X400/oneios`.
Status: **plan only; the tablet has not been touched.**

## Idea
OneOS boots a second Android as a container on top of Samsung's kernel and returns to One UI without flashing.
Here the guest would be a Linux rootfs (Debian/Ubuntu arm64) instead of an Android: there is no emulation, it is a
`chroot`/container on the 5.15 kernel that is already running. If the chroot does not produce a picture, we stop.

## Measured facts (2026-11-22, tablet SM-X400, Exynos 1380 `s5e8835`, Mali-G68 `0x9204`, kernel 5.15)
- Kernel: `CGROUPS NAMESPACES NET_NS SECCOMP BINFMT_MISC TUN DRM EXT4 F2FS FUSE OVERLAY_FS` = y.
  `USER_NS`, `PID_NS`, `DEVTMPFS` did not show up in the reading: **unchecked**.
- `/dev/dri/card0` and `/dev/mali0` exist. `MemAvailable` ≈ 4311 MiB with One UI stopped (of 5704).
- `flash.locked=1`, root is DFroot (in RAM, lost on reboot). Nothing needs to be unlocked.

## What OneOS already solves (reusable, in `switcher/assets/`)
`oneos_switch.sh` (4701 lines), `return_to_oneui.sh` (in-place return without a reboot, with a failsafe), guards that
keep the guest away from One UI's data, `zygote` shutdown, emergency key Vol+ and Vol- for 3 s.
Not reusable as is: `guest_prepare.sh` / `guest_start.sh` / `guest_props.sh` assume an Android (zygote,
system_server, servicemanager), and the app rejects anything that is not in its compatibility table.

## Phases (one per session; each ends with the return to One UI checked)
0. **Reading, no tablet.** Read `oneos_switch.sh` and `return_to_oneui.sh`: which steps belong to the Android guest and which
   to the switch/return. Output: a list of what is cut and what is kept. Check whether `docs/exynos*.md` cover the X400.
1. **Kernel probe (read-only).** `USER_NS`, `PID_NS`, `devtmpfs`, `/dev/dri/card0` (is KMS mode usable without
   `surfaceflinger`?), `/dev/input`, `/dev/mali0`. Named nodes; no tree walks.
2. **Rootfs on the PC.** Minimal arm64 `debootstrap` + `weston`/`kmscube`/`mesa` (≥ 26.3 for Panfrost G68) in an
   ext4 image in `~/linux-x400/out/` (git-ignored). No KDE yet.
3. **Chroot without a screen on the tablet.** Mount the image, `chroot`, network ping, `ls`, come back. It checks the
   mount and return mechanism without fighting the screen.
4. **Screen.** Stop `surfaceflinger` only if needed, `kmscube` on `card0`. First decision point:
   if no picture comes out, we stop here. Open question: Samsung's `mali_kbase` (Android) vs Mesa's Panfrost
   need different kernel drivers; with Samsung's kernel only Android's blob/Vulkan may work,
   or software (llvmpipe).
5. **Input and network.** Touch through `/dev/input`, Wi-Fi inherited from the stopped Android.
6. **Desktop.** Lightweight first (LXQt/XFCE); KDE Plasma only if RAM allows it (~1–1.5 GB at idle).

## Risks
- Root is lost on reboot; a failed return forces it to be redone.
- The GPU may end up on llvmpipe (slow) if Panfrost does not hook into Samsung's kernel.
- OneOS warns about data loss if a guest reaches One UI's partitions: the guards must be kept.
- One OneOS task requires removing the screen lock before switching.

## Proposed first step
Phase 0 (read scripts only) and phase 1 (read-only probe). It mounts nothing and does not touch the session.

---
## Results of phases 0 and 1 (2026-11-22, read-only; the tablet did not change, `session: none`)

### Phase 1: the tablet's kernel (`/proc/config.gz`, `/proc/filesystems`)
| Option | Value | Consequence |
|---|---|---|
| `USER_NS`, `PID_NS` | **not present** | no unprivileged containers and no PID namespace of its own: the Linux guest will see Android's processes |
| `DEVTMPFS` | **not present** | `/dev` has to be mounted by hand (tmpfs + `mknod`, or a bind of the host's `/dev`) |
| `SYSVIPC`, `CGROUP_DEVICE`, `SQUASHFS` | not present | rootfs on ext4 (not squashfs); no SysV shared memory |
| `MEMCG`, `UTS_NS`, `NET_NS`, `OVERLAY_FS`, `FUSE`, `F2FS`, `EXT4`, `UNIX98_PTYS`, `INPUT_EVDEV`, `INPUT_UINPUT` | yes | enough for chroot, pty and input |
| `DRM_PANFROST` | **not present** | Mesa's Panfrost has no kernel driver: the GPU is Samsung's `MALI_MIDGARD=m` (`/dev/mali0`) |
| `/dev/dri/card0`, `/dev/mali0` | exist, 0666 | |
| SELinux | Enforcing | the guest needs what OneOS does (`setenforce 0` at the switch) |

### Phase 0: how OneOS isolates the guest
- It only uses **`unshare -m` (mount namespace) + `pivot_root`** (`guest_start.sh:14`, launched with `unshare -m ... guest_start.sh`
  in `oneos_switch.sh:2073`). It uses no PID or user namespace. What it needs from the tablet's kernel **is already there**.
- The rest of the way into the guest is `nsenter -t <pid> -m` and `chroot "$STAGE/rootfs"`.
- `docs/devices.md` lists the SM-X400 (Tab S10 Lite) as "Not yet"; there is no profile for it. The script `oneos_switch.sh` refuses anything that
  fails its preflight checks (`refuse`, lines 17–180), among them `system.raw` in the stage; all of them happen before the first change
  to the phone.

### What changes in the plan
1. **GPU:** Panfrost is ruled out with this kernel. Real options: (a) `llvmpipe` in software, (b) Samsung's Vulkan/GLES
   in `/vendor` (bionic) with `libhybris`, a lot of work, (c) what we already did on iOS (Venus over the host's Vulkan), which is another route.
   Phase 4 becomes **"is llvmpipe enough?"**; it is the first thing to try.
2. **Phase 3** without a PID namespace: `systemd` cannot be PID 1 in a chroot; the desktop has to be started without `init` (a script that
   launches `dbus` + compositor) or with a minimal init.
3. **Phase 2:** rootfs on ext4 (not squashfs).

---
## Decision (2026-11-22): the GPU will be Venus
The Linux guest uses Mesa's `virtio` Vulkan driver (a Venus client) and a `virgl_renderer` on the Android side over Samsung's Vulkan (`/dev/mali0`).
It reuses what was already done in `~/SM-X400/oneios/runtime/venus` (read from there only; nothing is written to that folder). `llvmpipe` is not the main plan.
Still to be measured, without assumptions: how the client connects (vtest socket or equivalent) and whether a rootfs without PID/user namespaces allows it.

## Phase 2 in progress
- `x400/build-rootfs.sh`: Ubuntu Base 24.04.4 arm64 (sha256 checked) + `weston`, `mesa-vulkan-drivers`, `vulkan-tools`, `kmscube`...
  in `out/rootfs` with `qemu-user-static` (installed on the PC with apt). Log: `out/rootfs-build.log`. `out/rootfs.ext4` still has to be packaged.

---
## Results of reading Venus (2026-11-22, read only; no tablet)
- **Client in the rootfs:** `libvulkan_virtio.so` (Mesa 24.04) contains the vtest mode: strings `VTEST_SOCKET_NAME`, `/tmp/.virgl_test`,
  `VN_DEBUG`, "vtest protocol version (%d) too old". It is enabled with `VN_DEBUG=vtest` (to be confirmed when tried). Over a Unix socket: it needs
  neither `virtio-gpu` in the kernel nor DRM in the chroot.
- **Server in the iOS project:** `external/virglrenderer` (`7e17c8e`) ships `vtest/vtest_server.c` with the `--venus` option
  (if built with `venus=true`). But what runs on the tablet is `runtime/venus/build/venus-host` (aarch64 bionic, 2.9 MB, not static;
  `usage: venus-host --fd N`, it uses an already open fd, not a named socket) plus `venus-client`, which uses `socketpair`.
  That is the iOS project's own protocol, not vtest: **it does not work as is for Mesa**.
- **Sources (read with `git show f95de4b:...`, without touching that repo):** `runtime/venus/{venus-host.c,venus_wire.h,build-host.sh,build-probe.sh,
  virglrenderer-dma-heap.patch,stub/}`. `build-probe.sh` builds with NDK 28.2 (aarch64-linux-android30) the `src/venus/vkr_*` core
  without meson or epoxy.
- **`venus-host` does not speak vtest.** Its protocol (`venus_wire.h`) is an `AF_UNIX SOCK_SEQPACKET` with `VW_*` messages and a `memfd` with the guest's
  whole RAM (`VW_HELLO`), designed for `xnuhost`. A Linux with Mesa cannot use it as is.
- **Route chosen to try first:** build `virgl_test_server --venus` (`vtest/vtest_server.c`, the `--venus` option is already in the fork) for
  Android with the same NDK and the dma-heap patch, and expose its socket `/tmp/.virgl_test` in the chroot. Mesa uses `VN_DEBUG=vtest`.
  Without this there is no GPU; it is the first real risk. Alternative: write a small `vtest`→`VW_*` bridge (more work, reuses `venus-host`).
- **Warning:** `~/SM-X400/oneios` is in the middle of a `git rebase -i` (32 done, 395 pending, 24 `AA` files). It is not part of this fork; it is not touched.
  The sources are read from the commit, not from the working tree.

### Cost of the two Venus routes (read, nothing built)
- `vtest_renderer.c` uses 22 functions of the full `virgl_renderer_*` API (init, context, resource, fence, poll, transfer...). Those live in
  `src/virglrenderer.c`, which the iOS project **does not build**: `build-probe.sh` builds only `src/venus/vkr_*` and avoids meson and epoxy.
  Route (a) = build all of `libvirglrenderer` for bionic without GL (meson with epoxy left out): I do not know whether the fork allows it.
- `venus-host.c` (188 lines) already uses only `vkr_renderer_*` (8 calls) and builds with the NDK. Route (b) = write a small `vtest-venus.c`
  (~300 lines: socket `/tmp/.virgl_test`, the `vtest_protocol.h` protocol for the calls Mesa makes with Venus, and `vkr_renderer_*` underneath).
  It reuses what is already proven (`venus-host`, dma-heap, windows). **Recommended**; we have to measure which subset of vtest Mesa asks for.

### Correction (same date): neither (a) nor (b) as they are; (c) is better
- The rootfs ships **Mesa 25.2.8** (not 24.04). Its `vn_renderer_vtest.c` uses 12 vtest commands: protocol, capset, `CONTEXT_INIT`,
  `RESOURCE_CREATE_BLOB` (shmem through `SCM_RIGHTS`), `RESOURCE_UNREF`, `BUSY_WAIT`, `SUBMIT_CMD2` (per ring) and the 5 `SYNC_*` ones (timelines).
  The "~300 lines" for (b) was optimistic: the syncs and the multi-ring support are the delicate part.
- (a) collides with GL: `meson.build:71` requires epoxy unconditionally and `src/virglrenderer.c` includes `epoxy/gl.h` and calls `vrend_` 73 times.
- **(c):** build the fork's `vtest/vtest_server.c` + `vtest_renderer.c` (2141 lines, it already implements syncs with eventfd and blobs) with the NDK,
  and instead of `src/virglrenderer.c` give it a thin `virgl_renderer_*` → `vkr_renderer_*` layer for Venus only (22 functions; the GL ones,
  transfers and virgl caps return an error). It uses the protocol that is proven upstream with Mesa and the `vkr_*` core that already builds and runs on the tablet.

### Option (c) built (2026-11-22)
- `x400/venus/build.sh` builds with NDK 28.2 `out/venus-build/virgl_test_server` (aarch64 bionic, 2.9 MB; only `liblog libdl libm libc`).
  Sources: `out/virgl` = `virglrenderer` `7e17c8e` (from `git archive`) + `virglrenderer-dma-heap.patch` (from `f95de4b`) + `x400/venus/vtest-x400.patch`.
- The fork already shipped `src/venus/vkr_virgl_adapter.c` (the `virgl_context` → `vkr_renderer_*` bridge): **the layer for (c) already existed**.
  The only things added: `x400/venus/stub/epoxy/{gl,egl}.h` (types only), `x400/venus/vrend_stub.c` (37 GL/DRM functions that fail with a message)
  and `--venus` = `VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL` (no EGL).
- Tested: it starts on the tablet (`./x400vt --help` prints the usage) and was deleted. **Not tested:** that `--venus` brings Vulkan up or that a client connects.
- Testing with a client needs the rootfs on the tablet (phase 3): Mesa's `vulkaninfo` is glibc and only runs inside the chroot.

---
## Phase 3 done (2026-11-22): Mesa in the chroot sees the Mali-G68 through Venus
With One UI running, no session, nothing stopped. Everything inside `busybox unshare -m` (One UI does not see the mounts).
- Rootfs: `/data/local/tmp/x400-rootfs.ext4` mounted `loop,ro` on `/data/local/tmp/x400root`, plus tmpfs on `/tmp` and `/run`, bind of `/dev`, proc and sysfs.
- Server outside the chroot: `ANDROID_EMU_VK_LOADER_PATH=/system/lib64/libvulkan.so x400vt --venus --no-fork --socket-path=<root>/tmp/.virgl_test`.
- Client inside: `VN_DEBUG=vtest VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json vulkaninfo --summary` →
  `deviceName = Virtio-GPU Venus (Mali-G68)`, `driverName = venus`, `apiVersion 1.3.219`, `rc=0`. Repeated with the binary without traces: same.
- After each test: 0 mounts, 0 loops, 0 processes, 0 Mali reset messages in `dmesg`.
Three failures, in this order, each with its cause:
1. **SELinux:** the loop's `kworker` thread (`u:r:kernel:s0`) cannot read `shell_data_file`. Fix: `chcon u:object_r:apex_data_file:s0` on the image.
2. **`vkr_renderer_init` requires `ASYNC_FENCE_CB`** (and `vtest_server` did not pass it): "failed to initialise renderer". One-line patch.
3. **Hang in `SYNC_WAIT`:** with `ring 0`, Venus retires the fence *inside* `virgl_renderer_context_create_fence`, but `vtest_submit_cmd2_batch`
   added the submission to the list *after* calling it: the signal was lost and the client waited forever. Fix: add to the list first.
`x400/venus/vtest-x400.patch` holds 2 and 3 (on top of `7e17c8e` + dma-heap). Without `VN_DEBUG=vtest`, Mesa first tries the real DRM (`exynos-drmdpu`) and fails:
the variable is mandatory (or link `VK_ICD`/the chroot's environment).
Measured and not measured: **there is no performance data at all**; only the device was enumerated. Nothing was drawn.

---
## Phase 4a done (2026-11-22): Venus RENDERS on the Mali-G68 from the chroot (no screen)
`x400/test/vk-offscreen.c` (built on the PC with `aarch64-linux-gnu-gcc` against the rootfs's `libvulkan.so.1`): draws a triangle into a 512x512 image,
copies it to a visible buffer and checks two pixels. Result on the tablet, One UI running:
`center 255 128 0 255`, `corner 26 51 230 255`, **PIXELS OK**; 1 frame 10 ms (includes creating everything), **500 frames in 0.45 s = 1119 fps** (≈0.9 ms/frame
of a trivial 512x512 scene with one wait per frame). With 1000 and 300: 1099 and 1148 fps. No Mali resets (`dmesg`).
Scripts in `x400/phone/` (`run.sh` = everything inside `busybox unshare -m`; `chroot-venus.sh`; `check-clean.sh`; `force-clean.sh`). Usage: `NF=500 sh run.sh`.
### Four things that matter
1. **`VN_PERF=no_fence_feedback` has to be set.** With Mesa's *fence feedback* (the default) `vkWaitForFences` hangs ("stuck in fence wait") and the
   client closes the connection. **The cause has not been found**: the feedback makes the client read the fence state from shared memory, and what
   the GPU writes there does not reach it (suspicion: the `host visible` memory comes from the dma-heap with the patch, without coherence; not measured). It is a workaround, not a fix.
2. **Without a subpass dependency to `TRANSFER` the pixels come out as 0:** it was a bug in my test (Mali/Venus does not insert the barrier by itself). `vkCmdFillBuffer` and
   `vkCmdClearColorImage` + `CopyImageToBuffer` worked; the render pass without the dependency did not. It is not a Venus bug.
3. **Mounts pile up if `unshare -m` is not used:** in init's mount namespace, my 18 tests left 81 mounts and 12 loops, and several
   `umount`s failed with EINVAL (`/vk-offscreen//deleted`, bind of a replaced file). I cleaned them up with `umount` one by one (the last one first) and
   `losetup -d`; init is back to 175 mounts. **I caused this** by launching `chr.sh` without `unshare`: `run.sh` must always be used. Check with `check-clean.sh`.
4. **`weston`/`kmscube` were not tried on the screen**: what was done is only compute and copy to memory; no frame has been seen on the tablet.

---
## Phase 4b (2026-11-22): the screen, architecture (b) chosen and layer tested
Subagent report (sources: KWin 6.6, Mesa main): **A) (b)** a headless compositor in the chroot + an Android helper with a SurfaceControl layer (do not stop
surfaceflinger: direct KMS forces stopping HWC and risks the DSI panel and the root); **B) KWin 6.6.x `--virtual`** (Noop session, no logind), QPainter if there is no render
node; Zink only if Venus passes the requirements (do not pin Plasma 6.8: it drops QPainter); output through ScreenCast/PipeWire; **C) input**: `--virtual` does not create libinput,
own daemon that reads event8/event10 and injects them with `org_kde_kwin_fake_input` (`KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`).
Measured: Venus exposes `VK_KHR_maintenance4`, `dynamic_rendering`, `synchronization2`, `extended_dynamic_state{,2}`, `line_rasterization`, `custom_border_color`,
`provoking_vertex`, `transform_feedback`, `physical_device_drm`, `image_drm_format_modifier`; **`VK_KHR_maintenance5` and `VK_EXT_robustness2` are missing** (Zink GL 2.1 requirements). apiVersion 1.3.219.
Own helper `x400/display/` (X400Display.java + jni.c + `fb.h`; build with `build.sh`; `display.sh`): reads a shared file (header + BGRA, `seq` at the end) and
copies it to a BLAST layer. **Tested on the tablet with One UI running**: moving pattern at 2112x1320, capture with `screencap` = gradient + white bar visible, 34 fps
(300 frames in 8.9 s; the CPU copy and the `pattern` with 16 ms of `usleep` are the limit, not tuned), layer removed at the end (SIGTERM).
Three findings: (1) the screen was in `Dozing` and came out black: it has to be woken up (`KEYCODE_WAKEUP`, `svc power stayon true` during the test and set back to `false`);
(2) the screen is **landscape, 2112x1320** (`wm size` 1320x2112 with rotation 1), the layer must be 2112x1320; (3) **the keyguard is active** (`isKeyguardShowing=true`).

---
## Milestone (2026-11-22): KDE Plasma 6.6.7 VISIBLE on the tablet's screen
Test on the tablet only, One UI running. `x400/phone/run-desktop.sh` → `plasma.sh` (mounts `x400-plasma.ext4` `loop,ro` in an `unshare -m`, tmpfs on `/tmp` and `/run`,
Venus server outside, and `session-inner.sh` inside) + `display.sh` (SurfaceControl layer 2112x1320). Capture with `screencap` (`out/display/desktop-first.png`):
Plasma background, taskbar, clock 18:46 and date. First frame 30 s after startup. Session closed cleanly: 0 processes with root x400root, 0 mounts, `MemAvailable` ~3009 MiB.
Chain: KWin 6.6.7 `--virtual` with **llvmpipe** (`KWIN_COMPOSE=O2`, `LIBGL_ALWAYS_SOFTWARE=1`) → `zkde_screencast` → PipeWire → `capture` → shared file → layer.
What unblocked the capture: **`wireplumber`** (without it the stream stayed `paused`: nobody linked `kwin_wayland:output_1` to `x400-capture:input_1`) and llvmpipe instead of QPainter
(KWin 6.6.7 only allows screencast with `OpenGLCompositing`). Plasma without systemd: `kglobalacceld`, `kded6`, `kactivitymanagerd` started by hand, `kbuildsycoca6` before `plasmashell`.
Cleanup: the chroot shares Android's PID space; `plasma.sh` only kills processes whose root is the chroot (I tried without that first and 3 sessions were left alive).
### What is NOT there
- **GPU in the composition:** KWin uses llvmpipe (CPU). Venus only serves Vulkan apps. Zink would require `maintenance5` and `robustness2`, which Venus does not expose.
- **Touch input:** `x400/display/inputd.c` written and built (aarch64); first test in progress (touch injected with `sendevent`).
- **Stability and fps:** not measured over a long session. Real fps, CPU and heat are missing.

---
## Owner's decisions (2026-11-22)
- **One UI is stopped**, as OneOS does: the Linux guest is started and returned with OneOS's flow (framework stop, Vol+/Vol- keys, watchdog, return), not with my scripts `x400_switch.sh`/`x400_return.sh`, which stay as scaffolding.
- **The own dma-heap patch stays** (`x400/venus/vkr-dma-heap.patch`, MIT), already measured as good as OneiOS's.
- **Test with a real finger (first):** the desktop shows, touch responds and the touch lands where the finger is put (`inputd`'s rotation and scale are right at 2112x1320).
  One UI kept answering to touch although `inputd` said it had taken `event8` with `EVIOCGRAB`: **unexplained** (the log gave no error). With One UI stopped it does not matter.
### Measured the same day (llvmpipe, no GPU in the composition)
- Idle desktop: 4.6 fps of capture over 50 s (just the startup). With a `konsole` scrolling text: 5.8 fps, `kwin_wayland` 31 % of one core (8 cores), `konsole` 3.9 %, `capture` 0.5 %.
  Total CPU is almost free, so the limit is not the global CPU: still to look at `Cpus_allowed`/cpuset, the governor, KWin's threads and `/dev/pts` in the chroot (measurement in progress).
- Zink does not start: Samsung's Vulkan does not offer `VK_EXT_robustness2` (102 extensions) and Zink requires `nullDescriptor` (`zink_screen.c:3458`).

---
## GPU: what was measured on Zink + Venus, and what other projects do (2026-11-22)
### Measured on the tablet (chain of failures, each with its cause read in the code or in a backtrace)
1. Mesa 26's Zink requires `nullDescriptor` → **Debian's Mesa 25.0.7** (`x400/build-mesa25.sh`) does not require it and reaches OpenGL core 3.1 over Venus.
2. `failed to choose pdev`: Zink 25.0 picks the Vulkan device by `VkPhysicalDeviceDrmPropertiesEXT.renderMajor/Minor`; Venus announces 1/1 → own Vulkan layer
   (`x400/venus/layer`) that announces 226:128 (`renderD128`). Solved.
3. KWin crashes in `dri2_from_dma_bufs` (null pointer in `libvulkan_virtio`): **vtest has no dma-buf import** (`create_from_dma_buf = NULL`).
   → new command `VCMD_RESOURCE_IMPORT_FD` = 64 (25..38 are already used by Mesa 26 for DRM syncs and `RESOURCE_EXPORT_FD`), server `x400/venus/vtest-import.patch`
   and client `x400/venus/guest-mesa/` (Mesa 26.0.8 rebuilt, `build.sh`). Overcome: KWin gets past that.
4. Next crash: `zink_screen_import_dmabuf_semaphore` calls `vkGetSemaphoreFdKHR`, which is null because Venus only announces `VK_KHR_external_semaphore_fd` with
   `has_external_sync` and vtest leaves it `false`. Zink 25.0.7 does not check that it exists. Once it is announced, the next hang appears (point 6).
5. **The tablet's 5.15 kernel has no `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE`** (`ENOTTY`, measured with a program in `/data/local/tmp/dmasync`). Not even a real sync-file
   would synchronize a dma-buf; Zink ignores it if it fails. Without implicit synchronization, the wait has to be done by the CPU.
6. KWin hangs (`vn_WaitSemaphores`, `poll()` on the `export_syncobj` fd): solved without touching the server. `VN_PERF=no_fence_feedback,
   no_semaphore_feedback,no_query_feedback` (the feedback reads a slot in shared memory) and `export_syncobj` returning an already readable `eventfd`
   (the real fd of `VCMD_SYNC_WAIT` was signalled behind the work that was waiting for it). The 5.15 kernel has no `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE`.
   The vtest server also did not set `--multi-clients`: it only served one client at a time (KWin, kded6, plasmashell...).
7. **Cause of every Zink draw writing nothing** (the missing one): Samsung's Vulkan (Mali-G68) applies `vkCmdSetDepthCompareOp` **even though the
   depth test is disabled**. Zink emits `vkCmdSetDepthCompareOp(NEVER)` with `vkCmdSetDepthTestEnable(FALSE)` on every draw, and with NEVER or
   LESS no fragment passes. Found by removing one dynamic state at a time from Zink's pipelines (`X400_DYN_OFF`): only `DEPTH_COMPARE_OP` changed the
   result. Reproduced without Zink with `x400/test/vk-dynrender.c DEPTHOP=n` (0 and 1 do not draw; 2, 3 and 7 do). Fix: the layer `x400/venus/layer` sends
   `ALWAYS` to the driver while the test is off. With it, `gl-readback`, `gl-vertexid` and KWin draw.
   Ruled out along the way (≈30 tests, all with `PIXELS OK` on Samsung's Vulkan): dynamic rendering, `extended_dynamic_state` 1 and 2, vertex
   buffer, `BindVertexBuffers2`, Zink's SPIR-V, `LOAD` after a clear, barriers, several command buffers, timeline semaphore, "mesa zink" engine,
   mutable image, suballocation, memory, descriptors, `vkCreateDevice` features. An occlusion test (`any_samples_passed`) turned out to be **not
   reliable**: it gives 0 on lavapipe too, which draws correctly.

### What other projects do (read in their repositories; none of this was tried here)
- **Venus over vtest on Android** (DroidDeck #165, gogetwoo, Pixel 10/PowerVR): same obstacles —no dma-buf import in vtest, no `external_semaphore_fd`—;
  they made explicit synchronization optional and conclude that dma-buf import is the main cost of this route.
- **libhybris** (ARLinux, taowen; Mali G1-Ultra): a glibc process loads the vendor's Vulkan; the native driver imports dma-buf. It is the most used route today on Mali.
- **PanVK over kbase** (JimVulkan/mali-panvk: Mali-G68 on Galaxy Tab S9 FE, MIT; SaicharanKandukuri/tensor-g1-proot-gpu: Ubuntu in PRoot, G78): Mesa's PanVK
  talks directly to `/dev/mali0`. To present they use memory from `/dev/dma_heap/system` imported into kbase (`KBASE_IOCTL_MEM_IMPORT` type UMM) and wait for the GPU on
  the CPU before presenting: **it is the same thing we already have with `vkr-dma-heap.patch`**. Limits: their WSI is X11 with a private Termux:X11 protocol (KWin-Wayland does not
  have it), `kbase_kmod_bo_export` is not implemented, and with Zink they only reach OpenGL 2.1 (KWin 6 asks for 3.1). They warn about kernel reboots with kbase in containers.
  None of the devices in the chroot guide is Exynos/G68.
