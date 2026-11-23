# G1 input: result (2026-11-23)

## S Pen (`x400/display/inputd.c`)
`getevent -lp /dev/input/event10` (sec_e-pen): ABS_X 0..14680, ABS_Y 0..23488, ABS_PRESSURE 0..4095, ABS_DISTANCE 0..255, ABS_TILT_X/Y -63..63;
keys BTN_TOUCH, BTN_STYLUS, BTN_TOOL_RUBBER, BTN_DIGI, KEY_HOMEPAGE; switches SW_PEN_INSERTED, SW_MACHINE_COVER. No BTN_TOOL_PEN, no BTN_STYLUS2 in the capabilities.
- **Pressure, tilt, hover distance: no way in.** `fake-input.xml` has only pointer_motion(_absolute), button, axis, touch_*, keyboard_key(sym): no tablet requests.
  A uinput tablet does not help either: the session's KWin (`kwin_wayland --virtual`) has no libinput backend (its fds: no /dev/input, no udev socket;
  `libinput debug-events` in the guest also fails: "udev device never initialized", there is no udev). So the pen goes in as a mouse.
- Done: position (also while hovering), tip = left button, tip with the barrel button (BTN_STYLUS) held = right button, eraser tip (BTN_TOOL_RUBBER) = middle button.
  The button is chosen at tip-down and released with the tip. If the pen does not report a tool key, motion is sent always.
- Fixed a bug of the old code: `EVIOCGABS(ABS_MT_POSITION_X)` returns 0 (not an error) on a pen, so the pen range was 0x0 (`inputd.c: range 0x0` in inputd.log) and every
  position was garbage. Also `strstr(path,"event1")` matched event10; the side-key guard now parses the number (event0..4 refused for `--touch` and `--pen`).
- New: `--touch none` (pen only, no touch grab; what the tests use) and `--trace`.
- Test without a hand: `pentest` (uinput pen with sec_e-pen's axes) + `inputd --touch none --pen /dev/input/eventN` while the session runs.
  Evidence (cursor read through a KWin script, `workspace.cursorPos`): before `719 690`, after the sequence `1079 421`; the trace shows
  `pen button 272 1/0` (tip), `273 1/0` (tip + barrel), `274 1/0` (eraser) with motion between them. The real pen was not touched by a person: not tested with the real digitizer.

## Side keys (`x400/input/keys.c`)
Opens event0 and event1 `O_RDONLY`, no EVIOCGRAB (`grep GRAB keys.c`: only the comment). Vol+/Vol-: `wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+/-` on release or on each
auto-repeat; if both were held, nothing (so the 3 s return shortcut does not move the volume). Power, a short press (< 800 ms): backlight 0 / back to the saved value; longer: ignored.
- Test with a uinput keyboard (`pentest keys`) and a fake `wpctl`: calls were `5%+`, `5%-` for the single presses, none for Up+Down together. Power: the real
  `/sys/class/backlight/panel/brightness` went 11 -> 0 -> 11 and the log says `power -> screen off/on`. Running against the real event0/1 for 2 s: no error, exit by timeout.
- Not done / caveats: this is a backlight 0, not a suspend: the CPU and the touch stay on; a touch while "off" still clicks. One UI is stopped, so nothing else answers Power.
  PipeWire's default sink has no ID at this moment (`wpctl get-volume`: "Translate ID error"), because the audio is G3's: the volume keys work when G3 has a sink.
  A long Power press is left alone (no power menu). Volume with a real key press was not tried by hand.

## On-screen keyboard
Package: **plasma-keyboard** 6.6.6 (universe, resolute-updates). `apt-get -s install` adds exactly 3 packages: `plasma-keyboard`, `qt6-virtualkeyboard-plugin`, `qml6-module-org-kde-layershell`
(qml6-module-qtquick-virtualkeyboard is already in the image). It talks input-method-v1 to KWin, which is what KWin 6.6 supports; maliit and onboard are not needed.
`x400/input/kwinrc-keyboard` is the `[Wayland] InputMethod=/usr/share/applications/org.kde.plasma.keyboard.desktop` key. **Not tested**: the PC has no network to download the .deb
(`apt-get download`: "Temporary failure resolving"), so the .desktop name comes from the project's README, not from the package. Check it after installing.
`KWIN_IM_SHOW_ALWAYS=1` (KWin's environment) makes it pop up without a touch on a text field; by default KWin shows it only when a field is touched, which fake_input touch does.

## For G0
1. `x400/input/PACKAGES` -> `plasma-keyboard` in the image build.
2. Image: `/etc/xdg/kwinrc` with the lines of `x400/input/kwinrc-keyboard` (HOME is read-only; XDG_CONFIG_DIRS=/etc/xdg is already exported).
3. `deploy.sh` and `guest_start.sh`: also copy `out/input/keys` to the stage like `inputd` (build: `sh x400/input/build.sh`, which also rebuilds `x400/display/bin/inputd`).
4. `session-inner.sh`, after the `/tmp/inputd` line (89): `/tmp/keys > /tmp/keys.log 2>&1 &`
   (the inputd line stays as it is; `--pen /dev/input/event10` was already there). Optional, in the KWin environment: `export KWIN_IM_SHOW_ALWAYS=1`.
5. `guest_start.sh` kills leftovers of an earlier session: add `keys` to that list (it holds no grab, so it is harmless if left).
