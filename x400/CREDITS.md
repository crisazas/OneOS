# Credits for the x400 additions

This branch (`x400`) adds a Linux desktop guest for the Galaxy Tab S10 Lite (SM-X400) to OneOS. It stands on other people's work.
OneOS's own credits are in the top-level [`CREDITS.md`](../CREDITS.md) (OneLineage by WeWaffles, MysticGSI, and the rest) and are not repeated here.
The code of this branch that is ours is under the Apache License 2.0 like the rest of OneOS (`LICENSE`).

## OneiOS (OneiOS-A56), by Akshay CM (akshaynexus), Musicalista and Nguyễn Tuấn Nghĩa

<https://github.com/Musicalista/OneiOS-A56> (the project that runs iOS next to One UI on a rooted Galaxy; it is built on OneOS too).
Its authors, by commits of the revision read (`f95de4b`): Akshay CM / akshaynexus (about 530), Musicalista (84), Nguyễn Tuấn Nghĩa (18).

What this branch took from it, file by file. "Written here" means that no text was copied: the approach was read and rewritten.

| Here | From OneiOS | How | Terms in OneiOS's file |
| --- | --- | --- | --- |
| `x400/display/X400Display.java`, `x400/display/jni.c` | `runtime/display/java/OneiOSDisplay.java`, `runtime/display/src/jni.c` | The way a layer above One UI is made without system_server: a parentless `SurfaceControl` BLAST layer by reflection, a `BLASTBufferQueue`, and rows copied into the window with `ANativeWindow_lock`. Written here, much smaller (a file instead of its socket protocol). | `SPDX-License-Identifier: MIT` |
| `x400/venus/build.sh`, `x400/venus/stub/` | `runtime/venus/build-host.sh`, `build-probe.sh`, `stub/` | The list of virglrenderer sources and compiler flags that build the Venus core (`src/venus/vkr_*`) with the NDK and no meson or epoxy. Adapted. | none in the file (see below) |
| `x400/venus/vtest-x400.patch`, and the server built from it | `runtime/venus/virglrenderer-dma-heap.patch` | Our patch applies on top of OneiOS's patch, which gives host-visible memory from `/dev/dma_heap/system` where Samsung's driver exports no dma-buf. The patch itself is not copied into this repository: `x400/venus/build.sh` expects it in `out/virgl`. | **none in the file** (see below) |
| the Venus approach as a whole | `docs/gpu.md`, `runtime/venus/venus-host.c` | Using virglrenderer's Venus core on top of Samsung's Vulkan as the host side of the GPU. | `venus-host.c`: `SPDX-License-Identifier: MIT` |

### Licensing of OneiOS: what was checked (2026-11-22)

- OneiOS has **no license file**. The README of `akshaynexus/OneiOS` says "No license file yet - all rights reserved until the owner adds one"
  (the same text is in the revision read, `f95de4b`). The README of the fork `Musicalista/OneiOS-A56` says "Derived from the Apple Public Source
  License 2.0 (`LICENSE.APSL`) and MIT (`LICENSE`)", but both files return 404 on GitHub: the statement has no file behind it.
- So only what carries its own header is licensed: `OneiOSDisplay.java` and `venus-host.c` have `SPDX-License-Identifier: MIT`.
  `runtime/venus/build-host.sh` and `virglrenderer-dma-heap.patch` have no license line.
- **The dma-heap patch** changes `src/venus/vkr_device_memory.c`, `vkr_physical_device.c` and `vkr_physical_device.h`, which are
  `Copyright 2020 Google LLC, SPDX MIT`, and adds about 55 lines whose author and terms are not stated anywhere. MIT lets the patched files
  be used and redistributed with their notice; it says nothing about the new lines. **This branch does not contain the patch and must not
  redistribute it until its author says under which terms it is published.** Using it on one's own machine is not in question.
  The author of those lines could not be determined (in OneiOS it is a bare file without history).
- The `virglrenderer` that OneiOS builds is the fork of Droid-VM (<https://github.com/Droid-VM/virglrenderer>, `7e17c8e`, commits by HuJK).
  Upstream-inherited files (the `vkr_*` core) stay MIT; what Droid-VM wrote is GPL-2.0-or-later with additional permissions
  (`LICENSING.md`, `ADDITIONAL-PERMISSIONS` there), except `drm2kgsl_renderer.{c,h}` (MIT).
- `x400/venus/vtest-x400.patch` is this branch's own work and only touches virglrenderer's vtest files (MIT upstream).
- To remove the dependency: write the dma-heap allocation again on the MIT `vkr_*` code. Not done.

## virglrenderer and Mesa

The server is `virgl_test_server` of virglrenderer (<https://gitlab.freedesktop.org/virgl/virglrenderer>, MIT), at the revision OneiOS uses
(`7e17c8e`), with the Venus core `src/venus/vkr_*` and Mesa's `util` files it carries. The guest side is Mesa's Venus driver
(`libvulkan_virtio`, MIT), as packaged by Ubuntu. The vtest protocol is virglrenderer's.

## KDE, PipeWire, Wayland, Ubuntu

KDE Plasma 6 and KWin (GPL/LGPL, <https://kde.org>), PipeWire and WirePlumber (MIT), Wayland (MIT), Ubuntu 26.04 base (<https://ubuntu.com>).
`x400/display/capture.c` and `x400/display/inputd.c` use KDE's `zkde_screencast_unstable_v1` and `org_kde_kwin_fake_input` protocol
descriptions (from `plasma-wayland-protocols`, installed in the image and not copied here).

## OneOS, OneLineage

The session and return scripts that the Linux guest runs under are OneOS's, which started from WeWaffles' OneLineage (see `../CREDITS.md`
for its terms: they permit OneOS's publication, not reuse of the OneLineage-derived files elsewhere).
