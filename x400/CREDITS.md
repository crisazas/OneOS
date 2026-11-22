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
| `x400/venus/build.sh`, `x400/venus/stub/` | `runtime/venus/build-host.sh`, `build-probe.sh`, `stub/` | The list of virglrenderer sources and compiler flags that build the Venus core (`src/venus/vkr_*`) with the NDK and no meson or epoxy. Adapted. | none in the file |
| `x400/venus/vtest-x400.patch`, and the server built from it | `runtime/venus/virglrenderer-dma-heap.patch` | Our patch applies on top of OneiOS's patch, which gives host-visible memory from `/dev/dma_heap/system` where Samsung's driver exports no dma-buf. The patch itself is not copied into this repository: `x400/venus/build.sh` expects it in `out/virgl`. | **none in the file** (see below) |
| the Venus approach as a whole | `docs/gpu.md`, `runtime/venus/venus-host.c` | Using virglrenderer's Venus core on top of Samsung's Vulkan as the host side of the GPU. | `venus-host.c`: `SPDX-License-Identifier: MIT` |

OneiOS's own README says the project is derived from the Apple Public Source License 2.0 (`LICENSE.APSL`) and MIT (`LICENSE`).
Its repository has no top-level license file in the revision read, so each file's own header is what was followed. **The dma-heap patch has no
license line**: its terms are unknown to the author of this branch, and it must not be redistributed from this repository until its authors say.

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
