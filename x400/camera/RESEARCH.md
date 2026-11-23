# G5 cameras: research and design (read-only phase)

Device: SM-X400, kernel 5.15.197-android13, Android 17 / SDK 37, SELinux **Permissive** (`getenforce`), root shell context `u:r:ksu:s0`.
Nothing was stopped or restarted: cameraserver (pid 24135) and the camera provider (pid 24132) are the same pids before and after every test.
No camera device was opened (`ACameraManager_openCamera` is never called); no `/dev/video*` was opened by anything I ran.

## 1. What the tablet has

Sensors come from the device tree (`/proc/device-tree`), the Android HAL reports the same ones:

```
$ ls /proc/device-tree | grep -E "is_sensor|camera"
is_sensor0@15030000 ... is_sensor4@15070000   (0 and 1 status=okay, 2-4 disabled)
is_sensor_gc5035@6E     position=1 (front)  sensor_id=0xd2  sensor_name=GC5035
is_sensor_imx355@34     position=0 (rear)   sensor_id=0x80  sensor_name=IMX355
```

The Android camera2 view (output of `probe/camprobe`, full text in `probe/camprobe.out`):

| id | facing | sensor orient. | pixel array | physical mm | focal mm | focus | flash | HW level | fps ranges |
|---|---|---|---|---|---|---|---|---|---|
| 0 | BACK | 90 | 3264x2448 | 3.656x2.742 | 2.91 | AF (min dist 10) | no | LIMITED | 15-30 |
| 1 | FRONT | 270 | 2576x1932 | 2.885x2.164 | 2.12 | fixed | no | LIMITED | 15-30 |
| 2 | FRONT | 270 | 2576x1932 | identical to 1 | | | | | |

- Cameras: 3 ids, 2 physical sensors. Id 2 has characteristics byte-identical to id 1 (checked with `diff`); I did not find out why the HAL lists it (probably a Samsung vendor alias). Treat 0 and 1 as the real ones.
- `ACAMERA_REQUEST_AVAILABLE_CAPABILITIES` = `0 6 19` = BACKWARD_COMPATIBLE, BURST_CAPTURE, STREAM_USE_CASE (by the NDK enum; no RAW, no manual sensor).
- Output streams (id 0): PRIVATE (0x22) and YUV_420_888 (0x23) from 3264x2448 down to 176x144, includes 1920x1080, 1280x720, 640x480; JPEG (0x100) up to 3264x2448. Id 1: same list, max 2576x1932, plus 2560x1440 and 1920x1920.

Provider and HAL (what is installed and alive):

```
$ getprop | grep -i camera
[init.svc.cameraserver]: [running]
[init.svc.sec-camera-provider]: [running]
[vendor.camera.unihal.enable]: [true]
$ service list | grep -i camera
2  android.frameworks.cameraservice.service.ICameraService/default
5  android.hardware.camera.provider.ICameraProvider/internal/0
41 media.camera: [android.hardware.ICameraService]
$ cat /vendor/etc/init/vendor.samsung.hardware.camera.provider-service_64.rc
service sec-camera-provider /vendor/bin/hw/vendor.samsung.hardware.camera.provider-service_64
    interface aidl android.hardware.camera.provider.ICameraProvider/internal/0
    user cameraserver   group audio camera input ... vendor_secdir
$ cat /proc/<provider>/attr/current ; cat /proc/<cameraserver>/attr/current
u:r:hal_camera_default:s0 / u:r:cameraserver:s0
$ ls /vendor/lib64/hw | grep camera
camera.s5e8835.so   camera.unihal.default.so
$ ls /vendor/lib64 | grep -E "samsungcamera|camera2ndk"
libsamsungcamerahal.so  libsamsungcamerahwl_impl.so  libsamsungcamerahalutils.so  libcamera2ndk_vendor.so
```

`/vendor/etc/camera*` does not exist (no config files). `camera.s5e8835.so` contains the sensor driver tables (IMX355, GC5035, HI1336, ...).
`dumpsys media.camera` does not work from here (see 2B): `Permission Denial: can't dump CameraService from pid=..., uid=0`, and without the stubs it times out (`*** SERVICE 'media.camera' DUMP TIMEOUT (10000ms) EXPIRED ***`).

Kernel nodes (`for d in /sys/class/video4linux/video*: cat name`):

```
Exynos IS (the camera ISP) : video100 common | video101 exynos-is-ss0 | video102 exynos-is-ss1
                             video110-113 exynos-is-3indexs | video120 byrp | video130 rgbp | video140 mcfp
                             video150 yuvp | video160 mindexs | video170 lmeindex | video240-242 pindexs
                             video210-213 / video214-217 exynos-is-ssdevice_idvcvc (sensor virtual channels of ss0 / ss1)
Codecs / m2m               : video6-11 s5p-mfc-* (mfc), video12 exynos-jpeg.0, video50 exynos5-scaler:m2m, video155 pablo-gdc:m2m
```

- `/dev/media*` does **not exist** (no media controller node), so there is no media graph to configure.
- Owners: `video100,101,102,110,210` are `cameraserver:camera` mode 0660, label `video_device`; `video155` is `cameraserver:drmrpc`. The MFC ones belong to mediacodec.
- `/sys/devices/platform/15430000.exynos_is`, `15030000.is_sensor0`, `15060000.is_sensor1`: the Exynos IS (Pablo) ISP and the two CSI sensor interfaces.
- Kernel config (`zcat /proc/config.gz`): `CONFIG_VIDEO_DEV=y`, `CONFIG_VIDEO_V4L2=y`, `CONFIG_CAMERA_VENDER_MCD_V2=y`, `CONFIG_EXYNOS_DEVICE_CAMERA_MODULE=y`, `CONFIG_USB_VIDEO_CLASS=y` (a USB webcam would just work), `CONFIG_V4L2_FLASH_LED_CLASS` unset. **No v4l2loopback**: `zcat /proc/config.gz | grep -i LOOPBACK` shows only VSOCKETS_LOOPBACK/SPI test; `/vendor_dlkm/lib/modules/modules.load` was not checked for it; `CONFIG_MODULE_SIG=y` with `MODULE_SIG_PROTECT`, so building one out of tree needs Samsung's signing key or a different path. Also `CONFIG_UDMABUF` is not set; `/dev/dma_heap/{camera,camera-uncached,system,...}` exist (root:camera / system).
- Rear flash: none; no media/IR.

## 2. Routes to Linux

### A. Direct V4L2: not usable
The nodes are the Exynos "Pablo" IS driver (`exynos-is-*`, names above) with Samsung private ioctls and firmware-in-the-loop control (sensor power, CSI, 3A and the ISP pipeline are driven by `camera.s5e8835.so` + the `is_mcu`/ISP firmware). There is no media controller, no subdev for the sensors visible from user space, and no standard capture node: `exynos-is-ssdevice_idvcvc`/`pindexs` are Samsung's own per-pipeline buffer queues. A plain V4L2 client (`v4l2-ctl`, GStreamer v4l2src, Chrome) would get no usable format, and writing the Samsung sequence in a Linux program means reimplementing the 3A/ISP HAL. I did not open any of the nodes (opening them would power the sensors). Conclusion: **discard A**. (A USB webcam does work by `CONFIG_USB_VIDEO_CLASS`, but that is not the tablet's cameras.)

### B. Android HAL bridge via the NDK camera2 API: VIABLE
cameraserver + provider are alive, so an Android-side native program can use `libcamera2ndk.so` (bionic). Tested read-only: `probe/camprobe.c` calls `ACameraManager_create`, `ACameraManager_getCameraIdList`, `ACameraManager_getCameraCharacteristics` and prints the table above. It never opens a camera.

Build (NDK 28.2.13676358, in `probe/build.sh`):
```
$ x400/camera/probe/build.sh
built camprobe fakesvc
$ adb push camprobe fakesvc /data/local/tmp/
```

**Result 1: as root it hangs, and the hang is not about the caller's identity.**
```
$ adb shell 'su -c "cd /data/local/tmp; timeout 20 ./camprobe; echo rc=$?"'
rc=124                         (no output at all; getCameraIdList never returns)
$ debuggerd -b <camprobe>
#04 libcamera_client.so  android::hardware::BpCameraService::addListener(...)
#05 libcamera2ndk.so     android::acam::CameraManagerGlobal::getCameraServiceLocked()
#08 libcamera2ndk.so     ACameraManager_getCameraIdList
$ debuggerd -b <cameraserver>     (the binder thread that holds up addListener)
#03 libbinder.so          CppBackendShim::waitForService(String16 const&)
#04 libactivitymanager_aidl.so  android::ActivityManager::getService()
#06 cameraserver          CameraService::UidPolicy::unregisterMonitorUid(unsigned int, bool)
$ logcat
W PermissionChecker: Waiting for permission checker service
E PermissionChecker: Waiting too long for permission checker service, giving up
E PermissionChecker: checkPermission failed:
E CameraService: UidPolicy: Failed to add uid to observer: 0x80000001
W libbinder.ServiceManagerCppClient: Waited one second for activity (...)
$ service check permission_checker / activity / appops / sensor_privacy / media.camera.proxy   -> not found
```
Cause: One UI is stopped, so `system_server` does not exist and nobody registers `permission_checker`, `activity`, `appops`, `sensor_privacy`, `media.camera.proxy`. cameraserver blocks 10 s per call on each one (the other binder threads of cameraserver queue behind it: when the stand-ins appeared, logcat printed `Binder transaction to android.hardware.ICameraService, function: addListener ... took 700679ms` for the queued clients, i.e. several blocked calls from my earlier hung probes).

**Result 2: with stand-ins for those services it works, as root and as shell (uid 2000) and as an app uid.**
`probe/fakesvc.c` registers the services with `AServiceManager_addService` (the NDK 28 stub does not export `AServiceManager_*`/`ABinderProcess_*`, so it takes them with `dlsym` from the device's `libbinder_ndk.so`) and answers every transaction with "no exception, 0". It logs each call. Then:
```
$ ./fakesvc 0 &
addService permission_checker (android.permission.IPermissionChecker): 0
addService activity (android.app.IActivityManager): 0
addService appops (com.android.internal.app.IAppOpsService): 0
addService sensor_privacy (android.hardware.ISensorPrivacyManager): 0
addService media.camera.proxy (android.hardware.ICameraServiceProxy): 0
addService permission (android.os.IPermissionController): 0
addService processinfo (android.os.IProcessInfoService): 0
$ ./camprobe                      -> cameras: 3, ids "0" BACK, "1" FRONT, "2" FRONT, rc=0    (instant)
$ su 2000 -c ./camprobe           -> identical output (cmp: shell==root)      rc=0
$ su 10073 -c ./camprobe          -> same list                                 rc=0
$ grep tx fakesvc.out | sort | uniq -c       (what cameraserver, uid 1047, asked while listing)
 10 activity uid=1047        12 permission_checker uid=1047
  1 appops uid=1047           3 sensor_privacy uid=1047
```
Full output: `probe/camprobe.out`. cameraserver asks `permission_checker` (code 1, 12 calls) about the caller; my blanket reply `0` is read as granted (the listing worked), but I did not verify the exact semantics. SELinux is Permissive here, so a denial from policy would only log, not block; that is not proven for Enforcing. Note the uid 2000 test ran under `su 2000` so the SELinux context stayed `u:r:ksu:s0`; I did not test a real `u:r:shell:s0` (`adb shell` here already lands as root/ksu, `id -Z` = `u:r:ksu:s0`). Enforcing mode was not tested.

What this does **not** prove yet (needs the real `ACameraDevice` open, so it waits for phase 3): that `connectDevice` is accepted with the stand-in answers. Known unknowns: the transaction codes cameraserver sent (`activity` 4-7, `appops` 10, `sensor_privacy` 2/6/13, all answered `0`) were not mapped to AIDL methods; what `0` means for each (uid observer registration, MODE_ALLOWED, "privacy not enabled") is my reading, not verified. `dumpsys media.camera` still fails with `Permission Denial: can't dump CameraService from pid=..., uid=0` (DUMP is checked by cameraserver against the `permission` service/ActivityManager path, which the blanket answer does not satisfy). x400-camd must answer per interface (find the codes in the AIDL of this build: `aidl` is in build-tools, or decode `strings` of libpermission/libactivitymanager_aidl) instead of a blanket 0.
The services live in the stub process only; when it exits cameraserver is back to waiting. Also: if One UI ever starts, its real `system_server` would register the same names.

### C. Direct binder to cameraserver: discarded
`ICameraService` over `media.camera` is exactly what libcamera2ndk wraps (`BpCameraService::addListener` in the backtrace above), plus `ICameraDeviceUser` and a `BufferQueue`/`IGraphicBufferProducer` for the frames, all private AIDL of this Android build. B gives the same without writing the parcels.
Also noted: `service call android.hardware.camera.provider.ICameraProvider/internal/0 3` (getCameraIdList, read-only, the only thing I sent to the HAL) returned 4 device names `device@1.1/internal/0..3`, while camera2 shows 3 ids; that explains nothing about the duplicate by itself. The HAL interface is not for clients (cameraserver is its only one).

## 3. Recommendation

**Route B.** One Android-side daemon `x400-camd` (bionic, arm64, root) that:
1. Runs the permission/activity stand-ins itself (the `fakesvc` part, answering by interface).
2. Lists cameras (`ACameraManager`), opens one on request (`ACameraManager_openCamera`), creates an `AImageReader` (`AIMAGE_FORMAT_YUV_420_888`, 1280x720 or 1920x1080 at 30 fps) and a repeating preview request; no flash, AF continuous.
3. For each `AImage`: reads the three YUV planes (`AImage_getPlaneData`/strides, NV12 or I420 as the HAL returns) and copies into a `memfd` ring of N buffers (3 to 4), then sends the fd once per buffer plus a small header per frame over a SOCK_SEQPACKET Unix socket (`SCM_RIGHTS`), reusing the protocol of `x400/display/fdframe.h` (FRAME/DONE with buffer id). A 1080p NV12 frame is 3.1 MB, 93 MB/s at 30 fps: a CPU copy is fine, so no dma-buf/AHardwareBuffer import is needed (the NDK has no fd getter for `AHardwareBuffer` anyway).
4. In the guest, a small program `x400-camsrc` (C, libpipewire, same code style as `display/capture.c`) is the other end: it connects to the socket and publishes a PipeWire video **source** node (`pw_stream` with `PW_DIRECTION_OUTPUT`, `media.class=Video/Source`, `media.role=Camera`, `node.name=x400-camera-front`, format NV12 or YUY2) feeding the memfd buffers (it can hand the memfd directly with `SPA_DATA_MemFd`, no copy). Control (which camera, resolution, start/stop on demand when a client connects) goes the other way on the same socket.
5. The portal (`xdg-desktop-portal` + `xdg-desktop-portal-kde`, Camera interface) lists `Video/Source` nodes with role Camera via WirePlumber's `access-portal.lua`, so Firefox/Chromium/Kamoso/Zoom through the portal work. `pipewire-v4l2` (`pw-v4l2 app`) exposes the same node as `/dev/video` to V4L2-only apps without a kernel module.

Why not v4l2loopback: not in the kernel, modules are signature-checked (`CONFIG_MODULE_SIG=y`, `SIG_PROTECT`) and Samsung's key is not available. PipeWire-native avoids it. If it were ever needed, GStreamer `pipewiresrc ! v4l2sink` still needs the module, so it stays out.

Where `x400-camd` runs: **outside the chroot, in the Android namespace** (bionic binary, needs Android's `/dev/binderfs/{binder,hwbinder}` and `libcamera2ndk.so`). The socket and the memfd travel through `/data/local/tmp` or the shared `/dev` tree like `display`'s `FILE.sock` does; G0 starts it from `guest_start.sh`. The guest's `/dev/binder` is shared too, but the guest has no libcamera2ndk, so it must be Android-side.

## 4. Plan

1. (done) probe: list ids and characteristics; stand-ins make cameraserver answer.
2. `x400-camd` v0 (Android): fakesvc logic answering per interface + `ACameraManager_openCamera(id 1)` + `AImageReader` YUV, count frames, write the first frame as PPM/JPEG to `/data/local/tmp` and save a timing line (fps, latency). Run it by hand. **Risk (high): first time the sensor powers on from Linux-controlled state**; cameraserver may reject `connectDevice` with the stand-in replies (needs real `activity`/`appops` semantic; look at `logcat` for `CameraService: ... connectDevice`), or the HAL may need `vendor.camera.*` props set by Samsung's camera app.
3. Same for rear camera (AF) and check that opening the camera does not interfere with the screen/audio HAL (the provider's init rc writes camera threads to the `camera-daemon` cgroups; `/dev/dma_heap/camera` is group camera).
4. Socket + memfd ring (reuse `display/fdframe.h` shape) and `x400-camsrc` in the guest publishing the PipeWire Video/Source; test with `gst-launch-1.0 pipewiresrc ! videoconvert ! autovideosink` and `pw-cli ls Node | grep Video/Source`.
5. Portal: install the packages in `PACKAGES`, check `busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop | grep Camera` (IsCameraPresent = true), then Kamoso/Firefox.
6. Lifecycle: start the camera only while a client has the node `running` (PipeWire node state), stop it otherwise (battery, privacy; no LED here). Orientation: the sensors report 90 (rear) and 270 (front); rotate in camd or expose the rotation as a stream property so the desktop (in whatever orientation G2 sets) shows it upright; mirror the front one.
7. Autostart from `guest_start.sh` (G0 line: `nohup /data/local/tmp/x400-camd &`, before the session), restart-on-death, no ordering needed with cameraserver since it is already alive on boot.

Risks, in order: (1) camera open with stand-in services (unknown until step 2); (2) a crash in the HAL could take down `cameraserver`/provider; the base session does not use them, but a restart of the init services is then needed (`setprop ctl.restart cameraserver`), and if only a reboot recovers it, root is lost (DFroot, see CTX.md); (3) CPU cost of copying YUV: fine at 1080p30, to be measured; (4) frames come with HAL latency of a few frames; (5) One UI's own camera app is not available, so no HDR/portrait/beauty: just the plain preview YUV and JPEG; (6) front id 2 duplicate: ignore it.

## 5. What was not done (on purpose)
- No camera was opened; `ACameraDevice` and `AImageReader` are for the implementation phase.
- `dumpsys media.camera` still gets `Permission Denial` even with the stand-ins.
- No `u:r:shell:s0` (real ADB shell) or Enforcing SELinux run; the `su 2000` run kept `ksu`'s context.
- The shell test used `su 2000`, not a real shell context.
- `v4l-utils`, `gstreamer1.0-*`, portal not tested at runtime: only checked in the PC chroot with `apt-cache policy` and `apt-get -s install` (nothing installed). Result: pipewire/wireplumber/libspa-modules are already installed; to add: see `PACKAGES` (32 packages incl. dependencies for the full list).
