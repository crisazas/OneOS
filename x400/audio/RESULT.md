# G3 audio: result

**Status: no sound yet.** Routing is understood and applies cleanly, the PCM opens,
but the Awinic speaker amplifiers never see an I2S clock, so playback stalls with EIO.
Nothing here is marked as working audio: there was never a measurable signal.

## What works (with evidence)

- **Inventory.** `/proc/asound/cards`: 0 Exynos-Sound, 1 abox_vdma, 2 abox_dump. Playback PCMs on
  card 0: `RDMA0..11` (devices 0,2..11) and `calliope_0..9` (100..109). Capture: `WDMA0..11`
  (12..23), `calliope_10..30`, VTS (24,25). Every PCM accepts 8..384 kHz, 1..8 ch, S16/S24/S32
  (`x400-snd info 0 7 p`, access RW_INTERLEAVED + MMAP).
- **Android side.** `audioserver` is stopped (`init.svc.audioserver=stopped`), the audio HALs are
  alive (`android.hardware.audio.service`, `secaudiohalaidl`).
- **No tinyalsa tools on the device** (no tinymix/tinyplay/tinycap in /system, /vendor), so
  `x400-snd.c` is a static replacement over raw ALSA ioctls (controls list/get/set/batch,
  hw_params probe, tone playback, capture with RMS/peak). Runs both on the host and in the guest.
  Checked in the guest: `/tmp/x400-snd ctl 0 get aw_dev_0_hmute` -> `Disable`.
- **Route expansion.** `expand-path.py` flattens nested `<path>` of `/vendor/etc/mixer_paths.xml`.
  `routes/*.tsv` hold the expansions (init, media-speaker, media-mic, media-dualmic,
  media-usb-headphone). `x400-audio-route <name>` applies one: all 305 init controls and every
  route control were accepted (`batch` rc=0, no FAILED lines).
- **Speaker path found.** `media-speaker` = `dev-dual-speaker` (aw_dev_0/1 `prof=Music`,
  `switch=Enable`) + `route-normal-spkrcv-playback`: RDMA7 -> SIFS1 -> UAIF1 -> the two aw882xx
  (I2C 2-0034 / 2-0035); `ABOX VDSP1=speaker-ampprotection-playback` loads Awinic's algorithm on
  the A-Box DSP. So the speaker PCM is **card 0 device 7 (`pcmC0D7p`)**, 48 kHz, 2 ch, 32 bit.
- **Microphone path found.** `media-mic`: DMIC1 -> UAIF6 (serial LIF, 4 ch 24 bit) -> NSRC1 ->
  WDMA1 (**`pcmC0D13c`**, device 13).
- **PipeWire sees static nodes.** `pipewire.conf.d/50-x400-alsa.conf`, tried on a throw-away
  PipeWire instance (own runtime dir, live session untouched): `pw-cli ls Node` showed
  `x400.speakers` ("Altavoces", Audio/Sink) and `x400.microphone` ("Microfono", Audio/Source).
  This only proves the nodes are created; no data was ever played through them.

## What does not work, and why

1. **Speaker: the amplifiers get no I2S clock.** With the route applied, opening `pcmC0D7p`
   (also D0) and writing frames: the kernel logs `abox_rdma_trigger(1)`, `uaif_ops_hw_params:
   UAIF1-0 2ch, 48000Hz, 32bit`, `bclk=3072001`, `aw882xx_start_pa: enter`, then for both PAs
   `aw_dev_mode1_pll_check: iis_abnormal_cnt[N]`, `pll&clk check fail`,
   `aw_dev_syspll_check: mode2 check iis failed`, `aw882xx_start_pa: start failed, cnt:0..4`.
   The RDMA never consumes the ring: after 4 periods `writei` blocks ~10 s and returns EIO.
   Same result on device 0 and 7, with the `route-normal-spkrcv-playback` chain and with the
   plain `route-rdmas-to-sifs0` + `route-sifs0-to-uaif1` chain, with `ABOX VDSP0/1/3=none`, and
   with `ABOX UAIF1 Enable BCLK=On` (set before and during playback). The UAIF1 pins do switch from
   `aud-i2s1-idle` to `aud-i2s1-bus` (debugfs pinmux-pins) while the PCM is open.
   One more line is suspicious: `abox 14e50000.abox: 0x81: invalid sink dai:0x0` right when the
   stream starts.
   Not found: what Samsung's HAL does before the first write that this sequence lacks (a
   firmware/DSP IPC, a UAIF1 master-clock request, or a regulator). Not tried, because they are
   riskier than my brief: restarting the audio HAL, starting `audioserver`, or reading the HAL
   with a debugger. That is where to continue.
2. **Microphone.** With `media-mic` applied, `pcmC0D13c` opens and starts (`abox_wdma_trigger(1)`,
   `vts_runtime_resume`, `slif_soc_dmic_en_put: port0 PDM0 val1`, `vts_chk_dmic_clk_mode`), but
   `readi` never returns data (timeout after 8 s, 4 ch/24 bit and 2 ch/32 bit). Not measured.
3. **Capture of the amplifier's IV-sense / loopback** (`pcmC0D16c`, `D21c`) as a way to measure the
   tone: `prepare` fails with EINVAL (`no backend DAIs enabled for WDMA9`), so I could not even
   validate the measuring method.
4. **WirePlumber.** The rootfs has no udev data (`/run/udev` missing), so the SPA ALSA monitor
   would find nothing. The static `api.alsa.pcm.*` nodes above avoid it. There is no ACP profile
   or UCM for the Exynos card (and `alsa-ucm-conf` has none either), so the nodes must name the PCM
   directly. Whether WirePlumber then links them (it did not matter in my isolated test, which
   had no session manager) is not tested.
5. **Headphones / USB.** The tablet has no jack. USB audio needs `media-usb-headphone`
   (`routes/media-usb-headphone.tsv`: UDMA + `ABOX VDSP1=usb-playback`) and a USB DAC plugged in.
   No DAC was available; not tested. A USB DAC that the kernel enumerates as its own ALSA card
   would bypass A-Box and should work through PipeWire with no routing.
   Bluetooth audio belongs to G4.

## State left behind

All mixer controls I touched were restored from the snapshot taken before (the diff of the
`x400-snd ctl 0 list` output before/after shows only `ABOX Tickle/Cold` and a few ASRC ids that the
firmware itself changes on stream open/close). Volume was never raised: tones were 3 % amplitude
(and never got through). debugfs was mounted by me on the host to read DAPM and unmounted again.
`pipewire`/`wireplumber` of the session were not restarted (pids 16707, 16957 before and after).

## What G0 must wire

Only worth doing once the clock problem is solved; none of it blocks other groups.

1. Packages: `x400/audio/PACKAGES` (`alsa-utils`, `alsa-ucm-conf`, `pipewire-alsa`). None of them
   is needed for the current tools; `x400-snd` replaces them for routing. They help for later tests.
2. Build `x400/audio/build.sh` (static arm64) and put `x400-snd`, `x400-audio-route` and `routes/`
   together on the device (host `/data/local/tmp` or `/data/x400/audio`).
3. After solving the clock issue, in `session-inner.sh` (or the host side of `guest_start.sh`): run
   `x400-audio-route init media-speaker media-mic` once per boot, then install
   `pipewire.conf.d/50-x400-alsa.conf` into `/etc/pipewire/pipewire.conf.d/` of the guest (or
   `/run` since the rootfs is read-only) before pipewire starts.
4. Do not restart the Android audio HAL from the guest side unless the owner decides it.
