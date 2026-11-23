#!/bin/sh
# Checks every hardware group of the running x400 session and prints one line per check (x400/HARDWARE.md). Run on the PC with adb and root on the tablet:
#   sh x400/verify-session.sh
# Read-only except for: a 2 s tone at low amplitude through the audio bridge, a few seconds of each camera, and a rotation of the output that is put back.
# It never calls `dumpsys media.audio_flinger` (it crashes audioserver without system_server) and never opens a capture PCM.
set -u
SERIAL="${ANDROID_SERIAL:-$(adb get-serialno 2>/dev/null)}"
[ -n "$SERIAL" ] || { echo "no adb device" >&2; exit 1; }
adb -s "$SERIAL" shell 'su -c id' 2>/dev/null | grep -q 'uid=0' || { echo "no root on the tablet" >&2; exit 1; }
T=$(mktemp); trap 'rm -f "$T"' EXIT
cat > "$T" <<'EOS'
B=/data/adb/ksu/bin/busybox
K=$(pidof kwin_wayland | cut -d' ' -f1); [ -n "$K" ] || { echo "FAIL session: no kwin_wayland (is the session up?)"; exit 1; }
R=/proc/$K/root
[ -f $R/tmp/kwin.env ] || tr '\0' '\n' < /proc/$K/environ | sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z_0-9]*\)=\(.*\)\$/export \1='\2'/" | grep '^export ' > $R/tmp/kwin.env
cat > $R/tmp/vrun.sh <<'EOT'
. /tmp/kwin.env
export PATH=/usr/bin:/usr/sbin:/bin:/sbin XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-x400 HOME=/tmp
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket
exec "$@"
EOT
g() { timeout ${TMO:-40} $B nsenter -t $K -m $B chroot $R /bin/sh /tmp/vrun.sh "$@" 2>&1; }
ok() { echo "OK   $1"; }
bad() { echo "FAIL $1"; }
alive() { [ -n "$(pidof $1)" ] && ok "process $1 [$(pidof $1 | tr ' ' ',')]" || bad "process $1 not running"; }

echo "== session: $(date +%T) kwin $K, mode $(g kscreen-doctor -o | tr -d '\033' | grep -a -o '[0-9x]*@[0-9.]*[^ ]*\*' | head -1), tombstones $(ls /data/tombstones | wc -l)"
for p in kwin_wayland plasmashell pipewire wireplumber inputd keys x400-pwbridge x400-camsrc x400-sensord upowerd NetworkManager wpa_supplicant bluetoothd btattach; do alive $p; done
for p in x400-audiod x400-camd fakesvc audioserver; do alive $p; done

echo "== G1 input"
g cat /tmp/inputd.log | grep -a -q 'event8.*grabbed' && ok "inputd holds the touchscreen ($(g cat /tmp/inputd.log | grep -a grabbed | head -1))" || bad "inputd without the grab"
g cat /tmp/keys.log | grep -a -q 'without grab' && ok "keys reads event0/event1 without grab ($(g grep -a -c 'power ->' /tmp/keys.log) power presses so far)" || bad "keys log"
g cat /etc/xdg/kwinrc | grep -a -q plasma.keyboard && ok "kwinrc selects plasma-keyboard" || bad "kwinrc input method"

echo "== G2 sensors and power"
SP="g busctl --address=unix:path=/run/dbus/system_bus_socket"
$SP get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy HasAccelerometer | grep -a -q true && ok "SensorProxy HasAccelerometer true" || bad "SensorProxy accelerometer"
$SP get-property net.hadess.SensorProxy /net/hadess/SensorProxy net.hadess.SensorProxy HasAmbientLight | grep -a -q true && ok "SensorProxy HasAmbientLight true" || bad "SensorProxy light"
b=$(g upower -i /org/freedesktop/UPower/devices/battery_battery | grep -a -E 'state|percentage|voltage' | tr -s ' ' | tr '\n' ' ')
[ -n "$b" ] && ok "UPower battery: $b" || bad "UPower battery"
bl=$(g /tmp/x400/sensors/x400-brightness); echo "$bl" | grep -a -q '^[0-9]* 308 ' && ok "brightness (value max percent): $bl" || bad "brightness: $bl"
g kscreen-doctor output.1.rotation.left >/dev/null; sleep 2; r1=$(g kscreen-doctor -o | tr -d '\033' | grep -a Rotation | sed 's/.*0m//')
g kscreen-doctor output.1.rotation.none >/dev/null; sleep 2; r2=$(g kscreen-doctor -o | tr -d '\033' | grep -a Rotation | sed 's/.*0m//')
[ "$r1" = 2 ] && [ "$r2" = 1 ] && ok "KWin rotation left=2 then none=1 (put back)" || bad "rotation $r1 $r2"

echo "== G3 audio"
A=/data/local/tmp/oneos/x400/groups/audio
logcat -d -b all 2>/dev/null | grep -a -q 'initStreamVolume() stream 3, min 0, max 15' && ok "audio_policy has STREAM_MUSIC range 0..15 (x400-setvol)" || bad "audio_policy volume range not set"
g python3 -c "
import wave,struct,math
w=wave.open('/tmp/vt.wav','w'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh',int(3000*math.sin(2*math.pi*1000*i/48000)),int(3000*math.sin(2*math.pi*1000*i/48000))) for i in range(48000*2)))
w.close()"
n0=$(wc -l < /data/local/tmp/oneos/logs/x400-audiod.log)
g pw-cat -p --target x400.altavoces /tmp/vt.wav; sleep 1
tail -n +$((n0+1)) /data/local/tmp/oneos/logs/x400-audiod.log | grep -a -E 'out: .*bytes' | tail -n 1 | grep -a -q bytes && ok "tone of the guest reached x400-audiod: $(tail -n +$((n0+1)) /data/local/tmp/oneos/logs/x400-audiod.log | grep -a 'bytes' | tail -n 1 | cut -c1-90)" || bad "no bytes at x400-audiod"
echo "NOTE speaker output itself is judged by ear (the tablet's own microphone loses a continuous tone after ~5 s; x400/audio/RESULT.md)"

echo "== G4 radios"
g nmcli -t -f GENERAL.STATE device show wlan0 | grep -a -q connected && ok "wlan0 $(g nmcli -t -f GENERAL.STATE,IP4.ADDRESS device show wlan0 | tr '\n' ' ')" || bad "wlan0 not connected"
c=$(g nmcli -t networking connectivity); [ "$c" = full ] && ok "NetworkManager connectivity: full" || bad "connectivity: $c"
h=$(g python3 -c "import urllib.request;print(urllib.request.urlopen('https://example.com',timeout=10).status)" | tail -n 1); [ "$h" = 200 ] && ok "HTTPS example.com -> 200" || bad "HTTPS: $h"
g hciconfig hci0 | grep -a -q 'UP RUNNING' && ok "hci0 UP RUNNING, $(g hciconfig hci0 | grep -a 'BD Address' | tr -s ' ' | cut -c1-40)" || bad "hci0 not up"
g bluetoothctl show | grep -a -q 'Powered: yes' && ok "bluetoothctl Powered: yes" || bad "bluetooth not powered"

echo "== G5 cameras"
nv=$(g pw-cli ls Node | grep -a -c v4l2_input); [ "$nv" = 0 ] && ok "no useless v4l2 nodes from the ISP" || bad "$nv v4l2 nodes"
ng=$(g pw-cli ls Node | grep -a -c -E 'x400-camera-front|x400-camera-back|x400.altavoces|x400.microfono'); [ "$ng" -ge 4 ] && ok "our PipeWire nodes: camera front/back, Altavoces, Microfono" || bad "our nodes: $ng"
for cam in front back; do
  o=$(TMO=45 g /tmp/x400/camera/x400-camtest x400-camera-$cam 8 /tmp/vc-$cam.ppm | grep -a fps | head -1)
  echo "$o" | grep -a -q fps && ok "camera $cam delivered $o to the guest" || bad "camera $cam: no frames"
done
sleep 4; [ "$(getprop service.camera.running)" = 0 ] && ok "cameras closed again after the clients left" || bad "service.camera.running=$(getprop service.camera.running)"

echo "== G6 display"
grep -a 'frames shown' /data/local/tmp/oneos/data/x400/logs/display.log | tail -n 1 | cut -c1-100 | sed 's/^/OK   helper: /'
echo "== end: tombstones $(ls /data/tombstones | wc -l), x400vt [$(pidof x400vt)], kwin [$(pidof kwin_wayland)]"
EOS
adb -s "$SERIAL" push "$T" /data/local/tmp/x400-verify.sh >/dev/null
adb -s "$SERIAL" shell 'su -c "sh /data/local/tmp/x400-verify.sh"' 2>&1 | tr -d '\r'
