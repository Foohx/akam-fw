# Convergence plan: 1080p firmware to the code level of the 2K, with an optimisation pass

> **Superseded (2026-11-23).** The state described below is out of date: this snapshot predates later work (the settings API, flip and mirror on the 1080p, speaker and picture on the 2K) and the camera program is now `akamd` on both cameras (files `akam-*`, `akam.conf`). The source of truth for what each camera does today is [docs/features.md](../features.md); the old to new names are in `tools/dev/path-map.txt`. Kept for its reasoning and measurements only; do not read its status columns as current.

> Historical document: the file paths below predate the repository reorganisation (`src/common`, `src/ak_rtsp`, `firmware/<model>/app`); the old to new table is `tools/dev/path-map.txt`.

Status: plan only. Written from a read of the code (no camera access, no code changes). Line numbers are those of `dev`
at 3f9debd. Everything about hardware behaviour that was not already measured is marked **HW** (needs a camera).

Scope: `src/ak_rtsp` (validated LSC Smart Indoor 1080p, 38-64 MB, static musl, kernel encode mode through
raw ioctls) and `firmware/lsc-rotatable-2k-ak3918av130/app` (2K, dynamic uClibc against the vendor libs, `akam2k_*`).
Rules that stay: no-regression (`docs/hal-architecture.md` section "No-regression rule"), light and fast
(budgets measured, opt-in features), the 1080p is the validated reference and is never changed without a proof.

## 0. Ground truth found while reading

- The 2K already **compiles 13 files of `src/ak_rtsp` unmodified, from where they are**
  (`SHARED_AK` in the 2K `Makefile`: `h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtp_clock tx_pace net_tx md5 mqtt
  mqtt_ha night_logic cfg_file`, plus SmolRTSP). `src/common` holds only `time32_shim.c`. So "move to `src/common`" is
  first a `git mv` plus include-path change for files both products already build, which is the lowest-risk step.
- The 2K duplicates, instead of sharing, a second copy of several modules: RTSP session loop (`rtsp_session.c`, 1218 lines
  vs `rtsp_session.c` 1758), MQTT service (`mqtt_bind.c` 1291 vs `mqtt_svc.c`+`mqtt_ops.c`), audio capture
  (`audio.c` vs `audio.c`), night driver (`akam2k_night*.c` vs `night.c`), LEDs (`akam2k_leds*.c`+`light_logic.c`
  vs `leds.c`), config (`video_cfg.c` vs `video_cfg.c`). Convergence means removing that duplication, in the right direction
  per module (section 2).
- The control-API spec (`specs/control-api.md` section 11) already says the 1080p adopts `settings_reg` and turns port
  8091 into an adapter. This plan sequences that.

## 1. Feature matrix

Legend: **S** shared candidate (pure logic or HAL-interface, one source for both), **M** model-specific (binding or
hardware), **N/A** not applicable to that model. "2K only" = present on the 2K, missing on the 1080p.

### 1a. 2K has, 1080p lacks

| Feature | 2K module | 1080p today | Class | Note |
|---|---|---|---|---|
| LED pattern engine (named emitters, `on/off/blink`, rules by state, `pat` grammar) | `light_logic.c`, `hal_light.h`, `hal_light_sysfs.c`, `leds.c` | `leds.c` (488 lines): fixed status LED + IR policy on LED-class `state_led`, own thread | **S** | 1080p `state_led` is `LIGHT_LEDCLASS` in the same descriptor; `light_logic` already speaks that kind |
| System metrics (CPU %, load, mem, uptime, wifi, route) | `hal_sys.c` (pure parsers + `slurp`) | `ctl_sys.c` counters only (clients, frames), no /proc metrics | **S** | generic, no model path |
| Video settings applied **live** (kbps/gop via set_attr, QP via set_qp, fps/rc reopen) | `vlive.c`, `video.c`, `video_cfg.c`, `abi.h` | `video_cfg.c`+`video_ctl.c`: read once at start, "restart to apply" | **S** logic, **M** driver | 2K goes through `libmpi_venc`; the 1080p has no lib and would use raw ioctls (3b) |
| Min/max QP, rc mode, peak kbps as settings | `video.c` | hard-coded 28/43/VBR+ (`venc.c:229-236`) | **S** | needed for 3b |
| Front button, short/long press | `button_logic.c`, `hal_input.h`, `hal_input_sysfs.c`, `button.c` | none known | **S** logic, **M** GPIO | only if the 1080p board has a user button: **HW**/schematic check (`docs/models.md`) |
| Detection zones, anchors, tracking | `zones.c`, `zlive.c`, `track_logic.c`, `akam2k_track*.c` | none | **N/A** (NPU + PTZ) | zone *geometry/hysteresis* (`detstate.c`) could be shared if the 1080p ever gets motion detection: out of scope |
| Scene / PTZ | `hal_ptz*.c`, `akam2k_ptz*.c` | no motor | **N/A** | |
| NPU person detection | `detect.c` | none | **N/A** | |
| Store with atomic persistence (tmp+fsync+rename, skip-if-equal) | `store.c` (68 lines) | `cfg_write_atomic` in `cfg_file.c` (already shared with 2K) + per-module ad hoc code | **S** | `cfg_file.c` is already the common one; the skip-if-equal check lives in `video_ctl.c:79` and `store.c`: unify |
| Settings registry (`st_def_t`), signed HTTP API (8092), generated MQTT entities | designed in `specs/control-api.md`, not implemented | text port 8091 (`control.c`, 904 lines), hand-written `mqtt_ha`/`mqtt_ops` | **S** | the largest convergence item; after everything else |
| Hot update (card-less appfs `--hot`) | `cardless-appfs.md` 6c | SD/maintenance-flag flash path | **M** | flash mechanics differ per model; keep separate (not a code-sharing item) |
| Snapshot JPEG ring | `snap.c` | none | **M**/optional | 2K encodes JPEG via vendor lib; the 1080p kernel interface for JPEG is unknown |
| Stream ring with `a2k_stream_next` + wake pipe (no polling) | `stream.c` | direct ioctl GET_STREAM in the RTSP loop | **M** | the 1080p cannot copy this: frames come from the kernel ring (see 4a for the poll fix) |

### 1b. 1080p has, 2K lacks

| Feature | 1080p module | Class | Note |
|---|---|---|---|
| ONVIF (media/device/imaging) + WS-Discovery | `onvif*.c`, `soap.c`, `http_srv.c`, `wsd.c`, `cred.c` | **S** candidate | the 2K is RTSP-only today; the stack is generic over a small `onvif_ops` (stream URI, profiles, imaging getters). Opt-in, `-Os`; adds roughly +60 KB: needs a budget decision on the 2K (RAM is larger there) |
| Two-way audio / talk (RTSP backchannel, `audio_out`, `audio_jitter`, `audio_vad`, `talk`, `g711`, `rtp_in`) | `audio_out.c`, `talk.c`, ... | **S** logic, **M** PCM device | 2K speaker path unresolved (`audio-findings.md`); pure parts (jitter, VAD, G.711, talk state) are shareable now |
| Kernel-log filter and rate limiter | `klog.c` (pure) + `klog_run.c` (I/O) | **S** | cheap, host-tested, useful on both |
| Control port 8091 (`ctl_gate`, `ctl_sys`, `control.c`) | | **S** | becomes the registry adapter (section 3, step 8) |
| Software watchdog (`swdog.c`: no frame for N s, stalled loop beats) | | **S** | 2K has `DEAD_MS` in `cap.c:24` only for capture; the beat/stall logic (pure `swdog_st_*`) is reusable |
| Image settings (`imgq`, `isp_tune`, `isp_blocks`, `ae`, `aec_custom`, `isp.c`) | | **M** | raw ISP tunnel on `/dev/isp-param-0`; the 2K uses `ak_vpss_effect_set`. Only the *setting grammar* (`isp.brightness` -50..50 etc.) is shareable |
| Wi-Fi/NTP helper variants (`tools/ci`, `akam_ntp.sh`, `akam_hwcheck.sh`, wrapper scripts) | shell, outside `src/` | **S** (scripts) | already model-neutral shell; keep one copy, parametrise paths |
| Assistant integration (wizard, `akam image`, `akam video`, api-key, flash paths) | `tools/assistant` | **S** | model-aware through `docs/models.md`; 2K entries to add as keys appear |
| `rtsp_busy` second client "busy" reply, `rtsp_in` framer, auth, `tx_pace`, `net_tx` | | already **S** | already compiled by the 2K |
| Night/day (ISP calibration switch, VTS, IR-cut pulses) | `night.c`, `ircut.c`, `ae.c` | **M** driver + **S** `night_logic` | `night_logic.c` already shared |
| `ledsys` IR LED policy hook into night | `ledsys.c` | **S** (becomes `hal_light` rule) | |

## 2. Target architecture

### 2.1 Layout

```
src/common/                 pure logic + HAL interfaces, no model path, no vendor header, host-testable
  time32_shim.c             (exists)
  net/   h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtsp_busy rtp_clock tx_pace net_tx md5 sha1 sha256 b64 cred
  api/   json_min api_sig api_keys api_limit settings_reg api_events  (control API v1 security, JSON, registry, event ring; moved at P1-P3)
  logic/ night_logic light_logic(=leds engine) button_logic swdog klog hal_sys settings_reg(later) cfg_file
         audio_jitter audio_vad g711 video_cfg (pure part)
  hal/   hal_light.h hal_input.h hal_sys.h hal_ircut.h hal_venc.h (new, section 2.3)
  drv/   hal_light_sysfs.c hal_input_sysfs.c  (generic sysfs drivers, selected by descriptor kind)
src/ak_rtsp/               1080p binding: descriptors + raw-ioctl drivers + process glue
  ak1080_hal.c  (new: light/ircut/venc descriptors)  venc.c vi.c isp*.c ae.c night.c main.c rtsp_session.c ...
firmware/lsc-rotatable-2k-.../app/   2K binding: hal.c, vendor-lib drivers, akam2k_* glue
```

Rule: a file goes to `src/common` only if it has no `#include` of a vendor header, no device path, no `ioctl`, and a
host test already runs it (all candidates above do). `ak_ioctls.h`, `isp*.c`, `venc.c`, `vi.c`, `audio.c` (PCM ioctls),
`ircut.c` (GPIO pulses), `ledsys.c`, `ae.c`, `night.c` (driver half) stay model bindings.

### 2.2 Direction of convergence per duplicated module

| Module | Keep as the single copy | Reason |
|---|---|---|
| RTSP session loop | **1080p `rtsp_session.c` logic split**: busy slot, talk, pacing, idle already factored in shared units; the two loops differ in the *frame source* (kernel ring vs `a2k_stream`) and threading (1 thread vs thread per client). Share the units, keep two loops | merging the loops forces the 2K threading model on the 1080p: not worth the risk |
| MQTT service | 2K `mqtt_bind.c` is the newer structure (registry-ready); port the 1080p `mqtt_ops` onto it **only when** the settings registry lands (step 8) | avoid two MQTT rewrites |
| LED/light | `light_logic` + `hal_light` (2K) | it is the smaller, driver-agnostic design the HAL doc names; `leds.c` features (status/ir keys, `leds_load_config` file) become rules + registry keys |
| Night driver | per-model; share `night_logic` (done) and the `night_tuning_t` keys | drivers touch different ISPs |
| Video settings | `video_cfg.c` parse/format (pure) shared; apply = `hal_venc` per model | |

### 2.3 New HAL interface: `hal_venc` (the part that unlocks 3b)

```c
typedef struct { int kbps, max_kbps, fps, gop, min_qp, max_qp, rc; } hal_venc_params_t;
typedef struct {
  int (*apply_live)(int chn, const hal_venc_params_t *want, const hal_venc_params_t *cur); /* 0 ok, <0: caller reopens */
  int (*reopen)(int chn, const hal_venc_params_t *want);
  int (*request_idr)(int chn);
  int (*stat)(int chn, hal_venc_stat_t *out);      /* current QP, bytes/s, frames: for the measurement protocol */
} hal_venc_ops_t;
```
The 2K implements it over `libmpi_venc` (`vlive.c` today). The 1080p implements it over raw ioctls in `venc.c`.
`video_cfg` validates (ranges per model descriptor), `hal_venc` applies; the "which setters work, fall back to reopen" state
machine of `vlive.c` moves to `src/common/logic/vlive.c` (pure: takes the setters as ops).

### 2.4 Build system

- Both Makefiles list the shared sources once through a new `src/build/sources.mk`:
  `COMMON_SRCS := net/... logic/... drv/...` and `COMMON_DIR := $(ROOT)/src/common`. The 1080p Makefile (cwd `src/ak_rtsp`)
  adds `-I../common -I../common/hal` and compiles `../common/...` objects; the 2K replaces `SHARED_AK` paths.
- Per-object flags are preserved (`-Os` for `ONVIF_OBJS`/`MQTT_OBJS`, `-fstack-protector-strong` for `HARDENED_OBJS`); the
  lists keep naming files by basename so a moved file keeps its flags (use `vpath` or `$(notdir)` patterns).
- Host tests keep working: they reference `src/ak_rtsp/<file>.c` by path (e.g. `tests/leds_test.sh`); each move updates
  the path in the one test that builds it (`grep -l <file> tests/*.sh tests/native/*`), a mechanical change.
- `make repro` (new, step 0) builds the 1080p binary reproducibly and prints a hash (2.5).

Golden history: `caded296...` (step 0, before any move) then `ffa4da4c...` after `feature/1080p-qp-tuning` (QP keys
`minqp`/`maxqp` in `video_cfg`): behaviour-identical by default; hash changes because of new code. The proof is not the
hash but `tests/video_apply_test.sh`: with no settings file the 96-byte CREATE_ENC payload equals the pre-change fixture byte for byte.

**Feature selection (done with P1/P2).** `src/build/features.mk` is the per-model switch: a model Makefile sets
`AKAM_FEAT_API`, `AKAM_FEAT_PTZ`, `AKAM_FEAT_BUTTON`, `AKAM_FEAT_SCENE`, `AKAM_FEAT_PATROL`,
`AKAM_FEAT_TRACK` to `1` or `0` (unset = off, unknown names and other values are an error), then lists a module's objects
only through `$(call feat_objs,<NAME>,<objects>)`. A feature that is off compiles nothing, so a model never carries
another model's dead code. The 2K Makefile has it on for everything it ships today  and gates the shared API
objects with it; the 1080p includes it since P4 and turns on only what it adopts, one step at a time (P4-P9: `SYS`, `LIGHT`, `API` are on,
`PTZ BUTTON SCENE PATROL TRACK` are off, so no 2K-only code is compiled into `ak_rtsp`). `make -C <app dir> features` prints the on/off sets. The link order is kept, so enabling a
feature through `feat_objs` is byte-identical to listing the objects by hand.

### 2.5 Keeping the 1080p binary testable byte for byte

Today it cannot be compared: `version.o` is forced to rebuild with `BUILD_TIMESTAMP` and an incrementing counter
(`gen_version.sh`), and `-g` embeds source paths. Step 0 adds, without changing behaviour:

1. `gen_version.sh` honours `SOURCE_DATE_EPOCH` / `AKAM_REPRO=1`: fixed timestamp, counter 0, git hash from env.
2. `make repro`: `CFLAGS += -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(ROOT)=/src -g0`, then `strip`, then
   `sha256sum ak_rtsp` and `arm-...objdump -d ak_rtsp | sha256sum`.
3. A committed golden file `tests/golden/ak_rtsp.sha256` plus `tools/ci/repro_check.sh` (the compiler is pinned: `zig` version
   in the script header; a different zig changes the hash, so the golden also records the zig version).
4. Preconditions that keep the hash stable across a pure file move: identical `SRCS` **link order** (`-ffunction-sections`
   with `--gc-sections` lays out by input order), identical per-object flags, identical symbol names, `static` unchanged.
   Moving a file therefore changes only paths: with `-ffile-prefix-map` and `-g0` the stripped binary is identical.

Classification of each step below: **BI** = byte-identical stripped binary expected and checked by `repro_check.sh`;
**BC** = behaviour change by design: full host suite (`tests/local_check` via `tools/ci/local_check.sh`) plus hardware
validation on akam-01 and akam-02 (section 3 gives the checklist).

## 3. Migration plan (small increments, lowest risk first)

Every step: its own branch off `dev`, one commit series, merged locally only after its proof; never a push. Rollback for
all BI steps is `git revert` of the move (the golden hash proves the revert restores the binary). For BC steps rollback is
(a) revert plus (b) the previous `ak_rtsp` on the camera from `/tmp` or the previous squashfs (the wrapper respawns
`ak_rtsp`; the flash is never touched, `rot2k-autonomy` rule applies to the 2K as well: RAM runs first).

| # | Step | Files moved/changed | 1080p proof | Rollback | Needs HW |
|---|---|---|---|---|---|
| 0 | Reproducible build + golden hash | `gen_version.sh`, `Makefile` (`repro` target), `tests/golden/`, `tools/ci/repro_check.sh` | defines the baseline: two clean builds give the same hash; hash checked in CI | revert | no |
| 1 | Move already-shared pure files to `src/common` | `git mv` of the 13 `SHARED_AK` files (+ `rtsp_busy sha1 b64 cred` that are pure), both Makefiles, test paths | **BI** (`repro_check.sh` equal); host suite green; 2K `make poc3` objects unchanged (`size`/hash of its `.o`) | revert | no |
| 2 | Pure-logic extractions that exist already as separate `*_logic`/pure files: `swdog` (the `swdog_st_*` half), `klog.c`, `audio_jitter`, `audio_vad`, `g711`, `video_cfg` parse/format half | split files (no logic change): `swdog.c` into `swdog_core.c`+glue, `video_cfg.c` into `video_cfg_core.c`+load glue | **BI** | revert | no |
| 3 | `hal_sys` metrics on the 1080p | `ctl_sys.c` gains `sys.*` read-only keys through `hal_sys_sample` (10 s cadence, 1 slurp of 5 small files, no thread: sampled from an existing timer, see 4a #8); `LIST`/MQTT sensors | **BC**, additive: with the keys unread the binary differs only by the new code; host test `hal_sys` against fixtures (exists for the 2K); measure +code size (<= 3 KB) and CPU (none while nobody reads); hardware: read the keys on akam-01 | revert | 30 min on akam-01 (RAM run) |
| 4 | LED engine on the 1080p `state_led` | `ak1080_hal.c` descriptor `{name:"status", kind:LIGHT_LEDCLASS, led_dir:"/sys/class/leds/state_led"}` (+ the IR LED is GPIO inside `ircut.c`/`ledsys.c`: keep, expose as emitter later); `leds.c` rules re-expressed as `light_logic` rules; keep `led.status`, `night.ir_led`, `led.state` keys and the `leds` file format | **BC**: a side-by-side host test drives both the old `leds.c` and the new engine with the same input traces (night, no-wifi, client, boot) and requires the same write sequences to a fake sysfs (`tests/leds_test.sh` is the oracle; it must pass unchanged against the adapter); hardware: `state_led` blink patterns for the 5 states, `led.status=off/on/auto`, persistence across reboot | revert + old `leds.c` stays in git for one release behind `LEDS_LEGACY=1` build flag | akam-01 and akam-02, visual |
| 5 | `hal_venc` + QP/rc settings + live apply (3b protocol) | new `hal_venc`, `venc.c` (setters), `video_cfg` keys `min_qp/max_qp/rc`, `video_ctl.c` (live apply), `ctl_*`, `mqtt_ops` | **BC** by design, but **defaults identical to today** (28/43/VBR+, restart-only) until the user changes them: with defaults the CREATE_ENC payload is byte-identical (`DEBUG_IOCTL_DUMP` dump diff against `tests/fixtures`). Live setters only on explicit request | revert; or set the keys back; `rm /etc/config/akam/video` restores defaults | yes: the whole 3b protocol, UART attached for the first setters (a bad rc word oopsed the kernel twice: see `venc.c:238-262`) |
| 6 | Atomic-store unification | `video_ctl.c`/`leds.c`/`night.c` saved-file writers call one `store_write_if_changed` (the skip-if-equal logic of `store.c` over `cfg_write_atomic`) | **BC** minor: file bytes on flash must be identical for the same input (golden fixtures) | revert | no (host: file bytes) |
| 7 | Button (only if present) | `button_logic`, `hal_input_sysfs`, `ak1080_hal.c` GPIO descriptor | n/a until the pin is identified | revert | yes: find the GPIO on the board first |
| 8 | Settings registry + 8091 adapter + signed API 8092 + generated MQTT | `settings_reg`, the 1080p `st_def_t` table (keys of `control-port.md` section 6), `control.c` becomes an adapter, `mqtt_ops` generated | **BC**: the 8091 contract test (`tests/control_test.sh`, `tests/akam_mqtt_test.sh`) must pass **unchanged** (same keys, replies, write rules); budget gate of `control-api.md` section 9 (binary +40 KB, RSS +64 KB) measured on the camera | revert; the text port keeps working because the adapter replaces the table, not the protocol | yes: full control-port + MQTT pass, 72 h soak |
| 9 | Optimisation fixes (section 4a) | see 4a; each is its own commit | **BC**, each measured with the CPU protocol (3b.4 / 4a) | per-commit revert | yes |

Order rationale: 0-2 are free; 3 is additive; 4 is visible but isolated and oracle-tested; 5 is the user-facing quality
fix and is placed early because it is the biggest value, but after 0-2 so its diff is reviewable; 8 last because it touches
every key.

Steps 9 (optimisation) may be run **before** 4-8 where they are independent: the CPU fix (4a #1-#3) is worth doing first on
its own if the measurement of 4a confirms it, since it also makes every later hardware measurement quieter.

## 3b. 1080p image quality ("horrible")

### 3b.1 What the code sends today (`venc.c:218-262`, `platform.h:27-33`, `video_cfg.h`)

H.264 Main L4.0, 1920x1080, `rc_mode = VENC_RC_VBRP`, `minqp 28`, `maxqp 43`, `initqp 99` (above range, so the encoder starts
at its own default), `max_kbps = video_kbps()`, `goplen = gop_s * fps`, `rc.minqp/maxqp 28/43`, `rc.flag=1` (MMA), `flag2=40`
(MVV range), `srd_threshold=128`, `smart_mode=0`. Nothing about these is user-settable except kbps/fps/gop, and all of them
only at the next start. User settings in play: cam .51 `bitrate=1024 gop=1`, cam .52 `fps=25`.

### 3b.2 Likely causes, ranked

1. **GOP of 1 s at 1024 kbit/s (cam .51).** One IDR per second; on this encoder an I-frame at usable QP is ~100-135 KB
   (measured `ENC_*` comment in `rtsp_session.c:~950`: ~134 KB I vs 7-9 KB P on a static scene). Fitting 25 % to 40 % of the
   second's 128 KB budget into one keyframe forces the rate controller to the QP ceiling on I-frames and every P-frame
   that references them inherits the blockiness. Expected symptom: a periodic "pumping" every second. Fix: gop >= 2 s at
   this bitrate, or raise the bitrate when gop is 1 s (rule of thumb: bitrate >= 2 Mbit/s per 1 s GOP at 1080p).
2. **Bitrate too low for 1080p with the QP ceiling fixed at 43.** 1.0-1.5 Mbit/s for 1080p Main without B-frames is at or
   under the usual minimum (2-4 Mbit/s for 15 fps surveillance). With VBR+, when the ceiling is reached the controller
   climbs to `maxqp 43`: QP 43 is a very coarse quantiser (visible 16x16 blocks, smeared edges). The 1536 kbit/s default
   comes from the stock ini of a 2304x1296-class sibling; at 1080p it is a floor, not a comfortable value.
3. **fps 25 with the same bitrate (cam .52).** Bits per frame fall from 102 kbit (15 fps at 1536) to 61 kbit; I-frame share
   grows. The shorter exposure limit (VTS 1370, longest exposure ~40 ms, `video_cfg.h` comment) pushes gain up in dim rooms,
   so noise rises exactly when the encoder has fewer bits: noise is expensive to code and ends up as blocks. The comment
   says 30 fps was already refused for the same reason.
4. **Noise in the source, not only the encoder.** `specs/image-day-night-leds.md` section 7 records an unexplained grain difference to
   stock; candidates named there are the rate-control sub-struct and sharpening re-applied after a day/night switch.
   Both are measurable (3b.4) before changing anything.
5. **Wrong rc sub-struct (`img.venc_rc=legacy`).** Our `rc` words (`minqp 28, maxqp 43, srd 128, flag2 40`) are not what stock
   sends (stock: all zero but `flag=1`, `rc_flag=1`); stock sends `qp 28/43` only in the main struct. `stock` is implemented
   (`venc.c:290-303`) but opt-in and never survived a hardware run; two earlier attempts with rc.flag=0 oopsed. Test it from
   `/tmp/akam_image` with UART attached only.
6. **Drops from TCP pacing** (`tx_pace.c`): a slow Wi-Fi link makes the stream skip to the next keyframe; with a 1 s GOP this
   shows up as freezes plus pumping. `ctl_sys_note_video_skipped()` counts it: check the counter first.
7. **Encoder starvation at max QP**: the `tx_pace.h` header notes an encoder whose stream buffer is held re-encodes at maximum
   QP. Today `GET_STREAM` is drained within ~4 ms of availability (`PLAY_IDLE_POLL_MS`), so this is unlikely, but a CPU
   starved process (the 90 % CPU issue, 4a) could miss frame deadlines: another reason to fix CPU first.
8. Profile/entropy: Main profile, no B-frames, CABAC unknown (kernel CREATE_ENC takes profile from `QUERY_DMA` cache,
   `venc.c:90-95`). **HW**: read the SPS (`h264_params`) in a capture to see what the encoder emits; a High profile or B-frames
   would need a different `QUERY_DMA` input and is risky (DMA pool sizing).

### 3b.3 Proposed changes (smallest first)

1. **Documentation + guard, no firmware change:** `video_cfg` validation warns when `bitrate / gop_s < 1000` (i.e. bits per
   keyframe-second below the safe floor) and the assistant's `akam video` prints the warning and a suggested pairing
   (`1536/2`, `2000/2`, `2500/1`). Also change the default `ENC_KBPS` to 2048 **only after** the measurement shows no pacing
   problems at that rate (single client, 15 fps, one Wi-Fi hop: 2 Mbit/s is within the 256 KiB send queue behaviour).
2. **Make QP and rc settings keys** (`video.min_qp`, `video.max_qp`, `video.rc` = `vbrp|cbr`, `video.init_qp`) in `video_cfg`
   (ranges: min 10..51, max <= 51 and >= min+4, from `libmpi_venc` `venc_check_and_format_param` rules in
   `venc-live-params.md` section 2; the 1080p kernel check is assumed equal: **HW**). Defaults stay 28/43/VBR+. A lower
   `max_qp` (38..40) trades fewer artefacts for bitrate overshoot above the ceiling: only useful with headroom (item 1).
3. **Live apply on the 1080p (hypothesis, HW):** the 2K lib ioctls are in the same `'V'` (0x56) family as the 1080p's
   (`CREATE_ENC 0x406056e0`, `GET_STREAM 0x402056ea`; the 2K `set_attr 0x406056f2` is also 96 bytes, the size of
   `venc_create_enc_req`). The 2K results (`venc-live-params.md` section 7: set_attr kbps/gop applied live, set_qp returned 0,
   set_fps refused) suggest the 1080p kernel module (`ak_venc_adapter`) exposes the same set. Candidates, to be probed one
   at a time exactly as in that document's test sequence (hook the stock to learn which ones the 1080p stock calls:
   `isp_hook.c` already logs ioctls):
   `0x406056f2` SET_ATTR (96 B, same layout as CREATE_ENC), `0x400c56fb` SET_QP `{id,min,max}`, `0x400456f3` REQUEST_IDR,
   `0x400856e3` SET_FPS. Only IDR-on-demand would also remove the current "no force-IDR ioctl" limitation noted in
   `tx_pace.h:30`. Until proven, live apply stays disabled and the setting uses the existing restart path.
4. **Do not change** `rc` words, MMA, `srd_threshold`, resolution or profile without the UART and an explicit opt-in file
   (`venc.c` history: two kernel oopses).
5. Day/night: confirm that the **night** switch (10 fps, VTS 3426) is not what the user is watching at the time of the bad
   picture: the night path changes sensor fps without changing the encoder's fps/gop (`night.c`), so the encoder is told 25 fps
   but receives 10: rate control sees a frame deficit. (`ae.c` sets frame rate for day; night sets it separately.)
   **HW**: compare encoder-side fps vs sensor fps at night in the capture log.

### 3b.4 Measurement protocol (same scene, same time of day, tripod, akam-01 and akam-02 alternately)

Every run: 5 min, RAM-only settings (`/tmp`), restore after. One variable per run. Log with the 8091 DEBUG mode on so
`rtsp_session.c` prints the existing `[ak_rtsp] bitrate:` lines (2-GOP windows) and `ae` windows.

1. Baseline matrix (no code change): `{kbps 1024, 1536, 2048, 2500} x {gop 1, 2, 4 s} x {fps 15, 25}` = 24 cells; start with
   the user's two real settings, then vary one axis.
2. Capture: `ffmpeg -rtsp_transport tcp -i rtsp://... -c copy -t 300 out.mkv` (no re-encode) and, for objective numbers,
   `ffprobe -show_frames -select_streams v -of csv` -> I/P sizes, pict types, timestamps (per-GOP size, I/P ratio, effective
   fps). `tools/hw_tests/image_noise.py` gives temporal/spatial noise of two captures (exists).
3. Decode-side QP: `ffmpeg -debug qp` or `h264_metadata`/`ffmpeg -vf "qp"` is not always available; use `ffprobe -show_frames`
   + `-flags2 +export_mvs` for motion and, for QP, `ffmpeg -i out.mkv -vf codecview` or `x264`-style `trace_headers` bsf
   (`ffmpeg -bsf:v trace_headers`) to read `slice_qp_delta` per slice: average QP per GOP, % of frames at QP >= 42.
4. Quality metrics: full-reference is impossible (no ground truth), so use **A/B against the stock firmware** (the validated
   `anyka_ipc` at the same scene, same bitrate): side-by-side crop (1:1, 400x300) of edges and text; PSNR/SSIM between the
   two decodes after registration is meaningless, use `tools/hw_tests/image_noise.py` + a no-reference score
   (`ffmpeg -vf blurdetect,blockdetect` where present) and a fixed set of five 1:1 crops reviewed by eye.
5. Pacing/health evidence in the same run: `ctl_sys` counters (frames sent, skipped), `GET video.*`, Wi-Fi level
   (`/proc/net/wireless`), CPU of `ak_rtsp` (4a protocol). A run with skipped frames > 0 is discarded from the quality matrix.
6. Decision rule: the default is changed only if the new setting wins on (a) average QP down by >= 3 and (b) no pacing skips
   over 5 min on both cameras, and the user prefers it blind on the 1:1 crops.
7. After step 5 of section 3 lands: repeat the matrix on `min_qp/max_qp` (28/43, 28/40, 24/40) and `venc_rc` `legacy` vs `stock`.

## 4. Optimisation audit

Method note: findings are from reading; no profile exists. The known symptom is `ak_rtsp` ~90 % CPU, 81 % of it system
time. System time dominated by syscalls and kernel ioctls means the cost is in **how often we enter the kernel**, not in
our own computation. Ranked by likely share.

### 4a. 1080p `ak_rtsp` (CPU)

Measure first (cheap, no code change): per-thread split with
`for t in /proc/$(pidof ak_rtsp)/task/*; do echo $(cat $t/comm) $(awk '{print $14,$15}' $t/stat); done` twice, 10 s apart (utime/stime
per thread), then `strace -c -f -p <pid>` for 10 s with and without a client. The streaming thread is `main`/`handle_client`.

| # | Where | Problem | Impact | Fix |
|---|---|---|---|---|
| 1 | `rtsp_session.c:1577` `wait_ms = video_idle ? PLAY_IDLE_POLL_MS(4) : 0` and `rtsp_session.c:1585` | While PLAYing, the loop runs `poll(4 ms)` + a non-blocking `GET_STREAM` ioctl (`rtsp_session.c:896-910`) + `send_audio_frame` + `send_rtcp_reports` + auth/idle clocks every pass. At 15 fps a frame arrives every 66 ms, so ~15 of every ~16 passes find nothing: ~250 loop passes/s, ~500+ syscalls/s, each `GET_STREAM` is a kernel call into `ak_venc_adapter` (the comment at `main.c:382-387` already records that spinning this call pinned a core "hot in the kernel") | **High**: likely the dominant sys share; also latency jitter | (a) **HW**: test `poll(g_chn0_fd, POLLIN)` / `select` readiness on `/dev/venc-chn0` (stock `anyka_ipc` does not poll the fd as far as known; if the driver supports it this removes the polling entirely). (b) otherwise **frame-paced sleep**: after a frame at time `t` (known from the vclock, `rtp_clock.h`) sleep until `t + period - 3 ms` (use the learned sensor period) then poll 1-2 ms steps: ~2-3 wakeups per frame instead of 16. Audio sending piggybacks on the same wakeups (audio periods are 32 ms), RTSP input still wakes `poll` at once |
| 2 | `rtsp_session.c:1699` + `main.c:388` `IDLE_DRAIN_POLL_MS 20` | With no client, `drain_venc_ring()` (up to 16 `GET_STREAM`+`RELEASE_STREAM` pairs, `rtsp_session.c:861-890`) runs every 20 ms = 50 wakeups/s; 20 ms is needed only to keep the 777 KB ring from wrapping (3 s at 2 Mbit/s, `main.c:~380`) | Medium | Same fix as 1 (frame-paced). Also lower to ~40 ms when the ring is verifiably deep enough: ring 777600 B holds > 1 s at 3 Mbit/s |
| 3 | `audio.c:176-205` playback thread | Writes **silence** continuously, forever (even without talk), 8 kHz period of 32 ms = 31 blocking `write`s/s into the PCM driver, with a `clock_gettime` pair each, only to keep the capture alive ("a theory", comment `audio.c:145-155`). Plus capture `read` thread (31 reads/s) | Medium (31 wakeups/s + driver time; sys) | **HW**: check whether capture still works when playback is opened only while talking (the file already has `g_playback_pending` start-order logic). If the silence stream is required, write 4 periods per `write` (125 ms): 8 wakeups/s instead of 31, buffer permitting (SET_PERIODS 4) |
| 4 | `ae.c:281-292` | Every 66 ms: `memset(expo, 0, 0x720)` then `GET_EXPOSURE` (0x720 B copy_from/to_user) + `GET_AE_RUN_INFO`: 2 ioctls x 15 Hz, with a 1.8 KB `memset` each. The tick advances the on-chip AE ("the tick that advances the ISP state", comment) so it cannot be dropped | Low-medium (30 syscalls/s, ~27 KB/s of copies) | Keep cadence (required by the ISP) but memset only `run_info`/the first words actually read (`expo` is read at index 1 only for the log line; `priv_exp` is used only inside the logging branch: skip the whole GET_EXPOSURE copy when not logging **if the tick is not required** HW check: stock reads both every tick). Also sleeps with `usleep` drifting: use absolute deadlines as `night_run.c:70-85` does |
| 5 | `night.c:234` | 10 wakeups/s forever (100 ms `usleep`), plus `ae_stats_seq` read; the logic needs one sample per AE tick, and the AE thread already produces them at 15 Hz | Low | Fold into the AE thread (call `nl_step` from `ae_loop` on the published sample every 7th tick) and delete the thread: one fewer thread, 10 wakeups/s, 1 stack (the stack is the larger saving on 38 MB: default pthread stack is 128 KiB virtual) |
| 6 | `rtsp_session.c:1004-1006` per frame | `tx_pace_backlog` (`SIOCOUTQ` ioctl) + `tx_pace_sndbuf` (`getsockopt`) twice (`kcap`) per frame: 3 extra syscalls/frame | Low (45 syscalls/s) | Cache `sndbuf` per connection (constant after `setsockopt` at connect); keep the `SIOCOUTQ`; only needed in TCP mode |
| 7 | `rtsp_session.c:799` via SmolRTSP `send_packet`, `rtsp_session.c:1270-1295` `TxWriter` | Each RTP packet is serialised through `vwritef`/`write` calls (header printf-style formatting into a 512 B stack buffer, then payload) and each part is its own `send(MSG_DONTWAIT)` when the queue is empty: 2-3 syscalls per ~1400-byte packet, ~140 pkt/s at 1.5 Mbit/s, i.e. ~300-420 send syscalls/s, each a kernel TCP segment path | Medium (sys) | Coalesce per frame: build the interleaved `$ch len + RTP header + payload` into the existing 256 KiB queue buffer (`net_tx` already has `queue_append`) and flush with one `send` (or `writev`) per frame; or set `MSG_MORE` on all but the last part of a packet. Behaviour (bytes on the wire) is identical: BC with a byte-compare test of the interleaved stream against the current output |
| 8 | `rtsp_session.c:988` | `h264_params_scan` of up to `PARAMS_SCAN_BYTES` of every I-frame (also on the drain path, `rtsp_session.c:885`): byte loop over the start of 130 KB frames | Low (once per GOP) | Fine as is; skip the scan once SPS/PPS are cached and unchanged (`h264_params_ready`) except on demand |
| 9 | `rtsp_session.c:940-1000` | Per-frame `static` bitrate window with `double`/printf at 2xGOP; `printf` of an I-frame log when debug off is guarded; but `fprintf`/`perror` paths and the `[ak_rtsp]` log still go through stdout teed to a pipe/serial | Low | confirm with `strace -c` that `write(1,...)` is rare; if serial console is attached log volume costs CPU: keep the existing quiet defaults |
| 10 | `rtsp_session.c:1621` auth/ctl notes | `ctl_sys_note_rtsp_auth` + `rtsp_auth_session_ok` once per second: fine | None | - |
| 11 | `swdog.c:101` | 1 wakeup/s, plus `swdog_beat`/`swdog_mode` call `clock_gettime` twice per loop pass (`rtsp_session.c:1572-1575`, `1700`): with 250 passes/s that is 500 `clock_gettime` calls/s (vDSO on a 4.4 kernel with the **time32 shim**: `time32_shim.c` forces a real syscall for `clock_gettime`!) | Medium (syscalls, not vDSO) | **Notable:** the shim replaces musl's `clock_gettime` with a time32 syscall because the kernel ignores time64 ones; each `rtp_mono_us()` / `sw_now_ms()` is therefore a real `clock_gettime` syscall. The loop calls `rtp_mono_us()` ~6 times per pass (`rtsp_session.c:1572-1700`), so fix 1 (fewer passes) and a single `now` per pass cut hundreds of syscalls/s. Take `now_us` once at the top of the pass and pass it down |
| 12 | `control.c` log pump (`log_pump` thread) and accept loop (5 s `poll`) | idle: blocked, 1 wakeup per 5 s | None | - |
| 13 | `wsd.c`, `http_srv.c`, `mqtt_svc.c` (opt-in) | `poll` with computed timeouts: OK. MQTT `poll(…, 20)` at `mqtt_svc.c:389` is a 20 ms poll in the DNS wait path only | Low | none unless MQTT is on and DNS is slow |
| 14 | `leds.c:290-300` | LED thread polls at `tick_ms` always (x4 when not auto): wake-pipe exists, so wake only for blink phases | Low | In non-blinking states sleep until the next *event* (`poll(-1)` on the wake pipe) instead of a periodic tick |

Quick wins expected to account for most of the 81 % sys: #1 + #11 (loop passes and time syscalls), #7 (send coalescing), #3
(playback silence). Order of work: measure -> #11 (single `now` per pass, trivial, no behaviour change) -> #1b (frame
pacing) -> #7 -> #3 -> #4-#5.

Memory/binary on 38 MB (1080p): per thread default stacks (pthread 128 KiB virtual in musl; only touched pages are RSS):
threads today are main, ae, night, leds, audio capture, audio playback, swdog, control accept, control log pump,
wsd/http/mqtt when enabled: set `pthread_attr_setstacksize(…, 32 KiB)` on the small threads (ae, night, leds, swdog, log
pump) to bound worst-case commit; `-Os` for non-video-path objects (already done for ONVIF/MQTT); `--gc-sections` is on. The
256 KiB TCP queue (`net_tx`) is allocated at PLAY only (good); consider 192 KiB: the biggest keyframe is ~134 KB.
Flash writes: `video_ctl.c:79` skips identical writes (good); `leds`/`night`/`img.wb` saves are per SET: add the same
skip-if-equal (step 6).

### 4b. 2K app (`firmware/lsc-rotatable-2k-ak3918av130/app`)

Threads: capture x2 (main, sub), detect, night (decider + reader), leds, ptz, button, mqtt, audio, snap, rtsp accept + one per client,
main loop (100 ms).

| # | Where | Problem | Impact | Fix |
|---|---|---|---|---|
| 1 | `cap.c:277-280` | `ak_vi_get_frame` is the blocking source; on error `usleep(5000)` loops up to 10 s: 200 wakeups/s per channel while the VI is not delivering | Low normally, high in a failure window | exponential backoff 5 -> 50 ms |
| 2 | `cap.c:269-274` | Frames not encoded (sub-rate): `rc=-1` then the release path: fine. The `memset(&fr/&st)` per frame small | None | - |
| 3 | `stream.c:64-69` `a2k_stream_publish` | `malloc(sizeof *f + len)` + `memcpy` of every encoded frame (up to 130 KB) per frame, per channel: ~190 KB/s of malloc/free and memcpy at 1.5 Mbit/s, plus heap fragmentation on a 64 MB device | Medium | Ring of fixed preallocated frame slots sized to the max frame (refcounted slot reuse), or a single arena ring; saves malloc/free churn and the VmHWM spikes; the vendor stream buffer must still be released promptly, so the copy stays (one copy), only the allocator goes |
| 4 | `leds_run.c:48` `TICK_US 50000` | LED thread wakes 20x/s forever; in the stock state (`A2K_PAT_ON`, `leds.c:221`) it only recomputes inputs (`a2k_night_get` mutex, `a2k_rtsp_playing`) | Low-medium | sleep to the next pattern edge, or 500 ms when the pattern is steady; wake on events (night/person/stream notifies) through a pipe as the 1080p `leds.c` does |
| 5 | `button.c:80` `A2K_BUTTON_POLL_MS 20` | 50 wakeups/s forever + a `pread` per wake, for a button that is pressed a few times a day | Medium (largest pure-wakeup cost of the 2K threads) | `poll()` on the GPIO value file with `edge=both` (sysfs gpio supports `POLLPRI`): zero wakeups idle. Fallback 20 -> 50 ms (debounce in `button_logic` already uses timestamps) |
| 6 | `night_run.c:93-100` reader | `usleep(50 ms)` inner loop waiting for the next read period or a kick: 20 wakeups/s on top of the decider's 10/s (`NL_POLL_MS 100`) | Low-medium | one condvar/`pthread_cond_timedwait` with the absolute deadline, or merge reader and decider into one thread; wake by the kick |
| 7 | `detect.c:217` `wait_until` | 50 ms sliced sleep to honour stop: up to 20 wakeups/s while waiting between inferences | Low | `poll` on a stop pipe, or `pthread_cond_timedwait` |
| 8 | `main.c:612` main loop `usleep(100000)` | 10 wakeups/s to check counters; work every 100 ms is a few mutex ops | Low | `sleep(1)` or condvar signalled on stop (the stats are every `STATS_EVERY_S`) |
| 9 | `rtsp_session.c:922` client poll `POLL_MAX_MS 250` | wakes 4x/s per playing client for the stop flag and auth re-check; frames wake it through the stream wake pipe (good design) | Low | fine; could be 1000 ms with a stop pipe |
| 10 | `audio.c:170`, `snap.c`, `mqtt_bind.c:1269` (`usleep(10000)` in stop wait) | small | Low | - |
| 11 | `hal_sys.c:162-179` `slurp` | Every sample opens/reads/closes 5 `/proc` files with `fopen` (stdio buffer 4 KiB each, malloc/free) once per `SYS_EVERY_S`; `hal_sys_net_ipv4` reads `/proc/net/route` every second from the LED thread (`leds_run.c:29`) | Low | use `open/read/close` into a stack buffer (no stdio malloc); cache the route result and refresh via netlink or at 5 s |
| 12 | `store.c:45` | `a2k_store_write` already skips identical content (good); `fsync` per write: fine because rare. `detstate.c` rewrites the state file every `HEARTBEAT_MS 5000` (`detect.c:30`): **flash write every 5 s** if the state dir is on flash | **High for flash wear if on flash** | confirm the path: it must be `/tmp` (RAM). If it is under `/config`, move to `/tmp` or write only on change plus every 60 s; **HW**: check `mount` for the path |
| 13 | `track.c:218` | `a2k_store_write` on each tracker position change? (`t->path`) | Possible flash wear | rate-limit to once per N seconds and on stop; persist only anchors, not live pan/tilt |
| 14 | `snap.c:177-199` | JPEG ring: `tmp file + rename` per snapshot into the ring directory | Medium if the dir is on flash | must be tmpfs; assert at start (`statfs` magic) and refuse flash |
| 15 | `mqtt_bind.c` | Per-publish buffers; single `poll`; reconnect backoff 5-60 s: good | Low | - |
| 16 | Build (`Makefile` 2K): `-Os`, no `-ffunction-sections` in the poc0/2/4 rules; poc3 has `-ffunction-sections -fdata-sections` via `POC3_CFLAGS` but the **link** line needs `-Wl,--gc-sections` | Binary size | check the link line adds `--gc-sections` and `-Wl,-O1`; compare `size` before/after | |
| 17 | Per-client thread default stack (uClibc: 8 MiB virtual `RLIMIT_STACK` unless `pthread_attr_setstacksize`): `rtsp_session.c:1110` | RSS only touched pages, but the reserved VA counts against `ulimit`/overcommit on a 38 MB device | Low-medium | set 64 KiB explicitly (the code already uses `pthread_attr_t at` in most places; check each) |

Totals: the 2K has no CPU hot loop; its idle wakeup budget is led 20 + button 50 + night 10 + reader 20 + main 10 + detect up to 20
= ~130 wakeups/s. Targets: button edge-driven (-50), LED on events (-20), reader merged (-20), main on condvar (-10) =
~30/s. That matters for battery-less but heat/CPU-bound 38 MB cameras mostly through scheduler load and cache effects, not
for watts: priority below correctness items 12-14 (flash wear).

## 5. Risks and what needs hardware

| Risk | Mitigation |
|---|---|
| A **kernel oops** from a wrong venc word (history: two oopses on `rc` words, `venc.c:238-262`) | UART attached; run from RAM; only SET_ATTR/SET_QP/IDR probes, one at a time, after the stock ioctl capture shows the 1080p stock uses them; never write the rc sub-struct from a live setter |
| Byte-identical claim broken by the toolchain (zig version, `-g` paths, version stamp) | step 0 pins zig, fixed stamp, `-ffile-prefix-map`, `-g0`; golden file records the compiler version; two clean builds must agree before any move |
| Link-order change alters layout | `SRCS` order frozen; moved files keep their position in the list |
| Unifying LED engines changes blink timing | oracle test on write sequences (step 4); visual check on both cameras |
| Frame-paced polling misses frames at a bitrate burst | keep a 1-2 ms short poll window after the expected time; counter `late_frames` in `ctl_sys`; ring headroom is 1+ s; HW soak with a moving scene |
| Dropping the playback-silence stream breaks audio capture (EPERM theory in `audio.c`) | HW A/B with capture running 10 minutes; keep the stream as default until proven |
| Settings registry grows the binary | gate +40 KB / RSS +64 KB (`control-api.md` section 9), measured before merge |
| Quality change not reproducible across scenes/lighting | blind A/B on both cameras, fixed protocol (3b.4), keep the default conservative |

Needs a camera (user's akam-01 / akam-02 for the 1080p; the 2K camera for 4b): CPU per-thread profile and `strace -c`
(4a), `poll()` readiness of `/dev/venc-chn0`, the venc live ioctl probes (3b.3), the stock-ioctl capture on the 1080p
stock, `state_led` pattern checks, button GPIO identification, the playback-silence A/B, the 2K state-file path check
(4b #12/#14), the full 8091/MQTT contract run and 72 h soak after step 8. Everything else (steps 0-2, host parts of 3, 4, 6,
8 and the spec work) is host-only.

## 6. Suggested sequence

1. Measure CPU on akam-01 (per-thread + `strace -c`), no code change (4a). In parallel run the 3b.4 baseline matrix: it needs no code.
2. Step 0, then 1, 2 (host only, byte-identical).
3. 4a #11 and #1 (the CPU fix) with hardware validation; then 4a #7, #3.
4. Step 5 (`hal_venc`, QP/rc settings) and the live-apply probes; adopt the quality defaults from the matrix.
5. Steps 3, 4, 6, then 7 if the button exists, then 8 (registry/API), then 4b fixes on the 2K (flash-wear items 12-14 first).

## 7. Parity porting plan (1080p to the level of the 2K), from section 3 step 3 on

Directive: the 1080p reaches the same features and remote control as the 2K, each within its own hardware. The row-by-row
state is in [parity-matrix.md](parity-matrix.md). Steps 0-2 (section 3) are done on `dev`: the 17 shared net/logic files
and `klog audio_jitter audio_vad g711` are in `src/common`. This section replaces section 3 steps 3-8 with finer steps.

### 7.1 Start condition (do not start earlier)

Two branches touch the files this plan moves or edits. Start only after both merge to `dev`, then rebase:

| In flight | Touches | Why it blocks |
|---|---|---|
| `integ/wifi-api` | 2K net/API files (`netreg.c`, `akam2k_api.c`, `settings.c`, `akam2k_httpd.c`) | P1-P3 and P8 move or reuse exactly these and their host tests |
| `feature/1080p-qp-tuning` | `src/ak_rtsp` `video_cfg.c`, `venc.c` | P10 builds on the QP keys it introduces; P4-P9 edit the same Makefile |

Even file moves that neither branch edits (`sha256 json_min api_sig api_keys api_limit`) conflict on the Makefiles and test
paths: wait.

### 7.2 Rules for every step

- **Byte-identical (BI) for pure moves.** Moving a file the 1080p does not link cannot change its binary: prove it with
  `tools/ci/repro_check.sh` (golden hash from step 0) and the 2K `make poc3` objects (`size`/hash). Moving a file the 1080p
  already links keeps `SRCS` link order and per-object flags.
- **Behaviour-identical by default (BC) for everything else.** New code is off or read-only until configured; with no new
  setting the camera behaves exactly as today. Order of proof: host suite (`tools/ci/local_check.sh`), RAM run from `/tmp` on
  **akam-01** (UART attached), 15 min soak, `video.fps_actual`, `ctl_sys` skipped counter, CPU of `ak_rtsp` equal to the
  baseline (4a protocol), then akam-02, then the flashed image.
- One step = one branch off `dev`, one reviewable diff (about 600 changed lines at most, moves excluded), merged locally after
  its proof, never pushed. Rollback = `git revert` (BI) or the previous binary in `/tmp` (BC). Never touch `mtd0`.
- `src/common` rule: no vendor header, no device path, no ioctl, a host test already runs it (section 2.1).
- 2K first: a shared module is validated on the 2K camera (its owner; checklists A1-A9, `hardware-test-checklists.md`), then
  bound to the 1080p. A 2K camera pass is not needed for BI moves.

### 7.3 1080p constraints that shape the steps

| Constraint | Consequence |
|---|---|
| One RTSP client slot, 777 KB encoder ring, 256 KiB TCP queue at PLAY | no feature adds a second video consumer; new threads stay off the video path; the API must not stall a keyframe being paced |
| RAM 64 MiB, about 40 MiB usable (`specs/architecture.md`) | per step: binary and RSS delta measured on akam-01 (`VmRSS`, `MemAvailable` before/after) and recorded in the commit. Registry plus API gate: +56 KB binary, +64 KB RSS (`specs/control-api.md` s9; measured on the host only, camera RSS not measured). Small threads get 32-64 KiB stacks |
| 8 MiB SPI NOR | no new periodic flash writes: every store call is skip-if-equal; the audit file is written rarely (limiter) |
| CPU already near 90 % (81 % sys, section 4a) | do the CPU fix (4a #11, #1) before adding threads; each step reports CPU before/after |
| Updates by SD card / maintenance flash (no hot appfs) | new modules ship inside the same `ak_rtsp` binary; no separate update path; rollback is the previous card image |
| 2.4 GHz only, no PTZ, no NPU | `net.wifi_band`, `ptz.*`, `detect.*`, `track.*`, `zones.*` are absent from the 1080p descriptor, not stubs |
| Static musl and raw ioctls (2K: dynamic uClibc and vendor libs) | drivers stay per model; only logic moves |

### 7.4 Model descriptors (what each model binds)

Shared modules take model data from a descriptor file, never from `#ifdef`:

| Descriptor | 2K (`firmware/lsc-rotatable-2k-ak3918av130/app/`) | 1080p (`src/ak_rtsp/`) | Used by |
|---|---|---|---|
| lights | `hal.c`: `ir` GPIO 48, `blue` GPIO 56 | `ak1080_hal.c` (new): `status` LEDCLASS `state_led`; IR emitter later | `light_logic`, `hal_light_sysfs` |
| settings table | `settings.c` (`st_def_t`) | `ak1080_settings.c` (new): `night.* ae.* isp.* video.* led.* sys.* net.hostname`, aliases for the 8091 names | `settings_reg`, API, MQTT |
| venc limits and ops | `video.h`, `vlive.c` | `ak1080_venc.c` (new) over `venc.c` | `video_cfg`, `vlive` |
| sys paths | `/proc`, wifi iface | same, iface name from config | `hal_sys` |
| API env (clock, key file, version) | `a2k_api_env_t` in `main.c` | equivalent struct in `main.c` | `api_*`, events |

### 7.5 Ordered steps

Sizes: S under 100 lines, M under 300, L up to 600 (moves excluded). BI/BC as in 7.2. HW = needs akam-01/akam-02.

| # | Step | Moves / changes | Class | Size | Proof | HW |
|---|---|---|---|---|---|---|
| P0 | Re-baseline | after both branches merge: rebase, new golden hash, CPU baseline and `VmRSS` of akam-01 recorded here | BI | S | `repro_check.sh`, measurement | read only |

**Status: P0 (hashes only, no hardware measurement, CPU/`VmRSS` still to do on akam-01), P1 and P2 done** on `convergence/p1-p2`. 1080p golden `6431e2c2...1479` unchanged; 2K `akam2k_rtsp` stripped `d472c552efc2d0e9e67218d3499b54d36cdbfe8e8d2b5658a4b794d719ddc6bf` identical before and after (the moved objects keep their place in the link order, `POC3_API` between `http_req` and `settings_reg`). sha256 lives in `net/`, the other four in `api/`.

**Status: P3 done** on `convergence/p3` (pure moves, no 1080p binary change). 1080p golden `6431e2c2...1479` unchanged; 2K `akam2k_rtsp` stripped `893cf9336a967bfa9845a893a45e343d6377784f378b9a4e55bc20fa5ced2b50` identical before and after (the 2K link order is kept token by token in `POC3_ORDER`; common objects are `c:` entries gated by `feat_objs`). Moved: `settings_reg` and the event ring (`akam2k_apievt` -> `api/api_events`, file rename only: the `a2k_api_event*` symbols and `A2K_EVT_*` macros keep their names, renaming them is a later source change) to `src/common/api`; `hal_sys`, `light_logic` + `hal_light.h`, `hal_input` (debouncer, needed by `button_logic.h`), `button_logic`, `button_gesture`, `button_cfg` to `src/common/logic`. Gates: `API` (events, registry), `BUTTON`, and two new features `SYS` and `LIGHT`. Left in the 2K app on purpose: `hal_light_sysfs.c` and `hal_input_sysfs.c` (they call the 2K pin whitelist `akam2k_pin`: bindings, not generic yet; they move when the pin helper becomes a `hal_gpio` interface) and `akam2k_store.[ch]` (carries the 2K `/config/akam` path constants and the anti-brick `rename()` listing; its skip-if-equal helper merges with `cfg_file` in a later step).

**Status: P4, P6, P7 and P8 done** on `convergence/p4-p8` (host-tested, never run on a camera; checklist B1-B14 of `hardware-test-checklists.md`).
One commit per step; the golden hash (`tests/golden/ak_rtsp_1080p.sha256`) is updated per step with its size delta. P5 (`swdog` split) was skipped on
purpose: nothing in P4-P8 needed it. What each step did, and where it departs from the table below:

- **P4** `ctl_sys.c` collects with one `hal_sys_sample()` and adds the six `sys.*` names (`sys.cpu_pct sys.load1_x100 sys.mem_total_kb sys.mem_avail_kb
  sys.uptime_s net.wifi_lq`; `net.wifi_rssi` already existed). No timer and no 10 s cadence: sampled when read (cache 1 s), zero cost while nobody reads.
  Stripped +2376 bytes. The old keys keep every assertion of `tests/ctl_sys_test.sh`.
- **P6** the status LED is the emitter `status` of a LED-class descriptor driven through `light_logic`; the rules `stream boot nowifi night idle` have patterns in the
  grammar of the 2K (`led_pat.[ch]`, the pattern code split out of `leds.c`). Defaults reproduce the old engine: `tests/leds_oracle_test.sh` runs the old engine (verbatim,
  `tests/native/leds_legacy.c`, kept as the oracle instead of a `LEDS_LEGACY` build flag) and the new one on the same traces, ~735k comparisons of write sequences,
  states, network checks, IR hook calls and the saved file; `leds_test.c` passes unchanged. The IR LEDs stay the `night.ir_led` rule inside `ircut.c` (not an emitter yet).
  Stripped +4904 bytes.
- **P7** `ak1080_settings.c`: 41 rows on the shared `settings_reg`; every row calls the module that owns the value; `control.c` offers a key it does not know to the registry
  (`control_set_registry`). Departure: the 8091 code was not rewritten onto the table (error texts stay byte for byte, the old code remains the writer behind the rows); `LIST` is
  unchanged. Stripped +17916 bytes.
- **P8** the generic API moved to `src/common` (`api_core api_httpd http_req api_backend.h`) and the 2K rebinds it (`api_bind.c`, 357-check HTTP test unchanged); the 1080p binds
  it in `ak1080_api.c`. Port 8092 opens only while a key exists, key tool `ak_rtsp --api-key`, ONE HTTP thread and no worker (settings run inline), buffers at accept,
  flash limiter, `night` and `setting` events, actions `led.blip led.blip_stop settings.reset`. Stripped +61560 bytes: the registry-plus-API total (+79.5 KB) is over the
  +56 KB gate of `specs/control-api.md` section 9, RSS not measured. `tests/ak1080_api_http_test.sh` (119 checks, ASan/UBSan) drives the real server.

Open after this branch: P9 to P13, the assistant side of the key tool (`akam api-key`), a writable `net.hostname` / `net.ntp` (needs the scripts, the app never spawns), the ONVIF port
list should also refuse 8092 (`onvif_cfg_port_ok` and `akam_onvif.sh` exclude 21-24, 554, 8091 only), and the camera pass of the checklist.

| # | Step | Moves / changes | Class | Size | Proof | HW |
|---|---|---|---|---|---|---|
| P1 | Pure moves, crypto/JSON | `sha256`, `json_min` from the 2K app to `src/common/logic`; both Makefiles via `common.mk`; test paths | BI | S | 2K objects equal, 1080p hash unchanged (not linked) | no |
| P2 | Pure moves, API security | `api_sig`, `api_keys`, `api_limit` to `src/common/logic` | BI | S | same | no |
| P3 (done) | Pure moves, registry and system | `settings_reg`, `hal_sys`, `light_logic` + `hal_light.h` + `hal_light_sysfs`, `button_logic`, the events ring (pure part of `akam2k_apievt.c`), the store helper (skip-if-equal of `store.c` over `cfg_file`) | BI | M | same, plus the 2K unit tests (`tests/rot2k_*`) on the new paths | no |
| P4 | `sys.*` on the 1080p | `ctl_sys.c` calls `hal_sys_sample` every 10 s from an existing timer (no new thread); keys `sys.cpu_pct sys.load1_x100 sys.mem_*_kb sys.uptime_s net.wifi_rssi` read-only on 8091 and `LIST` | BC additive | S | `control_test.sh` (old keys byte-equal), RSS delta | yes |
| P5 | `swdog` split | `swdog_st_*` pure half to `src/common/logic/swdog_core.c`, glue stays; the 2K capture watchdog adopts it in its own commit | BI (1080p), BC (2K) | M | `swdog_test.sh`, hash | 2K only |
| P6 | LED engine on `state_led` | `ak1080_hal.c` status descriptor; `leds.c` rules re-expressed as `light_logic` calls; `status=on|off|auto`, `ir_led auto|off` and the `/etc/config/akam/leds` file keep their meaning; IR emitter added when `ledsys.c` is wrapped | BC | M | `leds_test.sh` plus an oracle comparing old and new write sequences; visual check on both cameras | yes |
| P7 | Registry behind 8091 | `ak1080_settings.c`; `control.c` GET/SET/LIST become an adapter (aliases keep the old key names, errors and the write rule: loopback or debug flag); no new port | BC | L | 8091 contract test and `control_test.sh` pass unchanged; gate +56 KB / +64 KB measured | yes |
| P8 | Signed API on the 1080p | generic parts of `akam2k_httpd.c`/`akam2k_api.c` (env-injected) go to `src/common/net` with events and audit; 1080p binds them. **Port 8092 opens only when a key exists**; off by default; 64 KiB thread stack; connection buffers malloc'd at accept | BC, off by default | L | the 134-check HTTP test runs with both models' env; budget measured; `video.fps_actual` and CPU unchanged under a flood of bad requests; 2K camera A1-A9 done first | yes |
| P9 | MQTT generated from the registry | `mqtt_ops.c` entity list built from the registry; topics and HA discovery ids unchanged | BC | M | fake broker test equal; real broker run on the 2K first (still open) | yes, with broker |
| P10 | Live video and QP | after `feature/1080p-qp-tuning`: `hal_venc` ops, `vlive` state machine to `src/common/logic`; probe SET_ATTR/SET_QP/IDR on akam-01 one ioctl at a time (`venc-live-params.md`), UART attached, RAM only; keys `video.min_qp max_qp rc kbps gop fps` live where the kernel accepts, else the restart path; defaults identical (28/43/VBR+) | BC, defaults identical | L | probe log, bitrate window, no oops, quality matrix 3b.4 | yes |
| P11 | Hardware-bound features, only if the hardware exists | (a) second venc channel for `/stream1`, (b) button GPIO then `button_logic`, (c) JPEG snapshot. Investigation first; a negative result goes into the matrix as N/A | BC | S-M each | probe notes | yes |
| P12 | Logs | one `log.*` read key family over the klog ring on both models | BC additive | S | host + camera read | yes |
| P13 | Reverse parity (2K gaps) | opt-in ONVIF on the 2K from the generic stack, `klog` on the 2K, talk when the speaker path is solved | BC | L | per feature | 2K |

Rationale: P1-P3 are free and unblock the rest (no 1080p binary change). P4 and P5 are small and additive and give the first
camera numbers on shared code. P6 is visible and oracle-testable. The registry (P7) precedes the API (P8) and generated MQTT
(P9) because both read its table. Video (P10) comes last of the risky ones: it is the only step on a kernel interface that has
oopsed before.

### 7.6 Per-step validation checklist (akam-01 first)

1. Build from the branch, `make repro` hash recorded, host suite green, no new warnings.
2. Copy to `/tmp/ak_rtsp` on akam-01 with UART attached, run from RAM (the wrapper respawns the installed binary).
3. 15 min with one RTSP client: 14.9-15 fps, `ctl_sys` skipped 0, encoder errors 0, CPU within baseline, `VmRSS` delta recorded.
4. The step's own check (key reads, LED sequence, API call with a test key), then a second client gets 503.
5. akam-02 repeat; only then the flashed image and a power-cycle boot.
6. Record the measured deltas in the commit message and mark the row verified-on-hardware in `parity-matrix.md`.
