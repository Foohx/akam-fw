# Live encoder parameters, LSC Rotatable 2K (AK3918AV130, `libmpi_venc.so`)

Analysis of `dist/rot2k-app/vendor/appfs/lib/libmpi_venc.so` (stripped ARM, objdump on the exports; no headers shipped, nothing downloaded,
no camera access). Our current use: `ak_venc_open_ex(struct venc_param*, ...)` once per channel (`cap.c`, `abi.h`).
Confidence: HIGH = read straight from the disassembly, MEDIUM = inferred from strings/consistency, LOW = guess.

## 1. Common shape of the setters

Every `ak_venc_*` runtime function does: `rwlock_rdlock(g_tab)`, handle `h` must be 0..7 and `g_tab[h]` non-NULL (else return `0x0d000004`,
"venc user is NULL"), `mutex_lock(user+0x60)`, then ONE `ioctl(user->fd /*+0xdc*/, cmd, arg)`, unlock. ioctl failure returns -1 (after an
`ak_print` "IOC_* fail"); a bad argument returns `0x0d000003`. None checks a "stopped" state or closes anything: all apply to a running encoder
(HIGH for the lib side; whether the kernel driver accepts the value mid-GOP is MEDIUM/unknown, hence the test plan). The first word of every
ioctl struct is the encoder id (`user+0`), filled by the lib.

## 2. Exports and ABI (offsets from the disassembly)

| Function | Args (handle first) | ioctl (cmd, struct) | Checks / notes | Conf. |
|---|---|---|---|---|
| `ak_venc_set_fps` 0x46a0 | `(h, int fps)` | 0x400856e3, 8 B `{id, fps}` | fps <= 0 gives `0x0d000003`. Message IOC_SET_FRAME_RATE. Encoder side only: the vi channel fps is separate | HIGH |
| `ak_venc_set_qp` 0x54a0 | `(h, int min_qp, int max_qp)` | 0x400c56fb, 12 B `{id, min, max}` | max <= 51, min <= max, 10 <= min <= 51, else `0x0d000003` | HIGH |
| `ak_venc_reset_qp` 0x5654 | `(h)` | 0x400456fc | restores the open-time QP range | MEDIUM |
| `ak_venc_set_rc_param` 0x8e7c | `(h, const struct *p)` | 0x400256c7, `p` passed through | `p` NULL or `*(u16*)p > 51` gives `0x0d000003` ("uSliceQP error"): struct starts with a u16 slice QP (constant-QP value). Size unknown | MEDIUM |
| `ak_venc_get_rc_stat` 0x9010 | `(h, out)` | 0x400856c6, 8 B | current rate-control status | MEDIUM |
| `ak_venc_set_rc_type` 0x91e4 | `(h, int type)` | 0x400456c5, 4 B | type must be 0 or 1, else `0x0d000003`. Probably CBR/VBR toggle, mapping not proven | MEDIUM |
| `ak_venc_get_cur_qp` 0x8cf8 | `(h, out)` | 0x400456c8 | read-only, useful to observe rate control | MEDIUM |
| `ak_venc_set_iframe_param` 0x5c34 | `(h, iminqp, imaxqp, ...)` | 0x401056f9, 16 B | I-frame QP bounds (strings `IFrame_minqp/maxQP`), 10..51 style checks | MEDIUM |
| `ak_venc_request_idr` 0x452c | `(h)` | 0x400456f3 | already used by us | HIGH |
| `ak_venc_set_attr` 0x6538 / `_ex` 0x615c | `(h, const struct venc_param *p [, const venc_ext *e])`; `set_attr` tail-calls `_ex(h,p,NULL)` | 0x406056f2, 96 B | the generic live path, see below | HIGH (flow), MEDIUM (kernel acceptance) |
| `ak_venc_get_attr(_ex)` 0x668c / 0x6540 | `(h, out venc_param [, ext])` | none | copies the 64 B cache at `user+4` | HIGH |
| `ak_venc_set_spec_param` 0x4b84 | `(h, type, p)` | 0x402056ca, 32 B | per-codec special params (smart/SRD), not needed | LOW |
| `ak_venc_set_function` 0x516c | `(h, u32 flags)`, arg < 0x400 | 0x400856fd | feature flags, meaning unknown | LOW |
| `ak_venc_strictlylimit_frameSize` 0x5f7c | `(h, limit)` | 0x400856f7 | already used by us | HIGH |

There is NO `set_kbps`, `set_gop` or `set_rc_mode` export. Bitrate, GOP and rc mode change only through `set_attr`.

### `ak_venc_set_attr_ex(h, p, ext)` flow (HIGH)
1. `p` NULL gives `0x0d000003`; `ext` (28 B, our `venc_ext`) is optional.
2. `venc_check_and_format_param` (0x2534, same validator as open): enc_type 0/1, width/height range, fps (+4) and gop (+6) tied by a ladder
   (25/30/50/60 thresholds), gop <= 1000, init_qp (+20) 10..100, qp (+22,+24) <= 51 for H.264/5 (1..100 for JPEG), min <= max, target_kbps (+8) vs max_kbps (+10)
   ("birate error"), smart_goplen (+52) <= 1000, smart_q <= 100, smart_static <= 768, rc_mode (+16) <= 4.
3. If width/height (word 0), profile (+12) or enc_type (+36) differ from the cached params, it recomputes buffer sizes and returns `0x0d000011`
   when the new frame needs more than the stream buffer allocated at open (`user+0xb0`). If only fps/gop/kbps/qp/rc_mode differ, this is skipped.
4. `ioctl(fd, 0x406056f2, {id, converted 0x5c})`; on success updates the cache at `user+4` (and ext at `user+0x44`). ioctl error returns `0x0d000012`.

Return codes: 0 ok, -1 setter ioctl failed, `0x0d000003` bad param, `0x0d000004` bad handle, `0x0d000011` buffer too small, `0x0d000012` set_attr ioctl failed.

## 3. Mapping of our settings (fields of `struct venc_param`, `abi.h`)

| Setting | Field | Live call | Conf. |
|---|---|---|---|
| fps | +4 | `ak_venc_set_fps(h, fps)` (encoder only; lower the vi channel fps too if needed) | HIGH call, MEDIUM effect |
| target / max kbps | +8, +10 | `ak_venc_get_attr(h,&p); p.target_kbps=..; p.max_kbps=..; ak_venc_set_attr(h,&p)` | HIGH call, MEDIUM that the driver applies it live |
| gop | +6 | same get/modify/`set_attr` (validator ties gop to fps) | same |
| min / max QP | +22, +24 | `ak_venc_set_qp(h, min, max)` (min 10..51, max <= 51), or `set_attr` | HIGH |
| init QP | +20 | `set_attr` only, probably effective only at the next rate-control restart | LOW |
| rc mode CBR/VBR | +16 | `ak_venc_set_rc_type(h, 0/1)` (assumed 0 CBR, 1 VBR) or `set_attr` | MEDIUM |
| rc mode CONST_QP/AVBR/LOW_LATENCY | +16 | `set_attr` rc_mode 2/3/4 (+ `set_rc_param`), untested: treat as re-open only | LOW |
| IDR on demand | n/a | `ak_venc_request_idr(h)` (already used) | HIGH |
| resolution, profile, enc_type | +0..3, +12, +36 | re-open only: stream buffer sized at open (`0x0d000011`), SPS/codec change | HIGH |
| chroma_mode, quality_lvl, smart_*, gdr | +32.. | re-open only (not needed, untested through `set_attr`) | LOW |

## 4. Minimal hardware test sequence (one setter at a time)

Run from RAM; the app may crash and the supervisor restarts it. Add a debug-only trigger (a file polled in /tmp) that calls ONE setter, on the
sub channel (640x360) first, then main. Between steps wait 10 s with an RTSP client and the stream stats (bitrate per second, frame count, SPS/IDR
logs) running. Always log the return code. Restore the original value after each step.

1. Baseline: `ak_venc_get_attr` on both handles, compare with what we passed to open (confirms the 64 B layout). Read `get_cur_qp` and `get_rc_stat`. No state change.
2. `ak_venc_request_idr` (known good), confirms the helper path.
3. `ak_venc_set_fps(sub, 10)` then back to 15. Observe: rc 0, RTSP frame rate, no stall, stream still decodes, lower sub bitrate.
4. `ak_venc_set_qp(sub, 32, 45)` then back to (32, 50). Observe: rc 0, bitrate drift over 10 s, `get_cur_qp` within bounds. -1 means the driver refuses.
5. `get_attr`, halve `target_kbps`/`max_kbps` (sub), `ak_venc_set_attr`. Observe: rc, measured bitrate converges within a few GOPs, SPS unchanged (a new IDR is fine), client keeps playing.
6. Same with `gop` (15 to 30): observe the IDR interval and rc.
7. `ak_venc_set_rc_type(sub, 0)` then `1`: observe bitrate variance (CBR flatter) and rc. Only after steps 3 to 6 pass.
8. Repeat 3 to 6 on main (2304x1296) with smaller steps (about 20%), since a bad value there can stall the encoder.

Stop on: any `0x0d0000xx`, ioctl -1, frozen frames over 2 s, or a supervisor restart. No flash is touched. If step 5 fails, bitrate/gop stay
re-open only (current close/open behaviour).

## 5. Limits
- The kernel driver is not available: runtime acceptance of each ioctl is unverified.
- `set_rc_param` struct size, `set_rc_type` value meaning, `set_iframe_param` full arguments and the exact fps/gop ladder come from partial disassembly: verify with `get_*` readbacks.
- Stock `yy_cam` uses none of these setters (see `archive/rot2k-own-app-feasibility.md`); no public SDK header was found offline in the repo.

## 6. Implementation (app, `vlive.c`)
- The capture thread tries the live path before the re-open (`a2k_vlive_apply`): fps -> `set_fps`; QP -> `set_qp`; rc vbr/cbr -> `set_rc_type(h, rc_mode)`;
  kbps / peak / gop -> `get_attr`, edit, `set_attr` (fps, QP and rc taken from the running copy, width/height/profile/enc_type left as `get_attr` returned them).
  avbr (either side) is always a re-open. Any non-zero return: logged once, that setter is switched off for the run, and the existing re-open runs with the same `want`.
- `video_live=0` in akam2k.conf (`--video-live 0`) forces the re-open for everything (default 1).
- Hardware test hook: `--venc-test 1` (default off), the capture threads poll `/tmp/akam2k_venc_test`. One line per file, `main|sub` then `fps N`, `qp MIN MAX`,
  `kbps TARGET MAX`, `gop N`, `rc cbr|vbr` or `attr` (logs `get_attr`, `get_cur_qp`, `get_rc_stat` words). It runs the live setter only (no fallback, flags ignored),
  logs the return code (`venc-test:` lines), then deletes the file. Example from a shell on the camera: `echo "sub fps 10" > /tmp/akam2k_venc_test`.

## 7. Hardware result 2026-11-15 (RAM run, --venc-test)

| Setter | Return | Effect |
|---|---|---|
| set_fps (sub 10) | -1 (ioctl refused) | none, fps stays -> fps = re-open only |
| set_qp (sub 32..45) | 0 | not measurable on a static scene (get_attr cache not updated by set_qp) |
| set_attr kbps (main 2000/2500 -> 1000/1250 -> back) | 0 | bitrate 1650 -> ~800 kbit/s within 10 s, back to ~1650; no drop, no stall |
| set_attr gop (sub 30 -> 15) | 0 | sub keyframe every ~14 frames, main unchanged |
| set_rc_type (sub cbr, vbr) | 0 | effect not verified (cache not updated) |

The app stayed up throughout; both streams also dropped to 10 fps at dusk (sensor exposure, not a setter).

## 8. Sub stream size limit: 640x480 at most (hardware, 2026-11-18, RAM-only run)

Question: can the sub stream (stream1) be bigger than 640x360 (the stock size)? A `sub_res` boot option was tried for 960x540 and 1280x720 and run once on the 2K (RAM-only run of the
dev binary). Result: **no. On this SoC configuration the sub channel is limited to 640x480 (YUV420 460800 B).**

| sub size | `ak_vi_set_dev_attr` | `ak_venc_open_ex` chn 1 (Main and Baseline) |
|---|---|---|
| 1280x720 | warns `The 3DNR yuv size is out of range [460800:1843200][115200:460800][115200:460800] bytes` | `Venc [1] Create DMA Pool failed!`, rc 218103819 |
| 960x540 | same 3DNR warning | rc 218103811 |

The three ranges of the warning are the 3DNR buffer limits of the three VI channels (main 460800..1843200 B, sub 115200..460800 B, third 115200..460800 B). The sub maximum is 460800 B
= 640x480 YUV420, so 640x360 (345600 B) fits and 960x540 (777600 B) and 1280x720 (1382400 B) do not; the encoder DMA pool for the channel then cannot be created either. The board has
64 MB RAM and the media reserve is fixed by the boot args (not to be changed), so there is no room to lift it. The in-app fallback marker (RAM file, restart at 640x360) worked as designed.

Consequences, as shipped: the 960x540 and 1280x720 sizes and `akam2k_subres.c` were removed. `sub_res=640x360` is still accepted in `akam2k.conf` (no effect); any other value is logged
`unsupported on this SoC (sub max 640x480: 3DNR buffer / venc DMA pool)` and ignored. The sub now runs H.264 Main (the main channel proves Main with this encoder; the app retries Baseline
if `ak_venc_open_ex` refuses Main for the sub, logged), default 800 kbit/s target (peak 1000), QP 26..44. `sub_kbps` / `video1_*` still win (sub range 32..1000, peak 32..1500). The 128 KiB sub
frame cap is kept: 640x360 at QP 26 is about 1/9 of the main pixels, and the 128 KiB limit holds an IDR up to about 1 Mbit/s (the peak is 1000 kbit/s); the JPEG snapshot path is unchanged
(640x360, a multiple of 32 x 8).
