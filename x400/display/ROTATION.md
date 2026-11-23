# x400/display (G6): screen rotation and touch/pen follow KWin's output rotation

Tested on the live session (kwin pid 17278) with `kscreen-doctor output.1.rotation.{left,inverted,right,none}`. Screenshots of the real panel (`screencap -p` on the tablet,
one file per turn, read back with an image viewer) are in `/tmp/opencode/rot/`:
`G_none.png` `G_left.png` `G_inverted.png` `G_right.png` (the final code), `s0.png` (before any change, rotation none), `final-sheet.png` (the four turned so the taskbar is at the bottom).

## Geometry (measured, not assumed)
- The panel is 1320x2112 natively; Android shows it landscape (surface current transform ROTATE_90, `dumpsys SurfaceFlinger`: framebufferSpace 1320x2112 ROTATION_90).
- `screencap` returns 2112x1320 in all four turns (the panel's landscape view); the natural frame is that image turned 90 degrees clockwise.
  Where KWin's taskbar (the desktop's bottom) lands in it (`/tmp/opencode/rot/edge.py`: the brightest 44 px strip), final code:

  | output | KWin frame | taskbar edge in the screencap |
  |---|---|---|
  | none | 2112x1320 | bottom |
  | left | 1320x2112 | left (the panel's natural top) |
  | inverted | 2112x1320 | top |
  | right | 1320x2112 | right (the panel's natural bottom) |

  The old helper was not screenshotted rotated (it was only seen not to handle a portrait frame: it blits `min(ext, frame)` into a fixed 2112x1320 swapchain).
- So the tablet has to be physically held with that edge down: none = the usual landscape; left = natural top down; right = natural top up; inverted = landscape upside down.
  Upright check (`final-sheet.png`): turn each screencap so its taskbar is at the bottom and the desktop reads right, complete, no crop, no mirror, in all four.
- **Which physical turn a posture corresponds to is the accelerometer's side (G2, `x400-sensord`), not measured here**: no one turned the tablet. Display and touch follow KWin's output rotation, whatever decides it.

## What changed
- `capture.c`: binds `wl_output` v2, reads its `transform` (0 normal, 1 left, 2 inverted, 3 right; it follows `kscreen-doctor` live), sends it in the `flags` of every frame
  (`FDF_ROT_SHIFT`, `fdframe.h`). It also dispatches the Wayland queue (`on_wayland`) so the event arrives while it streams. The stream renegotiation itself already worked
  (`capture: format ... 1320x2112` appears in `capture.log` after `rotation.left`; no restart of capture or the session).
- `jni.c`: the swapchain is made by `vk_swapchain()`: landscape frames 2112x1320 with `preTransform` IDENTITY (as before; SurfaceFlinger turns them), portrait frames 1320x2112 with
  `preTransform` = the surface's current transform (ROTATE_90): the buffer is then already in the panel's own orientation. It is recreated (`vkDeviceWaitIdle`, destroy, create) when the frame
  switches between landscape and portrait: display.log says `vulkan: swapchain 1320x2112 (portrait, preTransform 2, surface current 2 extent 2112x1320)`.
  The frames of transform 1 and 2 are blitted with the destination corners swapped (a 180 degree turn costs nothing in `vkCmdBlitImage`: no shader, no render pass),
  those of 0 and 3 straight. `X400_FLIP=<mask>` (bit n = transform n) overrides it for a test; I used it to find the mask: 0x0 and 0xf both failed 2 of the 3 turns, 0x6 got all four right.
  `OUT_OF_DATE` on acquire is answered by skipping the frame. `X400Display.java` did not need a change: the layer stays 2112x1320, nothing is set on the SurfaceControl.
- `inputd.c`: `--orient FILE` (default `/tmp/x400-orientation.env`, `none` disables). It is read at start, on `SIGHUP` and when the file is replaced or written (inotify on its directory:
  `x400-rotate-hook` does `mv`, which fires). It takes `X400_KSCREEN_ROT` and `X400_LOGICAL_W/H`, sets `rot` and the size, lifts any finger or pen button that is down (a finger must touch again)
  and goes on with the **same open fds and the same EVIOCGRAB** (checked below). `--rot/--size` still give the start value; `--trace` now also prints each touch.
  Mapping used (`X400_KSCREEN_ROT` -> `map()` case): none 1, left 2, inverted 3, right 0. **This differs from G2's hook for left and right** (`X400_TOUCH_ROT`: left 0, right 2): derived from the table
  above (where the taskbar lands in the natural frame), I use the name and ignore `X400_TOUCH_ROT` unless the name is missing. If a finger test shows left and right of the touch swapped, swap them in `read_orient()`.

## Evidence
Final helper, each rotation, `edge.py` on the screencaps (taskbar edge): none bottom, left left, inverted top, right right; each image upright after one 90/180 turn, whole desktop visible.
During the tests `display.log` showed no vulkan error; `capture.log`: `output transform 1/2/3/0` in the order of the commands.

Frame rate (wl-client at 120 Hz, 14 s, `/tmp/opencode/rot/fps.sh`, the old helper built from HEAD against the new one, same session, two rounds each):

| helper | wl-client callbacks | frames shown (steady, per 300) |
|---|---|---|
| old, none | 105.7, 113.2 fps | 78.9, 84.0 |
| new, none | 109.1, 110.9 fps | 80.6, 80.9 |
| new, right (portrait swapchain) | 104.6 fps | 81.5 |

Same within the noise (the old numbers of the README: 81 shown); `acquire` mean 0.15 ms, as before.

Touch/pen (a uinput pen with sec_e-pen's range hovering over five points of the natural frame at 10 %/90 %/50 %, KWin's `workspace.cursorPos` read through a script;
`/tmp/opencode/rot/rottest.c`, `rtest2.sh`), **one running inputd with `--orient`, through the four orientations** written to the env file as the hook does.
The cursor was sampled while the pen cycled (not in step with the points), so the check is that every sampled position is one of the five expected points of that posture and that
the set changes with the posture; all 6 samples of each posture were (e.g. none: 1900,1187 / 1900,132 / 1056,660 / 211,1187; left: 132,211 / 660,1056 / 1187,1900 / 132,1900).
Expected points by the mapping (TL, TR, BL, BR of the natural frame, centre): none 211,1187 / 211,132 / 1900,1187 / 1900,132 / 1056,660; left 1187,1900 / 132,1900 / 1187,211 / 132,211 / 660,1056;
inverted 1900,132 / 1900,1187 / 211,132 / 211,1187 / 1056,660; right 132,211 / 1187,211 / 132,1900 / 1187,1900 / 660,1056.
inputd's log: `orientation left: rot 1 -> 2, size 2112x1320 -> 1320x2112`, then `inverted: 2 -> 3`, `right: 3 -> 0`, `none: 0 -> 1`.
This proves inputd applies the table; that the table matches the screen is by derivation from the screencaps (upright desktop = natural frame turned 180 for left, 0 for right, 90 clockwise for inverted,
90 counter-clockwise for none), not by a finger.
Live inputd replaced by the new one (`--touch /dev/input/event8 --pen /dev/input/event10 --grab`): `/proc/PID/fd` shows event8 and event10 open before and after the reload, pid unchanged and
alive after an inotify reload (file replaced) and after `kill -HUP` (file rewritten in place). Not tested: a real finger or the real S Pen in a rotated state.

## Not done / caveats
- The accelerometer was not physically turned; nothing verifies which posture KWin gets in the hand. The real touch of the screen was not tried with a finger in a rotated state.
- A rotation while the CPU path (`X400_NOVULKAN`, memfd frames) is in use is not handled: it only draws frames of the size of the layer.
- The last edit to `jni.c` (dropping a `vkResetFences(dev, 0, NULL)` from the OUT_OF_DATE branch) was made after the last on-device run: it builds, it was not run. That branch was never taken in the tests.
- A rotation re-creates the swapchain: one or two frames are skipped while it happens (no measurement of the length of the gap).
- The tablet rebooted (and lost root: `su` inaccessible, `uptime` 50 s) right after the last restore to `none`, so the final state could not be re-verified. The last screenshot before it:
  rotation 1, helper and capture running. I do not know the cause (nothing I ran writes to power or the kernel; other groups were testing in the same session).

## For G0 (exact lines)
1. Nothing new to stage: `capture`, `libx400display.so`, `x400-display.dex`, `inputd` are the same files, rebuilt (`sh x400/display/build.sh`, `sh x400/input/build.sh`; `capture` as in README).
   `display.sh` arguments are unchanged (`... 2112 1320 --power`).
2. `session-inner.sh`: the inputd line may stay as it is (`--orient` defaults to `/tmp/x400-orientation.env`). Before it make the file exist so inputd's inotify finds the directory (always true, /tmp):
   nothing to add. After KWin and the 120 Hz mode, and after the capture line, start the rotation daemon and keep the hook beside it:
   `/tmp/x400-sensord --rotate --hook /tmp/x400-rotate-hook > /tmp/sensord.log 2>&1 &` (the exact build/stage of `x400-sensord` and the hook is G2's list; the env
   `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS` of the session are needed by its `kscreen-doctor`).
3. No `SIGHUP` is needed from the hook (inotify does it); if you want a belt and braces: `pkill -HUP inputd` at the end of `x400-rotate-hook`.
4. `guest_start.sh`'s list of leftovers: add `x400-sensord` if it is not there.
