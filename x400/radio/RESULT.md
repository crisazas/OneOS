# x400/radio (G4) RESULT: Wi-Fi and Bluetooth work in the integrated session

Measured on the live session (started 07:05, root recovered, kernel 5.15, One UI stopped), 2026-11-23 07:10-07:23 (the guest's `date` is UTC-6 behind the log's 13:xx; both appear below).
Nothing was restarted: `kwin_wayland` 26791, `x400vt` 25771 and `capture` 32460 had the same pids at the end. No oops, no reboot, no `wlbt`/`scsc` error in `dmesg` during any step.

## 1. Result in one table
| Piece | State | Proof (section) |
|---|---|---|
| Wi-Fi: NetworkManager + Ubuntu wpa_supplicant on `wlan0`, saved network from One UI | **works** | 3 |
| Real connectivity from the guest (ICMP, DNS, HTTPS, NM `full`) | **works** | 3.4 |
| Bluetooth controller (`hci0`, BD address of One UI, Powered, scan) | **works** | 4 |
| Bluetooth pairing and A2DP audio | **not tried** | 6 |
| Clean stop (`guest.sh stop`), `scsc_h4_0` released | **works** | 5 |
| Both radios on at every session start | done through the flag files in `stage/` | 7 |
| plasma-nm and bluedevil in the tray | **work after plasmashell restarts once**; they do not at session start (see 8) | 8 |

## 2. What was prepared and changed in this round (`x400/radio/stage/`)
`btbridge.py`, `host.sh`, `guest.sh`, `conf/NetworkManager.conf`, `conf/bluetooth-main.conf`, `enable-wifi`, `enable-bt`, `.gitignore`; `test-btbridge.py` next to it.
Changes made while running the risky phase (each one came from a measured failure):
- `btbridge.py --probe`: the controller sends a **Command Status** `04 0f 04 00 01 00 00` (opcode 0) before the Reset's Command Complete. The first probe read only 7 bytes, got that status and returned rc=1 although the controller was fine. It now reads H4 events until the Command Complete of opcode `0c03` (and the test in `test-btbridge.py` sends the status first, 16/16 PASS in the guest).
- `host.sh` also copies `/mnt/vendor/efs/bluetooth/bt_addr` to `/run/x400-radio/bt_addr` (the guest does not see `/mnt/vendor`); `guest.sh` writes it to the `scsc_bt` parameter (B4). `host.sh` keeps `pid` and flag files in the output dir (no `rm -rf`).
- `guest.sh` does not start a second wpa_supplicant/NetworkManager/bridge if its pid files are alive (a second run during B5 launched a second wpa_supplicant: `dbus: Could not request service name: already registered`, which dropped the association for ~2 min until `nmcli connection up` re-attached it).
- `conf/NetworkManager.conf`: `[connectivity]` with `nmcheck.gnome.org` (see 3.5).
- `host.sh` runs in the guest's mount namespace: `$R/run` is a tmpfs there; from the plain Android namespace it fails with `Read-only file system`. Run it with `nsenter -t $(pidof kwin_wayland) -m` (that is how `guest_start.sh` runs it).

## 3. Wi-Fi (steps 0, W2, W3, W4)
### 3.1 Step 0: image, stage, overlays, keyfile
```
$ ls /usr/sbin/NetworkManager /usr/sbin/wpa_supplicant /usr/bin/btattach /usr/libexec/bluetooth/bluetoothd /usr/bin/nmcli    (all present; curl is NOT in the image)
$ cat /tmp/x400-radio.log          13:05:46 radio: both radios off: only the writable places are ready
$ mount | grep -E ' /(etc|var/lib) '
overlay on /etc type overlay (rw,relatime,seclabel,lowerdir=/etc,upperdir=/run/x400-radio/ov/etc/up,workdir=/run/x400-radio/ov/etc/work)
overlay on /var/lib type overlay (rw,relatime,seclabel,lowerdir=/var/lib,upperdir=/run/x400-radio/ov/var/lib/up,workdir=/run/x400-radio/ov/var/lib/work)
$ ls -l /run/x400-radio/*.nmconnection     -rw-------+ 1 root root 304 Nov 23 13:05 /run/x400-radio/x400-android-1.nmconnection
$ sed (psk redacted) /run/x400-radio/x400-android-1.nmconnection
[connection] id=Internet uuid=c8205c76-... type=wifi interface-name=wlan0 autoconnect=true autoconnect-priority=10
[wifi] mode=infrastructure ssid=Internet     [wifi-security] key-mgmt=wpa-psk psk=<REDACTED len=15>
[ipv4] method=auto route-table=1014          [ipv6] method=auto route-table=1014
```
Before: `iw dev wlan0 link` -> `Not connected.`; `getprop init.svc.wpa_supplicant` -> `stopped`; `wlan0` only had the stale lease `192.168.68.109/22`.
The `.nmconnection` really comes from One UI's `WifiConfigStore.xml` (`Internet`, WPA_PSK; the key was never printed or stored in git; the file lives in the guest's `/run`, tmpfs, mode 600).

### 3.2 W2/W3: first start (`touch /run/x400-radio/enable-wifi; sh /tmp/x400/radio/guest.sh`)
```
13:10:34 radio: wifi: enabled
13:10:35 radio: wifi: wpa_supplicant 12349, NetworkManager 12610
$ tail /tmp/wpa_supplicant.log
Successfully initialized wpa_supplicant
wlan0: Trying to associate with SSID 'Internet'
wlan0: Associated with 32:16:9d:76:48:db
wlan0: WPA: Key negotiation completed with 32:16:9d:76:48:db [PTK=CCMP GTK=CCMP]
wlan0: CTRL-EVENT-CONNECTED - Connection to 32:16:9d:76:48:db completed [id=0 id_str=]
```
So Ubuntu's wpa_supplicant 2.11 (`-u`, nl80211) works with `scsc_wlan`: the vendor binary fallback was not needed. `dmesg` showed no `slsi_`/`wlbt` error.

### 3.3 NetworkManager manages only wlan0, route and DNS
```
$ nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device
wlan0:wifi:connected:Internet        dummy0, ifb*, tunl0, gre*, sit0, lo, umts_*, p2p0, swlan0, wifi-aware0, wlan2, wlan3, p2p-dev-wlan0: all "unmanaged"
$ iw dev wlan0 link          Connected to <bssid> (on wlan0)  SSID: Internet  freq: 5220.0  signal: -33 dBm  rx bitrate: 6.0 MBit/s  tx bitrate: 866.6 MBit/s
$ ip -o addr show wlan0      inet 192.168.68.109/22 brd 192.168.71.255 scope global dynamic noprefixroute wlan0
$ ip route show table 1014   default via 192.168.68.1 dev wlan0 proto dhcp src 192.168.68.109 metric 600
                             192.168.68.0/22 dev wlan0 proto kernel scope link src 192.168.68.109 metric 600
$ ip rule | grep -E '^(31000|32000)'     31000: from all fwmark 0x0/0xffff iif lo lookup wlan0    32000: from all unreachable
$ ip route get 8.8.8.8       8.8.8.8 via 192.168.68.1 dev wlan0 table wlan0 src 192.168.68.109 uid 0
$ ls -la /etc/resolv.conf    /etc/resolv.conf -> /run/NetworkManager/resolv.conf    (nameserver 192.168.1.254, 192.168.68.1)
```
`route-table=1014` was the right number (`wlan0` in `/data/misc/net/rt_tables`); no `ip rule` had to be added. The udev entry trick (`/run/udev/data/n14`) was enough: `wlan0` is `connected`, never `unmanaged`.

### 3.4 Real connectivity from the guest
```
$ ping -c3 8.8.8.8      64 bytes from 8.8.8.8: icmp_seq=1 ttl=117 time=43.1 ms ... 3 packets transmitted, 3 received, 0% packet loss, rtt avg 42.507 ms
$ getent hosts example.com       2606:4700:10::6814:179a example.com / 2606:4700:10::ac42:93f3 example.com
$ python3 urllib https://example.com       HTTP 200 OK | server: cloudflare | bytes: 577        (no curl in the image)
$ busctl --system get-property org.freedesktop.NetworkManager /org/freedesktop/NetworkManager org.freedesktop.NetworkManager State    u 50 (connected global)
```
### 3.5 Connectivity "limited" fixed
After plasma-nm appeared, its Wi-Fi icon had an orange mark: NM's `Connectivity` was `3` (limited) because Ubuntu's default check URL (`http://connectivity-check.ubuntu.com/`) answers 204 but NM expects the 200 body (`ConnectivityCheckEnabled false`, `Uri ""` in this build/config). `conf/NetworkManager.conf` now has
```
[connectivity] enabled=true uri=http://nmcheck.gnome.org/check_network_status.txt response=NetworkManager is online interval=300
```
```
$ nmcli general reload conf; busctl ... CheckConnectivity     u 4        $ nmcli -t -f STATE,CONNECTIVITY general     connected:full
```
(the orange pixels in the tray icon were gone in the next screenshot: 10 orange pixels before, 0 after, 13:21.)

### 3.6 W4 (plasma-nm sees the connection)
`busctl --system` shows `org.freedesktop.NetworkManager` (pid 12610) and `fi.w1.wpa_supplicant1` (pid 12349); `busctl --user list | grep plasmanetworkmanagement` -> `org.kde.plasmanetworkmanagement 27323 kded6`; the tray shows the Wi-Fi icon (see 8).

## 4. Bluetooth (B1-B5)
```
$ python3 /tmp/x400/radio/btbridge.py --dry-run          /dev/scsc_h4_0 exists, mode 660, rdev 497:0; not opened
baselines: init.svc.vendor.bluetooth-1-1 running; lshal: android.hardware.bluetooth@1.0::IBluetoothHci/default 0/1 (pid 900); service_start_count=1; no holder of scsc_h4_0
```
**B3 (first opening of `/dev/scsc_h4_0`)**, 07:12:11, first with the old probe, then with the fixed one:
```
reply: 040f0400010000          rc=1   (old probe: it stopped after the Command Status)   start_count 1 -> 2
event: 040f0400010000
event: 040e0401030c00
reset complete, status 0       rc=0                                                      start_count 2 -> 3, holders after: none, dmesg: nothing new, Wi-Fi ping still 0% loss
```
So `scsc_h4_0` carries plain H4 HCI, can be opened by us while Android's HAL idles (`0/1`), and the firmware answers.
**B4 (BD address)** (`Read BD_ADDR` = `01 09 10 00` through a throw-away open, then closed):
```
before:  param bluetooth_address=0, controller answered BD_ADDR A5:F0:F0:13:0F:2E (= bluetooth_address_fallback)
efs:     /mnt/vendor/efs/bluetooth/bt_addr = 24:A4:52:12:2C:F7
$ echo 0x24a452122cf7 > /sys/module/scsc_bt/parameters/bluetooth_address ; cat it   40288170159351  (= 0x24a452122cf7 as a decimal integer)
after:   BD_ADDR 24:A4:52:12:2C:F7
```
The parameter is read at the controller's service start (each open that starts the service): the address changed with the very next open, no reboot.
**B5 (`touch /run/x400-radio/enable-bt`, `guest.sh`)**:
```
13:14:31 radio: bt: enabled     13:14:32 radio: bt: bridge 15211 on /dev/pts/0, btattach 15482, bluetoothd 15490
$ hciconfig -a   hci0: Type: Primary  Bus: UART   BD Address: 24:A4:52:12:2C:F7  ACL MTU: 1021:20  SCO MTU: 0:0  UP RUNNING
                 Name: 'Galaxy Tab Linux'  HCI Version: 5.3 (0xc)  Revision: 0x2e7c   Features: 0xbf 0xfe 0x8f 0xfe 0xdb 0xff 0x5b 0x87
$ bluetoothctl show      Controller 24:A4:52:12:2C:F7 (public)  Manufacturer: 0x0075 (117)  Version: 0x0c (12)  Powered: yes  PowerState: on  Pairable: no
                         UUID: Audio Sink / Audio Source / A/V Remote Control (Target) / Handsfree Audio Gateway / GATT / PnP / Device Information
$ bluetoothctl --timeout 15 scan on    12 devices in 15 s (MACs hidden here), among them: console-HP-Laptop-15-dy2xxx, [LG] webOS PJTR HU710PB, GoogleTV2762, TVPlayer, CAR-KIT, S503_353506, S534_ee988d
$ hciconfig hci0        RX bytes:15862 acl:0 sco:0 events:339 errors:0      TX bytes:3600 acl:0 sco:0 commands:74 errors:0
holder of scsc_h4_0: only pid 15211 (python3 btbridge.py)
```
`Privacy=off` kept bluetoothd from needing AF_ALG: `/tmp/bluetoothd.log` has no `Failed to open crypto`. It registered the A2DP endpoints (sbc, aptx_ll, faststream, opus_05, ...) from PipeWire's bluez5 plugin.

## 5. Clean stop and Android's side
```
$ sh /tmp/x400/radio/guest.sh stop        rc=0     (log: 13:16:47 radio: stop)
procs left (NetworkManager|wpa_supplicant|btbridge|btattach|bluetoothd): none        holders of scsc_h4_0: none        /sys/class/bluetooth: empty
$ tail /tmp/btbridge.log        btbridge: opened /dev/scsc_h4_0 / btbridge: closed /dev/scsc_h4_0
$ python3 btbridge.py --probe (a new open right after stop)   event: 040e0401030c00  reset complete, status 0  rc=0   (service_start_count=6)
$ getprop init.svc.vendor.bluetooth-1-1   running      dmesg: no new line with wlbt/scsc
```
What this proves: after `stop`, nothing of ours holds `scsc_h4_0`, and the node can be opened again (by my probe, the same way Android's `libbt-vendor` does: `open("/dev/scsc_h4_0")`). What it does **not** prove: that One UI's framework Bluetooth service initializes the HAL, since the framework is not running here; that needs a real return to One UI.
`stop` also did `ip addr flush dev wlan0` and left `wlan0` `down`; One UI's supplicant will take it back on return (not tested, the session is still up).
Second start from zero, same session (`guest.sh stop`, flag files removed from `/run`, then `guest.sh` with only the stage's flags, 13:22:23): `wpa_supplicant 3531`, `NetworkManager 3733`, `bridge 3761 on /dev/pts/0, btattach 4024, bluetoothd 4032`; `wlan0:connected:Internet`, `connected:full`, `ping 8.8.8.8 rtt avg 39.780 ms`, `getent hosts example.com`, `HTTPS 200`, `hci0 ... UP RUNNING`, `Controller 24:A4:52:12:2C:F7 ... Powered: yes`.

## 6. Not done / not proven
- Pairing a device and A2DP audio through PipeWire (only discovery was tried). HFP/SCO (calls, microphone) is not expected to work (SCO MTU 0:0).
- Return to One UI with the radios up (the framework's own Wi-Fi/Bluetooth restart after `guest.sh stop`); run `guest.sh stop` first (G0's return path).
- `bluetoothctl scan` found 12 devices; no other BLE/BR device types were tried. LE with `Privacy=off` only.
- Reboot-time behavior: the flags are in the stage, so a new session starts both radios; this was shown by a manual `guest.sh` run with the stage flags, not by restarting the session.
- NM/BlueZ state is RAM only (overlay on `/etc` and `/var/lib`): pairings and manually added networks vanish with the session. Only the One UI network is regenerated from `WifiConfigStore.xml` at each start.
  (Superseded on 2026-11-23: the guest's root is now a persistent overlay, `x400/oneos/guest_prepare.sh`, and `radio/stage/guest.sh` no longer makes the RAM overlays when `/etc` and `/var/lib` are writable, so networks and pairings are kept. Not re-measured for the radios: only files, packages and `/etc` were.)

## 7. Persistent flags
`x400/radio/stage/enable-wifi` and `x400/radio/stage/enable-bt` (empty files, committed). `deploy.sh` pushes `stage/` to `/data/local/tmp/oneos/x400/groups/radio/`, `guest_start.sh` binds it at `/tmp/x400/radio`, and `guest.sh` looks for `enable-*` there (and in `/run/x400-radio/`). To turn one off: delete the file from the stage (or `rm` it on the tablet in `/data/local/tmp/oneos/x400/groups/radio/`). `session-inner.sh` already runs `grp radio` after inputd.

## 8. plasma-nm and bluedevil in the tray
At session start `plasmashell` (pid 28529) loaded before the system bus had NetworkManager/BlueZ names: its tray showed only a red-crossed network icon (screenshot 13:18, 8 red pixels at (1943,1300)) and no Bluetooth item; `plasmashell` had `libplasmanm_internalplugin.so` mapped but not `libKF6BluezQt`/`libbluedevilcomponents`.
Restart done inside the guest, without touching KWin/x400vt/capture (pids identical afterwards):
```
$ kill -TERM 28529; (environment taken from /proc/28529/environ) setsid nohup plasmashell > /tmp/plasma2.log   -> pid 21977
maps now:  libKF6BluezQt.so.6.24.0, libbluedevilcomponents.so, org.kde.plasma.networkmanagement.so, libplasmanm_internalplugin.so
screenshot 13:19: tray shows a Bluetooth icon and a Wi-Fi icon (red-cross pixels 8 -> 0)
```
`kded6` (pid 27323, `org.kde.plasmanetworkmanagement`) was not restarted. **Order for G0:** in `session-inner.sh`, `grp radio` must run, and NetworkManager/BlueZ must be on the system bus, **before** `plasmashell` starts, or the applets are missing until plasmashell is restarted. Simple options: (a) start `plasmashell` after `grp radio` and a short wait (in the 13:22:23 start the radios logged `wifi: wpa_supplicant` after 1 s and `bt: bridge` after 2 s; the tray was not timed), or (b) after `grp radio`, `sleep 12; kill plasmashell; plasmashell &`. I did not change `session-inner.sh` (G0's file). The systray list in `plasma-org.kde.plasma.desktop-appletsrc` (`extraItems`) has no network/bluetooth entries and did not need any: the applets are shown by their kded/StatusNotifier registration.

## 9. Lines for G0
- `session-inner.sh`: move `grp radio` before `plasmashell` (or restart plasmashell once after it, see 8). `grp radio` already uses `/tmp/x400/radio/guest.sh`; it needs G2's `grp sensors` first for the system bus (it waits 10 s; started after sensors it is fine, started before it would create the bus itself with G2's config).
- Return path (`x400_return.sh` / before One UI starts): `sh /tmp/x400/radio/guest.sh stop`.
- Nothing else in `guest_start.sh`. `host.sh` must still be launched after `$R/run` is mounted and inside the session's mount namespace.
- `PACKAGES`: add `curl` to the image if you want `curl` for diagnostics (the image has none; python3 urllib was used).

## 10. `oneos_btaudio` (unchanged, for G0)
In the session before this one (pid 7991, started 04:37) `/data/local/tmp/oneos/oneos_btaudio.log` said `android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default registered again (round 5400): exception 0`, once per second, and `dmesg` got
`servicemanager: Service '...' originally registered from PID 7991 but it is being registered again` every second; at 06:11 it was sleeping, 3 threads, `VmRSS 2608 kB`, 117/214 ticks of utime/stime after ~90 min.
In the current session (started 07:05) it is a new process, pid 21530, 13/33 ticks of utime/stime at uptime 3682 s (I did not re-measure its log rate). It does not touch `scsc_h4_0`; all the radio steps ran fine next to it.
**Not dangerous for the session**, but it fills the `dmesg` ring (which the risky phase uses to look for `wlbt`/`scsc` errors: I filtered `IBluetoothAudio|Untracked|!@` lines out in every diff).

## 11. State left on the tablet
Both radios are running now (`wlan0` connected to `Internet`, `hci0` up). Files of mine to delete when no longer needed: `/data/local/tmp/g4/` (`iw`, `rfkill`, libnl), `/data/local/tmp/g4t/` (**contains a fake key `pa$$w0rd...` in `fake.xml`/`out/`**), `/data/local/tmp/*.sh` probe scripts, `/data/local/tmp/dmesg-*.txt`, `/data/local/tmp/shot-*.png`.
On the PC: `out/g4shots/` (screenshots, not in git), `/tmp/opencode/g4debs/`.
