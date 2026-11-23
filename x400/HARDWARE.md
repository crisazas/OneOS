# x400: hardware that the Linux session shares with Android

The session is a chroot on One UI's kernel: kernel, drivers and HALs are Android's, and the guest shares `/dev` and the network namespace.
Three ways to reach a device: (1) the kernel already exposes it in the Linux way (`/dev/input`, ALSA, IIO, rfkill): use it directly;
(2) only an Android HAL drives it: keep the HAL alive and talk to it, or replace its job; (3) needs Samsung's firmware daemon: the most expensive.

## Groups (each one owns its own files; nobody edits another group's)
| Group | Scope | Owns | Risk |
|---|---|---|---|
| G0 base | packages in the rootfs image, deploy, guest_start.sh/session-inner.sh wiring | `x400/build-rootfs-plasma.sh`, `x400/oneos/*`, `x400/phone/session-inner.sh` | low |
| G1 input | touch (done), S Pen (pressure, tilt, buttons), side keys, on-screen keyboard | `x400/input/` and `x400/display/inputd.c` | low (the way back must survive) |
| G2 power and sensors | accelerometer/gyro/light/auto-rotation, battery, brightness, suspend | `x400/sensors/` | low |
| G3 audio | speakers, microphone, headphones, volume keys through PipeWire | `x400/audio/` | low |
| G4 radios | Wi-Fi management and Bluetooth | `x400/radio/` | HIGH (writes to rfkill, UART, firmware) |
| G5 cameras | front and rear cameras | `x400/camera/` | HIGH |

## Phases
0. Base: one rootfs image with every package any group needs, deployed once. (G0)
1. Low risk, in parallel (G1, G2, G3) on the live session without restarting it; read-only research for G4 and G5.
2. Integration: one session restart, everything of phase 1 together, checked.
3. Risky, last (G4, G5): a reboot loses root (DFroot); get it back through KernelSU over adb and continue. Nothing from phase 1 depends on it.

## Rules for the groups
- Do not restart the session, `x400vt`, KWin or the display helper; G0 does. Do not touch the files of another group.
- Everything a group adds is a script or a source under its own directory plus one line in the list of packages it needs (`x400/<group>/PACKAGES`).
- Each group ends with `x400/<group>/RESULT.md`: what works (with the command and its output), what does not, and why.

## Status (2026-11-23, integrated session started by itself at 10:54, nothing done by hand)
| Group | What works | Evidence in |
|---|---|---|
| G1 input | touch (grabbed), S Pen as a mouse (position, tip, barrel button = right click, eraser = middle; no pressure/tilt: KWin's fake_input has no tablet), side keys without grab (volume, power = backlight), on-screen keyboard configured | `x400/input/RESULT.md` |
| G2 sensors | accelerometer and light through `net.hadess.SensorProxy`, battery through UPower, brightness, auto-rotation (`x400-sensord --rotate`) | `x400/sensors/RESULT.md` |
| G3 audio | guest -> PipeWire "Altavoces"/"Microfono" -> `x400-pwbridge` -> `x400-audiod` (Android, AAudio) -> speaker; heard by the owner. Needs the stream volume set (`x400-setvol`): without Android's framework the policy has no volume range | `x400/audio/RESULT.md` |
| G4 radios | Wi-Fi (NetworkManager + wpa_supplicant, `connectivity: full`, HTTPS 200) and Bluetooth (`hci0` through `scsc_h4_0`, scan found 12 devices); both start by themselves (`stage/enable-wifi`, `enable-bt`) and are stopped when the way back starts | `x400/radio/RESULT.md` |
| G5 cameras | front and rear as PipeWire `Video/Source` (`x400-camera-front/back`), ~30 fps, opened only while a client streams | `x400/camera/RESULT.md` |
| G6 rotation | display, `capture` and `inputd` follow KWin's output rotation in the four turns | `x400/display/ROTATION.md` |

Not tested by a person: rotating the tablet by hand, a finger or the real S Pen in portrait, the on-screen keyboard, pairing and A2DP, the portal's camera dialog, both cameras at once.

## How the groups are wired (G0)
- `x400/<group>/stage/` is pushed by `x400/oneos/deploy.sh` to `/data/local/tmp/oneos/x400/groups/<group>/`; `guest_start.sh` binds it in the guest at `/tmp/x400/<group>` and runs `stage/host.sh` on the Android side.
- `x400/phone/session-inner.sh` runs each `stage/guest.sh` (audio and camera after PipeWire; sensors and radio before plasmashell, because Plasma's applets look for the buses when they load; input after inputd).
- `guest_start.sh` stops the radio and camera groups when the way back starts (`oneos_rec_stop`), before the guest is killed: otherwise Bluetooth stays held by a dead relay in One UI.
- `x400/rootfs-extras.sh` installs every `x400/*/PACKAGES` (minus `x400/NOT-IN-IMAGE`) and the groups' `image-conf` into the image; start a session with `ONEOS_GUEST_AUDIO=0 ONEOS_RETURN_MODE=inplace sh oneos_switch.sh`.

## Verification run (2026-11-23 11:46, session that started by itself at 11:37)
`sh x400/verify-session.sh` (PC, adb, root on the tablet) checks every group and prints one line per check. Literal output of that run, 39 OK and no FAIL:

```
== session: 11:46:45 kwin 27086, mode 2112x1320@120.00*, tombstones 64
OK   process kwin_wayland [27086]
OK   process plasmashell [28812]
OK   process pipewire [26496]
OK   process wireplumber [26745]
OK   process inputd [30072]
OK   process keys [30075]
OK   process x400-pwbridge [26995]
OK   process x400-camsrc [27005]
OK   process x400-sensord [27641]
OK   process upowerd [27799]
OK   process NetworkManager [27844]
OK   process wpa_supplicant [27680]
OK   process bluetoothd [28168]
OK   process btattach [28166]
OK   process x400-audiod [25428]
OK   process x400-camd [25940]
OK   process fakesvc [25607]
OK   process audioserver [2296]
== G1 input
OK   inputd holds the touchscreen (inputd: /dev/input/event8 range 1319x2111 (grabbed))
OK   keys reads event0/event1 without grab (4 power presses so far)
OK   kwinrc selects plasma-keyboard
== G2 sensors and power
OK   SensorProxy HasAccelerometer true
OK   SensorProxy HasAmbientLight true
OK   UPower battery:  state: fully-charged  voltage: 4.376 V  percentage: 100%  History (voltage):
OK   brightness (value max percent): 11 308 3
OK   KWin rotation left=2 then none=1 (put back)
== G3 audio
OK   audio_policy has STREAM_MUSIC range 0..15 (x400-setvol)
OK   tone of the guest reached x400-audiod: 11:46:54 out: 71040 bytes, 17760 frames, rms=0.02614 peak=0.0562
NOTE speaker output itself is judged by ear (the tablet's own microphone loses a continuous tone after ~5 s; x400/audio/RESULT.md)
== G4 radios
OK   wlan0 GENERAL.STATE:100 (connected) IP4.ADDRESS[1]:192.168.68.109/22
OK   NetworkManager connectivity: full
OK   HTTPS example.com -> 200
OK   hci0 UP RUNNING, 	BD Address: 24:A4:52:12:2C:F7 ACL MTU:
OK   bluetoothctl Powered: yes
== G5 cameras
OK   no useless v4l2 nodes from the ISP
OK   our PipeWire nodes: camera front/back, Altavoces, Microfono
OK   camera front delivered 28.5 fps to the guest
OK   camera back delivered 28.5 fps to the guest
OK   cameras closed again after the clients left
== G6 display
OK   helper: x400-display: 7500 frames shown, 22.6 fps over the last 13.3 s (vulkan: acquire mean 0.13 ms max 1.9
== end: tombstones 64, x400vt [26099], kwin [27086]
```

Notes on the numbers:
- Cameras: 30.8 and 28.9 fps here; 31.0 and 29.3 in an earlier run of the same session. Image content was checked by looking at a frame of each (a room with a bottle and a lamp; a foam box on a desk).
- Audio: "reached x400-audiod" proves the PCM got to Android's AAudio; that the speaker sounds was heard by the owner. The tablet's own microphone is not a reliable witness for a continuous tone (see `x400/audio/RESULT.md`).
- The rotation check rotates the output to `left` and puts it back to `none`; nothing else in the script changes state.
- Helper fps (114.3 over the last 2.6 s) is a short window of an idle desktop, not a benchmark: the measured frame rates are in `x400/README.md`.

## Known crash (2026-11-23, kernel oops 06:22:59)
Opening an ABOX capture PCM that Android's audio HAL already holds (`pcmC0D118c`, the amplifier reference/IV-sense) while a playback stream runs panics the kernel:
`Oops: Fatal exception`, `pc: abox_wdma_mute_stream+0x38` from `snd_soc_dai_digital_mute` (`snd_soc_samsung_abox`). The reboot loses root and the session.
Never open `/dev/snd/pcmC0D118c`, `pcmC0D16c` or `pcmC0D21c` from outside audioserver. Get root back: open DFRoot, tap LAUNCH ROOT once if it is enabled; if a
reboot left KernelSU without the shell grant, KernelSU manager -> Superuser -> show system apps -> `com.android.shell` -> switch Superuser on (then `adb shell su -c id`).
