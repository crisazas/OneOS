# G5 cameras: result

Both cameras of the SM-X400 reach the Linux desktop as PipeWire `Video/Source` nodes, from the live session, without restarting it.
The camera stays closed until a client streams from the node. Tested 2026-11-23 on the live session (kwin_wayland pid 17278 untouched).
Research and the route choice: `RESEARCH.md`. Images used as evidence were saved in `/tmp/opencode/cam/` (not in the repo).

## What works (with the evidence)

1. **x400-camd opens the real cameras** (NDK camera2, `AImageReader` YUV_420_888, NV12 out). `x400-camd test ID W H OUT.ppm N`:
```
camera 1 640x480:  5 frames, rotation 270, facing front, 31.7 fps  luma mean 60.7 stddev 41.5 neighbour-diff 2.04
camera 1 1280x720: 5 frames, rotation 270, facing front, 30.6 fps  luma mean 65.4
camera 0 640x480:  5 frames, rotation 90,  facing back,  31.0 fps  luma mean 99.0 stddev 53.4 neighbour-diff 2.58
camera 0 1280x720: 5 frames, rotation 90,  facing back,  31.6 fps  luma mean 96.6
camera 2 640x480:  5 frames, front (same sensor as 1), 30.8 fps
```
   Not black (mean 60-100, histogram spread over all 16 bins) and not noise (neighbour difference ~2 of 255; white noise would be ~85).
   The saved images show a real room: a bottle, a lamp and a curtain from the front camera; a white foam box, a desk and a laptop with stickers from the rear one.
   The front image is dim and slightly grainy (a room at night); I did not judge sharpness or colour accuracy beyond that.
2. **Transport** `x400-camd serve`: unix socket `/data/local/tmp/oneos/rootfs/tmp/x400-cam.sock`, which the guest sees as `/tmp/x400-cam.sock` (`ls -l` in the guest: `srw-rw-rw- /tmp/x400-cam.sock`).
   Protocol in `camproto.h`: one request (camera id, size, fps), then `CAM_FORMAT` and a `CAM_FRAME` header + NV12 payload per frame. A plain copy: 1280x720 NV12 is 1.4 MB per frame, 41 MB/s, no descriptors.
   Not zero-copy on purpose (reliability first). `service.camera.running` was `0` before and after every client, `1` while streaming: the sensor is closed when the client leaves.
3. **x400-camsrc in the guest** (`pw_stream`, `media.class=Video/Source`, `media.role=Camera`, NV12 1280x720 and 640x480 at 30 fps). `pw-cli ls Node` in the live guest:
```
node.name = "x400-camera-front"  node.description = "Front camera (x400)"  media.class = "Video/Source"  media.role = "Camera"
node.name = "x400-camera-back"   node.description = "Rear camera (x400)"   media.class = "Video/Source"  media.role = "Camera"
```
   Idle state is `paused`/`suspended`, camera closed.
4. **Own consumer `x400-camtest`** (pw_stream, no GStreamer needed): `x400-camtest x400-camera-front 8 out.ppm`, final run through `host.sh` + `guest.sh`:
```
front: negotiated 1280x720 NV12; frame 1 luma 63.5 ... frame 8 luma 74.7; 28.5 fps; wrote /tmp/final-front.ppm
back:  negotiated 1280x720 NV12; frame 1 luma 99.4 ... frame 8 luma 97.0; 28.1 fps; wrote /tmp/final-back.ppm
```
   The luminance rises 63 to 75 on the front camera: auto-exposure converging, i.e. real sensor data. Both PPMs were viewed: the same scenes as in step 1.
5. **Wiring without editing `guest_start.sh`**: everything is in `x400/camera/stage/` (built by `build.sh`, binaries ignored by git).
   `host.sh` started from the session's mount namespace (as `guest_start.sh` does) brought up the four daemons by itself (the last one 2 s after the start): `fakesvc`, `sensorservice`, `sensormgr`, `x400-camd`; stop and start again from zero was repeated and gave the same result.

## Wiring for G0 (exact)

- `x400/oneos/deploy.sh` already pushes `x400/*/stage/` to `/data/local/tmp/oneos/x400/groups/camera/` (run `x400/camera/build.sh` first, so `stage/` has the five binaries).
- `guest_start.sh` already binds it at `/tmp/x400/camera` and runs `stage/host.sh` in the background as root: nothing to change there.
- **`x400/phone/session-inner.sh`, after `wireplumber ... &` (line ~50) add exactly:** `sh /tmp/x400/camera/guest.sh &`
  (`guest.sh` waits for `$XDG_RUNTIME_DIR/pipewire-0` itself and restarts `x400-camsrc` if it dies; log `/tmp/x400-camsrc.log`.)
- Packages: `x400/camera/PACKAGES` (portal, portal-kde, pipewire-v4l2, gstreamer1.0-pipewire; 29 new packages with dependencies).
- Host logs: `/data/local/tmp/oneos/data/x400/logs/group-camera.log` and `camera-{fakesvc,sensorservice,sensormgr,x400-camd}.log`. By hand: `sh /data/local/tmp/oneos/x400/groups/camera/stop.sh`.
- `host.sh` does nothing if a real `system_server` runs (One UI back) and stops its daemons if one appears.

## Why the camera needed more than `fakesvc` (what each piece fixes)

With One UI stopped, nobody registers what the camera stack calls. Each one found by a hang plus `debuggerd -b` backtraces and `logcat`:

| symptom | cause | fix (where) |
|---|---|---|
| `getCameraIdList` never returns | cameraserver waits 10 s per call for `permission_checker`, `activity`, `appops`, `sensor_privacy` | stand-ins (`probe/fakesvc.c`) |
| `openCamera` -> `-10012` "Camera is disabled" | `edmnativehelper` (Samsung EDM policy): registering a stand-in with an unknown interface makes cameraserver say "disabled"; with no service it waits 5 s per open and allows | do **not** register it (comment in `fakesvc.c`); costs ~5 s on each open |
| `connectDevice` fails, priority query `-129` | `processinfo` | stand-in that answers PROCESS_STATE_TOP |
| provider reports 0 cameras after a restart, requests hang | the HAL blocks forever on `android.frameworks.sensorservice@1.0::ISensorManager/default` (HIDL) | `/system/bin/sensorservice` run by hand (needs `package_native`, `virtualdevice_native` stand-ins) + `probe/sensormgr.cpp`, which registers the real `SensorManager` class of `libsensorservicehidl.so` through `dlsym` |
| rear camera: 0 frames, "waiting first pixel", HAL gives up after 15 s | the rear HAL enables 31 motion sensors through sensorservice; each `enableSensor` waits 5 s for `batterystats` | `batterystats` stand-in |
| first frame never delivered | the `AImageReader` BufferQueue lives in x400-camd: cameraserver calls `dequeueBuffer` back into it | start the binder thread pool in `x400-camd` (`ABinderProcess_*` via `dlsym`) |
| `libbinder ProcessState can not be used after fork` | process per client | one thread per client (`camd.c`) |

Side effect to know: `x400-camd`'s listening socket is under `/data/local/tmp/oneos/rootfs/tmp`, which only exists as a tmpfs in the session's mount namespace; from the root shell's namespace that path is the read-only image. Start `x400-camd` from `nsenter -t $(pidof kwin_wayland) -m` (`guest_start.sh` already does for `host.sh`).

## What does not work, or is not tested

- **The KDE Camera portal and real applications were not tested**: the portal packages are not in the live guest (read-only image) and I did not unpack them there. What is shown is the PipeWire node (`pw-cli`, `pw-dump`) and my own pw_stream consumer. Installing `PACKAGES` and checking `busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop | grep Camera` (IsCameraPresent) is the first thing to do after the next image build. WirePlumber's `access-portal.lua` is in the image.
- `pipewire-v4l2` (`/dev/video` for V4L2-only apps) not tested for the same reason.
- **Orientation is not handled**: Android reports sensor orientation 90 (rear) and 270 (front) and `camproto.h` carries it as `rotation`, but `camsrc` ignores it and publishes the frames as the HAL delivers them. The saved images looked upright, with the tablet in the position it was in during the tests (I did not note which); with the tablet turned the picture will not follow. The front camera is not mirrored. To do: rotate/mirror in `camsrc`, or publish a PipeWire transform.
- Only NV12 at 1280x720 and 640x480, 30 fps. No JPEG/high resolution, no AF control, no zoom. `ACAMERA_CONTROL_AF_MODE` is continuous-picture (`X400_NOAF=1` disables).
- Every open takes ~6 s (`edmnativehelper` 5 s wait, then the HAL's early sensor start); the first frames are dark until AE converges (3 to 5 frames).
- One client per camera: a second open of the same id is refused by cameraserver and reported to `camsrc`, which retries every 2 s. Two cameras at once were not tried (the HAL has a PiP path; untested).
- The front camera id `2` is the same sensor as `1` and is not offered.
- Not tried: Enforcing SELinux (stayed Permissive as agreed), One UI coming back while the group runs (`host.sh` is written to stop itself, never exercised), battery/thermal cost of a long stream, a reboot (DFroot) of the tablet.
- `fakesvc` answers `0` to everything not listed in its source; the meaning of each reply is my reading of cameraserver's behaviour, not decoded AIDL. `dumpsys media.camera` still says `Permission Denial`.
- The `sensorservice` started by `host.sh` is Android's own; `x400/sensors` (G2) says it "can be launched by hand but only waits..." and runs its own `x400-sensord` on the IIO nodes. They can both run, but the hub `enable` state is shared: with the camera HAL enabling 31 sensors through `sensorservice` while a client streams, G2's readings may change meanwhile (not measured).

## Risk and what I touched on the tablet

- **cameraserver and the camera provider were restarted once, not by me but because of me**: at 05:39 a hung probe (waiting on services nobody registered) plus my `debuggerd -b` dumps ended with cameraserver's own watchdog aborting it (`F DEBUG ... CameraServiceWatchdog`) and the provider dying; `init` restarted both (pids 24135/24132 became 1518/1543). I did not kill either. After that they ran normally through every test.
- Nothing was installed in Android; only executables in `/data/local/tmp` (removed at the end, together with temporary files). Nothing written to rfkill, ttySAC, firmware. No SELinux change.
- Everything I started is stopped: no `fakesvc`, `sensorservice`, `sensormgr`, `x400-camd`, `x400-camsrc` running (`pidof` empty), `service.camera.running=0`, KWin and the session untouched.

## Files

`camproto.h` protocol; `camd.c` Android daemon and `test` mode; `camsrc.c` guest PipeWire source; `camtest.c` guest consumer; `probe/{camprobe.c,fakesvc.c,sensormgr.cpp}`;
`stage/{host.sh,guest.sh,stop.sh}` (+ built binaries, not committed); `build.sh [android|guest]`; `PACKAGES`; `RESEARCH.md`.
