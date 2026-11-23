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
