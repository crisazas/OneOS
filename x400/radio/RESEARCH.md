# x400/radio (G4): Wi-Fi and Bluetooth, read-only research

Phase: research and design only. Nothing was written to rfkill, `/dev/ttySAC*`, `/sys` of the radio or `scsc_*` parameters, no Android service was
stopped or started, wpa_supplicant was not touched, the network was not changed. Everything below was measured on the live tablet
(adb root, session running since 04:37, kernel 5.15, One UI stopped) on 2026-11-23 between 05:09 and 05:55.

How the guest was reached (as CTX.md says): `B=/data/adb/ksu/bin/busybox; K=$(pidof kwin_wayland); $B nsenter -t $K -m $B chroot /proc/$K/root /bin/sh -c '...'`.
The guest has no `iw`, so `iw` and `rfkill` from the Ubuntu .debs were run **without installing anything** through the guest's own dynamic loader:
`$B nsenter -t $K -m /proc/$K/root/lib/ld-linux-aarch64.so.1 --library-path /data/local/tmp/g4/lib:/proc/$K/root/usr/lib/aarch64-linux-gnu /data/local/tmp/g4/iw ...`
(files left in `/data/local/tmp/g4/`: `iw`, `rfkill`, `lib/libnl-3.so.200`, `lib/libnl-genl-3.so.200`; delete them when G4 is done).

## 0. Summary (read this first)

| Question | Answer |
|---|---|
| Is Wi-Fi really connected right now? | **No.** wlan0 keeps a stale DHCP lease. `iw dev wlan0 link` says `Not connected.`, the gateway is `FAILED` in the neighbour table, `ping 8.8.8.8` fails, and nobody renews the lease. |
| Does an Android wpa_supplicant socket exist to talk to? | **No.** `init.svc.wpa_supplicant=stopped`, no `/data/vendor/wifi/wpa/sockets/*`, no `@android:wpa_wlan0`, no `ISupplicant` in servicemanager. The framework that started it is stopped. |
| Recommended Wi-Fi | NetworkManager + Ubuntu `wpa_supplicant` (nl80211, D-Bus) + `plasma-nm`, limited to `wlan0`, started by the session. Needs a system D-Bus and a rules-less `systemd-udevd` in the guest. |
| Is `/dev/ttySAC0` the Bluetooth UART? | **No.** It is the Exynos **debug UART** (`13800000.uart`, tx:0 rx:0, nobody opens it). Bluetooth lives in the WLBT combo chip and is reached through `/dev/scsc_h4_0` (module `scsc_bt`). |
| BT route A (bluetoothd on a UART hci) | Not applicable: there is no UART-attached controller. |
| BT route B (bluebinder / btproxy through `/dev/vhci`) | **Not viable**: `# CONFIG_BT_HCIVHCI is not set`, no `/dev/vhci`; libgbinder/bluebinder are not in Ubuntu. |
| Recommended Bluetooth | **Route C**: a small userspace relay `/dev/scsc_h4_0` <-> pty, with the pty attached to the kernel as an H4 HCI (`btattach -P h4`, `CONFIG_BT_HCIUART_H4=y`, `n_hci` registered), then stock `bluetoothd` + `bluedevil` + PipeWire. The Android HAL stays alive and untouched (nothing has the device open now). |
| Biggest risks | Opening `scsc_h4_0` starts the BT service of the **shared Wi-Fi/BT chip firmware**: a crash can reset WLBT (Wi-Fi too) and may need a reboot. The bridge must be released before returning to One UI. No `AF_ALG` in the kernel (BlueZ degrades, see 2.5). |

## 1. Wi-Fi

### 1.1 What is running (the facts that change the design)

```
$ getprop | grep init.svc | grep -E 'wpa|supp|wifi|bluetooth|wlbt|wificond'
init.svc.vendor.bluetooth-1-1: running      init.svc.vendor.wifi_hal_legacy: running
init.svc.vendor.wifi_hal_ext: running       init.svc.wificond: running
init.svc.wlbtd: running                     init.svc.wpal: running
init.svc.wpa_supplicant: stopped            <-- nobody drives the radio
$ getprop | grep -E 'init.svc.(zygote|surfaceflinger)'     zygote: stopped, zygote_next: running
$ service check android.hardware.wifi.supplicant.ISupplicant/default   -> not found
$ cmd wifi status      -> cmd: Can't find service: wifi        (no WifiService: system_server is not running)
$ service list | grep -i wifi    -> 68 wifinl80211: [android.net.wifi.nl80211.IWificond]   (wificond alive, idle, pid 8048)
```

The wpa_supplicant of One UI died with its framework (logcat shows `wpa_supplicant` in `am_cpu` only at 02:43 and 02:50, before the switch at 04:37).
wlan0 therefore only has what was left behind:

```
$ ip -o addr show wlan0         192.168.68.109/22 ... dynamic ... valid_lft 5188sec   (05:09)
                                                      ... valid_lft 3035sec   (05:44)   <-- counting down, expires ~06:35
$ ip neigh show dev wlan0       192.168.68.1  FAILED     (gateway does not answer)
$ ping -c2 -W2 8.8.8.8          100% packet loss
$ iw dev wlan0 link             Not connected.
$ cat /sys/class/net/wlan0/operstate   dormant        (flags 0x1003: UP, but no association)
$ ps -A | grep -i -E 'wpa|supp|dhcp'  only [wpal] (Samsung WLBT helper, not a supplicant)
```

So "wlan0 has an IP" is a stale lease. Today the guest has **no working network at all** (and no `/etc/resolv.conf`). Wi-Fi management is not only a UI
question: something must associate and run DHCP. This is also why a "status only" option (3) would show "disconnected".

### 1.2 (a) The three options

**Option 1: reuse Android's wpa_supplicant control socket. Not usable as it is.**
- `/data/vendor/wifi/wpa/sockets` **does not exist** (`ls: No such file or directory`); `/data/vendor/wifi/wpa` only has `wpa_supplicant.conf` (131 bytes, global flags) and `p2p_supplicant.conf`, owner `wifi:wifi` 0770.
  `/data/misc/wifi/sockets` and `/data/misc/wifi/mainline_supplicant/sockets` exist but are **empty** (`total 6`, only `.` and `..`).
- Only the init service defines the sockets: `/vendor/etc/init/android.hardware.wifi.supplicant-service.rc`:
  `service wpa_supplicant /vendor/bin/hw/wpa_supplicant -O/data/vendor/wifi/wpa/sockets -dd -g@android:wpa_wlan0 ... interface aidl android.hardware.wifi.supplicant.ISupplicant/default ... disabled oneshot`.
  It is `disabled`, started on demand by servicemanager when the framework asks for the AIDL service, which no longer exists. The global socket is **abstract** (`@android:wpa_wlan0`; `grep wpa /proc/net/unix` finds nothing but `@socket_wland`).
- The binaries do exist: `/vendor/bin/hw/wpa_supplicant` (3.2 MB, label `hal_wifi_supplicant_default_exec`), `/vendor/bin/wpa_cli` (`wpa_cli v2.11-devel-13`, same major as Ubuntu 2.11). Read-only probe:
  ```
  $ /vendor/bin/wpa_cli -p /data/vendor/wifi/wpa/sockets -i wlan0 status
  Failed to connect to non-global ctrl_ifname: wlan0  error: No such file or directory
  $ /vendor/bin/wpa_cli -g @android:wpa_wlan0 status
  Failed to connect to wpa_supplicant global interface: @android:wpa_wlan0  error: No such file or directory
  ```
  Because the vendor binary only learns its interfaces through AIDL calls (`ISupplicant.addStaInterface`) and has **0** D-Bus strings (`strings ... | grep -c fi.w1.wpa_supplicant1` = 0, Ubuntu's has 30), it can only be driven by hand with `-iwlan0 -Dnl80211 -c <conf> -O <dir>` plus `wpa_cli`. Possible as a fallback, but the guest does not see `/data` (`ls /data` -> no such file), Plasma cannot use it, and it still needs a DHCP client. **Keep only as a fallback for the nl80211 test (W2).**
- Protocol if ever used: the standard wpa_supplicant control interface (UNIX datagram socket `<dir>/wlan0`, `status`, `list_networks`, `scan_results`, `scan`, `add_network`...). I could not run `status`/`list_networks`/`scan_results` because there is no daemon; the only read-only call possible, the `scan dump` of the kernel cache (`iw dev wlan0 scan dump`), is empty.

**Option 2: NetworkManager. Recommended, with one correction.** NM has no "wpa_supplicant-socket" backend: it talks to wpa_supplicant over the
**D-Bus API** (`fi.w1.wpa_supplicant1`; the alternative backend is iwd, not in the rootfs). Ubuntu's wpa_supplicant 2.11 has that D-Bus interface and the nl80211 driver, so
the stack is `plasma-nm` -> NetworkManager (system bus) -> `wpa_supplicant -u` -> nl80211 -> `scsc_wlan` (cfg80211). It does not need Android's supplicant at all.

**Option 3: status only.** Works without any daemon but cannot connect, and the lease will expire at ~06:35. Not enough.

Why option 2 can work on this driver (cfg80211 is a loadable module, `CONFIG_CFG80211=m`, loaded: `cfg80211 ... 1 scsc_wlan`):
```
$ iw dev                     phy#0: wlan0 (managed, ifindex 14, addr ee:dc:7c:a9:c8:a4), wlan2/wlan3 (AP), swlan0, p2p0 (P2P), wifi-aware0
$ iw phy | sed -n '/Supported commands/,/Supported TX/p'
        * connect   * disconnect   * start_sched_scan   * set_pmksa   * update_ft_ies   * tdls_oper ...
        Device supports SAE with AUTHENTICATE command     Device supports scan flush     Device supports roaming
        Ciphers: CCMP-128, GCMP-128/256, CCMP-256, CMAC, GMAC-128/256, TKIP, WEP        (CONFIG_MAC80211 is not set: a FullMAC driver)
$ iw reg get                 global 00: DFS-UNSET     phy#0 (self-managed) country MX: DFS-FCC (2402-2482, 5170-5835)
$ rfkill list                0: phy0: Wireless LAN   Soft blocked: no   Hard blocked: no       (the only rfkill; there is no Bluetooth rfkill)
$ cat /sys/class/net/wlan0/uevent    DEVTYPE=wlan  INTERFACE=wlan0  IFINDEX=14        (NM identifies Wi-Fi by DEVTYPE=wlan and /sys/.../wireless or phy80211)
$ ls /sys/class/net/wlan0/phy80211   -> phy0 ... rfkill0
```
It is a FullMAC driver with the connect command, which wpa_supplicant's nl80211 driver handles (this is how Android's own supplicant uses it). The regulatory domain is
self-managed by the firmware (MX), so the guest's `wireless-regdb` most likely does not matter for the kernel (not verified; `CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y`); it is only a recommend of NM.
**Open risk** (not testable read-only): Samsung's supplicant has vendor driver commands that Ubuntu's lacks; if association fails, W2 below falls back to the vendor binary.

What NM needs that the guest does not have yet (all measured):

| Need | Now | Evidence |
|---|---|---|
| system D-Bus | missing (only the session bus) | `ls /run/dbus` empty; `dbus-daemon`, `messagebus` user and `/usr/share/dbus-1/system.conf` exist |
| udev database for wlan0 | missing, so NM would keep wlan0 "unmanaged (platform-init)" | `pyudev ... Devices.from_name(c,'net','wlan0').is_initialized -> False`; NM 1.54.3 `nmp-object.c`: a link without udev device is `initialized = FALSE` when udev is used, and `nm-device.c` only clears `NM_UNMANAGED_PLATFORM_INIT` for `pllink->initialized`. `/run/udev` does not exist. |
| writable and persistent `/etc/NetworkManager/system-connections`, `/var/lib/NetworkManager` | the rootfs is read-only | `/dev/block/loop58 on / type ext4 (ro,...)`; `/run`, `/tmp` are tmpfs; a f2fs dir is already bind-mounted at `/tmp/capture`, `/tmp/inputd`... so G0 has a pattern for persistent dirs |
| DNS | no `/etc/resolv.conf` at all | `cat /etc/resolv.conf: No such file`; Android's DNS is netd (`dnsproxyd`), not usable by glibc |
| routes that are actually used | NM's main-table routes would be ignored | see below |
| `AF_NETLINK`/nl80211 | works (iw ran) | |
| polkit | `polkitd` installed; the session is root | |

Routing trap (important): the shared network namespace is run by Android's netd with per-network tables and **no `main` rule**:
```
$ ip rule | grep -c main      -> 0
0: lookup local    11000: iif lo oif wlan0 uidrange 0-0 lookup wlan0   ...   31000: from all fwmark 0x0/0xffff iif lo lookup wlan0   32000: from all unreachable
$ ip route show table wlan0   default via 192.168.68.1 dev wlan0 / 192.168.68.0/22 dev wlan0      (table 1014, left by netd, from /data/misc/net/rt_tables)
$ ip route show table main    192.168.68.0/22 dev wlan0 proto kernel scope link src 192.168.68.109
```
A default route that NM puts in table `main` is never consulted (rule 32000 `unreachable` follows rule 31000 `lookup wlan0`). The fix is per profile `ipv4.route-table=1014`
(and `ipv6.route-table=1014`), so NM writes its routes into the table that rule 31000 reads (table 1014 is `wlan0`; the rule is `fwmark 0x0/0xffff`, which matches unmarked guest traffic).
To verify at implementation time with `ping -c1 1.1.1.1` and `ip route get 1.1.1.1`. Alternative: `ip rule add pref 30000 lookup main` once (changes Android's rule set; less clean).

NM must also be fenced in with `/etc/NetworkManager/conf.d/x400.conf` (otherwise it would grab `wlan2/3`, `p2p0`, `swlan0`, `rmnet*`, `dummy0`, `ifb*`, `tunl0`... which Android owns):
```
[main]
no-auto-default=*
dns=default
rc-manager=symlink
[keyfile]
unmanaged-devices=except:interface-name:wlan0
[device]
wifi.scan-rand-mac-address=no
[connection]
wifi.powersave=2
```
(and the image must have `/etc/resolv.conf -> /run/NetworkManager/resolv.conf`, a symlink created at build time; the file in `/run` is created by NM).

### 1.3 (b) Ubuntu packages (resolute 26.04, arm64, `apt-cache policy` in `out/rootfs-plasma`)

```
wpasupplicant 2:2.11-0ubuntu5   network-manager 1.54.3-2ubuntu3.1   plasma-nm 4:6.6.7-0ubuntu0.1   iw 6.17-1
wireless-regdb 2026.05.30-0ubuntu1~26.04.1   rfkill 2.41.3-3ubuntu2.2   libnl-3-200 / libnl-genl-3-200 / libnl-route-3-200 3.12.0-2
```
`apt-get install -s --no-install-recommends` in the chroot (nothing installed), counting only what is not in the image:
```
iw rfkill wpasupplicant wireless-regdb  => 8 packages, ~6 MB   (adds libnl-3-200, libnl-route-3-200, libnl-genl-3-200, libpcsclite1)
network-manager plasma-nm               => 46 packages, ~289 MB (!)
```
Heads-up for G0: `plasma-nm` Depends on `libqt6webenginecore6`, `libqt6webenginecore6-bin`, `libqt6webenginewidgets6` (+ `libqt6webengine6-data`): ~250 MB installed, about 60 MB of download
(only needed for the captive-portal / web login page). It cannot be avoided without dropping plasma-nm. The image has 3.9 GB free of 5.9 GB (`df -h /` in the guest: 5.9G, 1.7G used), so it fits; also pulled in: `netplan.io` + python-yaml, `libtss2-*`, `libopenconnect5`, `libxmlsec1`, `ethtool`, `libndp0`, `libteamdctl0`, `libnm0`.
`network-manager` brings its own DHCP client (internal), so `dhcpcd-base`/`udhcpc` are not needed. `dbus`, `dbus-daemon`, `polkitd`, `udev`, `libudev1`, `python3` are **already** in the image.

### 1.4 (c) How NM sees wlan0 (checked with iw and the NM sources)

- `iw dev` works from inside the guest namespace without any ioctl issue: nl80211 is reachable (output above). NM uses rtnetlink + nl80211 via libnm-platform (its own netlink code), not ioctls (`CONFIG_CFG80211_WEXT` is not set, `CONFIG_WIRELESS_EXT=y` for the old API, irrelevant).
- Link type: `uevent` has `DEVTYPE=wlan` -> `NM_LINK_TYPE_WIFI` (`_linktype_read_devtype` in `nm-linux-platform.c`); the `NMDeviceWifi` factory loads if the `libnm-device-plugin-wifi.so` plugin is there (it is shipped by `network-manager`).
- The `initialized`/udev gate described above is the one thing that will keep wlan0 unmanaged. Cheapest safe solution: run `systemd-udevd --daemon` in the guest with **empty rules directories** and
  `udevadm trigger --action=add --subsystem-match=net`; with no rules udevd only writes `/run/udev/data/n14` (marks the link initialized) and does not chown/chmod nodes in the **/dev shared with Android**, nor load modules.
  Bind-mount empty dirs over `/usr/lib/udev/rules.d` and `/etc/udev/rules.d` first (guest mount namespace only). This must be verified at implementation (step W1).
- NM will find 1 saved Android network (`WifiConfigStore.xml`: 1 `<Network>`, `WPA_PSK`); the PSK is not copied by this research. The user can re-enter it in plasma-nm.

### 1.5 Wi-Fi implementation plan (for after the other phases)

| Step | What | Risk |
|---|---|---|
| W0 | Image: install the packages (see PACKAGES); symlink `/etc/resolv.conf -> /run/NetworkManager/resolv.conf`; ship `x400.conf` above; create persistent bind-mount sources for `/etc/NetworkManager/system-connections` and `/var/lib/NetworkManager` | none at runtime |
| W1 | Session start (G0 `session-inner.sh`, before plasmashell): `dbus-daemon --system --fork`; bind empty rule dirs; `systemd-udevd --daemon`; `udevadm trigger --action=add -s net`; `udevadm settle`. Check: `pyudev ... is_initialized -> True`; `ls -la /dev` modes unchanged (compare before/after) | Low. If udevd touches `/dev` modes, stop it (`pkill systemd-udevd`) and fall back to writing `/run/udev/data/n<ifindex>` by hand |
| W2 | Test the supplicant alone: `wpa_supplicant -u -Dnl80211 -iwlan0 -O /run/wpa_supplicant` (no NM yet), `wpa_cli -p /run/wpa_supplicant status/scan/scan_results`. **First active step on the radio**: a scan is a firmware action. If `scan_results` come back, nl80211 works. If it fails with the Ubuntu build, try the vendor binary by hand (`/vendor/bin/hw/wpa_supplicant -iwlan0 -Dnl80211 -c <conf> -O <dir>`, needs access to `/data`) | Medium: a wedged WLAN firmware state would break Wi-Fi until reboot (reboot loses root: DFroot, get it back through KernelSU over adb). The adb transport is USB, so adb survives |
| W3 | Start `NetworkManager --no-daemon` with conf.d; `nmcli device` must list wlan0 `disconnected` (not `unmanaged`) and nothing else managed; connect with `nmcli device wifi connect <ssid> password ...` using `ipv4.route-table 1014` | Medium: NM sets the link up/down and replaces the stale 192.168.68.109. Keep USB adb as the way back |
| W4 | plasma-nm applet in the panel (system bus reachable from the session bus user) | Low |
| W5 | Return path: before `x400_return.sh` hands over to One UI, stop NM and wpa_supplicant, `ip addr flush dev wlan0`; One UI's supplicant (init `ctl.start`) is the one that must own wlan0 again | Medium: if NM keeps wlan0 and One UI starts, both fight |

Must NOT be done in this task and is not: stopping `wificond`/`vendor.wifi_hal_*`/`wlbtd`/`wpal`, touching `rfkill`, `iw ... scan` (a firmware action), writing `/sys/wifi/*`.

## 2. Bluetooth

### 2.1 (a) The HAL is alive

```
$ ps -A | grep bluetooth                900  bluetooth  android.hardware.bluetooth@1.1-service
$ getprop init.svc.vendor.bluetooth-1-1   running
$ lshal | grep -i bluetooth
DM,FC Y android.hardware.bluetooth@1.0::IBluetoothHci/default      0/1  900
DM,FC Y android.hardware.bluetooth@1.1::IBluetoothHci/default      0/1  900
DM,FC Y vendor.samsung.hardware.bluetooth@2.0::ISehBluetooth/default 0/1  900
$ cat /vendor/etc/init/android.hardware.bluetooth@1.1-service.rc
service vendor.bluetooth-1-1 /vendor/bin/hw/android.hardware.bluetooth@1.1-service
    interface android.hardware.bluetooth@1.1::IBluetoothHci default ... user bluetooth  group bluetooth  capabilities BLOCK_SUSPEND NET_ADMIN SYS_NICE
$ cat /proc/900/maps (libs): android.hardware.bluetooth@1.0-impl.so, vendor.samsung.hardware.bluetooth@2.0.so, (libbt-vendor.so loaded on initialize)
$ ls -l /proc/900/fd           0-2 /dev/null, 3 /dev/binderfs/hwbinder, 4 trace_marker, 6 /dev/pmsg0     <-- NO /dev/scsc_h4_0 open
```
It is a **HIDL** HAL (1.0/1.1; there is no AIDL `android.hardware.bluetooth.IBluetoothHci`), with the client counter `0/1`: nobody called `initialize()`, so the chip's BT service is idle
and `/dev/scsc_h4_0` is **not held by anybody** (`for p in /proc/[0-9]*; ls -l fd | grep scsc_h4` -> empty).

### 2.2 (b) `/dev/ttySAC0` is not Bluetooth

```
$ ls -la /sys/class/tty/ttySAC0/device      -> ../../../13800000.uart
$ cat /sys/devices/platform/13800000.uart/uevent     DRIVER=exynos-uart  OF_COMPATIBLE_0=samsung,exynos-uart  OF_ALIAS_0=uart0
$ cat /proc/tty/driver/s3c2410_serial       0: uart:S3C6400/10 mmio:0x13800000 irq:90 tx:0 rx:0 DSR|CD
$ od of_node/samsung,dbg-uart-baud          00 01 c2 00  (= 115200)       <-- the property names say "dbg-uart"
$ cat /proc/cmdline | tr ' ' '\n' | grep console     console=ram     (ttySAC0 is not the console: /sys/class/tty/ttySAC0/console = N)
$ dmesg | grep -i -E 'ttySAC|13800000|uart'          (no lines)
$ for p in /proc/[0-9]*; ... readlink fd | /dev/ttySAC*     (nobody has it open)
$ ls /sys/bus/platform/devices | grep -i uart        only 13800000.uart (a single UART in the whole SoC tree)
```
`/dev/ttySAC0` (`crw-rw---- root radio 204,64`) is the SoC's debug serial port with zero traffic. There is **no UART Bluetooth controller** on this board. `/sys/class/bluetooth` is empty and there is no Bluetooth rfkill:
`rfkill list` only shows `phy0: Wireless LAN`. The combo chip is the Samsung S621P (`quartz_s621p`, "WLBT", driver `scsc_wlbt` at `11b40000.scsc_wifibt`), shared by Wi-Fi and BT through shared memory (SCSC MX), **not** a serial link.

What the HAL actually opens (`strings /vendor/lib64/libbt-vendor.so`): `/dev/scsc_h4_0`, `/sys/module/scsc_bt/parameters/bluetooth_address`, `/data/vendor/conn/.wifiver.info` (firmware version path), property `vendor.bluetooth_fw_ver`, with log lines `Open h4 device failed: %s`, `Open h4 device retry failed`, `Read fw path failed`.
`/vendor/etc/bluetooth/` only has `bt_did.conf` (device-ID record, 529 bytes); there is no `*.hcd` or UART config. `/vendor/etc/init/*bluetooth*.rc` is the one above.

```
$ ls -laZ /dev/scsc_h4_0          crw-rw---- bluetooth bluetooth u:object_r:sec_radio_device:s0  497,0     (SELinux is Permissive; guest sees it as 1002:1002)
$ ls /sys/module/scsc_bt/parameters    bluetooth_address  bluetooth_address_fallback  btlog_enables  disable_service  firmware_control
                                       firmware_control_reset  force_crash  mxlog_filter  scsc_bt_qos_*  service_start_count
$ cat parameters/{bluetooth_address,bluetooth_address_fallback,service_start_count,disable_service}   0 / A5:F0:F0:13:0F:2E / 3 / N
$ ls -la /mnt/vendor/efs/bluetooth/bt_addr        -rw-rw---- system bluetooth 17   (24:A4:52:12:xx:xx; the real address, factory data)
$ grep -E 'CONFIG_SCSC_BT' /proc/config.gz     CONFIG_SCSC_BT=m   # CONFIG_SCSC_BT_BLUEZ is not set   # CONFIG_SCSC_BT_ADDRESS_IN_FILE is not set
$ lsmod: scsc_bt 135168 1 scsc_mx250_fm ; scsc_mx 409600 6 scsc_wlan,scsc_wifilogger,scsc_mx250_fm,scsc_bt,scsc_mmap,mx_client_test
```
Note: Samsung's driver has a `SCSC_BT_BLUEZ` mode that registers an `hciN` in the kernel directly, but it is **off** in this prebuilt, signed (`CONFIG_MODULE_SIG=y`) module and no `.ko` is on a filesystem I can read, so the kernel cannot be made to expose `hci0` by itself.

### 2.3 (c) Kernel capabilities for the routes

```
$ zcat /proc/config.gz | grep -E 'CONFIG_BT|UHID|RFKILL|CRYPTO_USER'
CONFIG_BT=y  CONFIG_BT_BREDR=y  CONFIG_BT_LE=y  CONFIG_BT_RFCOMM=y  CONFIG_BT_RFCOMM_TTY=y  CONFIG_BT_HIDP=y  CONFIG_BT_DEBUGFS=y
CONFIG_BT_HCIUART=y  CONFIG_BT_HCIUART_SERDEV=y  CONFIG_BT_HCIUART_H4=y  CONFIG_BT_HCIUART_LL=y  CONFIG_BT_HCIUART_BCM=y  CONFIG_BT_HCIUART_QCA=y  CONFIG_BT_HCIBTSDIO=y
# CONFIG_BT_HCIVHCI is not set      # CONFIG_BT_HCIBTUSB is not set   # CONFIG_BT_BNEP is not set   # CONFIG_BT_MSFTEXT is not set   # CONFIG_BT_AOSPEXT is not set
CONFIG_UHID=y  CONFIG_INPUT_UINPUT=y  CONFIG_RFKILL=y  # CONFIG_RFKILL_INPUT is not set
# CONFIG_CRYPTO_USER_API_HASH/SKCIPHER/RNG/AEAD is not set     (no AF_ALG)
$ cat /proc/tty/ldiscs                          n_tty 0 / input 2 / n_hci 15 / n_null 27       <-- the HCI line discipline is registered
$ ls /sys/module/hci_uart ; grep ' hci_uart_' /proc/kallsyms      hci_uart_tty_open, hci_uart_register_device, hci_register_dev, hci_recv_frame present
$ ls /dev | grep -E 'vhci|hci'                  (nothing)   -> no /dev/vhci
$ guest python3: socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI) -> ok ; socket(AF_ALG) -> [Errno 97] Address family not supported by protocol
$ mgmt socket READ_VERSION (read-only, HCI_CHANNEL_CONTROL)  reply 0100ffff0600010000011500   -> Command Complete, status 0, mgmt version 1.21 (kernel mgmt API alive)
$ /sys/class/bluetooth: empty;  /proc/net/protocols: HCI L2CAP SCO RFCOMM HIDP registered
$ ls -la /dev/ptmx /dev/pts ; mount | grep devpts     /dev/ptmx crw-rw-rw- ;  devpts on /dev/pts (rw, mode=620, ptmxmode=666)   (ptys work in the guest)
$ /proc/crypto contains cmac(aes), ecb(aes), cbc(aes) ...   (the kernel's own SMP can pair LE devices; only userspace AF_ALG is missing)
```

Verdict per route:

| Route | Verdict | Why |
|---|---|---|
| **A**: bluetoothd on an hci created by the kernel from a UART (HCI UART) | **Not applicable as stated** | The module/driver support exists (`hci_uart`, H4, `n_hci`) but there is no UART with a controller: `ttySAC0` is a debug port. `scsc_bt` does not create `hciN` (`SCSC_BT_BLUEZ` off). |
| **B**: bridge Android's stack to BlueZ via vhci (`bluebinder`, bluez `btproxy`) | **Not viable** | `bluebinder` (mer-hybris/droidian) and `btproxy` both end in `/dev/vhci`; `CONFIG_BT_HCIVHCI` is **not set** and `/dev/vhci` does not exist. `libgbinder`/`bluebinder` are not packaged in Ubuntu (`apt-cache search gbinder|bluebinder` -> nothing). "Gabeldorsche"/bluedroid is the host stack that the *framework* runs; it is not available (system_server is stopped) and has no BlueZ gateway. |
| **C**: userspace H4 relay `scsc_h4_0` <-> pty, pty attached as an H4 HCI (`btattach -B <pty> -P h4`), then BlueZ on `hci0` | **Viable** | All ingredients measured: `hci_uart` H4 built in, `n_hci` registered, ptys available, `btattach` is in the `bluez` .deb (`./usr/bin/btattach`), the HAL strings suggest that `scsc_h4_0` carries plain H4 HCI (`libbt-vendor` only shows open of the device, the address parameter and the firmware-version path; **inferred, not proven** until B3). The relay is ~60 lines of Python (python3 3.14 is in the image, no compiler needed). |
| C': same as C but as a HIDL client of the live HAL (`IBluetoothHci.initialize/sendHciCommand`) over `/dev/hwbinder` | Viable but heavier | No libgbinder in Ubuntu: a hand-written HIDL hwbinder client is needed. Advantage: the HAL keeps doing the vendor init. Use only if raw `scsc_h4_0` access misbehaves in step B3. |

Open points of route C that cannot be proven read-only (they are the first things to test): (1) whether `scsc_h4_0` allows a second opener later (the HAL is idle, so it is free now); (2) which BD address the controller uses: `bluetooth_address=0` now, so the module falls back to `A5:F0:F0:13:0F:2E`; Android's value is in `/mnt/vendor/efs/bluetooth/bt_addr` and `libbt-vendor` references the parameter file, so the relay should write the efs address to `bluetooth_address` before opening (a write under `/sys/module/scsc_bt`, not allowed in this phase); (3) the firmware download and controller init happen inside `scsc_mx` when the device is opened; I have not seen it happen without the HAL.

### 2.4 (d) Packages downloaded (not installed), arm64, in `/tmp/opencode/g4debs/` (outside the repo, not committed)

`apt-get download` inside the chroot failed (`Temporary failure resolving 'security.ubuntu.com'`: the chroot has no `resolv.conf`; I did not create one in it). The exact URIs
came from `apt-get download --print-uris` and were fetched from the PC with curl:

```
 117458 iw_6.17-1_arm64.deb                              1530966 wpasupplicant_2.11-0ubuntu5_arm64.deb
  59544 libnl-3-200_3.12.0-2_arm64.deb                    1492950 bluez_5.85-4ubuntu0.4_arm64.deb
  12440 libnl-genl-3-200_3.12.0-2_arm64.deb                 87102 libbluetooth3_5.85-4ubuntu0.4_arm64.deb
  33846 rfkill_2.41.3-3ubuntu2.2_arm64.deb                 677760 bluedevil_6.6.7-0ubuntu0.1_arm64.deb
   7146 wireless-regdb_2026.05.30-0ubuntu1~26.04.1_all.deb 439866 libspa-0.2-bluetooth_1.6.2-1ubuntu1.2_arm64.deb
```
`bluez` provides: `/usr/libexec/bluetooth/bluetoothd`, `bluetoothctl`, `btattach`, `btmgmt`, `btmon`, `hciconfig`, `hcitool`, `rfcomm`, `/usr/lib/systemd/system/bluetooth.service`, `/etc/bluetooth/main.conf`.
Simulated installs (`-s --no-install-recommends`): `bluez bluedevil libspa-0.2-bluetooth` => 25 new packages, ~30 MB (adds `bluez-obexd`, `libkf6bluezqt6`, `qml6-module-org-kde-bluezqt`, `libsbc1`, `liblc3-1`, `libldacbt-*`, `libfreeaptx0`, `kmod`, and the Evolution data server libs through bluedevil/obexd).
`rfkill` (util-linux) is 33 KB; `bluetoothd` reads rfkill through `/dev/rfkill` itself.
`iw`/`rfkill` ran correctly from these debs inside the guest's namespace (outputs in section 1).

### 2.5 BlueZ on this kernel: known gaps

- `bluetoothd` links `libudev`, `libasound`, `libdbus-1`, `libglib` (all present or pulled). It needs the **system D-Bus** (same W1 step), `/var/lib/bluetooth` (persistent pairings -> bind mount like NM) and `/run/udev`? (no: BlueZ does not need the udev db).
- **No `AF_ALG`** (`socket(38, ...)` fails): BlueZ 5.85 `bt_crypto_new()` (`src/shared/crypto.c`) returns NULL. Reading `gatt-db.c` (guards at lines 497/714/728) the GATT DB tolerates `crypto == NULL` (no DB hash / client caching), but `adapter.c:4243` (`generate_and_write_irk`, only when `Privacy` is enabled) and `adapter.c:9870` (`get_static_addr`, only when the controller reports BDADDR_ANY) fail with "Failed to open crypto". Mitigation: keep `Privacy` off (the default) and give the controller a real public address. SMP pairing is done by the kernel (`cmac(aes)`/`ecb(aes)` in `/proc/crypto`), so LE pairing is not blocked by this. Needs a real test (step B5).
- `CONFIG_BT_BNEP` is not set: no Bluetooth tethering/PAN. `CONFIG_BT_HIDP=y` and `UHID`/`uinput` are fine for keyboards and mice (HOG goes through uhid).
- Audio: A2DP is carried over ACL, so `libspa-0.2-bluetooth` (PipeWire's bluez5 plugin) works above hci0. SCO/HFP on SCSC is usually routed outside HCI (PCM/Samsung ISehBluetooth), so headset calls/mic are not expected. (G3 owns the PipeWire side.)
- Side observation, not G4's: `dmesg` is flooded, once per second, by `servicemanager: Service 'android.hardware.bluetooth.audio.IBluetoothAudioProviderFactory/default' originally registered from PID 7991 but it is being registered again` (PID 7991 = `oneos_btaudio`, started at 04:37; `/data/local/tmp/oneos/bt_bridge_vintf`). It does not touch `scsc_h4_0`, but G0 should know it exists.

## 3. Recommendation

1. **Wi-Fi**: NetworkManager + Ubuntu wpa_supplicant (D-Bus) + plasma-nm, restricted to `wlan0` with `route-table=1014`, started by the session with a system D-Bus and a rules-less udevd. Android's supplicant is dead and its socket does not exist, so nothing from Android is reused.
2. **Bluetooth**: route C (H4 relay `/dev/scsc_h4_0` <-> pty <-> `btattach -P h4` -> `hci0` -> `bluetoothd` -> `bluedevil`/PipeWire). The Android HAL is left running and idle.
3. Do both **after** the other phases; Wi-Fi first (the USB adb transport survives a Wi-Fi accident, and Wi-Fi is needed for the rest), Bluetooth second (it shares the chip).

## 4. Implementation plan (step, risk, what could leave the radio unusable until reboot)

Common rule: adb over USB (`sys.usb.state=sec_charging,adb`) is the way back; a reboot loses root (DFroot): restore it through KernelSU over adb and continue.

| Step | Action | Risk and failure mode |
|---|---|---|
| W0-W5 | Section 1.5 | W2 (first scan from the guest) and W3 (NM takes wlan0) are the active ones: **a wedged `scsc_wlan` vif/firmware leaves Wi-Fi *and Bluetooth* dead until reboot** (WLBT is one subsystem; `sysevent_wlbt` restarts it, `CONFIG_SCSC_WLBT_AUTORECOVERY_PERMANENT_DISABLE` is not set, so a crash normally recovers by itself) |
| B0 | Install packages (G0); `x400/radio/` adds `btbridge.py`, `radio-start.sh`, `radio-stop.sh`, conf files. Nothing starts | none |
| B1 | Read-only pre-checks at runtime: `lshal` still shows the HAL with `0/1`, no `scsc_h4` holder, `dmesg` clean of `scsc_bt`/`wlbt` errors, `service_start_count` noted | none |
| B2 | **System bus + `bluetoothd -n -d` with no controller**: `bluetoothctl show` -> "No default controller available" (proves the daemon, D-Bus and `AF_ALG` degradation at start) | low |
| B3 | **First contact with the radio**: relay opens `/dev/scsc_h4_0` (O_RDWR), sends one H4 `HCI_Reset` (`01 03 0c 00`), expects `04 0e 04 01 03 0c 00`, reads local version, **closes**. No pty yet. | **Medium-high.** Opening starts the BT service in the shared firmware (like the HAL's `initialize`, `service_start_count` +1). A firmware assert resets the WLBT subsystem: Wi-Fi drops and possibly stays down until reboot. Do it with Wi-Fi not needed. If `open` blocks or errors (`EBUSY`), stop: do not retry in a loop. |
| B4 | Write `bluetooth_address` from `/mnt/vendor/efs/bluetooth/bt_addr` into `/sys/module/scsc_bt/parameters/bluetooth_address` before B3's open (else the controller uses the fallback `A5:F0:F0:13:0F:2E`) | Low-medium: a sysfs module parameter, but a malformed value could make the firmware refuse to start. Test the format (`0x%llx` in `libbt-vendor`) on the readable value first |
| B5 | Pty + `btattach -B /dev/pts/N -P h4` -> `hciconfig`, `bluetoothctl power on`, scan, pair a device, A2DP through PipeWire | Medium: a bridge that dies leaves `hci0` attached to a dead pty (reads return nothing) and the device closed; `btattach` kill removes `hci0`. If the controller hangs after a killed bridge, only a WLBT restart (reboot) recovers it |
| B6 | Return path (`x400_return.sh`, G0): stop `bluetoothd`, kill `btbridge.py` and `btattach` **so `scsc_h4_0` is closed before One UI starts** (otherwise Android's HAL cannot initialize it: Bluetooth dead in One UI until reboot) | **High if forgotten** |
| B7 | Autostart in `session-inner.sh`, but only after B3-B5 passed on the live unit | low |

What I did **not** do: no read or write of the actual HCI traffic, no open of `/dev/scsc_h4_0` or `/dev/ttySAC0`, no write under `/sys/module/scsc_*`, no start/stop of any Android service, no `iw scan`, no change in `ip`/`rfkill`, nothing installed in the rootfs or in the guest.
Files left on the tablet: `/data/local/tmp/g4/` (iw, rfkill, 2 libnl libs) and `/data/local/tmp/g4*.sh` probe scripts (read-only; remove when no longer needed).
