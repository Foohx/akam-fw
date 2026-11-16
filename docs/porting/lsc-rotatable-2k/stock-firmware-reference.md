# Stock firmware reference — LSC Rotatable 2K (YYOS / `yy_cam`)

One-stop index of everything learned about the stock firmware, so later work does not redo the research.
Details and evidence (addresses, confidence) live in the linked findings docs; this page is the map.
Facts marked **HW** were observed on the real camera (akam-rot-01, 2026-11-14); **RE** = from offline reverse
engineering of the user's own backup; **UNVERIFIED** = third-party or inferred.

Never copy vendor binaries, keys, nvram values or `/config/tuya` content into the repo.

## Platform

| | |
|---|---|
| SoC / OS | AK3918AV130, ARM926EJ-S soft-float; YYOS / TuyaOS v1.6, kernel 4.4.302-cip94, **no IPv6** (HW) |
| userland | Buildroot 2018.02, uClibc-ng 1.0.31, BusyBox **1.30.1** (no `tar`, `cmp`, `timeout`, `pidof`) (HW) |
| RAM | 38 MiB usable; ~18 MB available with `yy_cam`, ~24 MB without (HW) |
| flash | 8 MiB NOR, 6 partitions: uboot/kernel/rootfs/appfs/config(jffs2)/factory — see [PLAN §2.2](PLAN.md) |
| Wi-Fi | ATBM6132C **USB** device on `ak_hcd` (HW) |
| sensor | SC1346 MIPI 1-lane, ISP2, Allegro encoder, NPU (`ak_npu`) |

## Boot chain

`rcS` (rootfs) → insmod cfg80211/mac80211/usb-common/usbcore/ak_hcd → mounts appfs (`mtd3`, no integrity check
there) → `/appfs/script/init.sh` reads `/mnt/sdcard/yy_debug.ini`: `mode=debug` runs `start_sh=` from the card
**in the background** (our hook), `mode=normal` runs `start.sh` → `start.sh` insmods video/NPU/motor/misc drivers
and runs `/appfs/bin/yy_cam &` if `nvram get STATUS` = active. `init.sh` then loops: reboots on a `mode=` change,
drops caches on low memory, and runs **OTA** when `/tmp/rmem_blk/yy_upgrade.bin` exists **and**
`nvram YYOS_UPGRADE=on` (RE). Details: [PLAN §2.3](PLAN.md).

## Watchdog (RE + HW)

`yy_cam` opens `/dev/watchdog` O_RDONLY, `WDIOC_SETTIMEOUT` 20 s, `WDIOC_KEEPALIVE` every 5 s (log line
`feed watchdog!`); clean exit → `WDIOC_SETOPTIONS DISABLECARD`. **HW-verified:** SIGKILL of a BusyBox `watchdog`
feeder resets the camera ~20 s later. Not armed before userspace opens it (MEDIUM). → [own-app-feasibility.md](own-app-feasibility.md)

## GPIO map (RE, defaults from `yy_cam`; sysfs `/sys/class/gpio`)

| GPIO | Function |
|---|---|
| **10** | **Wi-Fi enable, active LOW** (`[net] wifi_en_gpio_no/polarity`); sequence: export, out, 1, 0, sleep 1, insmod, sleep 2 (HW-verified) |
| 55 / 56 | red / blue LED |
| 9 | speaker enable |
| 24 / 25 | IR-cut coil, a pulse pair (300 ms, both low afterwards): see "Day/night and image" |
| 48 | IR light (active high) |
| 49 | white light (active high) |
| 22 | alarm |
| 50 / 71 | keys (inputs). 71 = the front button, hardware verified: active low, reads 1 released / 0 pressed (own app: `hal_input`, see `button-and-zones.md`); 50 is not used |

## Wi-Fi bring-up (done by `yy_cam`, not `start.sh`) (RE + HW)

GPIO10 sequence above → `insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1` → writes `/tmp/wpa.conf`
(`ctrl_interface=/tmp/wpa_supplicant`) → `wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf -B -P /tmp/wpa.pid`
→ `udhcpc -i wlan0 -x hostname:YY_CAM`. Credentials come from the Tuya store under `/config/tuya` (never read).
**When the cloud is unreachable, `yy_cam` tears Wi-Fi down and restarts DHCP repeatedly** (HW: 4× in 2 min) —
this drops dev-bound routes (our decloud keeper restores them). → [decloud-findings.md](decloud-findings.md)

## `/config/param.conf` keys (INI, `yy_conf_get_int(section, key, default)`; re-read at `yy_cam` start)

| Section | Key | Default / meaning |
|---|---|---|
| `[dsp]` | `video_chnN_max_bitrate`, `video_chnN_target_bitrate` | chn0 800 / 630 kbit/s (**the real encoder bitrate**, HW-verified) |
| `[dsp]` | `video_chnN_enc_mode` | 4 = AVBR (lib names: CBR 0, VBR 1, CONST_QP 2, …, AVBR 4) |
| `[dsp]` | `video_chnN_min_qp/max_qp/init_qp`, `gop`, `frame_rate`, `enc_profile`, `width/height`, `depth`, `smart_*`, `gdr_mode`, `quality_lvl`, `snap_jpeg_*` | stock chn0: Baseline 2304×1296 15 fps GOP 30 QP 32..51; depth 0 → 2 or 3 from DMA reserve |
| `[dsp]` | `ivs_hd_on_off`, `ivs_md_on_off`, `ivs_mt_on_off` | alarm switches (human / motion), **mt = auto-tracking with the motor (keep 0)** |
| `[dsp]` | `ao_vol` | speaker volume |
| `[cloud]` | `video_chnN_max_bit_rate` (note the spelling), `local_record_switch` | Tuya cloud media config, not the encoder |
| `[rtsp]` | `rtsp_on_off` | 1 → RTSP on :8554 `/stream0` `/stream1`, **no auth** (HW) |
| `[onvif]` | `onvif_on_off`, `onvif_username`, `onvif_password`, `onvif_server_port`, … | 1 → ONVIF on :5000, `admin` + empty password (HW) |
| `[ptz]` | `ptz_pan_step_home` 1275, `ptz_tilt_step_home` 816 | motor home position |
| `[net]` | `wifi_enable`, `wifi_en_gpio_no` 10, `wifi_en_gpio_polarity` 0, `wifi_is_need_write_mac` 0 | Wi-Fi power |
| `[alg]` | `ak_av130_svp2_hd_enable` (1), `_conf` (model path), thresholds… | AI detector |
| `[dp]`, `[led]` | `secret_mode_onoff`, `siren_onoff`, `siren_led_onoff`, `onoff` | misc |

Fallback file `/config/default_param.conf`; `yy_cam` has a path that deletes `param.conf` (factory reset).

## nvram (names only — values are device secrets)

`STATUS`, `MASTER_MAC`, `YYOS_UPGRADE`, `appfs/rootfs/kernel_version`, **`appfs_sign`, `rootfs_sign`,
`kernel_sign`** (md5-sized; U-Boot has `%s_sign` md5 code → flash-write gate RED, [PLAN §7bis](PLAN.md)),
`TY_*` and `accredit_key`/`accesskey_id`/`omok_*` (**Tuya credentials — never print**; `nvram show` prints them).

## Network services (stock)

TCP 6668 Tuya LAN, UDP 7000; RTSP 8554 + ONVIF 5000 only when enabled in param.conf. ONVIF quirks (HW):
`SetVideoEncoderConfiguration` accepted only with Quality 21..50 and **has no effect on the real encoder**;
**PTZ not wired** (no PTZ configuration, `ContinuousMove` → "PTZ Profile Not Exist"); events: only a hard-coded
`CellMotionDetector` "TestRule" template, never sent; `Subscribe` → "Invalid IPv4 Address".
→ [ptz-findings.md](ptz-findings.md), [npu-findings.md](npu-findings.md)

## Motor (RE + HW)

`/dev/tymotor` (misc 10,57): stop `0x0c00`, pos `0x801c0c01` (w1 = pan steps, no tilt read-back), move
`0x401c0c02` {dir,a,b}; dirs 0 up, 1–3 right, 4 down, 5–7 left; a second opener is accepted while `yy_cam` runs.
`pos` takes an axis selector in w0 (0 pan, 1 tilt) and answers in w1, so tilt is readable (offline RE, HIGH).
`a`/`b` = pan/tilt speed divisor, akam_ptz accepts 0..20 (1 step per `speed` ms; 0 keeps the previous speed; driver default 5/4).
The driver is open loop: step counters (no limit switch), moves 0..7 walk the counter to 0 or max, move_to (cmd 10)
is clamped to [0,max]. Counters start at 0/0 after boot; `yy_cam` sets them with two init requests: `0x40100c03`
{0,pan_max,tilt_max,0} (defaults 4000/2160) then `0x40280c04` {pan,tilt,0…} = "the camera is here" (home 1275/816),
1 s apart, then a driver-side sweep to the end stops (cmd 9 pan / 12 tilt, speed 1) re-synchronises the counters.
Driver built-in limits 6000/2200. `akam_ptz init --i-know` replays the two init requests (no sweep) with the driver limits set to the soft range (shifted frame, see ptz-findings.md).
`akam_ptz` move/goto/park are soft-limited (default, measured: pan 300..3650, tilt 300..1750, home 1275/1300, `akam/ptz.conf`), polled and
stall-protected; commands 9/10/12 are never sent (counters are open loop: a software sweep cannot measure the range). The own app's boot
calibration (`hal_ptz`) does the stock `check` with directional commands (declare-max-then-move), see ptz-findings.md "hal_ptz".
→ [ptz-findings.md](ptz-findings.md) ("Motor init without yy_cam", "Safe motion layer")

## Detection / NPU (RE + HW)

`yy_alg_init` runs before the cloud: threads `Y_ALG_AV130_HD` (human, NPU, ~5 % CPU), `Y_ALG_AV130_MD` (VPSS
motion), `Y_ALG_SOUND_DET`. Without the cloud: motion sensitivity 0, **no events**; results only go to the Tuya
cloud / SD recording. → [npu-findings.md](npu-findings.md)

Stable facts for running it ourselves (RE; the call sequence ran on hardware in PoC-4 with an empty scene, the output layout and class meaning are not yet exercised; evidence in [ai-own-app.md](ai-own-app.md)):

- Inference lives in the kernel driver `ak_npu.ko`; userland is `libplat_npu` → `libak_nne` (low level) and `libmpi_svp2` (API).
  `libakv_cnn_dt` is not loaded by `yy_cam`.
- Sequence: `ak_npu_init()` (no arg) → `ak_svp2_create_chn(handle 0, attr, conf{model path, mode 0})` → per frame
  `ak_vi_get_frame(16)` → `ak_svp2_process(0, in, out)` → `ak_svp2_release(out)` → `ak_vi_release_frame(16)`; the stock sleeps 100 ms
  between runs. Attr: model_num 1, env 0, model_type 10, classify 700 (0..1000), IoU 3 (0..10), target_type 0 (0 both labels,
  1 / 2 one label). Input 40 B: img_type 0 (YUV420SP), 320x192, ROI = whole image, phys + virt address. Output: count + array of 24-byte
  `{class, x1, y1, x2, y2, score 0..1000}` in input pixels.
- Frames: a **third VI channel**, id 16, 320x192 NV12, depth 2, 20 fps (yy media chn 3, `ak_av130_svp2_hd_chn=3`), no encoder; set
  with `ak_vi_set_chn_attr` next to chn 0/1 before `ak_vi_enable_dev`. Model 985,012 B, input 320x192 (train 320x180).
- Classes: 2 foreground labels (LOW on meaning); the stock only acts on class 0. `ak_av130_svp2_hd_skip_frames` and
  `..._score_threshold` are stored but never read.
- Model container (HIGH on layout): 0x30-byte header (version 2, header size, section A size 0x22f4, section B size, total = file size,
  label `0x0A00000D`, version), a layer-descriptor table (110 records, MobileNet/SSD-like) and a ~970 KB weight blob; checked by size and
  label only (no CRC or signature found). `ak_npu.ko` 2.0.17 holds the per-id post-processing for ids `0x0A000007..0x0A00002D` plus
  `0x0A010002..06`; only `0x0A00000D` is mapped to classes. → [npu-models.md](npu-models.md)
- `libak_nne` is a thin generic runner (`NNE_Create_CH` / `NNE_Run` on blob arrays, no post-processing); `libmpi_svp2` is the fixed pipeline
  (resize + driver-side decode; model from path or memory buffer; param families hd / fas / fr / gesture). `libakv_cnn_dt` is the older
  AV100 user-space CNN detector (own register programming, `slim320/480/640/640_pets/320_face`), unrelated to the AV130 path.
- Motion: `ak_vpss_md_get_stat(0, 1536-byte buf)` + software `ak_mrd` filter (`libak_mrd`), 5 fps; layout not decoded.

## Day/night and image (RE of `yy_cam`, `libplat_vpss`, `libplat_vi`; nothing of it ran on the camera yet)

All of this is **RE** from the user's own backup (function addresses are those of the stock `yy_cam`, stripped; the labels are the
names the binary itself logs or exports). Confidence is HIGH unless a line says otherwise. **UNVERIFIED** = needs the camera.
There is **no light sensor and no ADC**: no saradc, no photoresistor symbol or string anywhere in `yy_cam`. Day or night comes from
the ISP's image statistics only.

### Who decides (thread `Y_DN_SWITCH`, `yy_dn_switch_th` 0x2d5278, one pass per second)

`param.conf` section `[dn]` (loader 0x2d5618): `dn_mode` 0, `daytonight_threshold` 15410000, `nighttoday_threshold` 2359000,
`dynamic_threshold_offset` 40000, `dynamic_threshold_min` 15000, `lock_time` 60 (s), `whiteled_time` 30 (s), `photosensitive_type` 0.
`dn_mode` (set by the Tuya DP `night_mode` / `night_vision_mode`): 0 and 3 automatic, 1 forced day, 2 forced night, 4 "full colour"
(white-light) variant (path at 0x2d500c, not decoded). Forced modes only act when the ISP is in the other state.

On this SoC the stock does **not** measure the scene itself. `yy_media_av130_soft_ps_alg_is_support` (0x2a1220) always answers yes, so
`yy_dn_switch_init` registers the two thresholds with the vendor library and then only asks it for a verdict:

| Call | Meaning |
|---|---|
| `ak_vpss_set_soft_ps_param(0, 92-byte struct)` (once) | "automatic day/night parameters" (the lib's own message names are `ak_vpss_set_auto_day_night_param`). Fields (names from the lib's validation messages, values as the stock passes them): `day_to_night_lum` 15410000, `night_to_day_lum` 2359000, `awb_night_dis_max` 4000, `day_check_frame_num` 10, `night_check_frame_num` 10, `exp_stable_check_frame_num` 10, `gain_r` 1280, `gain_b` 1280, `gain_rb` 0 (lib default 1024), `blk_check_flag` 1, `low_lumi_th` 5, `high_lumi_th` 245, `non_infrared_ratio_th` 40, `highlight_lumi_th` 170, `highlight_blk_cnt_max` 45, lock time 0 (the lib's own lock thread stays off). The struct is transcribed in `firmware/.../app/akam2k_abi.h` (`struct vpss_dn_param`). |
| `ak_vpss_get_soft_ps_level(0, cur, &want)` (every second) | `cur` 0 = ISP in day, 1 = ISP in night; `want` 0 = should be day, 1 = should be night. The lib computes the luminance from the AE run info (`ak_vpss_get_ae_run_info`, `calc_cur_lumi`), applies its frame counts, and for night to day its **non-infrared ratio / block check from the AWB statistics**, so the camera's own IR light does not fool it. It fails while the AE is not stable and before `set_soft_ps_param` ran. |

**What `night_mode_check` does** (`libplat_vpss` 0xbbb8, called by `ak_vpss_get_soft_ps_level` 0xc18c when `cur != 0`; `day_mode_check` 0xae1c when `cur == 0`; before either the lib calls
`ak_vpss_wait_exp_stable(dev, exp_stable_check_frame_num, ...)`). The 92 bytes are copied verbatim to the lib's per-process state, so the fields are read at their struct offsets. In night mode, for
`night_check_frame_num` (10) frames spaced by one frame period it reads `ak_vpss_get_ev` and, with `blk_check_flag` 1, `ak_vpss_get_non_infrared_ratio` (input: `awb_night_dis_max`, `gain_r/b/rb`, `low_lumi_th`, `high_lumi_th`,
`non_infrared_ratio_th`, `revised_dis_max_ratio`); a frame counts as "day" when EV < `night_to_day_lum` **and** the ratio > `non_infrared_ratio_th`. The verdict is "day" only when all frames count; otherwise `want = 1`. A
"lock in night" state (`start lock in night...` / `locking in night...`, a timer compared with the word at +84 of the struct, which the stock leaves 0, and the EV against a reference taken at the lock start, 10 % change) can still
override it. MEDIUM (read from the disassembly, not run). **It does not keep the camera's own IR light out on the 2K**: dark room, IR LED on, filter out: verdict "day" after ~4.5 min (hardware 2026-11-15, see own-app-feasibility.md
"IR self-illumination"). The `cur` flag passed by our adapter (0 = ISP in day, 1 = ISP in night) and every field are the stock's; there is no separate "IR on" flag in the call. The stock's own handling of this is not known (it
may be hidden by the white-light variant or by a lock the Tuya app sets); the app now trusts a "day" verdict only with the IR LED off (IR-off probe of the shared 1080p policy).

The stock's own threshold path (a 64-bit luminance compared with `daytonight_threshold` / `nighttoday_threshold`, count above 5,
dynamic threshold) exists in `yy_dn.cpp` but reads its input through `yy_media_isp_get_ae_only_read_attr` and
`yy_media_isp_get_awb_global_statis_info`, which on the av130 build are **empty stubs**: that path is dead here (MEDIUM: it follows
from the stub code at 0x2a1630 / 0x2a1650 and from the call at 0x2d5370..0x2d53b8).

Lock (`yy_dn_lock`, 0x2d5110 / 0x2d51f0): after more than 5 switches, if the last two switches were at most 60 s apart the automatic
detection is locked ("frequent switching dn lock") until `lock_time` seconds have passed. MEDIUM.

### What it switches

`yy_dn_day_to_night` (0x2d3934): lock, `usleep(100 ms)`, **IR light on** (`yy_light_set_status(IR, 16)`), **ISP night**
(`yy_media_isp_set_running_mode(1)`), **IR-cut to night** (`yy_ircut_set_status(10)`), unlock, `sleep(1)`.
`yy_dn_night_to_day` (0x2d39cc): **IR-cut to day** (`yy_ircut_set_status(11)`), **IR light off** (`yy_light_set_status(IR, 15)`),
`usleep(100 ms)`, **ISP day** (`set_running_mode(0)`, or 2 when its argument is non-zero: the white-light "full colour" variant,
MEDIUM), `sleep(1)`.

- **ISP mode** (`yy_media_av130_isp_set_running_mode` 0x2a0f04): mode 0 or 2 calls `ak_vi_switch_mode(dev, 0)`, otherwise
  `ak_vi_switch_mode(dev, 1)` (libplat_vi `ak_vi_switch_mode`, with `night_status_change` / `save_soft_ir_info` / `isp_switch` behind it:
  it loads the day or night parameter set of the ISP conf and tells the soft-PS code that the IR light is on or off; the exact content
  of the conf file's two sets was not read, UNVERIFIED). Then a keyframe request on every video channel, `usleep(100 ms)`, and the
  stock **re-applies WDR** (see "WDR call chain" below; our app replays it since the `feature/rot2k-wdr` change, hardware check pending,
  see own-app-feasibility.md). Colour or mono comes from the ISP set; no separate "colour to mono"
  call exists (the string `mono` in the binary is a JSON/ONVIF text).
- **IR-cut** (`Y_IRCUT` thread `yy_ircut_th` 0x2a7ac0, `yy_ircut.cpp`): `[ircut]` `p_no` default **24**, `m_no` default **25**
  (`yy_conf_get_int`; with `m_no` 0 it would drive a single pin). Pins are exported through sysfs (`/sys/class/gpio`, `%s/gpio%d/direction`,
  `%s/gpio%d/value`) as outputs, the value files kept open. The thread polls its requested status every 10 ms; on a change it runs the
  pulse (0x2a7820) with a width of **300 ms**:
  - mode 1 (requested status 11 = **day**): pin 24 high, pin 25 low, 300 ms, **pin 24 low**;
  - mode 0 (requested status 10 = **night**): pin 24 low, pin 25 high, 300 ms, **pin 25 low**.
  Both pins are low after every pulse: the coil is released, never held. At init it pulses mode 1 then 0, and because the thread's
  requested status starts as 11 it then settles on mode 1 = **day**. Which physical filter position pin 24 or 25 gives is **UNVERIFIED**
  (the mapping day = pin 24 comes from the call sites, the direction of the coil is not readable from software).
- **IR light** (`Y_LIGHT`, `yy_light.cpp`): `[light]` `ir_no` **48**, `ir_polarity` 1 (1 = active high: on writes 1), `white_no` **49**,
  `white_polarity` 1, `alarm_enable` 0, `alarm_no` 22, `alarm_polarity` 1, `blink_fast_ms` 250, `blink_slow_ms` 1500. Status codes
  (`yy_light_set_status(light, status)`): IR (light 1) 15 off, 16 on, 17 / 18 blink fast / slow; white (light 0) 10 off, 11 on, 12 / 13
  blink; the two are mutually exclusive in the thread (white on forces IR off and the other way round: string "open white led and close IR
  led"); alarm light 19..22. Same sysfs mechanism.
- **LEDs** (`yy_led.cpp`, gpio 55 red / 56 blue by the GPIO map): `[led]` keys `onoff`, `red_no`, `red_polarity`, `red_def_value`, `green_*`,
  `blue_*`, `blink_fast_ms`, `blink_slow_ms`, `siren_led_onoff`. Not needed for day/night, not decoded further.

### WDR call chain (re-applied after every ISP mode switch)

RE of yy_cam (objdump, HIGH for the sequence and values, MEDIUM for the meaning "effect type 5 = WDR strength", from the Anyka SDK's
`ak_vpss_effect_set` type list as remembered; the type name is not in the binary):

1. `yy_media_av130_isp_set_running_mode(mode)` (0x2a0f04): after the `ak_vi_switch_mode` loop and the per-channel keyframe loop
   (`yy_media_av130_video_request_idr(ch)`, only for channels that are open), `usleep(0x186a0)` = **100 ms**, then calls 0x2a2560 with
   `(g->wdr_onoff, 1)` where `g->wdr_onoff` is the global at offset 0x358 (the result is only logged, the mode function returns nothing).
2. 0x2a2560 (`isp_set_wdr(onoff, apply)`, labelled `video_request_idr+0x7a8` by the symbol table): dispatches on `g->wdr_type`
   (offset 0x35c): **1** goes to 0x2a1eb4, **2** to 0x2a2074, anything else is a no-op (returns 0). Logs an error on failure.
3. **wdr_type 1** (0x2a1eb4, the `[dsp] wdr_type` default): a state word at g+0x360 (0 off, 1 on, -1 unknown). `onoff == 1` and
   (state is 0 or -1, or `apply == 1`): state = 1, then for each video device (count at g+12, **one** on this camera)
   `ak_vpss_effect_set(0, 5, 40)`. `onoff == 0` and (state is 1 or -1, or `apply == 1`): state = 0, `ak_vpss_effect_set(0, 5, 0)`.
   So the replay is **`ak_vpss_effect_set(0, 5, 40)`**, device constant 0, no struct.
4. **wdr_type 2** (0x2a2074, not the default): a heavier path: `ak_vpss_get_wdr_attr(dev, buf)` into a 0x4228-byte struct, copy into
   a per-device cache (g+0x41c), patch a 65-entry u16 table and a few 130-byte curve copies, then `ak_vpss_set_wdr_attr(dev, buf)`
   (get-modify-set). Our app does **not** implement it (layout UNVERIFIED, and not the default).
5. `wdr_onoff` source: `[dsp] wdr_onoff` of param.conf, loader default **1** (and `wdr_type` default **1**); `yy_media_av130_isp_set_wdr_onoff`
   (0x2a26f0) stores a new value there (and `yy_conf_set_int`s it) and calls the same 0x2a2560 with apply 0 (the Tuya / ONVIF switch).
   The value on the user's camera (param.conf) can be read later: if `wdr_onoff` is 0 or `wdr_type` is 2, use `wdr=0` in akam2k.conf and revisit.

6. **Re-check (review, objdump, HIGH):** 0x2a1eb4 with wdr_type 1 does `mov r2,#40; mov r1,#5; mov r0,#0; bl ak_vpss_effect_set` (0x2a1f8c..98) for
   on and `r2=0, r1=5, r0=0` (0x2a2028..34) for off, as written above. **Startup:** `yy_media_av130_video_init` (0x29ecf8) calls 0x2a2560 with
   `(g->wdr_onoff, 0)`, after flip (0x2a19e4) and anti-flicker (0x2a1670) and before sharpness / contrast / brightness; the state word is -1 at load
   (0x29c3e4), so with wdr_onoff 1 it does apply: the stock applies WDR once at init too. Our app applies it at start (akam2k_main.c, before the
   image effects) and the first day-mode `isp_mode` of the day/night start replays it again.
7. **Other effects are NOT replayed by the stock after a switch:** `yy_media_av130_isp_set_running_mode` ends right after the WDR call (0x2a112c..0x2a1178),
   and the sharpness (0x2a2ae4, type 4), contrast (0x2a2ddc, type 3) and brightness (0x2a30d4, type 1) setters are only called from video_init and the
   Tuya setter (0x296868..0x296938). Whether `ak_vi_switch_mode` itself resets them is not readable from yy_cam (UNVERIFIED, hardware check: set
   image_brightness, force a day/night switch, compare). Our app mirrors the stock: it replays only WDR.

### Image effects (`yy_media_av130_video_init`, after the encoders and the bind)

`[dsp]` keys and defaults (loader 0x29c350): `flip_mode` 0, `anti_flicker` 0, `wdr_type` 1, `wdr_onoff` 1, `sharpness` **-1**, `contrast` **-1**,
`brightness` **-1**, `contrast_offset` 14, `brightness_offset` 14 (not used by the setters), `sensor_num` 0, `zoom_value` 10,
`set_running_mode_type` 0. Applied in this order: flip, anti-flicker, WDR, sharpness, contrast, brightness, zoom.

- `ak_vpss_effect_set(dev, type, value)` (libplat_vpss 0x8a14): types 0..5 take -50..50 (`isp_set_effect`), 6 selects an ISP "style"
  (`isp_cfg_file_set_style`, 0..255), 7 sets the mains frequency (must be 50 or 60, `isp_set_hz`), 8 sets flip/mirror bits (0..3). The
  stock uses **1 = brightness, 3 = contrast, 4 = sharpness**, value = its 0..100 setting minus 50 (100 is clamped to 99). A setting of -1
  (the default) is out of range and **nothing is applied**: the defaults are the values of the ISP conf file. Types 0, 2, 5 (hue,
  saturation and a third one in the Anyka SDK; names UNVERIFIED) are never used by the stock.
- Mains frequency: `anti_flicker` 0 and 1 mean 50 Hz, 2 means 60 Hz; applied at **every** start (`ak_vpss_effect_set(dev, 7, hz)`), so the
  stock default is 50 Hz.
- Flip: `ak_vpss_switch_flip_mirror(dev, a, b)` with a, b derived from `flip_mode` and a per-sensor base orientation read from the config
  (`[dsp]` sensor flip field, not decoded): not copied by our app (a guess could turn the picture upside down).
- AWB: `yy_cam` makes **no** AWB control call. The ISP's own AWB runs from the conf file; the soft-PS parameters above only read it.
- ISP conf: the file named by `[dsp]` `sensor1_isp_conf_file` (our app loads `/appfs/conf/isp_sc1346_mipi_1lane_av130.bin`, as PoC-2).
  There is no second conf file for night: the day / night sets are inside it and `ak_vi_switch_mode` selects them (see above).

## Logs (RE + HW)

`/mnt/sdcard/yy_sdcard_log` present → stdout to `/mnt/sdcard/sdcard_log.txt` (60 MiB rotation). **It prints the
Wi-Fi SSID and password in clear** — never leave it on, never ship it. `yy_log_debug` → `YY_LOG_LEVEL`.

## Video SDK (MPI) — what our own app uses (RE + HW)

`ak_sdk_init` → `ak_mem_dma_get_reserved_size` (depth 3 if > 50 MiB else 2) → `ak_vi_open(0)` →
`ak_vi_load_sensor_cfg(0, "/appfs/conf/isp_sc1346_mipi_1lane_av130.bin")` → `ak_vi_set_dev_attr` (56 B) →
`ak_vi_set_chn_attr` for **both** chn0 and chn1 (20 B, depth at +16) → `ak_vi_enable_dev` → `ak_vi_enable_chn` →
`ak_venc_open_ex` (64 B + 28 B) → loop `get_frame/encode_frame/release`. Link also needs `-lplat_thread
-lplat_dbg`. Toolchain: Bootlin `armv5-eabi--uclibc--stable-2018.11-1`. Lib versions: vi V3.4.32, vpss V3.1.03,
venc V3.0.00, mem V2.0.01, app_video V2.0.00. The stock also calls `ak_vpss_effect_set` (mains frequency always; brightness, contrast, sharpness only when set) after venc open, and
`ak_vi_switch_mode` / `ak_vpss_get_soft_ps_level` for day/night: see "Day/night and image". → [own-app-feasibility.md](own-app-feasibility.md)

### Learned while building PoC-3 (the libc finding is offline RE; the first camera run passed, see own-app-feasibility.md)

- **RE (camera rootfs):** the camera's libc is uClibc-ng 1.0.31 **without** `__ctype_b_loc` (it exports `__ctype_b`,
  `getrandom`, `pthread_create`; pthreads are inside libc, no `libpthread`). The pinned Bootlin toolchain's `<ctype.h>`
  macros call `__ctype_b_loc`, so any binary using `isdigit()` etc. through those macros fails to load on the camera.
  Check new binaries with `make poc3-symcheck CAMERA_LIBC=...` (own-app-feasibility.md).
- **UNVERIFIED:** `ak_venc_request_idr(handle)` is assumed to make the next encoded frame an IDR (one call per
  encoder handle; we call it from the thread that owns the handle). Two encoders (main + sub) open at once and
  two threads pulling `ak_vi_get_frame` on different channels: assumed to work (stock uses one thread per stream).
- **UNVERIFIED:** main-stream venc options used by PoC-3: Main profile, VBR, target 2000 / max 2500 kbit/s, init QP 100,
  min 32, max 51 (PoC-2 values), `ak_venc_set_stream_buff(1 MiB)` when width > 1920 and init QP > 60 (as stock).
  Expected main I-frame size 100-250 KB; the app drops access units above 1 MiB.

## Audio (RE only, no hardware run, 2026-11-15)

`yy_cam` captures with `ak_ai_open/get_frame` (`libplat_ai`, `/dev/pcmC0D0c` via `libplat_osal`), 8 kHz mono 16-bit, 640-byte frames (40 ms), its own
G.711 u-law encoder (320 B per frame, no `ak_aenc`), thread `Y_MEDIA_AI` -> ring buffer -> RTSP (`PCMU/8000`). `param.conf` `[dsp]`: `ai_sample_rate` 8000, `ai_bitwidth` 16,
`ai_channel_mode` mono, `ai_frm_num` 25, `ai_num_per_frm` 320, `ai_enc_type` g711u, `ai_vol` 50, `ai_gain` 50 and `ao_*` equivalents (`ao_vol` 80, `ao_gain` 80); `[spk] spk_no` 9,
`spk_polarity` 1. NR/AGC/AEC attributes are set but the `ak_ai_enable_*` calls are not imported (VQE probably off, MEDIUM). Playback `/dev/pcmC0D0p`, AEC loopback `/dev/pcmC0D0l`.
Addresses, confidence levels, reuse map and plan: [audio-findings.md](audio-findings.md).

## Do not

Accept the in-app update V14.1.7; write any flash partition (gate RED); run `nvram set`; print nvram / wpa.conf /
`/config/tuya`; set `ivs_mt_on_off=1` while driving the motor ourselves.

## AI model classes (HW, walk test 2026-11-15, MEDIUM)

The stock human-detection model `model_0x0A00000D_V1.00.01.bin` (SVP2, input 320×192 NV12 from VI chn 16) returns
**class 0 = person/body** and **class 1 = head**, scores 0..1000 (~930 body / ~880 head on a person at 2–3 m in low
light); ~27 ms per inference on the NPU. → [ai-own-app.md](ai-own-app.md) §9

## Own app: detection next to the encoders (offline notes, 2026-11-15)

- The binary needs `libmpi_svp2.so`, `libplat_npu.so` and `libak_nne.so` from `/appfs/lib` (linked as DT_NEEDED, as the stock `yy_cam` does); the
  symcheck against the camera libc still passes with the detection code (it adds no new libc symbol outside what the camera exports).
- VI chn 16 (320x192, 20 fps, depth 2) QUEUES frames and `ak_vi_get_frame` returns the OLDEST queued one; a consumer slower than 20 fps
  gets stale frames (confirmed on hardware 2026-11-15: a 5 Hz consumer missed a still person entirely). The consumer must pull up to
  `depth` (2) frames per tick and keep the newest. `ak_vi_get_frame` BLOCKS on an empty queue (~50 ms), so never "get until empty"
  (ai-own-app.md section 11). The stock hd thread does one get per 100 ms loop without draining.
- **UNVERIFIED:** VI chn 16 (320x192, depth 2) enabled together with chn 0/1 AND two encoders running: PoC-4 ran chn 16 only
  with chn 0/1 drained by the program, never with encoders. See own-app-feasibility.md, "Person detection inside akam2k_rtsp".

## JPEG snapshot encoder (venc) — offline RE of `yy_cam` and `libmpi_venc.so`, 2026-11-15 (MEDIUM, no hardware run yet)

How the stock takes a JPEG, from the `yy_cam` disassembly (addresses of the 2K `yy_cam`) and the `libmpi_venc.so` validation code:

- **There is no separate snapshot API.** `yy_media_video_snap_jpeg` (0x295d74) → `yy_media_av130_video_snap_jpeg` (0x2a06e4) checks the per-channel
  flag at record offset +372 (`video_chnN_snap_jpeg_enable`, records of 152 bytes) and calls `yy_media_av130_video_get_frame` (0x29ff94), the generic
  "one encoded frame of a media channel" routine: `ak_vi_get_frame(vi chn)` (up to 3 tries), `ak_venc_encode_frame(handle, frame word 0, frame word 1,
  frame word 8, &stream)`, copy `stream.data/len` into a `yy_mem_hook_malloc` buffer, `ak_venc_release_stream`, `ak_vi_release_frame`;
  `yy_media_av130_video_release_jpeg` (0x2a0794) frees the copy. So a "JPEG snapshot" is **one `ak_venc_encode_frame` call on a VI frame with an encoder
  that was opened as JPEG**. The same handle table (`ctx + (258 + chn) * 4 + 4`, 8 handles at most inside `libmpi_venc`) holds H.264 and JPEG encoders.
- **How an encoder is made a JPEG one** (the single `ak_venc_open_ex` call site, 0x29e6d4, fed by the channel config): `video_chnN_enc_type` is the string
  `H264` → 0, `H265` → 2, `MJPEG` → **1** (`venc_param` +36); `video_chnN_enc_profile` is a strcmp chain `Baseline` 2, `Main` 0, `High` 1, `CBase` 3,
  `HevcMain` 4, `HevcMainStill` 5, `HevcMainIntra` 6, **`Jpeg` 7** (`venc_param` +12); `video_chnN_snap_jpeg_quality` (default 1) goes to `venc_param`
  +28. The channels 2 and 3 of the media table (`video_chn2/3_*`, whose RTSP types list JPEG/MJPEG) are where the stock can host such an encoder; the
  default config enables none of it (only chn 0/1 are encoded).
- **What `venc_check_and_format_param` (libmpi_venc 0x2534) requires of a JPEG encoder** (strings "profile error, not jpeg profile", "jpeg minqp error, can't
  smaller than 1", "jpeg maxqp error, can't larger than %d", "jpeg_qlevel error, can't larger than %d", "chroma_mode error"): `enc_type` must be 0 or 1;
  for type 1 the profile must be 7; min QP (+22) >= 1, max QP (+24) <= 100, min <= max; JPEG quality level (+28) **0..3** (a level, not a percentage:
  the stock default is 1); chroma mode (+32) <= 2; width a multiple of 32 and height a multiple of 8 (640x360 is both). Whether the bit-rate fields are
  ignored for JPEG is not known (our open call passes the sub stream's valid values).
- **Init QP rule (HARDWARE, 2026-11-15, 6123960 build):** the first open failed with the vendor log `venc_check_and_format_param:512 initqp error, can't
  smaller than 10 or can't larger than 100` then `ak_venc_open_ex:2633 param is invalid`, rc 0x0D000003 (218103811), handle -1. With `init_qp` 0 (our first guess,
  "the lib takes the mean") the open is refused: the init QP must be **10..100** for every encoder, JPEG included, independently of the JPEG min QP >= 1 rule
  above. The app now sets init_qp 50 (min 1, max 100). The stock chn defaults are init_qp 100 (main) and 41 (sub) with QP 32..51/50; the stock JPEG channel
  default is not known. The refusal was NOT a coexistence problem: both H.264 encoders were open and streaming fine (14.2 fps, 0 dropped).
- **Which frame size?** The encoder has a fixed width/height; the frame data pointer and length (frame words 0 and 1) are what the lib is given, so the
  JPEG encoder must be opened at the size of the VI channel whose frame it encodes. The app uses the sub channel (640x360, a frame its capture
  thread already holds) rather than a second reader on the queue.
- **UNVERIFIED on hardware:** that a third `ak_venc_open_ex` (JPEG) succeeds while the two H.264 encoders and the NPU are running (the lib's handle table has
  8 slots, the DMA pool is the limit), the real JPEG size at level 1 (a typical 640x360 JPEG is 30..60 KB; the app refuses anything over 200 KB), and the
  encode time. The app opens the encoder at start; a refusal is logged once (`ak_venc_open_ex(JPEG ...) = <rc>`) and only disables the snapshots.
