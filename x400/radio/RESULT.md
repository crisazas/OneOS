# x400/radio (G4) RESULT: implementation prepared, nothing risky run

**Stopped early, on purpose.** At ~06:23 the tablet rebooted by itself while I was only pushing files (`adb` dropped, came back with `uptime 2 min`,
`su: inaccessible or not found`, `kwin_wayland` gone). Root (DFroot) is lost until KernelSU is restored over adb; CTX.md says to report and stop, so I did not touch the device again.
I do not know what caused the reboot (no radio, rfkill, `/dev/scsc_h4_0`, `/dev/ttySAC*` or Android service was touched by me in this round; the only mounts I made were an overlay and a tmpfs scratch test
in the guest earlier, both unmounted). The last thing running on the PC side was `apt-get install -s` in the chroot (no relation to the tablet).
Consequence: `guest.sh` was written and syntax-checked but **not run end to end**; see "Not tested".

## 1. What is prepared (`x400/radio/stage/`, nothing there holds a secret)
| File | Job |
|---|---|
| `btbridge.py` | H4 relay `/dev/scsc_h4_0` <-> pty. Does not open the node until `SIGUSR1` (or `--open-now`); `SIGUSR2` closes it, `SIGTERM`/`SIGHUP`/`SIGINT`/EOF end it cleanly and remove the pty file. `--dry-run` only `stat`s the node. `--probe` = step B3 (opens, sends `HCI_Reset`, prints the reply, closes). `--trace FILE` logs both directions in hex. |
| `host.sh` | Android side, root, background. Reads `/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml` and writes one `x400-android-N.nmconnection` (mode 600) per saved network (WPA-PSK, SAE, open) into `/data/local/tmp/oneos/rootfs/run/x400-radio/` (dir 700) = the guest's `/run/x400-radio`, a **tmpfs** made by `guest_start.sh` before host.sh runs. Never prints a key; skips hex SSIDs, unsupported types and keys <8 chars. `ready` marks the end. |
| `guest.sh` | Guest side. `guest.sh` prepares writable places; with `enable-wifi` / `enable-bt` it starts that radio; `guest.sh stop` ends what it started (by pid file, never by name). |
| `conf/NetworkManager.conf` | `plugins=keyfile`, `unmanaged-devices=except:interface-name:wlan0`, `wifi.backend=wpa_supplicant`, `auth-polkit=false`, `dns=default`, `rc-manager=symlink`, `ipv4/ipv6.route-table=1014`. |
| `conf/bluetooth-main.conf` | `Privacy=off`, `JustWorksRepairing=never`, `GATT Cache=no` (no `AF_ALG` in this kernel). BlueZ state is `/var/lib/bluetooth` (overlay, RAM). |
| `.gitignore` | `*.nmconnection`, `bt_addr`, `ready`, `enable-*`. |
| `../test-btbridge.py` | the dry test below. |

Network table number (measured before the reboot): `cat /data/misc/net/rt_tables` -> `1014 wlan0`, `1000000014 wlan0_local`. Rules: `31000: from all fwmark 0x0/0xffff iif lo lookup wlan0` and
`32000: unreachable`; netd already had `default via 192.168.68.1 dev wlan0` in table 1014. So no extra `ip rule` is needed: NM only has to put its routes in table 1014 (`route-table=1014`, in the conf and in each generated keyfile).

### Flag files (both radios start OFF)
`enable-wifi` / `enable-bt` are looked for in `/tmp/x400/radio/` (the bound stage: G0 or host.sh can create them under `/data/local/tmp/oneos/x400/groups/radio/`) and in `/run/x400-radio/` (by hand in the guest).
A `deploy.sh` run replaces the stage, so flags in it are wiped on the next deploy.

### Writable places (the list G0 asked for)
Nothing needs to be mounted by `guest_start.sh`. `guest.sh` does, inside the guest's own mount namespace: `mount -t overlay overlay -o lowerdir=/etc,upperdir=/run/x400-radio/ov/etc/up,workdir=...` on **`/etc`** and on **`/var/lib`**
(upper layers in the guest's `/run` tmpfs, so RAM only; pairings and NM state vanish with the session), `mkdir -p` of `/etc/NetworkManager/system-connections` (700), `/var/lib/NetworkManager`, `/var/lib/bluetooth` (700),
`/run/NetworkManager`, `/run/wpa_supplicant`, `/run/dbus`, `/run/udev/data`, and `ln -s /run/NetworkManager/resolv.conf /etc/resolv.conf`. `/var/run` is already a symlink to `/run` in the image.
If the overlay on `/etc` fails, the log line `overlay on /etc failed` is in `/tmp/x400-radio.log` and NM cannot write its keyfiles.

### Order in `guest.sh`
1. writable places (above) 2. `wait_bus`: waits up to 10 s for `/run/dbus/system_bus_socket` (**G2's guest.sh creates it with `x400/sensors/system-bus.conf`; this script never creates a second bus**;
only if it is still absent after 10 s it starts one with G2's own config, so the bus is the same kind) 3. Wi-Fi if `enable-wifi`: udev database entry for wlan0, copy the keyfiles, `wpa_supplicant -u -O /run/wpa_supplicant`, `NetworkManager --no-daemon`
4. BT if `enable-bt`: `btbridge.py`, `SIGUSR1`, `btattach -B <pty> -P h4`, `bluetoothd -n -f /run/x400-bluetooth-main.conf`.
**No `systemd-udevd`**: `udev` is in the image, but NM only needs the database entry, so `guest.sh` writes `/run/udev/data/n<ifindex>` itself (tested below) and nothing touches `/dev`, which is shared with Android.

## 2. Dry-run evidence
**Relay (`python3 x400/radio/test-btbridge.py`, run in the live guest, python 3.14.4, a pty stands in for `/dev/scsc_h4_0`; `/dev/pts` is not mounted on the PC):** 16 of 16 PASS
```
PASS dry-run reports the node /dev/pts/0 exists, mode 620, rdev 136:0; not opened
PASS dry-run fails on a missing node          PASS pty file written /dev/pts/1       PASS node NOT opened before the order
PASS node opened after SIGUSR1                PASS HCI Reset reaches the controller side 01030c00
PASS answer comes back to the host side 040e0401030c00     PASS trace has both directions     PASS 10 KB host->controller intact 10240 bytes
PASS node closed after SIGUSR2   PASS node reopened on demand   PASS SIGTERM exits cleanly (0)   PASS pty file removed
PASS SIGHUP exits cleanly        PASS EOF on the device ends the bridge       PASS --probe sends Reset and sees the Command Complete reply: 040e0401030c00
```
**`host.sh` with a fake store (Android shell, `X400_RADIO_STORE`/`X400_RADIO_OUT` overrides):** three fake networks (WPA-PSK+SAE with `&amp;`, `&quot;`, `\`, `<>` in SSID/key; open; 3-char key) gave 2 keyfiles
(`rw-------`, dir `drwx------`) and `key too short for one network`; a key containing a backslash comes out escaped as `\\` (checked with `od -c` after fixing an `echo` that reduced it). The real store was **not** read in this round
(it has 1 network, `Internet`, WPA_PSK + auto-upgraded SAE; the key was never printed).
**NetworkManager config:** `NetworkManager --print-config --config=stage/conf/NetworkManager.conf --config-dir=/nonexistent` (PC build 1.46) accepts the file and shows `unmanaged-devices=except:interface-name:wlan0`,
`wifi.backend=wpa_supplicant`, `auth-polkit=false`, `ipv4.route-table=1014`. The image's NM is 1.54.3; same keys, not run.
**wlan0 "initialized" without udevd (live guest, then restored):** `pyudev` `is_initialized` was `False`; after `printf 'I:...\nE:INTERFACE=wlan0\nG:x400\n' > /run/udev/data/n14` it was `True`; after removing it, `False` again; `/run` back to `dbus mount user`.
**Mounts in the guest namespace:** `mount -t overlay ... lowerdir=/usr/share/doc,upperdir=/tmp/ovt/up,...` mounted, wrote `NEWFILE` only to the upper dir, unmounted; `mount -t tmpfs` also works. (The overlay on `/etc` and `/var/lib` themselves was not tried.)

## 3. Not tested (and why)
- **`guest.sh` end to end** (the reboot cut the test: it was going to run `guest.sh` with both radios off inside `unshare -m`). Run it first when root is back (step 0).
- Overlay on `/etc` and `/var/lib` themselves; `install`, `pkill`, `mountpoint`, `btattach`, `NetworkManager`, `wpa_supplicant` presence in the guest (`btattach`/NM/wpa come with the packages; the guest has none of them yet).
- Dependency check of `PACKAGES` with `apt-get install -s` against the rootfs status: started, no result before the reboot (the first round, in RESEARCH.md, was clean: 8 + 46 + 25 new packages, all from the archive). `dbus`, `udev`, `iproute2`, `python3` are in the image.
- `mac80211_hwsim` exists on the PC kernel (`modinfo` ok) but I did not load it nor run NM against it (not worth the cost; syntax validated with `--print-config`).
- Whether Ubuntu's `wpa_supplicant -u` works with `scsc_wlan` (that is W2), and whether `scsc_h4_0` is plain H4 (that is B3).

## 4. The risky phase, in order (run only on the integrated session, root restored)
Abort rule for all: first non-success -> stop, run `guest.sh stop`, do not retry in a loop, note `dmesg | tail -50`. USB adb survives a radio accident.
| # | Command | Success | Abort / risk |
|---|---|---|---|
| 0 | `adb shell su -c 'sh /data/local/tmp/oneos/x400/groups/radio/host.sh'` then in the guest `sh /tmp/x400/radio/guest.sh; cat /tmp/x400-radio.log` (no flags) | log `both radios off`; `ls /run/x400-radio` shows the keyfile; `mount \| grep -E ' /etc \| /var/lib '` shows two overlays; `touch /etc/NetworkManager/system-connections/p` works | overlay fails -> NM cannot run; fix before going on. No radio risk. |
| 1 | `python3 /tmp/x400/radio/btbridge.py --dry-run` | `/dev/scsc_h4_0 exists ...; not opened` | node missing -> stop BT |
| 2 | Before W2: `getprop init.svc.vendor.bluetooth-1-1` = running, `lshal \| grep IBluetoothHci` shows `0/1`, `dmesg \| grep -c -i wlbt`; note `/sys/module/scsc_bt/parameters/service_start_count` | baselines recorded | a holder of `scsc_h4_0` or a WLBT error already present -> stop |
| W2 | `touch /run/x400-radio/enable-wifi; sh /tmp/x400/radio/guest.sh; nmcli device; nmcli device wifi list` | `wlan0 disconnected` (not `unmanaged`), scan results appear, no new `slsi_`/`wlbt` errors | `unmanaged` -> udev entry (see 2); `vif`/firmware errors or `iw dev wlan0 scan` hangs -> `guest.sh stop`. Medium: WLBT firmware wedge = Wi-Fi and BT down until reboot. |
| W3 | `nmcli connection up x400-android-1; ip route get 1.1.1.1; ping -c3 1.1.1.1; ping -c3 ubuntu.com` | route via table 1014, replies, DNS works | no ping while `iw dev wlan0 link` is `Connected` -> table problem: `ip rule`, `ip route show table 1014`. Medium. |
| W4 | plasma-nm applet shows the network | | low |
| B3 | `python3 /tmp/x400/radio/btbridge.py --probe` (Wi-Fi may stay up; it shares the chip, prefer it off) | `reply: 040e0401030c00...` rc 0 | rc 1, `open` error, or hang >5 s -> stop BT, **do not** run B5. High: first open of `scsc_h4_0`; a firmware assert resets WLBT. |
| B4 | compare `/mnt/vendor/efs/bluetooth/bt_addr` with `hciconfig hci0` address; if needed write the efs address into `/sys/module/scsc_bt/parameters/bluetooth_address` **before** B5 | address matches | malformed value -> do not write. Low-medium (not automated here on purpose). |
| B5 | `touch /run/x400-radio/enable-bt; sh /tmp/x400/radio/guest.sh; hciconfig -a; bluetoothctl power on; bluetoothctl scan on` | `hci0` UP, devices listed, a keyboard/speaker pairs | `btattach` exits, `hci0` absent, `Failed to open crypto` in `/tmp/bluetoothd.log` -> `Privacy` must stay off; if `hci0` hangs, `guest.sh stop`. Medium. |
| B6 | Before returning to One UI: `sh /tmp/x400/radio/guest.sh stop`; `grep scsc_h4 /proc/*/fd` finds nothing | no holder | **forgetting it leaves Bluetooth dead in One UI until reboot.** |

## 5. Lines for G0
- `session-inner.sh`: after G2's `guest.sh`, `sh /tmp/x400/radio/guest.sh >/tmp/x400-radio.out 2>&1 &` (both radios stay off with no flag). Its return path (`x400_return.sh`/before One UI starts) must run `sh /tmp/x400/radio/guest.sh stop`.
- `guest_start.sh` needs **nothing new** for radio: `stage/` is already bound at `/tmp/x400/radio` and `host.sh` already starts in the background after `$R/run` is mounted (checked in the file). The key output goes to `$R/run/x400-radio` (tmpfs, 700); do not move the host.sh launch before the `/run` mount.
- PACKAGES unchanged (`x400/radio/PACKAGES`). The image does not need `systemd-udevd` work, `/etc/NetworkManager` or `/var/lib/bluetooth` (the packages create them; the overlay makes them writable).
- G2 contract: G4 waits for `/run/dbus/system_bus_socket` for 10 s and never starts a second bus unless it never appears (then it uses `/tmp/x400/sensors/system-bus.conf`).

## 6. Note for G0: `oneos_btaudio` floods `servicemanager`
`/data/local/tmp/oneos/oneos_btaudio.log` (pid 7991, started 04:37 with the session): `android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default registered again (round 5400): exception 0`, one round per second;
`dmesg` gets `servicemanager: Service '...IBluetoothAudioProviderFactory/default' originally registered from PID 7991 but it is being registered again from PID 7991. Bad state?...` every second (1634 `dmesg` lines at 06:11).
Measured at 06:11: process `S` (sleeping), 3 threads, `VmRSS` 2608 kB, utime/stime 117/214 ticks after ~90 min: negligible CPU/memory; the registration returns `exception 0` (accepted). It does not touch `scsc_h4_0`
(no process holds it). **Not dangerous for the session, but harmful for diagnosis:** it pushes older kernel messages out of the `dmesg` ring, and the risky phase relies on `dmesg` for `wlbt`/`scsc` errors.
Suggest G0 (owner of `oneos_btaudio`): register once and wait (or lower the rate), or at least take a `dmesg` snapshot before each risky step. I did not touch it.
Also seen: the load average was ~16 at 06:11 and 15 right after the reboot; I did not find the source (not G4).

## 7. State left on the tablet
`/data/local/tmp/g4/` (`iw`, `rfkill`, libnl), `/data/local/tmp/g4t/` (copies of stage files, a fake store `fake.xml`, a fake output dir `out/` with a fake key `pa$$w0rd...`; **delete**), probe scripts `/data/local/tmp/g4*.sh`.
Nothing of mine is mounted (the guest is gone with the reboot). Root must be restored through KernelSU over adb before any further work.
