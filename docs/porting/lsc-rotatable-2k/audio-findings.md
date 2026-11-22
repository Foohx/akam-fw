# Audio on the LSC Rotatable 2K — offline study (2026-11-15)

Goal: microphone into the RTSP stream of our own `akam2k_rtsp` (which replaced `yy_cam`), speaker/backchannel later.
**Offline reverse engineering only** (user's own firmware backup, extracted `yy_cam`, `appfs/lib/*.so`, `ak_pcm.ko`; no
camera access, no code change). Nothing here has run on the camera yet. Confidence: **HIGH** = read directly from
disassembly or data, **MEDIUM** = inferred from code plus naming, **UNVERIFIED** = guess or needs a hardware check.
Addresses are `yy_cam` virtual addresses unless a library is named. Offsets in `ak_*` structs are the stock call-site
view, not a header.

## 1. Stock audio path (what `yy_cam` does)

### 1.1 Vendor API used (HIGH)

`yy_cam` imports `ak_ai_open/close/start_capture/stop_capture/get_frame/release_frame/set_source/set_frame_length/
set_gain/set_volume/set_nr_attr/set_agc_attr/set_aslc_attr/set_aec_attr/set_lib_print_level`, `ak_ao_open/close/
send_frame/flush/set_volume/set_gain`, `ak_adec_*` (speaker decode) and `ak_print_set_level`. It does **not** import any
`ak_aenc_*`, `ak_ai_enable_nr/agc/aec`, or `ak_ai_set_driver_buf_periods`. Providers: `libplat_ai.so` (V1.4.02, ai),
`libplat_ao.so`, `libplat_vqe.so` + `libakaudiofilter.so` (V2.03.08, noise reduction / AGC / AEC filter),
`libplat_audio_effect.so` (V1.1.07), `libplat_osal.so` (device layer), `libmpi_adec.so`. Wrapper code is
`yy_media_av130_audio.cpp` (`yy_media_av130_audio_*`, 0x297a38..0x29a000).

### 1.2 Capture bring-up (HIGH, `yy_media_av130_audio_init` 0x297a38 -> init_ai 0x297140)

1. `yy_media_av130_audio_init` allocates a 616-byte context, reads the config (below), then `init_ai`.
2. `ak_ai_open(&pcm_param, &handle)` with `pcm_param = {sample_rate, bits, channels(1 mono / 2 stereo), 0}` (16 bytes, at
   fp-32); the handle is stored at context+596. Failure -> `-1`.
3. `ak_ai_set_source(handle, 0)` (0 = microphone, MEDIUM).
4. `ak_ai_set_frame_length(handle, bits*num_per_frm/8)` = **640 bytes** with defaults (320 samples = 40 ms at 8 kHz).
5. `yy_media_av130_audio_set_ai_vol` (0x2987c8) and `_set_ai_gain` (0x2988d0), see 1.4.
6. `ak_ai_set_nr_attr`, `ak_ai_set_agc_attr`, `ak_ai_set_aslc_attr`, `ak_ai_set_aec_attr` with static tables (1.5). Failures
   here are logged and **not fatal**.
7. `ak_ai_start_capture(handle)` (failure closes the handle, `-6`).
8. `ak_print_set_level(5, 1)` and `ak_ai_set_lib_print_level(handle, 1)` (library log verbosity).

Close (0x297730): `ak_ai_stop_capture`, `ak_ai_close`, handle = -1. `yy_media_av130_audio_deinit` (0x297bf0) calls it, then frees
the context. Nothing in this path touches video; the audio init order relative to `ak_vi_*`/`ak_venc_*` is **not
constrained by the code** (the thread `Y_MEDIA_AI` just starts when `yy_media_init` runs; HIGH for "no call to a video
function inside audio init", UNVERIFIED for hidden coupling inside the libs, see risks).

### 1.3 Parameters and defaults (HIGH, `yy_conf_get_int/str`, section `[dsp]`, 0x296c14 and 0x296d6c)

Read from `/config/param.conf` (fallback defaults below). The stock `param.conf` has no audio keys set on our unit as far
as the existing notes go (the table in `stock-firmware-reference.md` only lists `ao_vol`), so the defaults apply.

| Key `[dsp]` | Default | Meaning |
|---|---|---|
| `ai_sample_rate` | 8000 | Hz |
| `ai_bitwidth` | 16 | bits |
| `ai_channel_mode` | `mono` (`stereo` also accepted, anything else logs "not support") | channels |
| `ai_frm_num` | 25 | frames queued (ring depth, context +16) |
| `ai_num_per_frm` | 320 | samples per frame (40 ms) |
| `ai_enc_type` | `g711u` (`g711a`, `pcm`; anything else -> `pcm` with a log line) | output encoding |
| `ai_vol` | 50 | see 1.4 |
| `ai_gain` | 50 | see 1.4 |
| `ao_sample_rate` / `ao_bitwidth` / `ao_channel_mode` | 8000 / 16 / `mono` | speaker format |
| `ao_frm_num` / `ao_num_per_frm` | 25 / 320 | speaker frames |
| `ao_vol` / `ao_vol_offset` / `ao_gain` | 80 / 30 / 80 | speaker levels |
| `ao_dec_bufSize` | 20 | speaker decode buffer |

Speaker enable GPIO keys (section `[spk]`, `yy_media_init` 0x2953a0): `spk_no` default **9**, `spk_polarity` default **1**
(HIGH). Matches the existing GPIO map entry "9 = speaker enable".

### 1.4 Volume and gain (HIGH for the formula, UNVERIFIED for the unit)

- `ai_vol`: `ak_ai_set_volume(handle, ai_vol * 110 / 100 - 90)` (0x2987c8): 50 -> -35, range -90..+20. Logged as `ai_vol = %d`.
  The unit of the SDK argument is not documented in the lib strings.
- `ai_gain`: `g = ai_gain * 8 / 100`; if `g < 0` or `g > 8` then `g = 4`; `ak_ai_set_gain(handle, g)` (0x2988d0): default 50 -> **4**
  (analog mic gain step; the kernel module has `set_mic_gain`/`set_mic_power`). A failure closes the handle.
- Speaker: `ak_ao_set_volume` (0x299174) and `ak_ao_set_gain` (0x2993f4); `default_ao_gain` table = {3, 4}. Exact mapping:
  UNVERIFIED (not needed for the microphone).

### 1.5 NR / AGC / ASLC / AEC (data at fixed addresses, layout MEDIUM)

Sizes follow from the lib copy lengths (`ak_ai_set_nr_attr` copies 12 bytes, `..agc_attr` 20, `..aec_attr` 24 and reads
halfwords at +4/+6/+12 plus words at +16/+20). Raw initial values read from `.data`:

| Table | Address | Raw words (hex) |
|---|---|---|
| `user_ai_nr_attr` | 0x39e5ac (12 B) | `0xffd8, 0, 1` |
| `user_ai_agc_attr` | 0x39e5b8 (20 B) | `0x6000, 4, 0x50, ...` |
| `user_ai_aec_attr` | 0x39e5cc (24 B) | `0, 0x04000400, 0, 0x200, ...` |
| `user_ai_aslc_attr` | 0x39e5e4 (8 B) | `0x8000, 9`; the code overwrites it to `{0x8000, -8}` before the call |
| `default_ai_eq_attr` | 0x39e5f0 | `{0,10,50,63,125,250,500,1000}` (not applied in the audio init) |

**Finding (MEDIUM):** the stock calls the `set_*_attr` functions only. `ak_ai_enable_nr/agc/aec` (which map to
`AK_MPI_AI_EnableVqe`) are **not imported**, so the VQE chain is probably configured but **not enabled**: stock RTSP
microphone audio is likely raw ADC PCM, G.711-encoded. AEC is also off in effect (`user_ai_aec_attr` starts with 0, and
the stock has no far-end loopback feed in this path). Confirm with a hardware A/B (section 6, step 3). Do not enable AEC
before speaker support exists.

### 1.6 Frame read (HIGH, `..._get_ai_frame` 0x298324, `..._not_malloc_get_ai_frame` 0x297f78)

- `ak_ai_get_frame(handle, &frame, 0)` with a 24-byte `frame`; third argument 0 in both callers (MEDIUM that 0 = blocking
  in the Anyka SDK; the stock thread also sleeps 5 ms per loop, so it is robust either way).
- `frame` as used: `+0` data pointer, `+4` length (error "ak_ai_get_frame len == 0" if 0), `+8` 64-bit timestamp, `+16` sequence number.
  A frame longer than 640 bytes ("data is too large") is released and dropped.
- Encode: `ai_enc_type` `g711u` -> `yy_media_av130_audio_pcm_to_g711u` (0x299960), `g711a` -> `..._pcm_to_g711a` (0x2996d0), `pcm` -> copy.
  Own table-based G.711 code in `yy_cam` (no `ak_aenc_*`, no `libmpi_aenc` call): HIGH. Output 640 PCM bytes -> **320 G.711 bytes**
  per 40 ms.
- The frame is wrapped in a 52-byte header + payload (container type 105 = g711u, 106 = g711a, 101 = pcm; `+8` sequence,
  `+16` raw driver timestamp, `+24` timestamp x 1000, `+32` type, `+36` = 3, `+48` length). The driver timestamp is in
  milliseconds, the x1000 copy in microseconds (HIGH on the arithmetic, MEDIUM that the first is ms).
- `ak_ai_release_frame(handle, &frame)` right after the copy.

### 1.7 Threading and fan-out (HIGH)

Thread `Y_MEDIA_AI` (`yy_media_ai_th` 0x294c54): loop `..._not_malloc_get_ai_frame` -> `yy_rb_hook_write` into a ring
buffer (items up to 1280 bytes), `usleep(5000)` after each pass (10 ms on a write failure). The RTSP manager reads it with
`yy_rtsp_get_audio_frame` (0x256f48: `yy_rb_hook_read`, 10 ms sleep when empty). The RTSP server library
(`librtsp_server.so`) has an SDP builder for `PCMU`/`PCMA` (`m=audio 0 RTP/AVP 0` + `a=rtpmap:0 PCMU/8000`) and for
`mpeg4-generic` (AAC); the stock default produces PCMU (HW: measured earlier, `G.711 mu-law 8 kHz mono`, PLAN section 5).

### 1.8 A/V timestamps for RTSP (partly UNVERIFIED)

The RTSP callbacks (`yy_rtsp_get_frame_cb` 0x257188, `..get_sample_rate_cb`, `..get_codec_cb`) hand the server library a
64-bit timestamp per frame; for audio it is the driver timestamp x 1000 (1.6). How `librtsp_server.so` converts it into the
RTP timestamp (`rtpHeaderInit`, `rtsp_client_audio_send` 0xccc8) was **not** traced: UNVERIFIED. We do not need it:
`akam2k_rtsp` has its own RTP layer (section 3).

### 1.9 Kernel side and device nodes (HIGH)

`start.sh` loads `/appfs/drivers/ak_pcm.ko` (compatible `anyka,ak3918av130-adc-dac`) before `yy_cam`, so the nodes exist
without any action of ours as long as our card launcher runs after the stock boot script (UNVERIFIED on this exact flow, but
`ak_pcm.ko` is inserted unconditionally in `start.sh` line 42). `libplat_osal.so` opens `/dev/pcmC0D0c` (capture),
`/dev/pcmC0D0p` (playback), `/dev/pcmC0D0l` (loopback, the AEC far-end tap); it also contains `C1`/`C2` variants not used here.
The module's `capture_ioctl` uses the **same ABI as the 1080p driver** that `platforms/anyka-ak3918/audio.c` already drives: it checks
`0x401c50e0` (28-byte parameter struct, period count <= 0x50), `0x80045010`, `0x40045030`, `0x800850e6` and `0x80045030`
(HIGH, `ak_pcm.ko` capture_ioctl at 0x6698). The ioctl `0x40045081` on the playback fd is `osal_ao_enable_speaker`
(HIGH), same as the 1080p `AKPCM_IOC_SET_SPEAKER`.

### 1.10 Speaker amplifier (MEDIUM)

Two switches exist: the driver's `SET_SPEAKER` ioctl (above, issued by `libplat_ao`) and a stock-side GPIO (`yy_gpio_init`/
`yy_gpio_high_frequency_*`, `yy_media_spk_gpio_init` 0x293024, `yy_media_audio_set_spk_onoff` 0x293168 -> `[spk] spk_no`=9,
polarity 1; 1 ms sleep between steps). Which of the two actually gates the amplifier on this board is UNVERIFIED; the
1080p uses the ioctl and sysfs GPIO 69 (`audio_out.c`). Stock also has a "siren" and a talkback state
(`yy_media_audio_set_ao_talkback_status`) plus `ak_adec` (G.711 decode) for the speaker.

## 2. Free of the vendor audio libs: the 1080p path may work as is

The 1080p `platforms/anyka-ak3918/audio.c` never uses a vendor library: it opens `/dev/pcmC0D0c` and replays a captured ioctl
sequence (`SET_STATE(2)`, `GET_PERIODS`, `GET_PARS` with `{0, 8000, 1, 16, 512, 16, 0}`, `RESET_BUF(-1)`, `SET_MODE(0)`,
`SET_RATE(8000)`, `SET_STATE(2)`, `SET_PERIODS(2)`), then `read()`s 512-byte periods in a thread (hardware validated on the
1080p, `specs/audio.md`). Because the 2K's `ak_pcm.ko` accepts the same ioctl numbers (1.9), a first PoC can skip
`libplat_ai`, `libplat_vqe`, `libakaudiofilter` altogether. Two routes:

| | A. Raw ioctl (1080p method) | B. Vendor `ak_ai_*` (stock method) |
|---|---|---|
| Libs mapped | none | `libplat_ai`, `libplat_vqe`, `libakaudiofilter` (232 KB file), `libplat_audio_detect`, `libplat_audio_effect` + already-linked `libplat_osal/thread/dbg` |
| Extra app RSS (est., UNVERIFIED) | < 50 KB (ring 8 x 512 B) | 200..400 KB (lib pages and buffers) |
| Period / format | 8 kHz mono S16, 512 B period (32 ms) | 8 kHz mono S16, 640 B frame (40 ms) |
| Gain / volume | driver `set_capture_gain` ioctl (unmapped), else default | `ak_ai_set_gain/volume` as stock |
| NR / AGC / AEC later | no | yes |
| Risk | ioctl semantics differ subtly on AV130 (period sizes, mic gain default) | lib init order vs video, link list, more RAM |

Recommendation: **PoC with route A** (smallest RSS, no new link dependencies, reuses proven code), keep route B as the
fallback if the raw path reads silence or the mic gain is wrong, or if NR/AGC is wanted. The `hal_audio` interface hides
the choice (section 4).

## 3. What the 1080p `platforms/anyka-ak3918` already provides

Read, not modified. The 2K app (`platforms/anyka-ak3918av130/rtsp_session.c`) currently sets
`sp.audio_off = 1`.

**Reusable unchanged (phase A of PLAN 8ter), compile from their current place:**

| File | Use for audio |
|---|---|
| `g711.c/.h` | `g711_ulaw_encode_buf`, `g711_alaw_encode_buf`, decode for the backchannel; pure code, host-tested (`tests/g711_test.sh`) |
| `rtp_clock.c/.h` | `rtp_aclock_*` (audio sample counter -> monotonic time for RTCP SR), `rtp_astart_arm/release/holding/stale` (hold audio until the first video frame, drop backlog), `rtp_mono_us` — already linked by the 2K app for video |
| `rtsp_sdp.c/.h` | builds the audio section when `audio_off = 0`: **L16/8000/1, PT 97, `streamid=1`** (plus `backchannel` PCMU/PCMA); it has **no PCMU media section** for the downstream |
| `audio_jitter.c/.h`, `audio_vad.c/.h` | only for the backchannel/speaker (jitter buffer 120 ms prefill, half-duplex VAD); not needed for the microphone |

**Not reusable as is:**

- `audio.c`: includes `platform.h` (device path macros, 1080p globals) and `audio_out.h`; `audio_start()` also starts the
  playback/loopback/amp code (`audio_out_start`, 1080p GPIO 69). The capture function `audio_init_capture` and the thread
  are the part to copy (phase B extraction candidate: split `audio.c` into a capture-only file; must leave the 1080p binary
  byte-identical).
- `audio_out.c/.h`, `talk.c`: playback and ONVIF backchannel session, 1080p amp/GPIO and control port coupling.
- `rtsp_session.c::send_audio_frame` (line ~1112): the 1080p session logic, already duplicated in `rtsp_session.c` for video (the 2K
  file copies the session, so the audio send code is another copy of ~50 lines: byte-swap to network order, RTP timestamp
  = `audio_ts_base + sample index`, `rtp_aclock_observe`, drop on TCP backpressure).

**RTSP audio format decision.** Reusing `rtsp_sdp.c` unchanged gives **L16 (128 kbit/s)**, what the 1080p ships and what
ffmpeg/VLC/go2rtc accept. Stock gave **PCMU (64 kbit/s)**. Start with L16 (zero shared-code change), and decide later
whether Home Assistant / Frigate users need PCMU: that needs a PCMU variant of the SDP builder (`m=audio 0 RTP/AVP 0`,
`a=rtpmap:0 PCMU/8000`, a static payload type, RTP timestamp in 8 kHz samples = same counter) and is a phase B change to
`rtsp_sdp.c` (1080p output must stay byte-identical, so it is added behind a new parameter that the 1080p never sets).

**2K-specific glue (`hal_audio`)**, a small interface under the 2K directory (no shared code touched):

```
int      hal_audio_open(void);                          /* 0 ok; non-fatal for video */
void     hal_audio_close(void);
int      hal_audio_get_frame_ts(int16_t *buf, size_t max, size_t *n,
                                uint64_t *first_sample, int64_t *t_end_us); /* same contract as audio_get_frame_ts */
int      hal_audio_mic_enabled(void);                   /* SDP: audio_off = !enabled */
/* later: hal_audio_play(const int16_t*, size_t), hal_audio_amp(int on) */
```

Keep the 1080p `audio_get_frame_ts` contract so the copied `send_audio_frame` and the `rtp_astart` gate port unchanged.
Capture thread, 8-slot ring, supervisor with backoff 1/2/4/8/16/32/60 s (never retry a failing syscall faster than 1 s: a
100 Hz retry loop rebooted a 1080p camera), and an opt-in switch (card file, off by default like the other 2K features).

## 4. Memory and CPU budget

- Camera: `MemTotal` ~38 MB, ~14 MB free with video running (feasibility notes: ~20..24 MB without `yy_cam`); `akam2k_rtsp`
  RSS 1.48 MB (HWM 1.8 MB), CPU ~5 % with both H.264 streams.
- Route A: audio adds one thread (stack 64..128 KB virtual, a few KB resident), 8 x 512 B ring, one 512 B copy and one
  512 B byte-swap buffer per client: **< 50 KB RSS**. Route B: add the lib mappings (about 500 KB of file size in total, of which only touched pages count) and the
  620-byte context: budget **+0.3..0.5 MB**, to be measured.
- CPU: 8000 samples/s; PCM copy + byte-swap = 16 kB/s, G.711 table encode = 8 k bytes/s: far below 1 % of an ARM926 core
  (UNVERIFIED, measure `utime`/`stime` delta over 60 s). RTP audio adds ~31 packets/s (L16, 512 B) or 25 (PCMU, 320 B) per
  client.
- Hard limits for the validation: `MemAvailable` stays above 6 MB and RSS delta < 1 MB, otherwise route A only.

## 5. Risks and unknowns

1. **Init order / coupling with video (UNVERIFIED).** Stock inits audio in `yy_media_init` next to video; nothing in the
   audio path calls `ak_vi_*`, but both use `libplat_osal`/`ak_mem_*` and a shared DMA pool. Route A uses neither. If route B
   is used, call `ak_ai_open` after `ak_sdk_init`, then start capture only after video is up, and measure DMA/ION free before
   and after.
2. **Capture ioctl differences on AV130 (MEDIUM).** The numbers match, the 28-byte parameter struct limits match (`<= 0x50`
   periods); period size accepted (512 B in the 1080p) and the mic gain default on this board are not known. The stock lib
   uses 640 B frames; the kernel period it picks (`osal_ai_set_param`) was not traced.
3. **Microphone gain / level.** Without `ak_ai_set_gain` the ADC gain is the driver default; stock sets step 4 (`ai_gain` 50).
   A recording that is quiet or clipped is expected to be fixed by the gain only; check with a level meter (`sox stat`).
4. **VQE/NR state (MEDIUM).** Whether stock really runs with VQE off (1.5) decides if NR/AGC is needed to match the stock
   sound quality. A/B test with the stock `yy_cam` capture is not possible offline.
5. **Speaker enable (UNVERIFIED).** GPIO 9 vs driver ioctl; wrong order may click or stay silent. Do not drive GPIO 9 before a
   capture-only milestone is accepted; hardware autonomy rules apply (reversible steps only).
6. **A/V sync.** The shared `rtp_clock` PLL is validated on the 1080p; for the 2K the video clock is driven by the vendor
   encoder timestamps in `rtsp_session.c`, so check lip sync in VLC after step 4.
7. **Unknown:** how the SoC AEC loopback (`/dev/pcmC0D0l`) is fed in stock; whether `ak_pcm` keeps the mic open while
   `yy_cam` is gone (it should: the driver is a module, not a process); the exact mic power and bias (`set_mic_power` in
   the module) default at boot without `yy_cam`.
8. **Licence/redistribution:** route B only links the on-camera `.so` files (never redistributed, same as video).

## 6. Implementation plan (small, reversible hardware steps)

All steps run from `/tmp` on the camera with the PoC-1 stub (card removal returns stock). Reversal is always "kill the
process, remove the card/reboot". Nothing is written to flash or `/config`.

1. **Offline (no camera):** write `hal_audio` (route A) beside the 2K app; host-compile the shared files it uses; unit-test
   with a fake capture (reuse the 1080p audio tests as a model). `make poc3-symcheck` stays green.
2. **Device check (read-only):** on the camera, `ls -l /dev/pcmC*`, `lsmod | grep ak_pcm`,
   `grep -i pcm /proc/interrupts`. Expect `pcmC0D0c/p/l`. Nothing opened yet.
3. **PoC-A1, 5 s capture to RAM:** tiny tool `akam2k_arec` (route A, replay the ioctl sequence, read 5 s = 80 000 bytes x 2 = 160 kB
   to `/tmp/mic.raw`), `scp` to the PC, play with `ffplay -f s16le -ar 8000 -ac 1 mic.raw`. Pass: intelligible sound, no
   silence, level plausible, camera alive, no reset. Save any result under `test-images/rot2k/` (git-ignored).
   If silent: try route B tool (`ak_ai_open` with stock defaults, section 1.2) the same way.
4. **PoC-A2, level and gain:** repeat in a quiet room and with speech at 1 m; compare with a stock RTSP recording of the
   same scene made earlier (RTSP G.711 from `yy_cam`) if available; tune gain only through a documented knob.
5. **RTSP track (implemented offline, see section 9; `audio=1` in akam2k.conf, hardware check pending):** enable the audio track in `akam2k_rtsp` (card opt-in, `audio_off = 0`), `ffprobe` must report
   `pcm_s16be 8000 Hz mono` + H.264; 10 min soak: `MemAvailable`, VmRSS (< +0.1 MB route A), CPU, no frame drops, lip sync in
   VLC, ffmpeg and go2rtc; two clients; TCP backpressure client (audio dropped, video policy unchanged).
6. **Supervisor and kill test:** unplug-style failure (close the fd externally) must trigger the backoff path, not a reboot.
7. **Optional PCMU downstream:** phase B change to `rtsp_sdp.c` plus `g711_ulaw_encode_buf` in the audio send path, only if
   a target client needs it; the 1080p binary must stay byte-identical.
8. **Speaker/backchannel (later, separate study):** `/dev/pcmC0D0p` with the 1080p `audio_out` sequence, `SET_SPEAKER`,
   GPIO 9 test with the amp muted first, `audio_jitter` + `talk` reuse; AEC via `/dev/pcmC0D0l` only after one-way works.
9. **Document** in `archive/rot2k-own-app-feasibility.md` and `stock-firmware-reference.md`; run both models' test suites.

## 7. Sources (offline)

`yy_cam` disassembly: `yy_media_av130_audio_init` 0x297a38, init_ai 0x297140, config readers 0x296c14/0x296d6c, set_ai_vol
0x2987c8, set_ai_gain 0x2988d0, get_ai_frame 0x298324, `yy_media_ai_th` 0x294c54, `yy_rtsp_get_audio_frame` 0x256f48,
`yy_media_audio_set_spk_onoff` 0x293168, `yy_media_init` speaker keys 0x2953a0. Libraries: `libplat_ai.so` (V1.4.02),
`libplat_osal.so` (`osal_ai_*`, `osal_ao_enable_speaker` 0x4b90, device paths), `librtsp_server.so`, `ak_pcm.ko`
(`capture_ioctl` 0x6698). 1080p reference: `platforms/anyka-ak3918/audio.c`, `ak_ioctls.h`, `rtp_clock.h`, `rtsp_sdp.c`,
`specs/audio.md`.

## 8. PoC tool `akam2k_mic` (plan step 3, manual only)

Source `firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/` (host-tested core `mic_core.c`, unit test
`tests/rot2k_mic_unit.c`). Built by `make -C platforms/anyka-ak3918av130 poc3-dist ...` into
`dist/rot2k-app/akam2k_mic` (symcheck against the camera libc, `mic_*` lines in `BUILDINFO.txt`). It is NOT installed by
`build_card.sh`: copy it by hand. Plain libc, no vendor lib, 13.5 KB stripped.

What it does: opens `/dev/pcmC0D0c` read-only and replays the capture sequence of the 1080p `audio.c`
(`SET_STATE(2)`, `GET_PERIODS`, `GET_PARS` = commit of `struct akpcm_pars` {0, rate, 1, 16, 512 B, 16 periods},
`RESET_BUF(-1)`, `SET_MODE(0)`, `SET_RATE`, `SET_STATE(2)`, `SET_PERIODS(2)`), reads 512-byte periods, writes a mono s16le WAV
(header fixed up at the end) and prints bytes, frames, effective rate (CLOCK_MONOTONIC), min/max/peak/RMS in dBFS, read
errors and the period it got. It writes only OUTFILE (`/tmp/NAME` unless `-f`), never opens the playback or loopback node,
never touches GPIO 9. No `-g`: the only gain ioctl known from the 1080p tree is the playback one (a microphone gain stays
step 4 of the plan). `-r 16000` is untested on the hardware. A `flock` refuses a second `akam2k_mic` only: it cannot see
another capture user, so stop anything else that holds the microphone first (with the Akam stub running, `akam2k_rtsp` does
not open it yet). At most 5 consecutive read errors, an alarm of SECONDS+10 s, SIGINT/SIGTERM finalise the file.

Manual test (the camera stays untouched except /tmp, a reboot or removing the card undoes everything):

```sh
# on the PC
scp dist/rot2k-app/akam2k_mic root@CAMERA:/tmp/        # or: ssh root@CAMERA 'cat > /tmp/akam2k_mic' < dist/rot2k-app/akam2k_mic
ssh root@CAMERA 'chmod 755 /tmp/akam2k_mic && /tmp/akam2k_mic -s 5 /tmp/mic.wav'     # read the stats on stderr
ssh root@CAMERA 'cat /tmp/mic.wav' > test-images/rot2k/mic.wav                        # no scp needed
ssh root@CAMERA 'rm -f /tmp/mic.wav /tmp/akam2k_mic'
ffplay test-images/rot2k/mic.wav
```

Pass: exit 0, 80000 bytes for 5 s at 8 kHz, effective rate within 1 % of 8000, no read errors, RMS well above -90 dBFS and
peak below -1 dBFS, intelligible speech. Do it once in a quiet room and once speaking at 1 m. If a step fails, the tool says
which ioctl and stops; do not retry in a loop (the 1080p was once rebooted by a 100 Hz retry).

### 8.1 Hardware result (plan step 3: DONE)

Hardware 2026-11-15 (own-app mode, akam2k_rtsp running, mic not used by it): the ioctl sequence is accepted, 8000 Hz mono 16 bit, period 512 B x 16 echoed, 40000 frames in 4.992 s (effective 7961.5 Hz, first read excluded), 157 reads of 128..512 B, 0 errors. Quiet room: RMS about -55 dBFS, peak -42 dBFS, DC offset about -59 LSB, live signal (about 270-340 distinct sample values per second). Speech level / gain not yet measured (needs the user).

Next: the level check with a voice (speaking at 1 m). The RTSP track is implemented, see section 9.

## 9. RTSP track (plan step 5: IMPLEMENTED, verified on the camera 2026-11-15)

**Enable:** put `audio=1` in `akam/akam2k.conf` on the card (read once at boot by `own_app_run.sh`, passed as `--audio 1`), then reboot
(or restart the supervisor). **Default is off** (privacy, Akam vision: opt-in): no key or `audio=0` means the capture device is never
opened, no thread runs, the SDP is exactly what it was (video only). Any other value is ignored and logged by the supervisor. Both streams
(`/stream0`, `/stream1`) then carry the track: `ffprobe rtsp://<ip>:554/stream0` must show H.264 + `pcm_s16be 8000 Hz mono`.

**Design (all in `platforms/anyka-ak3918av130/`, nothing under `src/` touched):**

- `audio.c` (the only app file that names `/dev/pcmC0D0c` and calls `ioctl`, narrow audit in `tests/rot2k_overlay_test.sh`): one thread,
  read-only open + `flock`, the exact ioctl sequence of `helpers/mic/mic.c` (8000 Hz mono 16 bit, 512 B periods), one `read()` of up to 512 B
  per period. Open failure or 5 consecutive read errors: logged, device closed, retry with backoff 1, 2, 4, 8, 16, 32, then 60 s (the lost
  time is added to the stream position so the RTP timestamp jumps instead of overlapping, as the 1080p `audio.c`). The video never depends on
  it; while it is down the SDP has no audio track and a SETUP for it is a 404.
- `aring.c` (pure, host-tested): one ring of 32 periods (about 17 KiB, 1.024 s), one producer, any number of readers with their own
  cursor. A slow client loses the oldest periods (told "lost", continues at the oldest held); the producer never blocks (mutex held for a 512 B
  copy, wake pipes are non-blocking). Each period carries its first-sample index and the `CLOCK_MONOTONIC` time its `read()` returned.
- `rtsp_session.c`: the send logic of `send_audio_frame` of the 1080p `rtsp_session.c` duplicated (that file is not reusable). SDP from the shared
  `rtsp_sdp.c`, unchanged: `m=audio 0 RTP/AVP 97`, `L16/8000/1`, `a=control:streamid=1`, offered only while the capture is up. RTP over TCP
  interleaved, payload = samples in network byte order, marker set, one packet per period (at most 512 B).
- **Timestamps / A/V sync (the 1080p scheme, same shared `rtp_clock.c`):** audio RTP timestamp = per-session random origin + capture-stream
  sample index (exact sample counter, 8 kHz clock, no wall time). Video keeps its existing `rtp_ticks_from_us(capture_us)` (90 kHz). `rtp_astart_t`
  holds audio at PLAY until the first video frame of the session is sent, then drops every period captured before that frame's `capture_us`
  (audio starts within one period of video, not 1 s of ring backlog ahead); re-armed after a TCP stall resync. `rtp_aclock_t` maps the sample
  counter to monotonic time for the RTCP Sender Reports. **The SRs are new for the 2K app and sent only in a session that has the audio track**
  (every 5 s on the video and the audio channel, 1080p logic; the 1080p history says VLC drops the audio for good without any SR). A video-only
  session sends none, as before.
- Backpressure: while the client's video pacing is "skipping" the audio periods are consumed but not sent (timestamps keep their place); an
  ENOBUFS on the TCP queue drops that packet only. A client that SETUPs only the audio track works (no video pump, no gate).
- Shutdown: `a2k_audio_stop` runs after the RTSP clients, waits at most 2 s for the thread, closes the device.

**Budget (estimate, to be measured):** RSS +17 KiB ring (BSS, untouched when off) plus a 64 KiB thread stack (virtual, little touched) and about
1 KB per client; CPU: one wake per 32 ms per audio client (about 31/s) plus 16 kB/s of payload per client, expected well under 1 % of the core;
network +128 kbit/s plus RTP/TCP overhead per audio client.

**To verify on the hardware (step 5 soak):** `ffprobe` shows the track; 10 min soak (`MemAvailable`, VmRSS, CPU, no video frame drops); lip sync in
VLC, ffmpeg and go2rtc (the SRs are untested against a real player); two clients on both streams; a TCP backpressure client; kill test (step 6:
the capture fd closed externally must hit the backoff path). PCMU (step 7) still needs a `rtsp_sdp.c` phase-B change and is not part of this step.
`akam2k_mic` and the app cannot capture at once (the `flock` refuses the second).

### 9.1 Hardware result (2026-11-15, build 24267e0b, `audio=1`)

- Boot: `audio: capture running (/dev/pcmC0D0c, 8000 Hz mono 16 bit, 512-byte periods)`, about 31.4 periods/s, 0 reopens.
- `ffprobe stream0`: H.264 Main 2304x1296 + `pcm_s16be, 8000 Hz, mono, 128 kb/s`.
- 60 s recording with ffmpeg (`test-images/rot2k/2026-11-15_audio_rtsp_60s.{mkv,wav}`, not in git): 649 video and 1876 audio packets (1876 x 256 samples = 60.0 s); last video pts 59.959 s, last audio pts 59.975 s (no measurable drift over a minute). Quiet room: RMS -52 dBFS, peak -30 dBFS, no DC offset after decoding.
- Camera during and after: stream0 10.0 fps, 0 dropped frames; VmRSS 1844 kB (about +30 kB against the build without audio), CPU 19 % with clients, MemAvailable unchanged.
- Two simultaneous clients (stream0 + stream1, 20 s): both receive video (201 / 200 packets) and audio (624 / 604 packets).
- SIGKILL of the app while streaming: the supervisor restarted it, audio capture up again after 1.8 s, the SDP offers the audio track again.
- Still to do with the user: speech level (gain) and lip sync by eye in VLC; a long soak (hours) is running.
