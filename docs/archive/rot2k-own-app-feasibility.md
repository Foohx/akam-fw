# LSC Rotatable 2K: replacing `yy_cam` with an Akam app, feasibility study (offline)

> **Superseded (2026-11-24).** The study led to the Akam application that replaces `yy_cam`. The source of truth for what each camera does today is [docs/features.md](../features.md); the old to new names are in `tools/dev/path-map.txt`. Kept for its reasoning and findings only; do not read its status as current.

> Historical document: the file paths below predate the repository reorganisation (`src/common`, `src/ak_rtsp`, `firmware/<model>/app`); the old to new table is `tools/dev/path-map.txt`.

Status: **offline reverse engineering only, nothing was run on a camera** (2026-11-14). Same method and conventions
as [ptz-findings.md](../porting/lsc-rotatable-2k/ptz-findings.md) and [npu-findings.md](../porting/lsc-rotatable-2k/npu-findings.md): addresses are virtual addresses in the
stock `/appfs/bin/yy_cam` (ARM32 EABI5, stripped, dynamic symbols kept, `.text` 0x75cb0, `.rodata` 0x2f3dd8) or
offsets in the named `.so`. Only symbol names, log strings, addresses and struct field notes are reproduced; no vendor
code, header, dump, key, credential or nvram value. Context: [PLAN.md](rot2k-porting-plan.md) sections 2, 2.4, 7bis, 8, 8bis
(points 6 and 7). Confidence: HIGH = read directly in instructions or strings, MEDIUM = consistent inference, LOW =
guess. Everything stays UNVERIFIED in the sense of PLAN section 9 until a PoC below runs on the camera.

## 0. Verdict

**GO for PoC-0 to PoC-2, MEDIUM confidence** for a full replacement (PoC-3 and beyond). Why:

- The video path is **thin and fully known**: `yy_cam` itself does not use the high-level `libapp_video`; it drives
  five vendor calls in a loop (`ak_vi_get_frame`, `ak_venc_encode_frame`, `ak_venc_release_stream`,
  `ak_vi_release_frame`) after about ten setup calls (section 1). ISP auto-exposure and white balance run **inside the
  vendor libraries** (threads created by `libplat_vpss`), so we never write ISP algorithms (HIGH on the call graph,
  MEDIUM on "no extra ISP call is needed").
- The bitrate, profile, GOP and rate-control are **plain config keys** read from `/config/param.conf` `[dsp]` at
  start (section 1.4). Point 4 of the roadmap can be tried **without any new code** (opt-in edit, UNVERIFIED).
- Booting without `yy_cam` is easy: a bind mount of our binary over `/appfs/bin/yy_cam` lets the stock `start.sh`
  do all driver work (section 3.2). Wi-Fi and the watchdog are done by `yy_cam` itself, so our side must do both.
- Toolchain: a pinned prebuilt uClibc ARM926 soft-float toolchain matches the camera almost exactly (section 4).
- Not blocking but open: the **flash question** (PLAN 7bis, 8bis point 5) is independent and still RED; this study is
  card-only by design. Main technical risks: struct layouts recovered without headers, the unknown arming state of the
  hardware watchdog, RAM headroom, and the Wi-Fi bring-up from scratch (section 6).

## 1. Video bring-up (point 1)

### 1.1 Facts about the libraries

| Item | Value |
|---|---|
| Layering | `yy_cam` imports `ak_sdk_*`, `ak_vi_*`, `ak_vpss_*`, `ak_venc_*`, `ak_mem_*`, `ak_thread_*` directly (HIGH). `libapp_video` is in its NEEDED list but **no `ak_app_video_*` symbol is imported** by `yy_cam`; only `libapp_its` (the ISP tuning server) imports it |
| Versions (strings) | `libplat_vi V3.4.32`, `libplat_isp_sdk V9.1.00`, `libplat_mem V2.0.01`, `libapp_video V2.0.00`, `libplat_osal V2.0.04`, `libplat_log V2.0.00` |
| Kernel nodes the libs open | `/dev/video-N-M` (VI), `/dev/v4l-subdev0..3`, `/dev/isp-param-N` (ISP), `/dev/venc`, `/dev/venc-chn%d`. The libs are a userspace layer over the same node names the 1080p `ak_rtsp` drives, but the ISP algorithms and the Allegro glue live in userspace, so a clean-room ioctl path is not realistic: link the vendor `.so` |
| Link order | every vendor lib has only `libc.so.0` in NEEDED, symbols are resolved globally: link all of `libplat_vi libplat_vpss libplat_isp_sdk libmpi_venc libplat_mem libplat_common libplat_log libplat_osal` (+ `libplat_thread`, `libplat_timer`, `libplat_dbg` as needed) |
| Runtime path | the stock `rcS` exports `LD_LIBRARY_PATH=/appfs/lib`; `init.sh` and `start.sh` inherit it |

### 1.2 Ordered call sequence (function `yy_media_av130_video_init_framesource` and neighbours, source file
`yy_media_av130_video.cpp`)

| # | Call | Arguments and notes (confidence) |
|---|---|---|
| 0 | kernel side, by `start.sh` | `insmod ak_isp.ko`, `sensor_sc1346.ko check_id=1`, `ak_venc_adapter.ko`, `ak_venc_bridge.ko`, `ak_npu.ko` (section 3.1); `rcS` earlier loaded `ak_ion.ko`, `ak_uio.ko` (HIGH) |
| 1 | `ak_sdk_init(&p)` (in `libplat_common`) | 16-byte zeroed struct; only word +12 is set, to 1 when `/mnt/sdcard/yy_ak_isp_debug` or `/config/yy_ak_isp_debug` exists (then `ak_its_start(0x223d)` = TCP 8765 ISP tuning server, `libapp_its`). Initialises mutex/sem/mem/DMA helpers. Returns 0 (HIGH) |
| 2 | `ak_mem_get_total_size`, `ak_mem_dma_get_reserved_size` | informational plus a branch: reserved DMA pool above 50 MiB selects mode value 3, else 2 (meaning LOW) |
| 3 | `ak_vi_open(dev_id)` | `dev_id` = int from config `sensor1_dev_id` (default 0), once per sensor (HIGH) |
| 4 | `ak_vi_load_sensor_cfg(dev_id, "/appfs/conf/isp_sc1346_mipi_1lane_av130.bin")` | path = `/appfs/conf/%s` with config `sensor1_isp_conf_file`. This is the whole ISP init from our side: inside, `libplat_vi` calls `isp_open`, `isp_cfg_file_set_path`, `isp_match_sensor_cfgfile`, and `libplat_vpss` starts its ISP thread(s) (MEDIUM) |
| 5 | `ak_vi_set_dev_attr(dev_id, &attr)` | `attr` is 56 bytes, zeroed then: +0 dev id, +20/+24/+28/+32 crop left/top/width/height, +36/+40 max width/height, +44 frame rate, +48/+52 sub-stream max width/height; +4..+16 left 0 (interface mode, data path, data type, all 0 in the defaults). Defaults: max 2304x1296, sub max 640x360, 15 fps, flip 0 (HIGH on offsets, MEDIUM on the field names) |
| 6 | `ak_vi_enable_dev(dev_id)` | |
| 7 | `ak_vi_set_chn_attr(chn, &cattr)` | 20 bytes per channel: +0 chn id, +4 fps, +8 width, +12 height, +16 frame depth (HIGH on sources, MEDIUM on names). Default channels: chn0 2304x1296, chn1 640x360 |
| 8 | `ak_vi_enable_chn(chn)` | |
| 9 | `ak_venc_open_ex(&param, &ext, &handle)` | `param` 64 bytes (below), `ext` 28 bytes zeroed with the byte at +24 set to 1 (meaning LOW), `handle` out (HIGH). Returns 0 on success |
| 10 | `ak_venc_strictlylimit_frameSize(handle, 400)`, and `ak_venc_set_stream_buff(handle, 1 MiB)` when width > 1920, height > 1080 and init QP > 60 (true for chn0 by default) | per-channel tuning (HIGH) |
| loop | `ak_vi_get_frame(chn, &frame)` (up to 3 retries) then `ak_venc_encode_frame(handle, frame.f0, frame.f1, third frame word, &stream)` then copy `stream->data` (+0) of `stream->len` (+4) bytes then `ak_venc_release_stream(handle, stream)` then `ak_vi_release_frame(chn, &frame)` | the synchronous pull model: one thread per stream, no callbacks. Frame struct 40+ bytes at +0..+? (data refs, 64-bit timestamp near +8, in ms; `yy_cam` multiplies by 1000). `ak_venc_encode_frame` args are (handle, frame word 0, frame word 1, a third frame word, `video_stream *`): exact semantics need PoC-2 (MEDIUM) |
| IDR | `ak_venc_request_idr(handle)` | used for new RTSP/cloud clients (HIGH that it is imported) |
| teardown | `ak_venc_close`, `ak_vi_disable_chn`, `ak_vi_disable_dev`, `ak_vi_close`, `ak_sdk_exit` | |

`venc param` (64 bytes at offset 0, filled from the per-channel config; offsets HIGH, names HIGH where printed by
`libmpi_venc`'s own `venc%d_*` dump): u16 width +0, height +2, fps +4, GOP length +6, target kbps +8 (when the
config value is 0 it is 0.9 x max), max kbps +10; u32 profile +12 (Main 0, High 1, Baseline 2, CBase 3, Hevc* 4..6,
Jpeg 7); u32 rate-control mode +16 (the config key `enc_mode`; names from the lib: CBR 0, VBR 1, CONST_QP 2,
LOW_LATENCY 3, AVBR 4); u16 initial QP +20 (when 0: mean of min and max), min QP +22, max QP +24; u32 +28 JPEG quality
level, +32 chroma mode (MEDIUM); u32 +36 encoder type (H264 0, MJPEG 1, H265 2); u16 +52..+56 and u32 +60 smart-mode
fields (smart GOP length, quality, static value, GDR mode; MEDIUM). Validation messages in `ak_venc_open_ex`: width
and height ranges, width multiple of an alignment, height multiple of 8, "bitrate: target vs max", QP ranges, GOP
nonzero, GDR mode 0..3 without 1.

### 1.3 How the stock app sets things up (so our app can copy the defaults)

All values are `yy_conf_get_int/str("dsp", key, default)` reads at start. Defaults in the code (HIGH):

| Key (`[dsp]`) | chn0 (main) | chn1 (sub) |
|---|---|---|
| `video_chnN_width` x `height` | 2304 x 1296 | 640 x 360 |
| `enc_type` / `enc_profile` | H264 / Baseline | H264 / Baseline |
| `enc_mode` (= rate-control mode) | **4 (AVBR)** | 1 (VBR) |
| `frame_rate` / `gop` | 15 / 30 | 15 / 30 |
| `init_qp` / `min_qp` / `max_qp` | 100 / 32 / 51 | 41 / 32 / 50 |
| `max_bitrate` / `target_bitrate` (kbps) | **800 / 630** | 256 / 180 |
| `quality_lvl`, `chroma_mode`, `smart_mode`, `smart_goplen`, `gdr_mode`, `depth` | 30, 1, 0, 0, 0, 0 | same |
| `sensor1_*` | name `sc1346`, isp conf `isp_sc1346_mipi_1lane_av130.bin`, crop 1280x720 (probably unused), max 2304x1296, sub max 640x360, 15 fps | |

### 1.4 Point 4 (bitrate / profile / GOP / rate control): can we just change them?

**Exact keys (re-checked in the instructions, HIGH):** the encoder setup (function at 0x29a000 to 0x29c334, one 152-byte
per-channel config struct) reads **section `[dsp]`**, keys `video_chn0_max_bitrate` (default 800) and
`video_chn0_target_bitrate` (default 630), unit **kbit/s**, via `yy_conf_get_int("dsp", key, default)`; same family for
`enc_mode`, `enc_profile`, `gop`, `frame_rate`, `init_qp`, `min_qp`, `max_qp`. A **different** key,
`video_chn0_max_bit_rate` (with the underscore, default 1024), is read from section **`[cloud]`** by the Tuya cloud media
config (log line "cloud video conf"); it does not drive the encoder (likely the 1024 seen in ONVIF; MEDIUM). Editing
the wrong spelling or section does nothing. The live `/config/param.conf` has **no** video key today (only `[dsp]`
`ao_vol`, `ivs_mt_on_off`), so the code defaults apply and added keys must go in `[dsp]`. Not verified: that
`yy_conf_get_int` re-reads the file at each restart rather than a cached copy, and that nothing rewrites
`param.conf` (a factory-reset path does `rm -rf /config/param.conf`).

**Likely yes, with no code (UNVERIFIED).** The stock default `max_bitrate=800`, `target_bitrate=630` with AVBR
explains the measured ~770 kbit/s and the artefacts (PLAN 8bis point 4). The keys above are read from
`/config/param.conf` section `[dsp]` (fallback `/config/default_param.conf`) when `yy_cam` starts, so the existing
opt-in `enable_local.sh` mechanism (backup of `/config` first, restart `yy_cam`) can add for example
`video_chn0_max_bitrate=2048`, `video_chn0_target_bitrate=1536`, `video_chn0_enc_mode=1` (VBR), a lower
`video_chn0_min_qp`. Caveats: (a) fields are u16 so 65535 kbps is the ceiling; (b) `yy_cam` imports **no**
`ak_venc_set_*` function (only `open_ex`, `close`, `request_idr`, `set_stream_buff`, `strictlylimit_frameSize`,
`encode_frame`, `release_stream`), so its encoder is only configured by `open_ex` at (re)start, there is no runtime
bitrate change in the stock app, and Tuya DP "HD/SD" switches presumably re-open the encoder (LOW); (c) the ONVIF
`BitrateLimit` 1024 seen in `GetVideoEncoderConfiguration` is a separate static value and does not drive the encoder
(MEDIUM); (d) the 2K sensor at 15 fps through the Allegro encoder and a 38 MiB system may limit how high we can go,
measure. Our own app would additionally have `ak_venc_set_rc_param/set_qp/set_fps/set_rc_type` for live tuning
(exports of `libmpi_venc`, signatures unknown). Profile: Baseline/Main/High are all accepted by the lib (strings).

### 1.5 Does `libapp_video` offer an easier API?

Exports: `ak_app_video_set_chn_attr`, `_bind_chn`, `_unbind_chn`, `_set_dst_chn_active`, `_get_chn_bind_list`,
`_set_cb`, `_unset_cb`, `_venc_get_stream`, `_get_version`. It binds a VI channel to one or more encoders and runs a
thread (`mpp_vi_str_%d`) that pulls frames, drops frames above the encoder frame rate, encodes, and hands streams
out through a callback or `ak_app_video_venc_get_stream`; it also supports stitch channels. It imports
`ak_venc_bind_vi_chn`, `ak_venc_bind_activate`, `ak_venc_get_stream` (a **bound/asynchronous** encoder mode that
`yy_cam` does not use). **Recommendation: do not start with it.** The direct loop is what `yy_cam` proves in
production, is five calls, and gives us control of timestamps and threading; `libapp_video` is a possible
simplification for a later frame-rate-divided second stream (MEDIUM). `libapp_its` shows it is used by the ISP
tuning server, so it is exercised but not by the main path.

## 2. Watchdog (point 2)

Read in `yy_watch_dog_init` (0x2e8b70), the feed thread (0x2e8a44) and `yy_watch_dog_deinit` (0x2e8dd0). All HIGH
unless noted.

- `main` calls `yy_watch_dog_init(20, 5)` early (0x793c8): timeout 20 s, feed period 5 s.
- Init: `open("/dev/watchdog", O_RDONLY)`, `fcntl(fd, F_SETFD, FD_CLOEXEC)`, `ioctl(fd, WDIOC_SETTIMEOUT
  (0xc0045706), &timeout /*20*/)`, then starts a thread (named by `prctl(PR_SET_NAME)`) that loops `sleep(1)` and
  every >= 5 s does `ioctl(fd, WDIOC_KEEPALIVE (0x80045705))` and logs the "feed watchdog!" line. Nothing is written to
  the fd (no 'V', no heartbeat character).
- Deinit (clean exit only): `ioctl(fd, WDIOC_SETOPTIONS (0x80045704), &1 /*WDIOS_DISABLECARD*/)` then `close`. So the
  stock app disables the card explicitly rather than relying on a magic close (whether the driver honours
  DISABLECARD or has `nowayout` is unknown, MEDIUM that it does).
- Kernel driver: devicetree compatible `anyka,ak3918av130-wdt` (strings of the kernel backup); no `nowayout` string
  found. Whether the watchdog is armed before userspace opens it is **unknown**; evidence for "not armed at boot":
  stock boots spend well over 10 s between kernel start and `yy_watch_dog_init`, and no reset happens (palier 1, MEDIUM).
- If nobody feeds after open: hardware reset after the programmed timeout (20 s with the stock value; if the driver
  clamps the maximum, shorter, unknown).

**Safe takeover procedure** for our app (card only, no `yy_cam`):

1. Open `/dev/watchdog` as early as possible (first thing of the process, before any slow vendor init), set the
   timeout to 20 s, then feed from a **dedicated thread that never blocks on vendor calls** (period 5 s, as stock).
   A blocked `ak_vi_get_frame` must not stop the feed only if we want a hang to be survivable: the
   opposite design is better for the final app: tie the feed to a heartbeat of the video thread so a wedged encoder
   resets the camera (design choice for PoC-3; for PoC-1/2 feed unconditionally from the dedicated thread).
2. Before our own code exists (PoC-1), use the stock `/sbin/watchdog` BusyBox applet (present in the stock rootfs:
   `watchdog -t 5 -T 20 /dev/watchdog`, it opens, sets the timeout and keeps feeding; stop it only with SIGTERM
   (BusyBox writes the magic close `V`; it disarms only if the driver honours it, UNVERIFIED), a SIGKILL leaves the card armed and unfed, i.e. a reset in 20 s).
   Rule: exactly one feeder at any time, and no gap between "opened" and "first feed" longer than the timeout.
3. Clean shutdown or a debug stop: `WDIOC_SETOPTIONS` with `WDIOS_DISABLECARD`, then close. A crash of the feeder
   means a reset in at most 20 s: that is the intended safety net, the card removal path stays reversible.
4. Two openers: the driver is single-open in the usual Linux model, so never start `yy_cam` and our feeder together.

## 3. Boot without `yy_cam` (point 3)

### 3.1 What `start.sh` does (read in full) that must be preserved

`/appfs/script/start.sh`: symlink `/appfs/conf` to `/tmp/conf` and `webrtc_profile.ini`; `mkdir /tmp/rmem_blk`;
`insmod` in this order with these parameters: `ak_efuse`, `ak_pcm`, `ak_i2c`, `ak_isp`, `sensor_sc1346 check_id=1`,
`ak_venc_adapter`, `ak_venc_bridge`, `ak_npu`, `ty_driver` (if present), `ak_eth` (if present), `ak_saradc`,
`ak_pwm_char`, `ak_rtc`; sets `eth0` MAC from nvram `MASTER_MAC` (only if well formed); optional coredump and
`telnetd` flags on the card; VM tuning (`dirty_*`, `min_free_kbytes=1200`, `drop_caches`); finally runs
`/appfs/bin/yy_cam &` **only if nvram `STATUS` is `active`** (the live unit is). Before it, `rcS` already did mmc,
cfg80211, mac80211, usb, `ak_ion`, `ak_uio`, `nvram.ko`, mounted appfs and `/config`.

### 3.2 Recommended mechanism: bind mount, not a copy of `start.sh`

The overlay already bind-mounts a tmpfs `shadow`. Do the same for the app: in `akam_boot.sh`, when the card has a flag
`akam/own_app`, run `mount --bind /mnt/sdcard/akam/akam_app /appfs/bin/yy_cam` (target file exists on the read-only
squashfs; a bind mount over a regular file is standard Linux behaviour but UNVERIFIED on this kernel/BusyBox until PoC-1) **before** the hand-off to `start.sh`. The stock script then does every
driver, symlink and sysctl step and "starts `yy_cam`", which is our binary. No duplicated insmod list to maintain, and
removing the card restores the stock boot (a bind mount never survives a reboot). To keep the real `yy_cam`
reachable: `mount --bind /appfs /tmp/appfs_real` first (non-recursive clone, original file visible), or skip the bind
when a crash counter on the card says so (section 8, PoC-1 safety).

### 3.3 Wi-Fi, DHCP, credentials: done by `yy_cam`, not by `start.sh` (HIGH)

`yy_net_manager_wifi` in `yy_cam` does, in order: `insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1` (the driver is
**not** loaded by `start.sh`; `/dev/atbm_ioctl` serves country code and BLE); writes **`/tmp/wpa.conf`** from a template
(`ctrl_interface=/tmp/wpa_supplicant`, a `network={ ... scan_ssid=1 ... psk="%s" }` block, or `key_mgmt=NONE`); runs
`wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf -B -P /tmp/wpa.pid` (binary `/appfs/bin/wpa_supplicant`, a
`wpa_cli`-style client over the ctrl socket polls `wpa_state`); then `udhcpc -i wlan0 -x hostname:YY_CAM` (DHCP, or a
static IP from its own config), adds a gateway/multicast route, writes `/etc/resolv.conf` or `/tmp/resolv.conf`. It also
mounts a RAM block device (`rmem_blk.ko rmem_phys_addr=.. rmem_size=..`, `mkfs.vfat`, mounted at `/tmp/rmem_blk`) for
audio prompts and the OTA staging file; our app does not need it (and the `decloud` rule "never create
`/tmp/rmem_blk/yy_upgrade.bin`" stays).

Credentials: the SSID and key of the paired network are stored by the Tuya SDK under `/config/tuya` (pairing by BLE,
AP or QR code). **They were not read** and our app must not parse them. Our app (or `akam_boot.sh`) therefore needs
its own source: a `wpa_supplicant.conf` that the user provides on the card (`akam/wifi.conf`, git-ignored, never in the
repo), written by the assistant from the user's input. The regulatory country is set through `/dev/atbm_ioctl` in the
stock app; use `country=XX` in our conf and check the 5 GHz channels (open question, section 6).

Time: the stock app gets time from Tuya; the camera has no RTC and boots in 1970, and the card's BusyBox has no `ntpd`
applet. **Opt-in time sync (implemented, not yet run on the camera):** the shared 1080p script
`src/scripts/akam_ntp.sh` is copied UNCHANGED by `build_card.sh` to `akam/akam_ntp.sh`, and
`firmware/lsc-rotatable-2k-ak3918av130/sntp/` holds `akam2k_sntp` (about 9 KB, uClibc armv5, built by the same
`make poc3-dist` and checked by the same symcheck), a one-shot SNTPv4 client that accepts the `ntpd -n -q -p SERVER ...`
command line the script uses. It resolves with `getaddrinfo` (or a dotted IPv4), asks each server in order for 3 s on
UDP 123, accepts only a mode-4 reply of stratum 1..15 whose originate timestamp equals the random-ish transmit timestamp
it sent, computes the offset from the four timestamps and steps the clock with `settimeofday` (exit 0, or 1 when no
server answered; no resident process). NTP seconds are handled in 64 bits: the era is taken from the top bit (2036 is
read as era 1), and a result that does not fit the camera's 32-bit `time_t` (after 2038-01-19) or is before 2020 is
refused, never wrapped.

To enable it, create the card file `akam/ntp` (same format as the 1080p: one `server=NAME` line per server, at most 3,
a host name or a dotted IPv4, LF only), for example one line with your router's address (the DHCP gateway):

    server=<your router's IP, e.g. the DHCP gateway>

The stub starts `akam_ntp.sh run` detached once the network is healthy, only when that file exists: it retries after 15,
30, 60, 120, 300 then 600 s until the year is 2024 or later, then every 6 h, logging to `/tmp/akam_ntp.log` (RAM, capped).
It checks the clock itself, so no file means no process and no traffic. The binary is verified against
`akam/bin/akam2k_sntp.BUILDINFO` and run from a copy in `/tmp`. `akam/decloud` only blackholes the Internet (two /1
routes, the LAN subnet route stays), so a LAN NTP server keeps working with decloud on; a public pool name in `akam/ntp`
is the user's choice and is blackholed by decloud. Caveat: the clock steps once (1970 to now) while the app supervisor
may already be running, so its first restart-window and "stable run" measurements (wall clock `date +%s`) can be off for
that one step; the app itself uses monotonic clocks. Off again: delete `akam/ntp` (the helper ends at its next pass or
at reboot).

## 4. Toolchain (point 4)

Facts: `yy_cam` and the libs were built with **Buildroot 2018.02.7 (Anyka V1.0.05), GCC 5.5.0, uClibc-ng 1.0.31**,
target ARM926EJ-S (ARMv5TEJ), soft-float EABI5, interpreter `/lib/ld-uClibc.so.0`; single `libc.so.0` (pthread, dl,
math inside); `libstdc++.so.6.0.21` and `libgcc_s` are in the stock rootfs. Our app can be plain C (the vendor libs
need only `libc.so.0`), so no libstdc++ dependency.

| Option | Verdict |
|---|---|
| zig cc | rejected: no uClibc target, musl ABI differs; a musl-static binary cannot `dlopen` or link a `libc.so.0` library |
| Build the exact Buildroot 2018.02.7 toolchain | most faithful but slow and brittle on a 2026 host (old GCC sources); keep as fallback |
| **Bootlin prebuilt `armv5-eabi--uclibc--stable-2018.11-1`** | **recommended**: arm926ej-s, `--with-float=soft`, aapcs-linux, GCC 7.3.0, binutils 2.29.1, uClibc-ng **1.0.30**, 4.1 headers (read from the tarball's `buildroot.config` and file list). Same soname and single-lib layout as the camera's 1.0.31; a binary linked against the older libc runs on the newer one |
| Link against the camera's own `libc.so.0` and `ld-uClibc` taken from the backup | cannot work alone: no headers and no linker in the backup; but the vendor `.so` are taken from it at build time (below) |

Pinning, consistent with `tools/build/deps.lock` and `bootstrap.sh`: URL + sha256 in a lock file
([`app/toolchain.lock`](../../platforms/anyka-ak3918av130/toolchain/toolchain.lock)), downloaded by
[`app/fetch_toolchain.sh`](../../platforms/anyka-ak3918av130/toolchain/fetch_toolchain.sh) into the git-ignored
`tools/.toolchain/`, hash mismatch aborts. The tarball is 58,143,779 bytes, sha256
`198d4a32899e3bfd4dbbc7c59fa1b8da9fa9b5d3ecc02c55fc64f470e8ca2fbb` (equal to our computation and to the file Bootlin
publishes next to it). Vendor `.so` for linking come from the user's own backup through
[`app/prepare_vendor.sh`](../../platforms/anyka-ak3918av130/toolchain/prepare_vendor.sh) (checks `mtd3.bin`
against the backup's `SHA256SUMS`, `unsquashfs` of `lib/` only, output under git-ignored `dist/`). No vendor header is
needed or used: we declare only the prototypes we call (struct layouts of section 1, written ourselves).
**Isolation:** a separate directory, toolchain, output (`dist/rot2k-app/`) and Makefile; none of `src/ak_rtsp`, the zig
bootstrap, CI groups or releases is touched (zero 1080p regression). Status: the tarball was downloaded and its text
metadata read; **its compiler was not executed** (needs an explicit go, see the report).

Known toolchain risks: C++ vendor interfaces are not involved (all `ak_*` are C); the `-march=armv5te` code runs on
ARMv5TEJ; keep soft-float for every object. If 1.0.30 vs 1.0.31 symbol versions bite, fall back to Buildroot 2018.02.7.

## 5. RAM budget (estimate, LOW-MEDIUM)

Camera: `MemTotal` 38112 kB (`mem=64M`, the rest is the reserved ION/DMA pool used by `ak_mem_dma_*`, where the VI
frames, ISP 3DNR buffers and encoder buffers live; the study did not read its size).

| Item | Stock (measured, palier 1) | Own app (estimate) |
|---|---|---|
| Free with video running | 16 to 18 MB | |
| `yy_cam` RSS | ~8 MB, 31 threads (Tuya, ONVIF, RTSP, audio, NPU, cloud) | removed |
| Our process: code + heap + 2 stream rings (main 1 to 2 MiB, sub 256 KiB) + RTSP/control | | 2 to 4 MB |
| Vendor lib mappings (file-backed, shared, evictable) | in both | similar |
| NPU model (~960 KiB, only if detection is wanted) | stock loads it | optional |
| Expected free with video | | **~20 to 24 MB**, i.e. more headroom than stock |

Per-thread stacks: set small (64 KiB) for our threads; the vendor libs create their own threads (ISP). Measure in
PoC-2.

## 6. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| Struct layouts recovered without headers are wrong (esp. frame struct, `ext`, dev attr names) | crash or garbage in PoC-2 | no debugger on the camera: use the field notes and incremental bring-up (first `ak_vi_get_sensor_resolution`, then one stage at a time), sizes validated by the libs' own error messages |
| Hardware watchdog arming state at boot unknown | reset before our feeder runs | feeder first, busybox `watchdog` in PoC-1, crash counter fallback |
| Hidden side effects of `yy_cam` setup: IR cut, IR LED, status LED GPIOs, day/night policy, sensor power | black or pink image at night, no IR | day-light PoC first; map pins from the stock app later (ptz-findings method); `/dev` nodes for LEDs are done by `yy_cam` code |
| ISP tuning in user space needs thread start order we do not know | green or black frames | keep the call order of section 1.2; compare one frame against a stock RTSP frame |
| Wi-Fi bring-up: ATBM driver, country code, 5 GHz, credentials source | no network = no SSH | credentials from our own card file; fallback = card log + crash counter returns to stock |
| Audio (G.711 over `ak_ai_*`/`libplat_ai`), two-way, RTSP audio track | missing feature | out of scope of PoC-1..3, `ak_ai_*` and `ak_adec/ao` calls are visible in `yy_cam` |
| PTZ and detection are in `yy_cam` code (`yy_ptz_motor`, `Y_ALG_AV130_HD`) | lost when `yy_cam` stops | PTZ already solved by `akam_ptz`; NPU detection: Phase 3 (npu-findings) |
| Flash install needs the 7bis gate and a CH341A recovery | card-less install blocked | unchanged: this study stays on the card |
| Vendor `.so` license | cannot redistribute | never commit or ship them; they stay on the camera (card flow) or come from the user's backup |

## 7. Reproduction notes

Disassembly was done with `gdb -batch` (`set architecture arm`, `disassemble start,end`) on the extracted binaries,
a small script resolving literal-pool strings and `@plt` names, and `readelf --dyn-syms`. Useful anchors in
`yy_cam`: `yy_media_av130_video_init_framesource` 0x29d218 (+ log strings), the encoder init containing
`ak_venc_open_ex` (call 0x29e6d4), the frame loop `yy_media_av130_video_not_malloc_get_frame` 0x29f904, config table
reads 0x29a000 to 0x29b560 (key names with defaults), watchdog 0x2e8a44 to 0x2e8ee0.

## 8. PoC plan (each step reversible by removing the card)

Common rules: the card is the only thing that changes; `akam_boot.sh` keeps always reaching the stock hand-off unless
the step's flag file is present; every step logs to `<card>/akam/*.log`; no flash write; the user powers the camera.

**PoC-0: toolchain and linkage, stop nothing.** Build `app/poc0_versions.c` (after `fetch_toolchain.sh` and
`prepare_vendor.sh`), copy it to the card, run over SSH with the stock app still running:
`LD_LIBRARY_PATH=/appfs/lib /mnt/sdcard/akam/poc0_versions`. Pass: it prints the five version strings (`V3.4.32` for the
VI lib etc.), proves loader, soname and ABI match. Abort: `ld.so` errors (then try the Buildroot 2018.02.7 toolchain),
or any side effect on `yy_cam` (none expected: the five getters were disassembled in the stock libs, each is a 12-byte function returning a constant
string pointer, no device access, no `ak_sdk_init`; the libs' init arrays hold a single compiler `frame_dummy`-type
entry, HIGH; the process opens no `/dev` node and never touches the watchdog). Cost: minutes.

**PoC-1: boot without `yy_cam`, our process feeds the watchdog, Wi-Fi up, SSH ok.** IMPLEMENTED (card side, host
tests green, not yet run on hardware). Design:

- Flag `akam/own_app` (card builder `--own-app`). Without it `akam_boot.sh` behaves exactly as before.
- With it, BEFORE the hand-off, `akam_boot.sh` reads `akam/own_app_boots` (missing = 0, non-numeric = 3), and if it is
  already >= 3 logs `own_app disabled after 3 unfinished boots` and leaves `yy_cam` alone. Otherwise it writes n+1
  (`sync`, read back; an unwritable card means stock boots), copies `akam/own_app_stub.sh` to `/tmp` and
  `mount --bind`s that copy over `/appfs/bin/yy_cam` (RAM only; the squashfs is untouched). If the mount fails the
  counter is restored and stock boots. Then the normal hand-off to `start.sh`, which loads the drivers and "starts
  yy_cam", i.e. the stub.
- The stub, in this order: (1) ONE feeder, the card busybox 1.36.1 `watchdog -F -t 5 -T 20 /dev/watchdog` (`-F`
  keeps it in the foreground so the pid is known; BusyBox 1.30.1 stock has the same `-t/-T/-F` options, 1.36.1 is
  used because it is the one we control); (2) `insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1`;
  (3) copy `akam/wpa.conf` to `/tmp/wpa.conf` (0600, never read or logged); (4) `wpa_supplicant -Dnl80211 -i wlan0
  -c /tmp/wpa.conf -B -P /tmp/wpa.pid`, `udhcpc -b -i wlan0 -x hostname:akam-rot2k`; (5) health within 120 s (60 x 2 s):
  an IPv4 on `wlan0` and a default route (the gateway ping is logged, informational: a router that drops ping must not
  cause reboot cycles). Healthy: counter reset to 0 (`sync`), `own_app PoC-1 healthy`, then idle forever. Unhealthy
  (or no `akam/wpa.conf`): the feeder is SIGKILLed, the hardware resets the camera in about 20 s, the counter stays
  incremented, and the third unfinished boot falls back to stock. NTP is not done in PoC-1 (added later as the opt-in `akam/ntp` time sync, section 3 "Time").
- The stub never exits on its own. On SIGTERM it stops the feeder with SIGTERM: BusyBox `watchdog.c` handles SIGTERM
  by writing the magic close character `V` and closing the device, which disarms only if the Anyka driver honours
  the magic close (stock uses `WDIOC_SETOPTIONS` DISABLECARD instead; `nowayout` UNVERIFIED). If it does not disarm,
  the camera resets within 20 s, the safe direction. So the earlier statement "SIGTERM disarms" is conditional.
- Anti-brick: no flash, no nvram. The static scan allows the literal `/dev/watchdog` in exactly one file
  (`own_app_stub.sh`, once), like `/dev/tymotor`. The watchdog is not flash: its worst effect is a reboot, and a reboot
  is the recovery path (bind mounts are gone, the counter counts up to the stock fallback, removing the card gives
  stock at once). A hung stub with a live feeder cannot happen silently: the health check ends in SIGKILL of the feeder.
  Residual risk: if the feeder cannot start (applet missing) nothing is armed and a failed boot leaves the camera
  up without network until power-cycled; the counter still advances on the next boot.

Hardware procedure (user powers the camera; the coordinator never contacts it):
1. Card with the overlay built by `build_card.sh <card> --authorized-keys KEY.pub --own-app` (it never creates
   `akam/wpa.conf`; the card already holds the working copy, or pass `--wpa-conf FILE`). Check `akam/own_app` exists and
   `akam/own_app_boots` does not (or holds 0). Camera is reachable by SSH on stock first, as a baseline.
2. Power-cycle. Expected `akam/akam_boot.log` lines, in order: `boot: akam_boot.sh start`, `boot: own_app: stub
   bind-mounted over yy_cam (unfinished boots: 1 of 3)`, `boot: handed off to /appfs/script/start.sh`, then (stub)
   `own_app: stub start`, `watchdog feeder started, pid N`, `insmod atbm6132c ok` (or "returned an error"),
   `wpa_supplicant started`, `udhcpc started (hostname akam-rot2k)`, `gateway X answers ping`, `own_app PoC-1 healthy`.
3. Pass: SSH answers within about 60 s of power-on; `akam/own_app_boots` holds 0; no reboot for 10 minutes; over SSH
   `ps` shows one `watchdog -F -t 5 -T 20`, no `yy_cam`, and `mount | grep yy_cam` shows the bind mount.
4. Failure shape: `own_app PoC-1 FAILED: ...` then `killing the watchdog feeder`; the camera reboots within about 20 s and
   the log shows `unfinished boots: 2 of 3`, then 3 of 3, then `own_app disabled after 3 unfinished boots` and the
   stock `yy_cam` boots.
Abort and rollback: via SSH or telnet `rm /mnt/sdcard/akam/own_app` and reboot (or `reboot` with the counter at 3);
or power off and remove the card (stock boot, nothing was written to flash). To retry after a fallback, delete
`akam/own_app_boots`. Abort criteria: two unexplained reboots after a healthy log line, no SSH although the log says
healthy, any write attempt to flash (none exists).

**PoC-2: one H.264 frame from the sensor to the card.** Replace the stub's idle with `akam_app` (C): watchdog thread
first; `ak_sdk_init`; `ak_vi_open`; `ak_vi_load_sensor_cfg`; `ak_vi_set_dev_attr`; `ak_vi_enable_dev`; chn1 first (640x360
Baseline, simplest), then chn0; `ak_venc_open_ex`; one `get_frame` guarded by a timeout (separate thread plus
`alarm`), `encode_frame`, write the Annex B bytes to `/mnt/sdcard/akam/frame.h264`, `release_*`, then exit cleanly
(watchdog DISABLECARD). Step order: sensor resolution only, then VI frame size, then encode. Pass: `ffprobe`/`ffmpeg`
decode the file at the expected size; image looks like the stock RTSP frame (exposure and colour); RSS and free RAM
logged. Abort: the process hangs more than 30 s in a vendor call, kernel oops, green/black frame that the ISP conf
does not explain, free RAM below 6 MB. The watchdog resets the camera if it wedges; card removal returns stock.

**PoC-3: RTSP stream.** Loop frames to an RTSP server reusing `smolrtsp` (plain C, builds with this toolchain; the
musl `time64` workaround of the 1080p does not apply to uClibc) and the shared control port, stream0 and stream1,
`request_idr` on client connect, bitrate/QP/GOP from our config (also answers point 4). Pass: VLC/ffplay and Frigate
play both streams for an hour; CPU idle margin and RAM stable; watchdog fed from a heartbeat tied to the video thread.
Abort: sustained frame drops, RAM trending down, any reset. Then audio, ONVIF/MQTT reuse, auth, IR/LED, PTZ glue,
decloud-less operation, NTP, and only then 8bis point 7 (flash), gated by 7bis and the CH341A.

## 9. Go / no-go

**GO (MEDIUM)** for PoC-0, PoC-1, PoC-2 as defined: each is cheap, card-only and reversible. Decide on PoC-3 after
PoC-2 shows a correct frame and the RAM figure. **NO-GO signals** that would stop the project: ISP/AE refuses to run
without side effects we cannot reproduce (black or colour-cast frames after matching the call order), the vendor libs
cannot be loaded by a binary from the pinned toolchain even after trying Buildroot 2018.02.7, or free RAM with video
under our process is not clearly better than stock. The independent flash gate (PLAN 7bis, 8bis points 5 and 7)
remains RED and unaffected.

## PoC-0 result 2026-11-14 (hardware, akam-rot-01) — PASS

Bootlin `armv5-eabi--uclibc--stable-2018.11-1` (sha256 verified) + vendor libs from the user's backup. The link needs
`-lplat_thread -lplat_dbg` too (the vendor libs use their symbols without listing them in NEEDED). Run over SSH with
`LD_LIBRARY_PATH=/appfs/lib`, `yy_cam` running, no side effect (yy_cam still up, no reboot):
`libplat_vi V3.4.32`, `libplat_vpss V3.1.03`, `libmpi_venc V3.0.00`, `libplat_mem V2.0.01`, `libapp_video V2.0.00`.
→ a uClibc-ng 1.0.30 binary loads and calls the camera's vendor libraries (uClibc 1.0.31).

## PoC-1 result 2026-11-14 (hardware, first run) - safety chain VERIFIED, Wi-Fi bring-up to fix

Boots 1 to 3 each logged `stub start`, `watchdog feeder started`, `insmod atbm6132c ok`,
`FAILED: wpa_supplicant did not start`, `killing the watchdog feeder (SIGKILL)`. The hardware reset followed about 20 s
later, and boot 4 logged `own_app disabled after 3 unfinished boots` and the stock `yy_cam` came back with Wi-Fi.
Verified facts: (1) the bind mount over `/appfs/bin/yy_cam` works on this kernel; (2) the busybox `watchdog` feeder
arms the hardware watchdog and a SIGKILL of the feeder resets the camera in about 20 s; (3) the 3-boot counter falls
back to stock. Cause of the failure (hypothesis): `wpa_supplicant` was started 1 s after `insmod`, before `wlan0`
existed (the firmware load is asynchronous), and its output was discarded. Fix: wait up to 20 s for
`/sys/class/net/wlan0` (the delay is logged), `ip link set wlan0 up`, up to 3 `wpa_supplicant` attempts 3 s apart,
output kept in `/tmp/wpa.out`, and the last 5 lines, minus any line containing psk, passphrase, password or ssid,
appended to the card log. The 120 s health budget is measured from the stub start and the safety behaviour is unchanged.

## PoC-1 result 2026-11-14 (hardware, runs 2 and 3) - wlan0 never appears; cause found offline, fix applied

Runs 2 and 3 (with the wait-for-wlan0 fix): `insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1` returned OK but
`wlan0` never appeared (20 s wait), also after exporting gpio55 as an output at 1 before the insmod. The USB stack
(`usbcore`, `ak_hcd`, `usb_common`, `mac80211`, `cfg80211`) is loaded by the rootfs `rcS` in both stock and PoC-1 boots,
so it is not the difference. The ATBM6132C is a USB device: it needs its enable GPIO driven by `yy_cam`.

Offline reverse engineering of `yy_cam` (ARM, not stripped of function symbols; no camera contact). Evidence:

- Config loader `0x2abde0` reads section `[net]` (config file `/config/param.conf`, then `/config/default_param.conf`;
  both only on the camera, not read here): `wifi_enable` default 1, **`wifi_en_gpio_no` default 10**,
  **`wifi_en_gpio_polarity` default 0**, `wifi_model_name` `atbm6132c`, `wifi_insmod_driver_cmd` default
  `insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1`, `wifi_rmmod_driver_cmd` empty, `wifi_is_need_write_mac` default 0
  (no MAC write), `wifi_sta_interface_name` `wlan0`.
- `yy_net_manager_wifi_init` (`0x2ac978`): if enable and gpio != 0: `yy_gpio_init(gpio, 1 = out, value = (polarity == 0))`,
  i.e. the DISABLED level first (1 for polarity 0); then `yy_net_manager_wifi_insmod_driver` (`0x2ac7dc`); then `sleep(2)`.
- `yy_net_manager_wifi_insmod_driver`: `yy_gpio_set_value(gpio, polarity == 1 ? 1 : 0)` = the ENABLED level (0 for the
  default polarity 0, so the pin is active low), `sleep(1)`, then `yy_shell_cmd_execute(insmod_cmd)`. The matching rmmod
  function (`0x2ac8a0`) runs the rmmod command, `sleep(1)`, and sets the disabled level.
- `yy_gpio_*` (`0x2e66a0` and following) use plain sysfs: `/sys/class/gpio/export` if `gpioN` is absent, `gpioN/direction`
  (`out` for 1), `gpioN/value` (`%d`). No `/dev/mem`, no ioctl, no USB VBUS GPIO or `/sys` ak_hcd write, no extra module.
- Other `yy_gpio_init` users and their default pins: red LED gpio55 (polarity 1), blue LED gpio56, speaker gpio9,
  IR-cut gpio24/25, IR light gpio48, white light gpio49, alarm gpio22, keys gpio50 (reset) and gpio71 (call), both inputs.
  So **gpio55 is the red LED, not the Wi-Fi enable**, and the stock gpio snapshot (outputs 9,10,24,25,48,49,56 at 0,
  gpio55 at 1, gpio50/71 inputs at 1) is consistent with the default Wi-Fi pin gpio10 sitting at its enabled level 0.
- After the insmod: no `ifconfig wlan0 up` string exists; `wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf -B -P
  /tmp/wpa.pid` and `udhcpc -i wlan0 -x hostname:YY_CAM` are run later by the net manager; the country code goes through
  `/dev/atbm_ioctl` (`open_atbm_ioctl`), which the stub does not need to associate. Rootfs `rcS`, `init.sh` and `start.sh`
  contain nothing Wi-Fi-power related (`rcS` only loads the USB host modules; `start.sh` only starts `yy_cam`).

Confidence: HIGH that the stock sequence is gpio10 (default), active low: export, out, 1, 0, sleep 1, insmod, sleep 2,
and that nothing else is done; MEDIUM-HIGH that this camera uses the default pin and polarity (the camera's
`[net]` overrides in `/config/param.conf` were not read; the stock gpio10 = 0 observation fits the defaults).
Not hardware-verified yet.

Corrected bring-up in `own_app_stub.sh` (RAM only, sysfs only, no new device path, the anti-brick scan is unchanged):
gpio10 export, `out`, value 1, value 0, `sleep 1`, `insmod ... wifi_bt_comb=1`, `sleep 2`, wait for `wlan0` (20 s), as before.
The gpio55 write is removed. Overrides for experiments: `AKAM_WIFI_EN_GPIO`, `AKAM_WIFI_EN_POL`.
If `wlan0` still does not appear, the stub logs (card log, bounded, filtered): the gpio10 direction and value, the
`/sys/bus/usb/devices` listing, every `idVendor`/`idProduct`, and the last 15 `dmesg` lines matching
`usb|atbm|hcd|wlan|80211` minus anything with psk, ssid, passphrase or password. An empty USB device list means the
chip is not powered or not enumerated (pin or polarity wrong); a device with no `wlan0` means a driver or firmware problem.

## PoC-1 result run 4, 2026-11-14 — PASS

Stock Wi-Fi power sequence (gpio10, active low) fixed it: `wlan0` present at once, `wpa_supplicant` attempt 1,
DHCP lease (same address), gateway answers, "own_app PoC-1 healthy" at +18 s, counter reset to 0. Stable: uptime
> 10 min with only our busybox watchdog feeder (no reset), decloud blackholes + keeper running, RAM 20.9 MB free /
24.1 MB available (vs ~18 MB available with yy_cam), CPU idle. No video in this mode (expected).

## PoC-2 implementation (offline, 2026-11-14): `app/poc2_frame.c`, not yet run on hardware

`make -C firmware/lsc-rotatable-2k-ak3918av130/app poc2` (or `all`) builds `dist/rot2k-app/poc2_frame` (ARM EABI5,
uClibc, dynamic, same link line as PoC-0). No vendor header: struct sizes and offsets are `_Static_assert`ed in the
source, which is also compile-checked by `tests/rot2k_overlay_test.sh` when the pinned toolchain is present.

**Correction to section 1.2 (re-read in the `yy_cam` disassembly, HIGH):** the stock order is `ak_vi_open`,
`ak_vi_load_sensor_cfg`, `ak_vi_set_dev_attr`, **`ak_vi_set_chn_attr` for every channel (0x29da9c), then
`ak_vi_enable_dev` (0x29db58), then `ak_vi_enable_chn` for every channel (0x29dc78)**, then `ak_venc_open_ex`. The
program follows this order, not "enable dev before chn attr". `ak_vi_set_chn_attr(chn, &attr)` takes the channel id as
first argument and also in the struct word +0 (chn0 = 0, chn1 = 1 by the `video_chnN_chn_id` defaults).

Re-checked and confirmed: dev attr 56 bytes (`sensor1_*` defaults dev 0, crop 0/0/1280/720 kept as stock, max
2304x1296, sub max 640x360, 15 fps; +4..+16 zero); chn attr 20 bytes (id, fps, w, h, depth); `ak_venc_open_ex(&64, &28,
&handle)` with the byte at +24 of the 28-byte ext set to 1; `ak_venc_strictlylimit_frameSize(h, 400)`; frame loop
`ak_vi_get_frame` (3 tries), `ak_venc_encode_frame(handle, frame word 0, frame word 1, frame word 8, &stream)` with the
5-argument form, stream `data +0, len +4`, then `release_stream`, `release_frame`; `ak_venc_request_idr(handle)` takes
only the handle. Venc param offsets, completed from the config loader (0x29a8d8..0x29b520, key defaults there): +28 =
`snap_jpeg_quality` (1), +32 `chroma_mode` (1), +40 `enc_buf_size` (0), +44 `quality_lvl` (30), +48 `smart_mode` (0),
+52 `smart_goplen`, +54 never written (0), +56 `smart_static_value`, +60 `gdr_mode` (all 0). Chn1 defaults: VBR, init QP
41, min 32, max 50. Unknown by name (LOW): frame words +16..+28 and +36, dev attr words +4..+16, the stream `type`
at +20 (its value is only logged; the program detects IDR by scanning NAL types).

What it does: `poc2_frame [-n frames=30] [-c 1|0] [-b] [-p base|main] [-k kbps=1000] [-t secs=20] OUTFILE`. OUTFILE must be
under `/mnt/` or `/tmp/`. Steps and return codes are printed with a ms clock and `VmRSS/VmHWM/MemFree/MemAvailable`;
per frame: input bytes, timestamp, output bytes, stream type, NAL counts (SPS 7, PPS 8, IDR 5, slice 1). `alarm()` (default 20 s,
`-t` 5..120) ends the process from a signal handler (`_exit(124)` after printing the step); SIGSEGV/SIGBUS/SIGILL
do the same (`_exit(2)`); SIGINT/SIGTERM finish the loop and clean up. On a failed step it logs it, tears down what was
opened in reverse (`venc_close`, `disable_chn`, `disable_dev`, `vi_close`, `sdk_exit`) and exits 1. It opens only
OUTFILE and two `/proc` text files, never a device node, GPIO, flash, nvram or `/dev/watchdog`. `-b` also enables the
other VI channel and pulls/discards its frames (the stock always runs both; use it if chn1 alone gives no frames).
`-c 0` uses the stock main defaults (2304x1296, AVBR unless `-k` is given, then VBR).

### PoC-2 hardware procedure (the coordinator runs it, the camera stays on the PoC-1 stub)

1. Copy: `scp poc2_frame root@CAM:/tmp/` (RAM, not the card), `chmod 755 /tmp/poc2_frame`. Baseline `free`, and check
   `ps` shows no `yy_cam` and the stub's one `watchdog` feeder (it keeps the camera alive during the test).
2. Run: `LD_LIBRARY_PATH=/appfs/lib /tmp/poc2_frame -n 30 /mnt/sdcard/akam/poc2.h264; echo rc=$?`. Expected: every step
   `= 0`, 30 `frame N:` lines, `done: 30 frames written`, `exit 0`, in well under 20 s. Save the console output.
   Fallbacks in this order: `-p main`; then `-c 0 -n 5` (`-b` is obsolete: both channels are always enabled).
3. Pull the file and check on the host: `ffprobe -hide_banner poc2.h264` (expect h264, 640x360 for chn1), then
   `mkdir -p test-images/rot2k && ffmpeg -i poc2.h264 -frames:v 1 test-images/rot2k/$(date +%F)_poc2_chn1_640x360.png`.
   `test-images/rot2k/` at the repo root is git-ignored locally (`.git/info/exclude`): never add it to a tracked
   `.gitignore` and never commit the images. Compare exposure and colour with a stock RTSP sub-stream frame.
4. Pass: exit 0, valid decodable H.264, correct size, plausible image, `MemAvailable` stays above 6 MB, camera still
   answers SSH afterwards, no reboot. Record RSS/HWM and the time of the first frame.
5. Abort criteria: the program's own guard fires (`TIMEOUT ... in step: X`, exit 124: the step X is the finding), a
   crash line (exit 2), a kernel oops or a reset (just let it reboot or power-cycle; nothing persistent was written, the
   PoC-1 counter logic still applies), a green or black frame the ISP conf does not explain, MemAvailable below
   6 MB. Do not retry in a loop after a timeout: report the step and the log first. Cleanup: `rm /tmp/poc2_frame`.

### PoC-2 run 1 (hardware) and fixes

Run 1: `ak_sdk_init`, `ak_vi_open`, `ak_vi_load_sensor_cfg`, `ak_vi_set_dev_attr` all 0; `ak_vi_set_chn_attr(1)` = 257
with "the channel[1] frame depth [0] is out of range [1 - 4]"; the cleanup was clean. Cause and fix (HIGH, disassembly):
the chn attr word +16 is `video_chnN_depth`, default 0 in the config, but the stock then substitutes 3 when
`ak_mem_dma_get_reserved_size` reports more than 50 MiB (52428800), else 2 (0x29d294..0x29d2ac, used at 0x29d9cc). The
program now makes the same two `ak_mem_*` calls right after `ak_sdk_init` and applies that depth.
The dev attr warnings (`3DNR yuv size[0][0][0] out of range`, `reset to CROP[1280, 720]`) are stock behaviour:
`yy_cam` imports neither `ak_vi_set_dev_3DNR_*` nor any other 3DNR call, its dev attr has no 3DNR field (56 bytes, all
words accounted for), and the crop 1280x720 is the stock `sensor1_crop_*` default which the lib also reads back from
the sensor itself ("vi_dev_get_sensor_crop w:1280 h:720"); rc stays 0, so nothing is changed. The output is now capped at
8 MiB and OUTFILE may not contain "..".

### PoC-2 run 2, run 3 (hardware): PASSED with both VI channels

Run 2 (chn1 only): `ak_vi_enable_dev` failed, vendor log "Channel [0] is not existed!": the lib expects channel 0
to be configured before the device is enabled, even when only the sub stream is encoded. Run 3 (both channels set and
enabled, the unused one pulled and discarded per frame, as the stock does) **passed**: rc 0, 30 frames, 17394 bytes of
H.264 Baseline 640x360 that ffmpeg decodes, about 70 ms per frame (about 14 fps at the 15 fps sensor setting), the first
frame black (AE still converging), frame 30 a real image (Y 17..178), RSS 1.2 MB / HWM 1.7 MB, MemAvailable 23.1 MB,
clean teardown, no reset. Reported NAL totals "sps 2 pps 2 idr 2 slice 58" were **double counted** by the scanner (a
4-byte start code 00 00 00 01 also matched as 00 00 01 one byte later): the real figures are one SPS, one PPS, one IDR
and 29 slices. Fixed in the program.

Program changes after run 3: both channels are now always configured and enabled (the `-b` option is accepted and
ignored, the fallback list in the procedure above loses its first item); the NAL scan skips past each start code;
the per-frame length from the lib is compared with the remaining 8 MiB budget without overflow; numeric options are
parsed strictly (no trailing garbage, range-checked); `-p` accepts only `base` or `main`. The overlay test now also
scans app sources for nvram/mtd//dev/reboot/system/popen/exec/fork/socket and asserts the path restriction, the cap and
that `alarm()` is armed before `run()`.

## PoC-3 implementation (2026-11-14): `akam2k_rtsp`, first hardware run passed (see "Hardware result")

`make -C firmware/lsc-rotatable-2k-ak3918av130/app poc3 SMOLRTSP=<checkout with build/_deps>` builds
`dist/rot2k-app/akam2k_rtsp` (ARM EABI5, uClibc, dynamic, 71 KB stripped, no warnings). SmolRTSP is the fetched
dependency the 1080p build also uses (it is not tracked in git): pass the path of a checkout that has `build/_deps`.
`make poc3-symcheck CAMERA_LIBC=<libuClibc-1.0.31.so of the 2K camera, from your own backup>` (not the 1080p camera's libc: different build) verifies that every libc symbol of the
binary is exported by the **camera's** libc (see "Finding" below).

**What it does.** The PoC-2 bring-up sequence for both VI channels, one encoder per channel (main 2304x1296 H.264 Main,
VBR 2000 kbit/s max 2500, GOP 30, 15 fps; sub 640x360 Main (Baseline retry), VBR 800 kbit/s QP 26..44, GOP 30), one capture thread per channel
(pull, encode, copy into a bounded frame ring, release the encoder buffer at once). RTSP on port 554:
`rtsp://<ip>:554/stream0` (main) and `/stream1` (sub), RTP/H.264 over **TCP interleaved** (a UDP SETUP gets
`461 Unsupported Transport`, so players fall back to TCP; UDP is not implemented in PoC-3), video only, SDP with
`sprop-parameter-sets` taken from the first keyframe, up to 4 clients (the fifth gets `503`), one thread per client.
A client starts on a keyframe: a keyframe still in the ring is used at once, otherwise `ak_venc_request_idr` is
requested (merged to one per 300 ms) and frames are dropped until it arrives. A slow client never blocks the capture:
its writes are non-blocking (private queue, `net_tx`), the shared `tx_pace` policy drops frames for that client only
and resumes on a keyframe, a client that falls more than 6 frames behind (the ring depth, see "Auto-start") resynchronises on a keyframe, a dead or
hopeless one is dropped. Optional Digest authentication: if the credentials file exists (default
`/mnt/sdcard/akam/rtsp.conf`, `-a` to change) every request but OPTIONS needs it. The file is the one-line format of
`src/net/rtsp/rtsp_auth.h`: `user:akam:<md5 hex of "user:akam:password">` (realm `akam`); an invalid file refuses
everybody. Basic is not offered (it would need the clear password on the card). Off by default (no file).

Options: `-p/--port`, `-m/--main-kbps`, `-s/--sub-kbps`, `-g/--gop`, `-a/--auth`, `-n/--frames N` (exit after N main
frames), `-t/--seconds N` (exit after N s; with neither, run until SIGTERM/SIGINT). SIGTERM/SIGINT: client threads
end (connections are closed, no RTSP goodbye is sent), capture threads join, then `venc_close`, `vi_disable_chn`, `vi_disable_dev`, `vi_close`, `sdk_exit`
(guarded by `alarm(20)`; bring-up is guarded by `alarm(60)`; both end the process with the step name on a hang). It prints
every 10 s the fps and kbit/s of each stream, CPU %, clients and VmRSS/VmHWM, and a summary at exit; exit 1 when a
stream never produced a frame or a capture thread gave up (no frame for 10 s).

**Shared code reused unmodified (phase A of PLAN 8ter):** from `src/ak_rtsp`: `h264_params.c` (SPS/PPS, fmtp),
`rtsp_sdp.c` (SDP text, video only via `audio_off`), `rtsp_auth.c` + `md5.c` (Digest), `rtsp_in.c` (stream framing,
RTCP BYE scan), `rtsp_idle.c` (pre-PLAY idle policy), `rtp_clock.c` (capture clock, 90 kHz ticks), `net_tx.c` (bounded
writer and private queue), `tx_pace.c` (backpressure); all of `third_party/smolrtsp` (request parser, responses,
RTP/NAL/FU-A packetiser, TCP transport). Nothing under `src/` is modified (`git diff dev --stat -- src/` is empty).
**Not reusable as is:** `rtsp_session.c` (one blocking client wired to the 1080p `/dev` encoder ring, the audio track, LED
and control port: replaced by the thin adapter `rtsp_session.c`, which copies its session logic), `rtsp_busy.c` (the
single-client 503 and its "one viewer at a time" text are the 1080p policy; unused). **Phase-B extraction candidates**
(each is a pure move that must leave the 1080p binary byte-identical): the session/controller part of `rtsp_session.c`
(OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN/GET_PARAMETER handling, `Client_before` auth glue, `TxWriter`, the NAL walk
`send_frame_nalus`, the pacing block of `send_video_frame`) behind a "media source" interface (current frame, key flag,
timestamp, SPS/PPS, request_idr) with the encoder ring, audio, LED and control calls on the model side; and the
`rtsp_auth` file path, which is a compile-time define. Today the 2K copy duplicates about 400 lines of that logic.

**Finding (offline, from the camera's rootfs):** the pinned toolchain's `<ctype.h>` expands `isdigit()`/`isalpha()`
into `__ctype_b_loc()`, which the camera's libuClibc-1.0.31 does **not** export: the first build would have died at
start-up with an unresolved symbol. SmolRTSP's parser is the only user; the SmolRTSP objects are compiled with
`-include ctype_compat.h` (drops the macros, the classification functions are exported by both libcs), the files
themselves stay unmodified. `poc3-symcheck` guards this for every future change.

**Host validation done (no camera):** a stand-in capture replaying two x264 files (2304x1296 Main, 640x360) through the
same stream/RTSP/shared code on the host (ASan+UBSan): `ffprobe` over TCP reports h264 Main 2304x1296 and 640x360
15 fps; 4 simultaneous clients (3 main, 1 sub) decoded 8 s each with no error; the 5th got 503; a client that never
reads was dropped by `tx_pace` while the capture stayed at 15.0 fps; an abrupt disconnect and TEARDOWN freed the slot;
Digest: no/wrong credentials 401, good ones play; SIGTERM and `--frames` exit 0 with a clean teardown; no sanitizer report.

### PoC-3 hardware procedure (the coordinator runs it, the camera stays on the PoC-1 stub)

1. Copy: `scp akam2k_rtsp root@CAM:/tmp/` (RAM, not the card), `chmod 755 /tmp/akam2k_rtsp`. Port 554 and the video HW
   must be free (no `yy_cam`; the stub's one `watchdog` feeder keeps the camera alive). Baseline `free`.
2. Run: `LD_LIBRARY_PATH=/appfs/lib /tmp/akam2k_rtsp --seconds 120 > /tmp/a2k.log 2>&1 &` (add `-a /nonexistent`
   if a stray `akam/rtsp.conf` is on the card). Expected in the log: every bring-up step `= 0`, both
   `ak_venc_open_ex ... = 0`, then `stats` lines every 10 s (about 15.0 fps; about 2000 kbit/s main, about 500 kbit/s sub).
3. From the host while it runs (stream0, then stream1, and both at once):
   `ffprobe -hide_banner -rtsp_transport tcp rtsp://CAM:554/stream0` (expect h264 Main 2304x1296, 15 fps) and
   `.../stream1` (640x360). Grab a frame: `mkdir -p test-images/rot2k && ffmpeg -rtsp_transport tcp -i rtsp://CAM:554/stream0 -frames:v 1 -y test-images/rot2k/$(date +%F)_poc3_stream0.jpg`
   and the same for stream1 (`..._poc3_stream1.jpg`). `test-images/rot2k/` is git-ignored locally: never commit the images.
   Measure fps and bitrate from the packet timestamps:
   `ffprobe -v error -rtsp_transport tcp -select_streams v -show_entries packet=pts_time,size,flags -read_intervals %+30 -of csv=p=0 rtsp://CAM:554/stream0 > s0.csv`
   then fps = packets / (last pts - first pts), kbit/s = sum(size) * 8 / duration / 1000, keyframes = rows with a `K` flag
   (expect one per 2 s); compare with the camera's `stats` lines. Open a 2nd and 3rd viewer and kill one with Ctrl-C: the
   others must keep playing and the log shows `dropping client ... (N frames sent, M skipped)`.
4. On the camera: `top -b -n 1 | head` or the log's `stats: ... CPU x %, VmRSS y kB, VmHWM z kB` (target: VmRSS under 6 MB,
   CPU margin left), `free` before/after, no `yy_cam`, still answers SSH. After the 120 s the program exits by itself:
   expect `summary` lines, `ak_venc_close`/`ak_vi_disable_*`/`ak_sdk_exit` = 0 and `exit 0`. `rm /tmp/akam2k_rtsp /tmp/a2k.log`.
5. Pass: both streams play and decode, about 15 fps, bitrate near the targets, RSS under 6 MB, no reset, clean exit.
   Abort: `TIMEOUT ... in step: X` (exit 124, step X is the finding), a crash line (exit 2), "no frame for 10 s" on a
   channel (the two threads pulling different VI channels concurrently is the main unverified point: if only one channel
   delivers, report which and the log), a reset or kernel oops. Do not loop after a failure: report the log first.
   Optional auth check: put `cam:akam:<md5 of cam:akam:secret>` in `akam/rtsp.conf` and use `rtsp://cam:secret@CAM:554/stream0`.

### PoC-3 hardware result (2026-11-14, camera in PoC-1 mode, Bootlin-built binary)

Confidence: measured (one run, one camera, one dark static scene; not a load test).

- `poc3-symcheck` against the 2K camera's own libuClibc-1.0.31 passes (it exports `__ctype_b`, not `__ctype_b_loc`).
- `ffprobe`: stream0 h264 Main 2304x1296 15 fps, stream1 Baseline 640x360 15 fps over TCP. Measured 14.2 fps and about
  700 kbit/s on the main stream (dark static scene, VBR: well under the 2000 kbit/s target, so this is NOT a bitrate
  validation); the server log shows 14.3 fps on both streams, 0 dropped frames, CPU 5 %, VmRSS 1.48 MB (VmHWM 1.8 MB).
- Several connect/TEARDOWN cycles ran clean; a frame was grabbed from the stream.
- Still unmeasured: a moving or bright scene (I-frame sizes, bitrate near the target), 4 simultaneous clients on the
  camera, slow/dead clients on the camera, Digest on the camera, a long run, `ak_venc_request_idr` effect on a real
  mid-GOP request, UDP (not implemented).

## Auto-start (2026-11-15): the stub starts `akam2k_rtsp` and keeps it up

Status: **offline, host-tested (busybox ash under qemu-arm and bash), not yet run on hardware**. Hardware procedure below.

**Card layout.** `make -C firmware/lsc-rotatable-2k-ak3918av130/app poc3-dist SMOLRTSP=... CAMERA_LIBC=<libuClibc-1.0.31.so of your backup>`
runs `poc3-symcheck`, strips the binary and writes `dist/rot2k-app/BUILDINFO.txt` (`sha256`, `source_sha256` from
`app/source_hash.sh`, `interpreter`, `symcheck=passed`, `camera_libc_sha256`). `build_card.sh <dir> ... --own-app-bin DIR`
(default `$AKAM_OWN_APP_DIST`, else `dist/rot2k-app` when `akam2k_rtsp` is there) installs `akam/bin/akam2k_rtsp` and
`akam/bin/akam2k_rtsp.BUILDINFO` and **refuses** a binary whose BUILDINFO lacks `symcheck=passed`, does not match the
binary, was built from other sources, or that is not a 32-bit ARM ELF requesting `/lib/ld-uClibc.so.0`. Only that file
is copied: no vendor library ever goes on the card (`LD_LIBRARY_PATH=/appfs/lib` at run time). Optional
`akam/akam2k.conf` (never created by the builder), `akam/no_app` flag file to keep the PoC-1 behaviour.

**Supervisor** (`overlay/akam/own_app_run.sh`, a separate process started by the stub after "healthy"; it can only exit
itself, so an app crash loop cannot touch the busybox watchdog feeder or Wi-Fi):
- sha256 of the card file against the BUILDINFO, copy to `/tmp/akam2k_rtsp`, sha256 of the copy again; mismatch, missing
  hash or missing applet = refuse and log (the card busybox 1.36.1 has `sha256sum`, checked under qemu-arm).
- Options from `akam/akam2k.conf`, `key=value`, `#` comments; keys `main_kbps` 64..8000, `sub_kbps` 32..2000, `gop` 1..300,
  `port` 1..65535. Digits only, never evaluated; a bad line or key is logged and ignored (defaults = the app's: 2000 / 800
  kbit/s, GOP 30, port 554).
- stdout/stderr appended to `akam/akam2k.log`, copy-truncated at 256 KiB into `akam2k.log.1` (one generation, 512 KiB
  total), checked every 5 s and at each start. `oom_score_adj` 500 so the OOM killer prefers the app.
- Restart on exit with backoff 2, 4, 8 ... 60 s (a run of 120 s or more resets it). More than 5 exits in 10 minutes:
  logs "giving up" and the supervisor exits; the stub, the feeder, Wi-Fi and SSH carry on. A reboot retries.
- Stop: SIGTERM to the stub stops the supervisor first (app SIGTERM, 10 s grace, SIGKILL; the stub waits at most 20 s
  and then SIGKILLs the supervisor), then the feeder exactly as before. The app never opens `/dev/watchdog`.

**Memory guard.** The frame ring is now 6 frames per stream, frames over 512 KiB (main) or 128 KiB (sub) are dropped with
a keyframe request (existing behaviour). Worst case 6 x (512 + 128) KiB = 3.75 MiB (was 2 x 8 x 1 MiB = 16 MiB against
about 22 MB available), plus the frames being sent. A normal 2304x1296 IDR is 100-250 KB at 2000 kbit/s, so the caps leave
margin; a drop is counted in the `dropped` figure of the stats line. Target RSS stays under 6 MB.

**Hardware procedure** (the coordinator; the camera is on the PoC-1 card, so a bad run is undone by the counter fallback
or by removing the card):
1. Build `poc3-dist`, then over SSH replace on the card `akam/own_app_stub.sh`, `akam/own_app_run.sh`, `akam/bin/akam2k_rtsp`
   and `akam/bin/akam2k_rtsp.BUILDINFO` (or rebuild the card tree with `build_card.sh --own-app-bin`). Check
   `busybox sha256sum akam/bin/akam2k_rtsp` equals the BUILDINFO `sha256`. Make sure no `akam/no_app` and no old `/tmp` app run.
2. Reboot. Expected in `akam/akam_boot.log`: `own_app PoC-1 healthy`, `app supervisor started`, `akam2k_rtsp verified`,
   `akam2k_rtsp started, pid N`; `akam/akam2k.log` shows the bring-up steps `= 0` and `stats` lines.
3. From the host: `ffprobe -rtsp_transport tcp rtsp://CAM:554/stream0` and `/stream1` (2304x1296 Main, 640x360).
4. `kill -9` the app once: expect `exited with status 137`, `restarting akam2k_rtsp in 2 s`, the streams back within
   a few seconds (new pid), SSH and Wi-Fi untouched, the feeder pid unchanged (`ps | grep watchdog`).
5. After 10 minutes with one client: `akam/akam2k.log` stats (CPU about 5 %, VmRSS under 6 MB, VmHWM), `free`, log sizes
   below 256 KiB each. Optional: `akam/akam2k.conf` with `main_kbps=3000` and a reboot, check the `options:` log line.
6. Stop test (optional): `kill -TERM` the stub shows `stopping the app, then the feeder`, app gone before the feeder.
   Pass: all of the above, no reset. If the camera resets, remove the card (stock boots) and read the card log first.

## Auto-start hardware result 2026-11-15 — PASS

Card updated over SSH with the dev scripts + `make poc3-dist` binary (sha256 a8fdefae…, symcheck passed vs the 2K libc),
then reboot: stub healthy at +18 s, supervisor verified the binary and started `akam2k_rtsp`; RTSP 554 answered
~20 s after boot, stream0 Main 2304×1296. `kill -9` of the app → logged "exited with status 137", restarted 2 s later,
RTSP back in ~6 s. RAM after restart: ~22 MB available. The camera is now an RTSP camera running only our code.

## Person detection inside `akam2k_rtsp` (2026-11-15, offline, host-tested, not yet run on hardware)

`akam2k_rtsp` now runs the stock human-detection model next to both RTSP streams (PoC-4 pipeline, [ai-own-app.md](../porting/lsc-rotatable-2k/ai-own-app.md) section 10).
No new listening port, no new socket, no change under `src/` (the shared RTSP sources are still compiled unmodified).

**Files** (all under `firmware/lsc-rotatable-2k-ak3918av130/app/`):
- `detect.c/.h`: the detection thread, the ONLY file with NPU vendor calls. `ak_npu_init`, `ak_svp2_create_chn`
  (stock model `/appfs/resource/model/model_0x0A00000D_V1.00.01.bin`, a fixed constant checked by `a2k_model_path_ok`: under
  that directory, no `..`), then at `detect_hz` the PoC-4 loop `ak_vi_get_frame(16)`, `ak_mem_dma_vaddr2paddr`,
  `ak_svp2_process`, validated results, `ak_svp2_release`, `ak_vi_release_frame` (the frame is released right after the
  call, before any state work and before the pause, so the third channel never holds a buffer the encoders could need).
- `detstate.c/.h`: pure logic, no vendor call. Hysteresis (ON after K=2 consecutive inferences with a class 0
  box, OFF after M=3 s without one; a single empty inference between positives never turns it OFF), the best-box
  collector (class 1 = head only counted and its best score kept as extra info, it never drives the state), and the state
  file writer.
- `cap.c`: VI chn 16 (320x192, 20 fps, depth 2) is `set_chn_attr`'d with chn 0/1 BEFORE `enable_dev` and enabled with
  them, exactly as in PoC-4. A refusal of chn 16 only switches detection off (the streams go on).
- `abi.h`: our transcription of the svp2 structs (sizes `_Static_assert`ed) and prototypes.

**Options** (`akam2k_rtsp`): `-D 0|1` (detect, default 1), `-H 1..10` (Hz, default 5), `-T 500..950` (classify threshold, default 700),
plus long-only `--detect-on-count 1..10` (K, default 2) and `--detect-off-s 1..60` (M, default 3). IoU 3 and target_type 0 are the stock values.
Supervisor keys in `akam/akam2k.conf` (whitelist in `own_app_run.sh`, same digits-only rules): `detect` 0..1, `detect_hz` 1..10,
`detect_threshold` 500..950, passed as `-D`/`-H`/`-T`; out of range = logged and ignored (default stays). The app validates again.
K and M are not in the conf (compile-time defaults, long options for tests).

**Events.** One log line per transition: `detect: person ON score=930 box=x1,y1,x2,y2` and
`detect: person OFF after 12.3s (no person for 3.0s) last score=... box=...` (presence duration, then the absence). The state
file `/tmp/akam2k_detect.state` (key=value, written to `.tmp` then `rename()`d: a reader sees the old or the new file, never a
torn one) is rewritten at every transition and at least every 5 s (`seq` and `updated` move, so a bridge can tell a live writer
from a stale file). Keys: `version`, `status` (`run`, `stopped`, `failed`), `enabled`, `hz`, `threshold`, `seq`, `updated` (epoch
s), `uptime_ms`, `person` (0/1), `score`, `class`, `box` (x1,y1,x2,y2 in the 320x192 image), `heads`, `head_score`,
`last_event` (`on`/`off`/`none`), `last_event_ms`, `last_event_time` (epoch s, 0 = none yet), `inferences`. A later MQTT bridge
only has to read this file.

**Robustness.** NPU init, model load, a chn 16 that yields no frame for 5 s, 3 consecutive inference failures (the stock
`ak_npu_reset_hw` is called after each), or `MemAvailable` under 6 MB (checked every 60 s) end the detection with ONE log
line and `status=failed`; the encoders, the RTSP server and the exit status never depend on it. The detection thread's
stack is 192 KiB (virtual). Teardown: detection thread (svp2 destroy, then NPU deinit, inside the thread) is joined first,
then chn 16 disabled, venc closed, vi closed, `ak_sdk_exit`. The `alarm(20)` teardown guard still bounds a wedged call.
The binary now links `libmpi_svp2`, `libplat_npu` and `libak_nne` (DT_NEEDED, all stock libs in `/appfs/lib`).

**Scan rules** (`tests/rot2k_overlay_test.sh`): still no device, flash, nvram, GPIO, watchdog, ioctl, open, mmap or spawning in
any app source; sockets stay in `rtsp_session.c` only; the new exception is `rename()`/`unlink()` in `detstate.c`
only (the temp-file publish), whose single `fopen` takes the path from the caller. Unit test `tests/rot2k_detect_unit.c`
(hysteresis, collector, state file format and atomic write, model path rule).

**Budget targets** (to be measured): total app CPU under 20 % at 5 Hz (PoC-4 alone: about 5 % at 7.6 Hz), VmRSS under 6 MB, and
the model memory (about 3 MB, outside RSS) read from `MemAvailable` in the `mem after npu init` / `mem after model load` log lines.

### Detection hardware procedure (the coordinator runs it; the user walks in view, ONE STEP AT A TIME)

Rule: after every step the coordinator stops, reports what it saw and waits for the user's feedback before the next one.
The camera is on the PoC-1 card; a bad run is undone by removing the card or by `akam/no_app`.

1. **Build and install, detection OFF.** `make poc3-dist` (SMOLRTSP, CAMERA_LIBC as above), copy `akam/bin/akam2k_rtsp`,
   `.BUILDINFO` and `own_app_run.sh` to the card over SSH, write `akam/akam2k.conf` with `detect=0`, check the sha256, reboot.
   Expect `detection off` in the first app log line, both streams as before, CPU about 5 %. **Pause: report to the user.**
2. **Detection ON, empty room, no client.** Set `detect=1` (or remove the line), restart the app (`kill` the app, the supervisor
   restarts it). Expect in `akam2k.log`: `ak_npu_init() = 0`, `ak_svp2_create_chn(...) = 0`, `mem after npu init`, `mem after model
   load` (note MemAvailable before/after), `detect: running at 5 Hz`, a `stats` line with CPU and VmRSS, and
   `/tmp/akam2k_detect.state` with `status=run person=0` and a moving `seq`. No `person ON`. Let it run 2 minutes.
   **Pause: report CPU, VmRSS, MemAvailable and any false ON; the user does nothing yet.**
3. **One RTSP client on stream0 plus stream1 (ffprobe/ffplay), empty room.** Same checks; frame rates stay 15 fps and the
   stats line keeps CPU under 20 %. **Pause.**
4. **The user walks into view, stands still.** Expect `detect: person ON score=...` within about 0.5 s and
   `person=1` in the state file (box, `heads`). **Pause: the user says whether the box/ON matched what they did.**
5. **The user leaves the view.** Expect `person OFF after ...` about 3 s after the last positive inference. **Pause.**
6. **Flicker and sensitivity.** The user moves in and out of view a few times, partly hidden, then sits still. Count ON/OFF
   events against what the user did. If it is too sensitive or too deaf, change `detect_threshold` (500..950) and restart
   the app, one change per run. **Pause after each run.**
7. **Soak and teardown.** 10 minutes with one client: no stream stall, VmHWM, logs under 256 KiB. Then `kill -TERM` the app
   once: expect the teardown lines in order (`detect: ak_svp2_destroy_chn`, `ak_npu_deinit`, `ak_vi_disable_chn(16)`,
   `ak_venc_close`, `ak_vi_close`, `ak_sdk_exit`) and exit 0; then `kill -9` once and check the app comes back with detection.
   Abort criteria at any step: `MemAvailable` under 6 MB, an encoder frame rate drop, a reset of the camera (remove the card).

## MQTT and Home Assistant inside `akam2k_rtsp` (2026-11-15, offline, host-tested, not yet run on hardware)

Same user experience as the 1080p (`specs/mqtt.md`), driven by the detection thread: the app publishes the camera and the
person state to a broker and announces it to Home Assistant by MQTT discovery. Read-only: it publishes, it never subscribes,
there is no command topic. Off unless the card file `akam/mqtt` exists with `enabled=1` (no thread, no socket, nothing).

**Files.** `app/akam2k_mqttcfg.[ch]` (pure: strict config parser, topics, discovery documents, state and attribute payloads;
fuzzed, golden JSON), `app/akam2k_mqtt.[ch]` (the client thread, the only app file that may `connect()`), a one-line hook in
`detect.c` (`write_state()` hands the same snapshot to `a2k_mqtt_detection()`: no polling of the state file, which is
kept for SSH debugging), `--mqtt-conf FILE` in `main.c` (a path only, default `/mnt/sdcard/akam/mqtt`; the password file is `mqtt_pass` in the same directory).

**Shared code reused unmodified (phase A of PLAN 8ter):** `src/net/mqtt/mqtt.c` (the MQTT 3.1.1 codec: CONNECT with will,
PUBLISH, PINGREQ, DISCONNECT, packet framing and decoding, `mqtt_backoff_ms`) and `mqtt_ha.c` (only `mqtt_ha_topic_ok`, the
topic rule; the rest is dropped by `--gc-sections`). `git diff dev -- src/` is empty. **Not reusable as is:**
`mqtt_ha.c`'s entity table is the 1080p camera's (LED, IR, night mode, picture sliders...) and is a static array inside the
file, and `mqtt_svc.c` is the 1080p service (global state, `control.c` ops, reload through the 8091 port, command handling,
two-file config). **Phase-B candidates** (each a pure move that must leave the 1080p binary byte-identical): (1) make the
entity table of `mqtt_ha.c` a parameter of `mqtt_ha_t` (table pointer plus count) so a model supplies its own entities and
the discovery JSON builder (`jb_*`, device block, availability) is shared, instead of the 2K copy in `mqtt_cfg.c`;
(2) split `mqtt_svc.c` into a transport-agnostic session (connect, announce, keepalive, backoff, stop) with a net ops table
and the 1080p ops, which the 2K client re-implements today with its own `a2k_mqtt_net_t`; (3) the config parser with a
pluggable key set and a shared `mqtt_pass` reader (the 2K has its own copy of both, same rules).

### Configuration: `akam/mqtt` and `akam/mqtt_pass` (on the card, FAT)

Same two files, keys, format and defaults as the 1080p `/etc/config/akam/mqtt` and `mqtt_pass` (so the same layout and, later, the same assistant tooling work on both models). `akam/mqtt`:

```
enabled=1
host=192.0.2.10
port=1883
user=homeassistant
client_id=akam-rot2k
base_topic=akam/rot2k
discovery=on
discovery_prefix=homeassistant
interval=30
```

`enabled` and `host` are required, every other key is optional (`port` 1883, `discovery` on, `discovery_prefix`
`homeassistant`, `interval` 30 s between periodic republishes, `client_id` and `base_topic` default to the host name and
`akam/<hostname>`). One `key=value` per line, LF or CRLF, at most 768 bytes, no blank line, no comment, no repeated key; an
unknown key, a bad value (a lone `-` as `user` included) makes the **whole file invalid** (the app logs "invalid, MQTT stays off"
and nothing from the file). Value rules are those of `specs/mqtt.md` section 2. No TLS (the 1080p has none): the password travels in clear on the LAN, use a broker user with minimal rights.

**The password** is the only content of `akam/mqtt_pass`, exactly the 1080p rules: one line, at most 128 characters, no control character (a CR or tab refuses it), one trailing LF tolerated, absent file = no password, ignored when no `user` is set; an unreadable, non-regular, too long or invalid file keeps MQTT off (log line without the content). The only difference: the 1080p also refuses a file readable by other users (mode 0600); the card is FAT and has no usable mode, so no mode check exists here (anyone with the card or SSH can read it; say so to the user). It is read once at app start, not at each connect (there is no reload: change it, restart the app). The app reads the file itself: the supervisor `own_app_run.sh` does not know it exists (its whitelist is
unchanged, a scan test asserts it never mentions MQTT or a password), `ps` shows at most `--mqtt-conf <path>` (a path, only
when that test option is used), the password is never an argument or an environment variable. It is never logged (the start
line says `password set (not logged)` and does not print the user name), never published, never in a discovery document, and
is wiped from the thread's copy at stop.

**DNS.** `host` is an IPv4 address (nothing is resolved) or a name. A name goes through the camera's resolver
(`getaddrinfo`, IPv4); decloud blackholes the WAN, so the name must be answered by the **LAN** resolver (the router). `.local`
names do not work (no mDNS). A lookup that blocks only blocks the MQTT thread (a stop waits at most about 3 s for it).
Prefer the broker's LAN IP with a DHCP reservation.

### Topics and payloads (identical scheme to the 1080p; model id `lsc-rotatable-2k-ak3918av130`)

`BASE` = `base_topic` (default `akam/<hostname>`, the 2K stub's host name is `akam-rot2k`), `NODE` = `akam_<wlan0 MAC without
colons>` (read from `/sys/class/net/wlan0/address`; without it the session ends with the reason `mac` and is retried with the
backoff), `PREFIX` = `discovery_prefix`.

| Topic | Content |
|---|---|
| `BASE/status` | `online` (retained) while connected, `offline` (retained), also the will (LWT) |
| `PREFIX/<component>/NODE/<entity>/config` | retained discovery document, one per entity (none sent when `discovery=off`) |
| `BASE/person/state` | `ON` / `OFF` (binary_sensor, device_class `occupancy`), not retained |
| `BASE/person/attributes` | JSON `{"score":812,"class":0,"box":[x1,y1,x2,y2],"heads":1,"head_score":640,"detection":"run","last_detection":"2026-11-15T09:46:40+00:00"}` (score 0..1000, box in the 320x192 detection frame; score, class and box are 0 while OFF; `detection` is `run`, `stopped` or `failed`, so a dead detection is visible) |
| `BASE/person_score/state` | best person score in percent, `81.2` (sensor) |
| `BASE/last_detection/state` | ISO 8601 UTC time of the last OFF to ON transition (sensor, device_class `timestamp`); not sent while the clock is unset |
| `BASE/viewers/state` + `BASE/viewers/attributes` | RTSP clients (sensor); attributes `{"stream_main":"rtsp://<ip>:554/stream0","stream_sub":"rtsp://<ip>:554/stream1"}`, `<ip>` is the local address of the MQTT socket, never with credentials |
| `BASE/app_version/state` | the app version (diagnostic sensor); also `sw_version` of the device |

The device block (identifiers `akam_<mac>`, MAC connection, name = host name, manufacturer `Akam`, model
`lsc-rotatable-2k-ak3918av130`, `sw_version`) follows the 1080p's, so Home Assistant merges it with the ONVIF device. A state
is published at every detection transition (ON or OFF, with attributes, score and last detection), at every (re)connect, and
every `interval` seconds. Publishing is QoS 0 (as the 1080p).

### Robustness

- **Own thread** (64 KiB stack, nice 5, below the video threads). The detection thread only copies a snapshot under a mutex
  and writes one byte to a pipe; it never waits for the network. Capture, encoders, RTSP and the NPU never depend on MQTT.
- **Bounded queue**: a ring of 8 transitions. When the broker is slow or gone the oldest transition is dropped; the current
  state is always published at (re)connect, so a long outage loses history, never the truth.
- **Reconnect**: backoff 5 s doubling to 60 s with +-20 % spread (the 1080p's `mqtt_backoff_ms`), restarted from 5 s only after
  a session that held for 60 s (a broker that accepts then drops at once is not hammered). Keepalive 60 s, PINGREQ when idle
  for 30 s, a missing PINGRESP or CONNACK within 10 s ends the session. Reasons (`error:<reason>` in the log): `dns`,
  `connect`, `timeout`, `closed`, `protocol`, `mac`, `config`, `refused_*` (CONNACK codes).
- **Hostile or garbage broker bytes** end the session with `protocol`; nothing a broker sends is ever obeyed (no subscription;
  a stray PUBLISH is acknowledged if QoS 1 and dropped). Packets over 251 bytes are refused.
- **Crash scope.** MQTT code runs inside the app: a bug there takes the whole app (the supervisor restarts it). That is why
  the module is small, has no allocation, and is fuzzed: the config parser (300 000 random and mutated files) and the broker
  byte stream (300 sessions of random bytes) run under AddressSanitizer and UBSan in `tests/rot2k_mqtt_unit.c`.
- **Cost** (build time): the stripped app grows from 71 KB to 96 KB. Runtime cost on the camera is not measured yet.
- **Anti-brick scan** (`tests/rot2k_overlay_test.sh`): `socket` is allowed in `rtsp_session.c` and `mqtt_bind.c` only,
  `connect()` in `mqtt_bind.c` only (which may not bind, listen or accept), `bind/listen/accept` in `rtsp_session.c` only,
  datagram calls nowhere, `fopen` with a variable path only; positive and negative controls for each.

### Tests (host, no broker, no camera)

`tests/rot2k_mqtt_unit.c` (built with ASan and UBSan by `rot2k_overlay_test.sh`): config parsing (valid, defaults, 26 invalid
files, limits, CRLF, secrets never printed by `describe()` nor present in any captured log line), golden discovery documents,
state and attribute payloads, the fuzzers above, and the client against a scripted stub socket with a fake clock: dial
failures and the 5/10/20 s backoff, CONNECT carrying the will and the login, availability and discovery order and retain
flags, transitions published, no extra publish for a mere score change, periodic republish, keepalive, a missing PINGRESP,
CONNACK refusals, loss of the connection with and without a stable session, offline + DISCONNECT at stop, a stuck send with
101 reports in under 200 ms and the bounded ring, 300 sessions against garbage. `tests/rot2k_mqtt_sink_test.py` tests the
host tool below on loopback.

### Host tool: `tools/mqtt_sink.py`

A stand-alone MQTT 3.1.1 broker **sink** (Python standard library only, no mosquitto needed): `python3 mqtt_sink.py --bind
<PC LAN address> --port 1883`. It accepts any CONNECT (CONNACK 0), SUBSCRIBE (SUBACK, nothing is ever delivered), PINGREQ,
PUBLISH (QoS 0 printed; QoS 1 printed and acknowledged; QoS 2 refused by closing) and prints one line per event, including
the will of a client that drops without DISCONNECT. It never forwards, stores or retains anything and never prints a password
(`password: set`). It is a test instrument: bind it to a LAN address for the test only.

### MQTT hardware procedure (the coordinator runs it; ONE STEP AT A TIME, the user walks in view at steps 4 and 5)

Rule: after every step the coordinator stops, reports what it saw and waits for the user's feedback before the next one.
Prerequisite: the detection hardware procedure above passed (the app with detection runs from the card, SSH works).

1. **Build and install, MQTT OFF.** `make poc3-dist` (SMOLRTSP, CAMERA_LIBC as above), copy the binary and `.BUILDINFO` to the
   card, check the sha256 and reboot. There is no `akam/mqtt` yet: expect `mqtt: no /mnt/sdcard/akam/mqtt, MQTT off`
   in `akam2k.log`, streams and detection as before, no new thread (`ls /proc/<pid>/task | wc -l` unchanged). **Pause: report.**
2. **Start the sink on the PC.** `python3 firmware/lsc-rotatable-2k-ak3918av130/tools/mqtt_sink.py --bind <PC LAN IP> --port 1883`
   in a terminal that stays open; check from the PC that it is listening. Nothing on the camera yet. **Pause.**
3. **Put a test `mqtt.conf` on the card and restart the app.** Over SSH write `akam/mqtt` with `enabled=1`,
   `host=<PC LAN IP>`, `port=1883`, `user=test`, and `akam/mqtt_pass` holding `testpass` (a throw-away password), `base_topic=akam/rot2k`,
   `interval=30`, then `kill` the app (the supervisor restarts it). Expect in the log `mqtt: enabled, host ... password set (not
   logged)` then `mqtt: connected` (no `testpass`, no user name in the log); in the sink: `CONNECT ... will: akam/rot2k/status`,
   `PUBLISH akam/rot2k/status retain online`, five `homeassistant/.../config` documents, the viewers attributes with the
   camera's LAN address, the version and the first `person/state OFF`. `ps` must show no secret.
   **Pause: show the user the sink output.**
4. **The user walks into view and stands still.** Expect within about 0.5 s (2 confirming inferences): `PUBLISH
   akam/rot2k/person/state ON`, `person/attributes` with score, box and heads, `person_score/state`, `last_detection/state`
   (if the clock is set). **Pause: the user says whether it matched.**
5. **The user leaves the view.** Expect `person/state OFF` about 3 s after the last positive inference. Repeat in/out a few
   times: one ON and one OFF per real transition, no flicker flood. **Pause.**
6. **Broker loss and recovery.** Stop the sink (Ctrl-C): the log shows `error:closed` then retries at about 5, 10, 20 s
   (`error:connect`); the streams and detection keep running (check `ffprobe` fps and a `stats` line). Restart the sink: the
   camera reconnects within one backoff and republishes online, discovery and the current person state. **Pause.**
7. **Will and stop.** `kill -9` the app: the sink prints the will (`status offline`, tagged "will") when the TCP connection
   drops (the kernel closes the socket). After the supervisor restarts it, `kill -TERM` once: expect `offline` then
   `DISCONNECT` and no will. Then remove `akam/mqtt` and `akam/mqtt_pass`, restart, expect MQTT off
   again. **Pause: report.**
8. **Real Home Assistant (optional, later).** Point `host` at the user's broker only when the user asks: discovery shows the
   device `akam-rot2k` with the person occupancy sensor, merged with the ONVIF device.

Abort criteria: a stream fps drop or a `stats` CPU rise while MQTT reconnects, the app exiting (a crash line), a password
anywhere in the log, `MemAvailable` under 6 MB. A bad run is undone by deleting `akam/mqtt` (or `akam/no_app`).

### MQTT hardware result 2026-11-15 (steps 1–3, 7 of the procedure; person ON/OFF steps pending the user)

The PC's host firewall blocks inbound connections and our Dropbear build has TCP forwarding compiled out (by
design), so the test used a stdio bridge over SSH (camera-local `busybox nc -l -p 18830` ↔ PC sink on 127.0.0.1) —
no firewall or Dropbear change. Result: CONNECT (keepalive 60, user + password set, will on `akam/rot2k/status`), retained
`online`, 5 retained HA discovery configs (`akam_<mac>_{person,person_score,last_detection,viewers,app_version}`),
initial states (person OFF, score 0.0, viewers 0, version 0.1.0); SIGTERM → retained `offline` then DISCONNECT. The
throw-away password never appeared in the sink output or the app log. Notes: the client id defaulted to the host name
(`YYOS`, the stock hostname; fixed afterwards: the default is `akam-rot2k`, see "Detection zones and JPEG snapshot"), and the stream URLs use the local socket IP (127.0.0.1 through the
bridge; the LAN IP with a real broker). Test config removed from the card afterwards (MQTT off again).

## Detection zones and JPEG snapshot inside `akam2k_rtsp` (2026-11-15, offline, host-tested, not yet run on hardware)

Two optional additions on top of the detection and the MQTT bridge. Nothing changes without them: no `akam/zones` file means the whole image
(exactly the earlier behaviour), `snapshot=0` means no third encoder, no thread and no extra MQTT entity. `git diff dev -- src/` is empty (the 1080p tree is
untouched). The vendor ABI gains no new symbol: the JPEG encoder is one more call of `ak_venc_open_ex` / `ak_venc_encode_frame`.

### Detection zones: `akam/zones`

(Extended later: up to 8 zones, `ignore_<name>` rectangles and run-time editing from Home Assistant: see `button-and-zones.md`.)

**Why zones.** A person walking on the pavement behind the fence is not a visitor at the door. Up to 8 named rectangles restrict what counts; each zone gets
its own Home Assistant binary sensor, the global `person` stays "a person in any zone" (or "in every zone", `zone_mode=all`).

**File** `/mnt/sdcard/akam/zones` (`--zones-conf FILE` in tests), strict like `akam/mqtt`: `key=value` lines, LF (a trailing CR tolerated), at most 1024
bytes, no blank line, no comment, no space, at most one line per key. An empty file is valid (no zone). Parser: `app/zones.c` (pure).

```
porch=0,40,50,100
drive=50,40,100,100
zone_mode=any
```

- `<name>=x1,y1,x2,y2`: percent of the image (the 320x192 detection frame), integers 0..100, origin top left, `x1 < x2` and `y1 < y2`. Name: a lower case
  letter then up to 15 of `a-z 0-9 _` (it becomes the entity `person_<name>`); at most 8 zones, names unique; `score` (would collide with
  `person_score`) and `zone_mode` are reserved. A zone is enabled by being in the file: remove its line to disable it.
- `zone_mode=any|all` (default `any`): `any`: the global person is ON while a person stands in any zone. `all`: only while every zone holds a person at the
  same time (several people, or one person where zones overlap). The per-zone sensors do not depend on the mode.
- **An invalid file never silences the detection:** any other line, a bad value, a duplicate, a ninth zone, a NUL byte, a FIFO or a symbolic link in place of the file, more than 1024 bytes make
  the whole file invalid; the app logs `zones: ... is unreadable or invalid (nothing from it is used), the whole image counts` once and detects on the whole image.
  The content of a bad file is never logged.

**Membership rule: the feet point.** A person counts in a zone when the **bottom centre of its box** lies in the zone (edges included; the box corners are pixel
indexes, so the point is `((x1 + x2 + 1) / 2, y2 + 1)`). Chosen over a minimum overlap because (1) it is where the person stands: an arm, a head or the box of
a person walking just beside the zone edge cannot flip the zone the way a 20 % overlap can; (2) it needs no tuned threshold; (3) a person cut by the
bottom of the image still gets the right answer (the box is clamped to the image, so the bottom edge is the image edge, 100 %). The cost: a person whose
feet are hidden behind a table is placed where the box ends; a zone meant for such a spot must reach up to the table edge. A box outside every zone
does not count at all (the detection result is otherwise untouched: threshold, class 0 = person, heads stay extra information).

**Hysteresis.** The global machine is unchanged (K = 2 consecutive positive inferences for ON, M = 3 s without one for OFF). Each zone has its own
machine with the same K and M, fed by the best person box whose feet are in that zone in the same inference (`a2k_zdet_*` in `detstate.c`). A person
walking from the porch to the drive therefore turns `person_drive` ON after 2 inferences while `person_porch` holds for 3 s, then goes OFF.

**Outputs.** One log line per zone transition (`detect: zone porch person ON/OFF ...`); the state file gains `zones=N`, `zone_mode=any|all` and one
`zone_<name>=0|1` line per zone, only when zones are configured (without zones the file is byte for byte the old one). MQTT (below): one binary_sensor
per zone.

### MQTT additions

- **Zones:** discovery `homeassistant/binary_sensor/<node>/person_<zone>/config` (retained, `device_class` occupancy, name `Person <zone>`, same availability and
  device block, no attributes, no command topic), state `BASE/person_<zone>/state` `ON`/`OFF` (not retained), published with every transition and every periodic
  republish. A change of one zone alone (the global person unchanged) is a transition and is queued like any other (same 8 entry bounded ring).
  A zone removed from the file leaves its retained discovery document in the broker: delete the entity in Home Assistant or clear that topic.
- **Snapshot:** discovery `homeassistant/image/<node>/snapshot/config` (retained): `{"name":"Person snapshot","unique_id":"<node>_snapshot",
  "availability_topic":"BASE/status","image_topic":"BASE/snapshot","content_type":"image/jpeg","device":{...}}`; the JPEG itself is published **raw
  (not Base64), not retained** on `BASE/snapshot` (HA MQTT `image` platform with `image_topic`). A snapshot over 200 KB, an empty one, or one taken while
  the broker is away is skipped and counted (`a2k_mqtt_snap_stats`); only the newest unsent snapshot is kept (a slow broker never builds a queue).
  With `snapshot=0` (or when the encoder could not be opened) the image entity is not announced, and an empty retained config is published once on its topic
  so that an entity announced by an earlier run disappears. The packet is built in place (one copy of the JPEG, one send).
- **Client id:** the default is now `akam-rot2k` (the stock host name `YYOS` was found to be the default in the hardware run); `client_id=` in `akam/mqtt` still overrides it.
  The `base_topic` default is unchanged (`akam/<hostname>`).

### JPEG snapshot on detection

**What.** When the global person goes ON, **one** JPEG of the sub channel (640x360) is taken, kept in `/tmp` and published. Rate limited (default one per 10 s).

**How the JPEG is made.** The vendor's own JPEG encoder (evidence in [stock-firmware-reference.md](../porting/lsc-rotatable-2k/stock-firmware-reference.md), "JPEG snapshot encoder": the stock
snapshot is `ak_venc_encode_frame` on a VI frame with an encoder opened by the same `ak_venc_open_ex` with `enc_type` 1 and profile 7 `Jpeg`): the app opens a third
encoder handle next to the two H.264 ones (640x360, JPEG quality level 1 = the stock `snap_jpeg_quality` default, QP 1..100, `venc_param` of `cap.c`,
`jpeg_defaults()`). The vendor lib's JPEG quality is a level 0..3, not the percentage the first spec guessed (about 70): the stock default 1 is used. Not software JPEG:
no libjpeg is linked, nothing new in the binary except about 600 lines of C.

**Why on the sub capture thread, and how capture is protected.** A JPEG needs a VI frame. A second reader of VI chn 1 would steal frames from the sub stream (the
queue is shared), so the snapshot is encoded **from the frame the sub capture thread already holds**, once per request: the thread polls `a2k_snap_pending()` (a volatile
read) once per frame; only when the detection thread has asked it does one extra `ak_venc_encode_frame` on the same frame before releasing it (a few ms of a 66 ms
frame period, at most once per interval) and `a2k_snap_submit()` copies the bytes (at most 200 KB) and returns. Everything slow (the file writes, the MQTT hand-over) is in
**one dedicated thread** (`snap.c`, 64 KiB stack). The detection thread only calls `a2k_snap_request()` (a mutex and two integers). No extra VI channel, no extra
frame queue, no change to the RTSP path.

**Bounds.** One request at a time (a request the capture thread never served is asked again after 2 s); one JPEG in flight (a `malloc` of its own size, at most 200 KB,
freed after use; a second one while the writer is busy is dropped and counted); the MQTT module holds one more copy of the newest (at most 200 KB); `/tmp/akam2k_snap_<slot>.jpg`,
slot 0..2: **3 files at most, 600 KB at most** (`a2k_snapring_*`, pure and tested; a fixed set of names, so a restart overwrites instead of piling up); no snapshot when
`MemAvailable` is under 4 MB. Peak extra RAM about 400 KB for a moment, nothing permanent.

**Failure behaviour (all logged once, none touches the streams).** The JPEG encoder cannot be opened: `ak_venc_open_ex(JPEG ...) = <rc>`
then `snapshot: disabled (ak_venc_open_ex(JPEG) failed, rc 0x..., invalid parameter | not a parameter error)` (the first hardware run: init QP 0 refused, the lib wants 10..100; fixed to 50); the app goes on without snapshots and without the image entity. Three failed
encodes in a row: `snapshot: disabled (...)`. An oversized or empty JPEG: dropped and counted. No detection (`detect=0`): no third encoder at all.

**Keys** (`akam/akam2k.conf`, whitelisted and range checked by the supervisor like the others; the app validates again): `snapshot` 0..1 (default 1), `snapshot_interval` 5..300
(seconds between two snapshots, default 10); passed as `--snapshot` / `--snapshot-interval`. The zones are deliberately **not** a supervisor key: the app reads `akam/zones` itself
(a strict multi-line file does not fit the supervisor's "number only" rule). The files on the card stay readable only by their purpose: the app opens `akam/zones` read-only and
writes only under `/tmp`.

**Scan rules** (`tests/rot2k_overlay_test.sh`, unchanged strictness): no new socket (the image goes through the existing MQTT client), no device node, flash, nvram, GPIO,
watchdog, ioctl, open, mmap, spawn; `zones.c` has exactly one read-only `fopen` with a variable path (after an `lstat`), `snap.c` exactly one write-only `fopen` with a
variable path under the fixed `/tmp`; `rename`/`unlink` are now allowed in two files, the state writer and the snapshot ring (a positive control checks both, a negative control
flags them anywhere else); the third `ak_venc_open_ex` call site, profile 7 and `enc_type` 1 are asserted in `cap.c`.

### Tests (host, no camera, no broker)

`tests/rot2k_zones_unit.c` (under ASan/UBSan: parser accepted/refused cases, a 300 000 input fuzz with every accepted result re-validated, the file reader incl. FIFO,
symbolic link, directory and size cap, the feet point incl. the shared edge and a box clamped at the bottom, `zone_mode` any/all, per-zone hysteresis walking from one zone
to another, the state file lines); `tests/rot2k_snap_unit.c` (stub encoder: ring bounds over 5000 random sizes, rate limit, one request at a time, empty/oversized/unrequested
JPEGs, 3 failures disable, memory guard, 12 snapshots leave exactly 3 files and no temp file, `submit()` returns at once while the publisher is slow);
`tests/rot2k_mqtt_unit.c` (golden JSON of the zone and image discovery documents, zone states, retained flags, the raw snapshot publish through the stub socket, the 200 KB cap,
default client id `akam-rot2k`); `tests/rot2k_mqtt_sink_test.py` (the host sink summarises a binary payload and `--save-dir` keeps the JPEG, at most 20 files).

### Zones and snapshot hardware procedure (the coordinator runs it; the user stands in the room, ONE STEP AT A TIME)

Rule: after every step the coordinator stops, reports what it saw and waits for the user's feedback. Prerequisite: the detection and MQTT procedures above passed.
The camera is on the PoC-1 card; a bad run is undone by `akam/no_app`, by deleting `akam/zones`, or by `snapshot=0`.

1. **Build and install with both features OFF.** `make poc3-dist`, copy the binary, `.BUILDINFO` and `own_app_run.sh`, write `akam/akam2k.conf` with `snapshot=0`, no `akam/zones`,
   check the sha256, reboot. Expect `zones: no /mnt/sdcard/akam/zones, the whole image counts`, no `ak_venc_open_ex(JPEG`, `detect: no zone, the whole image counts`;
   streams, CPU and detection as before. **Pause: report.**
2. **Open the JPEG encoder only.** Set `snapshot=1` (no `akam/mqtt` yet), restart the app. Expect `ak_venc_open_ex(JPEG 640x360, qlevel 1) = 0, handle N` and
   `MemAvailable` in the next `mem` line; both streams at 15 fps, CPU unchanged within a few percent. If the call returns non-zero the app logs `snapshot: disabled (...)`
   and carries on: **stop here and report the return code** (that is the evidence the open could not be done next to the H.264 encoders; the next experiments are in the
   Fallbacks below). **Pause.**
3. **First snapshot (the user walks into view and stands still).** Expect `person ON`, then `snapshot: N bytes stored (slot 0) and handed to MQTT` within about 0.2 s; `ls -l /tmp/akam2k_snap_*`
   over SSH; copy the file to the PC (`cat` over SSH, no card writes) and open it. Report size (target under 200 KB), whether it is a valid 640x360 JPEG, its orientation and
   sharpness, the `stats` line (fps and CPU during the encode). **Pause: the user judges the picture.**
4. **Rate limit and ring.** The user walks out and in several times within 10 s, then for a minute. Expect at most one `snapshot:` line per `snapshot_interval` seconds, never more than 3 files
   in `/tmp`, no `.tmp` file, no frame-rate drop. Try `snapshot_interval=5` and `300` (one change per run). **Pause.**
5. **Zones on.** The user says where the two areas are (for example left and right half of the image, lower 60 %). Write `akam/zones` (`left=0,40,50,100`, `right=50,40,100,100`),
   restart. Expect `zones: 2 zone(s) from ... zone_mode any`, `detect: zone left = 0,40,50,100 %`, and `zone_left=0`, `zone_right=0` in `/tmp/akam2k_detect.state`. **Pause.**
6. **Walk the zones, one move at a time.** (a) The user stands in the left area: expect `detect: zone left person ON`, `zone_left=1`, global `person ON` (and a snapshot). (b) The user moves to the
   right area: `zone right ON` first (2 inferences), `zone left OFF` about 3 s after leaving it. (c) The user stands in the top part of the image or outside both areas (feet above the zones): no zone,
   no global person, even though the body is detected (check with `--detect-debug` if needed). (d) The user leaves the view: all OFF. Report each line against what the user did. **Pause after each.**
7. **`zone_mode=all`** (needs two people, or zones that overlap): add `zone_mode=all`, restart; one person in one zone must not turn the global person ON, two people (one per zone) must. Remove the line afterwards.
   **Pause.**
8. **An invalid file.** Put a typo in `akam/zones` (for example `left=0,40,50`), restart. Expect `zones: ... is unreadable or invalid (nothing from it is used)` and detection on the whole image; no content of the file in the log.
   Restore the file. **Pause.**
9. **MQTT with zones and the image (the sink on the PC).** `python3 firmware/lsc-rotatable-2k-ak3918av130/tools/mqtt_sink.py --bind <PC LAN IP> --port 1883 --save-dir /tmp/rot2k_snap_sink`, `akam/mqtt` as in the MQTT
   procedure (a throw-away password), restart. Expect the retained discovery documents `binary_sensor/.../person_left/config`, `person_right`, `image/.../snapshot/config` and an app client id `akam-rot2k` in
   `CONNECT`; the user steps into the left area: `person_left/state ON`, `person/state ON` and `PUBLISH <base>/snapshot <N bytes binary, jpeg, sha256 ...>`, the saved JPEG opens. With `snapshot=0`: only the empty
   retained config on the image topic. **Pause: report.**
10. **Soak and teardown.** 10 minutes with one client and a few walks: VmHWM, `MemAvailable`, no stream stall, log under 256 KiB. `kill -TERM` the app once: expect the teardown lines in order (`detect: ak_svp2_destroy_chn`,
    `ak_npu_deinit`, `ak_vi_disable_chn(16)`, `ak_venc_close(N) = 0 (JPEG)`, the two H.264 closes, `ak_vi_close`, `ak_sdk_exit`) and exit 0. Remove the test `akam/mqtt`, `akam/mqtt_pass` and `akam/zones` afterwards.

Abort criteria: an encoder or stream frame-rate drop that follows a snapshot, a `stats` CPU rise, `MemAvailable` under 6 MB, an app exit (a crash line), a reset of the camera (remove the card).

**Fallbacks if the JPEG open is refused (not implemented, only if step 2 fails):** (a) the same call with `rc_mode` or the bit-rate fields changed (the lib may validate them for JPEG too, the log
has the return code only); (b) open the JPEG encoder **before** the second H.264 encoder (the handle table or the DMA pool may depend on order); (c) a 2304x1296 JPEG of
VI chn 0 is not an option (it would exceed the 200 KB cap and the encoder memory); (d) a software JPEG of the 320x192 NV12 detection frame (no new library, about 150 lines, lower quality) in the same module.


---

## Day/night and image (IR-cut, IR light, effects)

Branch `feature/rot2k-daynight`. Facts and evidence (addresses, struct fields, pin map) are in [stock-firmware-reference.md](../porting/lsc-rotatable-2k/stock-firmware-reference.md)
("Day/night and image"). Everything below is **offline RE and host-tested code; nothing ran on the camera yet** (UNVERIFIED until the procedure at the end passes).
All of it is RAM state (GPIO levels, ISP registers): a reboot undoes it, no flash, no nvram, no card write.

### What the stock does, in one paragraph

No light sensor. A thread asks the vendor library once per second "given the ISP mode, which mode should it be in?" (`ak_vpss_get_soft_ps_level`); the library
computes the luminance from the AE, applies its frame counts and its non-infrared check (AWB), after the stock registered its thresholds once
(`ak_vpss_set_soft_ps_param`). On a change the stock lights the IR LED (gpio 48), switches the ISP set (`ak_vi_switch_mode`), pulses the IR-cut coil (gpio 24 / 25, 300 ms,
then both low) and requests a keyframe; the reverse for night to day. 60 s lock.

### What our app does

| Piece | File | Notes |
|---|---|---|
| Pins | `pin.c/.h` | sysfs (`/sys/class/gpio`, the mechanism the boot stub uses for gpio 10). A whitelist of exactly **24, 25, 48**: any other number is refused (unit-tested with every other pin of the stock map). Output only, never unexported. `a2k_pin_emergency_low()` (pwrite only, async-signal-safe) drives the coil pins low from the crash and timeout handlers. |
| IR-cut sequence | `ircut.c/.h` | day: pin 25 low, pin 24 high, 300 ms, pin 24 low; night: pin 24 low, pin 25 high, 300 ms, pin 25 low. The two coil pins are never high together and both are low afterwards; a failed write drives both low and returns an error. Pin access and sleep go through `a2k_ircut_ops_t` so the host test records the sequence and its timing. |
| Image | `image.c/.h` | `ak_vpss_effect_set` once after the streams are up: the mains frequency (50 Hz default, like the stock, which applies it at every start) and, **only when configured**, brightness / contrast / sharpness (0..100, applied as value - 50). The stock default is "not set" (the conf file's ISP values stay), so ours is too. No flip, no mirror (orientation base not read), no AWB call (the stock has none). |
| Vendor calls | `cap.c` (`a2k_isp_*`, declared in `isp.h`) | `ak_vpss_effect_set`, `ak_vpss_set_soft_ps_param` (the stock's 92-byte struct, `struct vpss_dn_param` in `abi.h`, with the stock values), `ak_vpss_get_soft_ps_level`, `ak_vi_switch_mode`, then a keyframe request on both streams. All four are exported by the camera's own libs (link checked). |
| Engine | `night_ctl.c/.h` | Pure code behind an ops table (host-tested). The vendor decision is the **measurement**; the **shared 1080p policy `src/features/night/night_logic.c`, compiled UNCHANGED**, decides when to act on it. |
| Thread | `night_run.c` | One 64 KiB thread, a 100 ms tick, the vendor decision read every 10th tick (1 s, the stock cadence). No thread for the forced modes. |
| MQTT | `akam2k_mqtt*.c` | One extra `binary_sensor` **`night`** ("Night mode", ON = night) with the attributes `mode`, `ir_led`, `automatic` (`ok` / `gave_up`), `switches`, `last_switch`, `reason`; published at every switch (bounded ring) and at every periodic republish. Read-only, like the other entities. |

### Reuse of the 1080p policy (PLAN 8ter): reuse, plus a thin adapter, no phase-B move needed

`night_logic.c` is hardware independent (a pure state machine fed one sample per 100 ms poll) and compiles cleanly with `-Wall -Wextra -Werror` for ARM. It is listed in the Makefile
(`SHARED_AK`, with `cfg_file.c` which it links for its saved-mode helper) and in `source_hash.sh`, exactly like the RTSP and MQTT files: **not copied, not edited, `git diff dev -- src/` is empty.**
Its input is the 1080p AE state (exposure with a ceiling of 16384, sensor and ISP gain, the AE's gain limits). The 2K has no such numbers in hand (the AE run-info layout of
`ak_vpss_get_ae_run_info` is not read), but it has something better: the vendor's own verdict. The adapter therefore turns the verdict into a sample of the policy's own vocabulary:
"night wanted" = a saturated sample (exposure at its ceiling, gain at its maximum), "day wanted" = an idle one, and runs the policy with `ir_active = 1` while the IR LED is in use (`ir_led = auto`), so that it uses its **IR-off probe**
(the first version ran it with `ir_active = 0`, assuming the library ignores the camera's own IR light: the hardware result of 2026-11-15 below disproved that). With `ir_led = off` there is no IR light of
our own, `ir_active = 0` and the verdict is read directly. What the policy then adds on top of the library:

- 30 consecutive "night" samples (3 s) before day to night; night to day only through an IR-off probe (see "IR self-illumination" below), or, with `ir_led = off`, 150 consecutive "day" samples (15 s);
- an anti-flap lock of 60 s after every switch (the stock `lock_time`), and **no switch in the first minute** after the start (the start counts as a switch: `lock_until` is set after `nl_reset`);
- a minimum night dwell of 120 s, doubled (up to x8) when the camera falls back to night within 10 minutes of a return to day;
- "no sample" (the library says "AE not stable") resets the confirmation.

A hard guard in the engine refuses any automatic switch under 60 s after the previous one (unreachable while the policy lock works; it resyncs the policy if it ever fires). After 3 failed
switches in a row the automatic mode switches itself off, in the day position (logged, MQTT `automatic: gave_up`).

**Tick cadence (measured on the camera, fix verified on the camera 2026-11-15).** The policy counts time in ticks and assumes one `nl_step` per 100 ms. The first version of the thread called the vendor read (`ak_vpss_get_soft_ps_level`: it blocks 1 to 1.5 s, waiting for exposure stability and reading about 10 frames) inside the tick, so the real tick was about 2.2 times longer than 100 ms and every duration (probe interval, settle, grace, confirmations, lock) was stretched by the same factor: the log said "next in 600 s", the probe came after about 1335 s. Fix: the vendor read moved to its own reader thread that publishes the latest verdict with a sequence number and a timestamp (mutex); the tick thread runs on absolute 100 ms monotonic deadlines and consumes each verdict at most once (the probe grace is checked against the reading's own timestamp). ISP, IR-cut and IR LED actions stay on the tick thread and bump a generation counter, so a reading that began before such a change is discarded. Host test: a 1.3 s fake read and a check that the probe comes at the logged interval in wall time. The review added a vendor mutex: `dn_level` (reader) and `isp_mode` / `dn_init` (tick) never overlap, as in the stock app where one thread does both. Hardware (2026-11-15, dark room, build 0b418d35): night after 67 s, probes then lamp-triggered probes at exactly the logged gaps (120 s, then 240 s; before the fix a logged 16 min gap took about 35 min), no false day, no restart, CPU 16 %. Steady state in the dark: one 6 s IR-off probe every 16 min (lamp gap cap), because the vendor verdict turns to "day" under the camera's own IR and re-arms the lamp detector.

Phase B (not needed now): feed the policy real AE numbers from `ak_vpss_get_ae_run_info` once their layout is read, to use its probe logic; or extract `night_logic` and the HAL interface (`hal_light`) to
a shared directory as a pure move.

### Switch sequence and end state (as the stock, `night_ctl.c`)

- **night:** IR LED on (unless `ir_led=off`), 100 ms, `ak_vi_switch_mode(0, 1)`, keyframe requests, IR-cut pulse to night.
- **day:** IR-cut pulse to day, IR LED off, 100 ms, `ak_vi_switch_mode(0, 0)`, keyframe requests.
- **start:** pins low, IR-cut pulse to **day** (the filter's position after a crash is unknown; the stock does the same at init), IR LED off, ISP day. With `night=night` the night sequence instead.
- **stop (SIGTERM or end of a test run):** before the capture teardown, the filter is moved to the **day position**, the coil released, the IR LED off, the ISP back to day mode. The pins keep that level after the process exits.
- **crash or timeout** (SIGSEGV, SIGALRM guard): both coil pins are written low from the signal handler. A `kill -9` cannot run anything: the worst case is a coil held for at most the rest of one 300 ms pulse
  when the kill lands inside it (the pulse is a single `usleep` in one thread). The IR LED stays on until the next start (which switches it off) or a reboot.

### Config (`akam/akam2k.conf`, whitelisted by the supervisor)

| Key | Values | Default | Passed as |
|---|---|---|---|
| `night` | `auto`, `day`, `night` (exact words) | `auto` | `--night` |
| `ir_led` | `auto` (IR LED lights in night mode), `off` (never) | `auto` | `--ir-led` |
| `image_brightness`, `image_contrast`, `image_sharpness` | 0..100 | not set (ISP conf file values) | `--image-brightness` ... |
| `mains_hz` | 50 or 60 | 50 | `--mains-hz` |
| `wdr` | 0 or 1 | 1 | `--wdr` (WDR re-applied after every ISP mode switch, as the stock) |
| `audio` | 0 or 1 | 0 | `--audio` (microphone track on both RTSP streams, L16/8000; off = the device is never opened; see audio-findings.md section 9) |

`night` and `ir_led` are the only keys of the supervisor that take a **word**: exact match against the allowed ones, never evaluated; anything else is logged and ignored (the default stays).
There is no forced-on IR LED and no run-time change: a change needs an app restart (`akam/no_app` or a reboot).

### Tests (host)

`tests/rot2k_night_unit.c` (ASan/UBSan): the pins on a temporary directory laid out like `/sys/class/gpio` (24 / 25 / 48 work; 0, 9, 10, 22, 23, 26, 47, 49, 50, 55, 56, 71, 100, -1 are refused and leave **no trace**: pin 10 stays
untouched, no directory is created, nothing is exported; the export of an unexported pin; the emergency release leaves the LED alone); the IR-cut sequence on a fake clock (event order, exactly 300 ms energised, never both high,
both low afterwards, a failing raise and a failing release end with both low); the engine on simulated vendor traces (steady dark, steady bright, dusk then dawn, 2 s flapping for 30 minutes = no switch, 10 s flapping for an hour = at most one move
per 60 s and the coil never left on, "not stable" errors for 15 minutes = nothing, errors every 20 s = night still comes, vendor parameters refused, pins not openable, forced day / night, `ir_led=off`, failing filter pulse retried, three failures
= automatic off, stop from night = day position, coil released, LED off); the word parsers; the image mapping and the calls made. `tests/rot2k_mqtt_unit.c`: golden JSON of the `night` discovery document, its state and attributes
(escaping, no time when the clock is unknown), the client test (published at once, not retained, unaffected by detection reports, nothing published when off). `tests/rot2k_overlay_test.sh`: the supervisor keys
(valid, out of range, injection attempts), the **anti-brick scan** extended with exactly one exception (`pin.c` may name the GPIO subsystem and `open()` its value files; with positive and negative controls: the same text in
any other file, `/dev/mem`, `system`, `mmap`, `creat` in the pin file are all flagged), checks that only the three pin numbers exist in the pin sources, that the only absolute path there is `/sys/class/gpio`, that pins are outputs only and never
unexported, that only `ircut.c` drives the pins, that no `ak_` call lives outside `cap.c`, that the crash handlers release the coil and that main stops the engine before the capture.

### Day/night and image hardware procedure (the coordinator runs most of it alone, at night or with the lens covered; user steps are marked)

Rule: after every step stop, report what was seen, and wait for feedback when a user step is marked. Prerequisite: the person-detection and MQTT procedures passed. Undo any step with `akam/no_app` or by removing the keys.

1. **Build, install, defaults.** `make poc3-dist`, copy the binary, `.BUILDINFO` and `own_app_run.sh`, no day/night key in `akam/akam2k.conf` (or none of the new ones), check the sha256, reboot. Expect in `akam/akam2k.log`:
   `ak_vpss_effect_set(0, 7, 50) = 0`, `image: ... mains 50 Hz, 0 failure(s)`, `day/night: automatic, start in day (filter in)`, `ak_vi_switch_mode(0, 0) = 0`, `ak_vpss_set_soft_ps_param(0) = 0`. Streams at 15 fps, CPU as before.
   Over SSH `cat /sys/class/gpio/gpio24/value /sys/class/gpio/gpio25/value /sys/class/gpio/gpio48/value` must read `0 0 0` (coil released, IR off). **Pause: report.** If a `ak_*` call returns non-zero, report the code and stop.
2. **Image statistics baseline (day, room lit or window).** Grab one main-stream frame (`ffmpeg -rtsp_transport tcp -i rtsp://<ip>:554/stream0 -frames:v 1 day.png`) and compute the mean of the U and V planes and the mean luma
   (`ffmpeg ... -vf "signalstats" ...`: SATAVG, UAVG, VAVG). Colour pictures: SATAVG clearly above 10. Keep the numbers.
3. **Force night, then day, by conf (the coordinator alone, lights off or the lens covered with a cloth).** Write `night=night`, reboot or restart the app. Expect the night sequence in the log (`IR LED`, `ak_vi_switch_mode(0, 1) = 0`) and
   `gpio48 = 1`; the coil pins read `0 0` again afterwards. Listen/look: a faint click of the IR-cut when the pulse runs, a red glow of the LEDs at the lens. Grab a frame: expect **mono** (SATAVG near 0, UAVG and VAVG near 128)
   and lit by the IR. Then `night=day`: `gpio48 = 0`, the filter clicks back, colour returns (SATAVG as in step 2). **If the picture is colour with a pink cast and IR at night, or mono in daylight, the two coil pins are swapped (the one
   unverified mapping): swap `A2K_IRCUT_DAY_PIN` / `A2K_IRCUT_NIGHT_PIN` in `ircut.h` and repeat.** Also compare the night picture with the stock's (known from earlier captures) for the WDR the stock re-applies after a mode switch;
   the WDR replay is now implemented (see "WDR replay after the mode switch" below): compare night and day pictures with `wdr=1` (default) against `wdr=0`. **Pause: report the two sets of numbers and the frames.**
4. **Coil check.** During a forced switch, read the coil pins at 100 ms intervals from a second SSH session (`while :; do cat gpio24/value gpio25/value; usleep ...; done`, busybox `usleep 100000`): at most 3 samples read `1`, never both at once, `0 0` after. Never `1`
   for longer. **Pause.**
5. **`ir_led=off` with `night=night`:** night mode without the IR LED (`gpio48 = 0`): the picture is dark and mono. Restore `ir_led`.
6. **Automatic mode, the evening (user step: the user dims the lights; or the coordinator waits for dusk).** `night=auto`. Watch the log with `tail -f`. Cover the lens (or dim the room): expect no switch in the first 60 s after the start,
   then `day/night: night: the vendor day/night decision held 3 s` (a few seconds to a minute after the room is dark), the night sequence, MQTT `night` ON. In the dark room, wait 30 min: expect **no** switch to day, and
   every 5 min first (after the first one at the end of the 120 s dwell), then every 10, 20, 40 min (the policy's probe backoff) `day/night: periodic probe with the IR off ...` then `probe failed ... IR back on` (the IR LED is off for about 10 s each time: gpio48 reads 0
   then 1 again). Uncover/lights on (real light): expect `day/night: day: IR off, ambient light: ... lit room, back to day`, **not earlier than 120 s after the night switch**, at the next probe (at most
   about 40 min with the backoff at its largest (the 1080p interval, 5 min doubling: same policy and defaults; settle 8 s instead of 4 s because the vendor verdict needs its own frame counts), normally the lamp detector brings the probe forward by a few minutes).
7. **Flapping test (user step: switch a lamp on and off every few seconds for a minute, then every 20 s for 5 minutes).** Expect no switch for the fast flapping, at most one move per minute for the slow one; count `switches` in the MQTT attributes.
8. **MQTT (the sink on the PC, as in the MQTT procedure).** Expect the retained `binary_sensor/.../night/config` document, `night/state ON|OFF` at every switch (not retained), the attributes (`mode`, `ir_led`, `switches`, `last_switch`, `reason`);
   in Home Assistant the "Night mode" binary sensor follows the camera.
9. **Image keys.** One at a time, restart after each: `image_brightness=70` (expect `ak_vpss_effect_set(0, 1, 20) = 0` and a brighter frame), `image_contrast=70`, `image_sharpness=70`, `mains_hz=60`. Compare frames; remove the keys.
   A value out of 0..100 is ignored by the supervisor and logged.
10. **Teardown and soak.** `kill -TERM` the app while it is in night mode (forced or automatic): expect `day/night: stopped, filter in (day position), coil released, IR LED off`, `ak_vi_switch_mode(0, 0) = 0` before the capture teardown lines, exit 0; the pins read
    `0 0 0` afterwards and the filter clicked once. One hour with the automatic mode in a room with a window: the number of switches in the log should be one or two per dusk and dawn.

Abort criteria: a coil pin reading `1` for more than about 400 ms, both coil pins high at once, `ak_vi_switch_mode` returning non-zero, a stream stall or frame-rate drop around a switch (a keyframe is requested, a short glitch is expected), an app exit,
a reset of the camera. Fall back to `night=day` (a forced day, which only moves the filter once at the start).

### IR self-illumination: hardware result 2026-11-15 (dawn test, dark room, `night=auto`, first version) and the fix

Log: `automatic, start in day` (2 s) -> `night: the vendor day/night decision held 3 s` (162 s) -> `day: the vendor day/night decision held 5 s` (429 s) -> `night: ... held 3 s` (590 s). In a dark room, with no
daylight yet, the vendor verdict said DAY 4.5 min after the night switch (IR LED on, filter out, mono image Y avg ~121), and night again 2.7 min after the return to day. **The library's non-infrared check does not
hold the camera's own IR light out.** The `cur` argument was right (RE of `ak_vpss_get_soft_ps_level` 0xc18c: `cur == 0` runs `day_mode_check`, otherwise `night_mode_check`; we pass 0/1 for the ISP mode) and the struct is
filled as the stock fills it, so there is no field to correct (details in stock-firmware-reference.md "What `night_mode_check` does"). Root cause: confidence MEDIUM-HIGH on "the IR light brightens the scene so the library's
exposure test passes and the ratio test does not reject it" (the 4.5 min and 2.7 min timing fit the lib's own lock / hysteresis, but the exact value of the ratio was not logged); HIGH that the verdict is not usable for "day" with the IR LED on.

Fix (a, b and c of the brief, in the adapter only; `night_logic.c` / `night.h` untouched):
- (a) nothing to change: the `cur` flag and the soft_ps fields already equal the stock's (the stock has the same weakness, or hides it by a lock the lib does not run with `lock_time` 0; not reproduced here).
- (b) `ir_active = 1` in auto IR mode: the policy waits the minimum night dwell (120 s), then probes (periodic, first one at the end of the dwell, interval 300 s, the 1080p default, doubling per failed probe up to x8, plus its lamp
  detector: the verdict turning to day for 3 s under the IR light brings a probe forward). A probe switches the IR LED off (pin 48 only, `a2k_ircut_led`), waits, reads the vendor verdict with the IR LED off, switches it back on.
  The verdict is trusted for "day" only there: readings count from 2 s after the LED went off, the first "night" reading fails the probe, a probe passes only after 8 s with the last 3 readings "day" (readings that fail
  because the AE is not stable do not count). The policy's own rule (10 further samples) then leads to the day switch. An IR-off probe costs about 9-11 s of a dark image every 5 min at first (about 3 % of the night), every 40 min once failed; a probe fails early (about 3 s) when the room is dark.
- (c) the confirmation for night to day is the probe itself; with `ir_led = off` (no IR light of our own) it is 15 s of "day" readings.
- Safety unchanged: coil pulses <= 300 ms, both coil pins low afterwards, IR LED off on stop (also in the middle of a probe) and after a failed day switch the IR LED is restored (it is the probe, not the switch, that turned it off).

Host tests (`tests/rot2k_night_unit.c`): an IR-fooled verdict in a dark room for 8 hours = 2 moves only (start, night), probes of at most 13 s; a 5 s "day" verdict = no switch; a real dawn (with the IR fooling) =
day within 41 min at the worst case of the backoff, never before the 120 s dwell; dawn before the first probe = day at the first probe (at the end of the dwell); `ir_led = off`: day 15 s after the verdict.

Hardware check of the fix: dark room, `night=auto`, reboot. Expect one night switch then, for 30+ min, only `periodic probe with the IR off` / `probe failed ... IR back on` lines (and gpio48 0 for ~10 s each), no `day:` line.
Then light the room (real light, lens not covered): `day: IR off, ambient light ... back to day` within one probe interval.

### Night flashes and dark phases: the lamp detector fed by the vendor verdict (hardware report 2026-11-18, fixed)

Symptom: "at night we sometimes get flashes of light, or the picture goes dark then comes back". Log (night, dark room, no lamp): periodic probe at 188 s (probe failed 6.8 s later), then
`exposure fell to 0 (< 50 % of the average 383): lamp? probe with the IR off` at 315 s, 562 s and 1050 s, each a failed probe (5-7 s). Every probe switches the IR LED off (picture goes dark, the AE climbs to its
ceiling) and back on (the AE overshoots: the flash).

Root cause: the adapter turns the vendor verdict into a binary sample for the shared policy (night = exposure 16384, day = exposure 0). Under our own IR light the verdict is not about the room (see above:
"day" in a dark room); here it flickered, so the policy's "exposure" was 0 for runs of 3 s or more. Its lamp detector (exposure below 50 % of a 30 s average for 3 s) fired on it, and since the average of a
binary 16384/0 signal itself decays to a few hundred (6850, 383, 299 in the log), "below 50 % of the average" held for every day run. So the "0" was not an invalid read (failed reads give no sample at all and
restart the counts) but the vendor's own "day" verdict, which cannot be told apart from a lamp, a dawn or the IR light.

Fix (adapter only, `night_ctl.c` `tick_sample`; the shared `night_logic.c` is untouched, so the 1080p binary is unchanged): in night with `ir_led = auto` and outside a probe the vendor verdict is not fed
to the policy; it gets a constant "dark" sample. The lamp detector can then never fire and the only probe trigger is the periodic one (first at the end of the 120 s dwell, then 300 s doubling per failed probe, up
to 40 min). The probe log line now says why it runs ("IR-off probe N (scheduled; ...)"), the failure line keeps the next interval. Result: 1 probe per 40 min once failed instead of 4 in 15 min.
Unit test `test_engine_lamp_false_trigger` (flaky "day" runs of 4 s under IR, and failing reads): at most the 5 scheduled probes in 2 h, no switch to day (it failed before the fix).

Do we still need the periodic IR-off probe? Yes: the vendor decision (`soft_ps`) is the only light measurement the 2K has, and it is exactly what the IR light fools (the stock has the same weakness or hides it
behind a lock we do not know). Without a probe the camera would never return to day in the morning. The cost is kept low by the back-off, not removed.

Drift check gating: during a probe (and for 10 s after a probe start / end or a day/night switch, `a2k_night_probe_busy()`) the PTZ drift check in `scene.c` (`hook_drift`) answers SKIP (retry later), also when a probe
begins while its frames are read: dark / flashing frames would be read as a displaced view, two of them agree, and the result would be a needless recalibration (RECAL). `ptz.c` is unchanged (it already retries on SKIP).
Test: `rot2k_scene_glue_unit.c` (SKIP during a probe, RECAL again after) and `test_engine_probe_busy`.

Probe duration (5-7 s): not shortened, on purpose. The IR-off window is the first valid vendor read (begins after the LED change, 1..1.5 s, and only once the lib's `wait_exp_stable` passes, i.e. after the AE has
settled: that is most of the 5-7 s) plus the 2 s grace for readings. A "night" verdict inside the transient could fail a probe in a lit room (a morning delayed up to 40 min), and the AE numbers
(`ak_vpss_get_ae_run_info`) are not decoded on the 2K, so no earlier or safer cut is measurable without the camera. The AE flash on IR back-on cannot be avoided with the known API (no exposure hold /
restore call is used or decoded; the IR LED is a plain GPIO, no dimming): fewer probes is the mitigation. Follow-up if wanted: decode the AE run info (phase B), read the exposure after ~1 s and end the probe
the moment it clearly saturates.

### Day/night hardware result 2026-11-15 (night, dark room, coordinator alone)

- `night=night` (forced): IR LED gpio48 = 1, coil pins 24/25 both 0 after the pulse, stream0 frame Y avg 121, saturation
  0 (mono, IR lit) → `test-images/rot2k/2026-11-15_daynight_forced_night_stream0.jpg` (local only).
- `night=day` (forced): gpio48 = 0, 24/25 = 0, frame Y avg 18, saturation 6 (colour, dark room) → consistent with the
  IR-cut mapping 24 = day (the bright IR image in night proves the filter left the path). MEDIUM until seen by eye.
- Stop path verified: "stopped, filter in (day position), coil released, IR LED off".
- Note: the supervisor reads `akam/akam2k.conf` only when it starts (at boot) — a `kill` of the app restarts it with the
  old options; change the conf, then reboot. Card conf removed afterwards → `night=auto` for the dawn test.

### Day/night flap fix — hardware result 2026-11-15 (dark room, 45 min, coordinator alone)

`night=auto`: night at 162 s, then only IR-off probes: probe 1 at 428 s (failed in 11 s: `hw_exp=16384 >= 2048`, IR
back on, "next in 600 s"), probe 2 at 1764 s (failed in 8 s, "next in 1200 s"). **No false return to day** (before
the fix: day at 429 s, night again at 590 s). Stream0 ~9.9 fps at night, 0 dropped, ~21 MB available. Observation: the
real gap between probes 1 and 2 was ~1325 s, longer than the logged 600 s (harmless — less darkening; dawn latency
longer); to check against the shared policy's timing. Dawn return to day still to observe.


### WDR replay after the mode switch

Status: **merged and running on the camera (2026-11-15, build 5c9bfb35).** Log: `ak_vpss_effect_set(0, 5, 40) = 0` at start (stock order), again on the first day mode and after the switch to night (68 s, dark room). Night pictures before/after (`test-images/rot2k/2026-11-15_wdr_{before,after}_stream0.jpg`, flat wall scene) show no visible regression; a high-contrast scene (window in daylight) is still to compare. The stock replays WDR 100 ms after every ISP mode switch
(call chain in stock-firmware-reference.md, "WDR call chain"): `ak_vpss_effect_set(0, 5, 40)` for the default `wdr_type 1` / `wdr_onoff 1`.
The engine does the same in the vendor-locked ISP step (`vendor_isp_mode`, under `vmu`, tick thread): after a successful `isp_mode` (which already
requests a keyframe on both streams), sleep 100 ms, then `a2k_isp_wdr(1)`. A failure is logged once and never aborts the switch; a failed mode
switch replays nothing. Config: `wdr=0|1` in akam2k.conf (`--wdr`), default 1; use 0 for a camera whose param.conf has `wdr_onoff=0` or `wdr_type=2`
(the latter, a get/modify/set of `ak_vpss_*_wdr_attr`, is not implemented). No new vendor symbol (`ak_vpss_effect_set` was already linked).
Hardware check: switch to night and back with `wdr=1` and `wdr=0`, compare the pictures (and the log line `ak_vpss_effect_set(0, 5, 40) = 0` after each
`ak_vi_switch_mode`); read the camera's `[dsp] wdr_onoff` / `wdr_type` from its param.conf first.

## Sub stream size limit (hardware, 2026-11-18)

The sub channel cannot go above 640x480 on this SoC configuration (64 MB RAM, fixed media reserve). A RAM-only run with `sub_res` 1280x720 gave, at `ak_vi_set_dev_attr`,
`The 3DNR yuv size is out of range [460800:1843200][115200:460800][115200:460800] bytes` (the sub 3DNR buffer maximum is 460800 B = 640x480 YUV420) and then, in `ak_venc_open_ex` chn 1
(Main and Baseline), `Venc [1] Create DMA Pool failed!` with rc 218103819. 960x540 gave the same 3DNR warning and rc 218103811. So the own app keeps the sub at 640x360 (now H.264 Main,
800 kbit/s, QP 26..44); details in `venc-live-params.md` section 8.
