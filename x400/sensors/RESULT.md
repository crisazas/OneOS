# x400/sensors (G2): sensors, rotation, battery, brightness, screen power

Everything below was run on the live session (kwin pid 17278, never restarted). Guest commands go through `nsenter` as in CTX.md.

## What works

| Point | State | Used |
|---|---|---|
| Accelerometer | works without Android's HAL | `x400-sensord` (writes `ssp_sensor/enable`, reads `raw_data`) |
| Ambient light | works, enabled the same way | `x400-sensord` |
| Auto-rotation | works through `x400-sensord --rotate` (kscreen-doctor) | tested with injected accelerometer values and live KWin |
| KWin's own auto-rotate over D-Bus | service is up and answers; **KWin never claimed it** | not proven (see below) |
| Battery | `upowerd` sees it (100 %, Full, 4.378 V) | `upower` package, not installed yet |
| Brightness | works | `x400-brightness` |
| Panel on/off | documented, not toggled | see "Screen power" |
| Gyroscope, magnetometer, grip | not done | not needed for rotation |

## 1. Sensors

IIO nodes exist (`/sys/bus/iio/devices/iio:device2` = `accelerometer_sensor`, 4 = `gyro_sensor`, 5 = `light_sensor`, 3 = `geomagnetic_sensor`) but they are **not standard IIO**: no `in_accel_*_raw`, only `buffer`, `scan_elements/in_timestamp_*`, and `/dev/iio:deviceN` is held by the Android sensors HAL (pid 916), so a second reader gets `EBUSY`:

    $ dd if=/dev/iio:device2 bs=64 count=2
    dd: can't open '/dev/iio:device2': Device or resource busy

So `iio-sensor-proxy` cannot be used (it wants `in_accel_x_raw`). The data is in Samsung's sysfs, but the hub keeps each sensor idle until it is enabled and Android's `sensorservice` is not running. Before enabling, `raw_data` is a stale value:

    $ cat /sys/class/sensors/accelerometer_sensor/raw_data   # 5 reads over 10 s, tablet untouched
    3072,-16,2726   (identical every time; ssp_sensor/enable=0x40000108020020, bit 1 not set)

The enable file takes `<sensor type><0|1>`, in **one write without a newline** (a shell `echo` fails with rc=1; busybox `dd` or `printf` works). Type 1 = accelerometer, 5 = light:

    $ printf '11' | busybox dd of=/sys/class/sensors/ssp_sensor/enable ; cat .../ssp_sensor/enable
    2                                  # bit 1 = accelerometer
    $ cat /sys/class/sensors/accelerometer_sensor/raw_data   # live, tablet flat on its back
    27,2,-4059   25,4,-4072   27,6,-4067   24,2,-4070   ...

Light, after `printf 51`: `lux` = `0,0,50,8,8,0`, first field is the lux (the STK61A10 reads 0 to 4 in this room).
Writing `ssp_sensor/enable` with a bare mask (`echo 34`) is **rejected** and the driver logs `set_sensors_enable: type = (3) or new_enable = (4) is wrong`; do not do that.
`/system/bin/sensorservice` can be launched by hand but only waits for the app-ops service and never registers; not a path.

Scale and axes (LSM6DSVTR, 4096 LSB = 1 g):

    flat, screen up:                     g = +0.006,+0.002,-0.990    -> z = -1 g when the screen faces up
    held landscape, desktop upright:     g = +0.961,+0.000,+0.296    -> +X = up of the desktop

So the sensor frame is the touch panel's natural portrait frame: X along the short edge, Y along the long one. In the normal landscape the desktop "up" is the sensor +X axis, same as `inputd` rot 1. Only these two poses were measured on the real tablet; the other three orientations come from the geometry and from injected values (point 2), **not from physically turning the tablet**. If a physical test shows left and right swapped, run the daemon with `--axes y,-x,z`-style remapping or swap `left`/`right` in `rot_name[]`.

`x400-sensord --print` output, real sensor, tablet held landscape:

    accel g=+0.961,+0.000,+0.296 orientation=normal lux=0
    accel g=+0.962,+0.002,+0.299 orientation=normal lux=2

The daemon switches the sensor on only while something needs it (`--rotate`, or a D-Bus client that claimed it) and writes `10`/`50` on exit. Not proven by a clean before/after: `ssp_sensor/enable` is also changed by the Android HAL (I read 0, 2, 16 and 134217744 at different times with nothing of mine running), so its value is not a usable check.
`kscreen-doctor` needs `WAYLAND_DISPLAY=wayland-x400`, `XDG_RUNTIME_DIR` and the session `DBUS_SESSION_BUS_ADDRESS` in the daemon's environment (without them it fails with `could not connect to display`); start the daemon from the same environment as the session.

## 2. Rotation

`kscreen-doctor output.1.rotation.<none|left|right|inverted>` works on the live KWin. The output size swaps for left/right:

    start: rotation 1 size 2112x1320
    set left     -> rotation 2 size 1320x2112
    set inverted -> rotation 4 size 2112x1320
    set right    -> rotation 8 size 1320x2112
    set none     -> rotation 1 size 2112x1320       (restored)

`x400-sensord --rotate --hook /tmp/x400-rotate-hook` end to end (accelerometer injected through `--accel-file`, KWin live):

    accel=4096,0,0     normal:      rotation 1 2112x1320 | X400_TOUCH_ROT=1
    accel=0,4096,0     left-up:     rotation 2 1320x2112 | X400_TOUCH_ROT=0
    accel=-4096,0,0    bottom-up:   rotation 4 2112x1320 | X400_TOUCH_ROT=3
    accel=0,-4096,0    right-up:    rotation 8 1320x2112 | X400_TOUCH_ROT=2
    accel=0,0,-4000    flat:        unchanged (still right-up)
    accel=4096,0,0     normal:      rotation 1 2112x1320   (final state, as at the start)

Orientation changes need 3 equal samples (~0.5 s) and a 15-degree margin past the sector edge, so it does not flap.

Touch and display (G0 and G1 own those files, nothing was touched):

| Orientation | kscreen-doctor | inputd `--rot` | logical size | capture/fb size |
|---|---|---|---|---|
| normal | none | 1 | 2112x1320 | 2112x1320 |
| left-up | left | 0 | 1320x2112 | 1320x2112 |
| bottom-up | inverted | 3 | 2112x1320 | 2112x1320 |
| right-up | right | 2 | 1320x2112 | 1320x2112 |

The `--rot` values come from the mapping in `x400/display/inputd.c` (`map()`): rot 1 is the current landscape, 0 is "no rotation" (portrait), 2 is the 180-degree turn, 3 is the opposite turn. They are **not verified with a finger**: nobody touched the screen in a rotated state, and in a rotated state `inputd` would also need its `--size` set to the logical size above. `capture` follows the output: after `rotation.left` the stream renegotiated to `1320x2112` and the frame header became `w=1320 h=2112`, while the helper in `X400Display`/`jni.c` paints a fixed landscape surface, so a rotated frame would come out sideways or cropped until G0 changes the helper. Rotating the session is therefore **not safe to enable** until G0 handles the portrait frame and `inputd` restarts (it has no runtime rotate; it needs a relaunch with `--rot N --size WxH`).

Hook: `x400-rotate-hook <orientation> <kscreen> <touch-rot> <w> <h>` writes `/tmp/x400-orientation.env`. G0 can replace it with whatever restarts `inputd`.

### KWin's own auto-rotate (not demonstrated)

KWin 6.6 (`libkwin.so.6`) reads `net.hadess.SensorProxy` over D-Bus (properties `HasAccelerometer`, `AccelerometerOrientation`, `HasAmbientLight`, `LightLevel`, methods `ClaimAccelerometer`/`ClaimLight`); it imports both `QDBusConnection::systemBus` and `sessionBus` and has no Qt Sensors dependency, so the Qt6 Sensors or iio-sensor-proxy path is not what it uses. `x400-sensord` serves that interface; `busctl introspect` shows all properties and methods. A second, throw-away `kwin_wayland --virtual` was started with the service on a private system bus (`system-bus.conf`, `/run/dbus/system_bus_socket`, a minimal `dbus-daemon`): KWin connected to that bus but never called `ClaimAccelerometer` (the accelerometer stayed off, `dbus-monitor` saw no call to the service). The strings in libkwin point to the reason: auto-rotate is enabled per output through `BackendOutput::setAutoRotateAvailable`, which a `--virtual` output does not set. I did not find a switch to force it and could not read the source (no network route to it). So with `--virtual`, **KWin itself will not rotate**; use `x400-sensord --rotate`. The D-Bus part stays useful for light (not tested against KWin either) and costs nothing.

## 3. Battery

The guest sees the host's real `power_supply` (the guest mounts sysfs, same tree):

    battery: type=Battery status=Full capacity=100 voltage_now=4378000 current_now=-3000 temp=284 present=1 health=Good
    usb: online=1        ac: online=0        wireless: online=0

`upower` 1.91.1 + `libupower-glib3` (arm64 debs from ports.ubuntu.com, unpacked under /tmp in the guest, **nothing installed**) with a private system bus:

    $ upower -e
    /org/freedesktop/UPower/devices/battery_battery
    /org/freedesktop/UPower/devices/line_power_ac   (and the other power_supply nodes)
    $ upower -i /org/freedesktop/UPower/devices/battery_battery
      present: yes   rechargeable: yes   state: fully-charged
      energy: 35.024 Wh   energy-full-design: 35.024 Wh   voltage: 4.378 V

No shim is needed for the battery: upowerd works on the real sysfs as is. Notes: it logs `Could not acquire inhibitor lock ... org.freedesktop.login1` (there is no logind, harmless for the battery) and wants `/etc/UPower/UPower.conf` (absent, defaults used). Plasma's battery applet talks to UPower over the **system** bus, so the guest needs the private system bus above (or a real `dbus-daemon --system`). Whether the Plasma applet then shows it was **not tested** (the live plasmashell was not restarted). USB charging state is `usb online=1`, `status=Full`; charging itself is done by the Android side's charger, nothing to do.

## 4. Brightness

Node: `/sys/class/backlight/panel` (driver `panel_drv_0`, type `raw`), `brightness` and `actual_brightness` 0..308, `bl_power` 0/4. Samsung's own `/sys/class/lcd/panel/` has no brightness attribute that matters. Test with `x400-brightness` inside the guest:

    original: 11 308 3
    set 50%:  154 308 50 actual=154
    set 200:  200 308 64 actual=200
    -10%:     170 308 55
    set 0:    1 308 0 (clamped to 1)
    off -> bl_power=4 actual=1 ; on -> bl_power=0
    restored: 11 308 3          (final brightness 11, bl_power 0, as before)

Incident: my first run of the script had no PATH in the chroot, so `cat` failed and it wrote the clamped minimum; the panel went to 1 for about a minute. I put it back to 11 and reran. The script now validates its input but a missing `cat` would still be the caller's problem.
`powerdevil` (needs a backlight through UPower or the `org.kde.Solid.PowerManagement` backend and logind for suspend): **not tested**. It will probably see the standard `backlight/panel` node, but it also needs `login1`; expect to run it without suspend or to write a small logind shim. Not done.

## 5. Screen power

- `svc power stayon true` fails in this state: `Failed to get power manager.` (One UI's `system_server` is stopped). Android's power manager is not available, so there is nothing to keep awake; the kernel does not suspend on its own while the `oneos_return` wake lock is held (`/sys/power/wake_lock`).
- Display state: `/sys/class/lcd/panel/display_on` reads 0 (root-only file) while the helper is painting, so it is not a reliable "panel on" flag. The visible panel power control is `backlight/panel/bl_power` (0 on, 4 off, tested above) plus `brightness`. Switching the actual DSI panel off is the display helper's job (G0): not tested here.
- Suspend to RAM (`/sys/power/state` = `freeze mem disk`) was **not tried**: it would stop the session and the adb link, and a wake-up path is not known.

## For G0 (exact list)

1. Packages in the rootfs image: `upower`, `libupower-glib3`, `libusb-1.0-0`, `usbutils` (dependencies of upower), plus `powerdevil` and its dependencies only if you want to try it (see `PACKAGES`). Build on the PC only: `libdbus-1-dev`.
2. Build `x400-sensord` (the exact command is in the header of `x400-sensord.c`; used here with `--sysroot=out/rootfs-plasma`, the headers from `libdbus-1-dev_1.16.2-2ubuntu4_arm64.deb` unpacked, and `-l:libdbus-1.so.3 -lm`). Deploy it with `x400-rotate-hook`, `x400-brightness` and `system-bus.conf` to `/tmp` or `/usr/local/bin` in the guest.
3. `session-inner.sh`, before Plasma: `mkdir -p /run/dbus` and `dbus-daemon --config-file=/path/system-bus.conf --nofork --nopidfile &`; export `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket` to everything that must see UPower. Then `upowerd &`.
4. After KWin and the mode setup: `x400-sensord --rotate --hook /path/x400-rotate-hook &` (add `--no-dbus` if there is no system bus; the rotation works without it).
5. Rotation only becomes usable when the helper handles a portrait frame and `inputd` is relaunched with the `--rot`/`--size` of the table above on every orientation change (what the hook is for). Until then start `x400-sensord` without `--rotate`.
6. Not decided by me: whether to run `powerdevil` (needs a logind shim), and the panel on/off from the display helper.

## Not done / caveats

- Physical rotation of the tablet was never done; only two real poses were measured (flat and landscape). Left/right are by geometry.
- No finger test of the touch table; no Plasma battery applet check; `powerdevil` not run.
- The sensor `enable` is a hub-wide state: if One UI is brought back, its `sensorservice` takes over and `x400-sensord` should be stopped first.
