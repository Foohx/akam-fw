# LSC Rotatable 2K: running person / motion detection in OUR app (offline study + PoC-4)

Status: offline reverse engineering of the stock `/appfs/bin/yy_cam` and its vendor libs, plus a hardware PoC program
(`platforms/anyka-ak3918av130/probes/poc4_detect.c`) that **has run on the camera** (results in section 8; the class meaning is still unverified). Same conventions as [npu-findings.md](npu-findings.md) and
[archive/rot2k-own-app-feasibility.md](../../archive/rot2k-own-app-feasibility.md): addresses are virtual addresses in the stock `yy_cam` (non-PIE, base
0x10000) or offsets inside the named vendor lib; no vendor code, dump, model content or value is reproduced, only symbol
and string names, addresses and struct notes. Confidence: HIGH = read directly in instructions, MEDIUM = inferred from
several consistent observations, LOW = guess. Statements are UNVERIFIED unless section 8 reports a hardware result for them.

Why this matters: our app replaces `yy_cam` (own-app-feasibility), so the stock `Y_ALG_AV130_HD` / `_MD` threads are gone.
Person detection and motion must be run by us. The model is the camera's own file, read by the vendor lib from
`/appfs/resource/model/` on the device: never copied, never redistributed.

## 1. Summary

1. **The whole NPU inference runs inside the kernel driver (`ak_npu.ko`) behind three userland libs we already ship with
   the camera:** `libplat_npu` (open the node, `NNE_Init`), `libak_nne` (low level, loaded by `libplat_npu`), `libmpi_svp2`
   (the API: `ak_svp2_create_chn` loads the model, `ak_svp2_process` runs it). `libakv_cnn_dt.so` is **not** in the NEEDED
   list of `yy_cam` and is not needed: the detection post-processing (anchors, NMS, class/score tables) is inside the kernel
   module (its strings carry the same `hd_param.*` range checks and `HD_180P`/`HD_360P`, model ids `0x0A000007..0x0A000020`).
   HIGH (NEEDED lists, import tables, kernel module strings).
2. **The stock "hd" thread gets its frames from a THIRD VI channel** (`ak_vi_set_chn_attr` with channel id **16**, 320x192, NV12,
   depth 2, 20 fps requested), not from a resized copy of the sub stream and not from VPSS. The model input is 320x180
   (train size inside the lib; the model header says 320x192). HIGH on the values (config defaults of `video_chn3_*`),
   MEDIUM that the lib accepts id 16 exactly like the stock does (it has `vi_set_third_chn_attr`; hardware will tell).
3. **Call sequence** (HIGH, section 2): `ak_npu_init()` (no argument), `ak_svp2_create_chn(0, &attr, &conf)` with the model
   path, then per inference `ak_vi_get_frame(16)` -> `ak_svp2_process(0, &in, &out)` -> read results -> `ak_svp2_release(&out)`
   -> `ak_vi_release_frame(16)`. The stock then sleeps 100 ms: it runs at most 10 inferences per second.
4. **Output**: up to N records of 24 bytes `{class, x1, y1, x2, y2, score}` in pixels of the 320x192 image, score 0..1000
   (HIGH on the layout). **Classes: LOW confidence.** The model is a single-shot detector with 2 foreground classes
   (3-way softmax: background + label 1 + label 2); `target_type` 0 = both, 1 = label 1 only, 2 = label 2 only (HIGH from the
   driver: "object=%d support:[0,2]" and the filter code). The stock reports only class id 0. Whether class 0 is "person",
   "head and shoulders" or "body" cannot be told offline: PoC-4 prints the class of every result.
5. **Cost**: stock measured (npu-findings, hardware): `Y_ALG_AV130_HD` about 5 % CPU, almost all system time (NPU driver),
   same idle and walking. RAM: model file 985,012 bytes plus lib allocations (unknown, PoC-4 measures `VmRSS` and
   `MemAvailable` after the model load). Our budget: one inference at 10 Hz, the stock figure is the reference.
6. **Motion fallback** (section 5) is `ak_vpss_md_get_stat(dev, buf)` (hardware block statistics from the ISP, 1536 bytes)
   plus a software region filter (`libak_mrd`). Cheap, no NPU, but the statistics layout is **not** decoded (LOW); PoC-4 `-M`
   prints layout-independent figures so the layout can be inferred from a moving hand.
7. **PoC-4 status**: the program builds clean for ARM and links against the 2K vendor libs; every symbol it imports exists
   in the camera's uClibc 1.0.31 and the vendor libs. Stages 1-4 ran on hardware (section 8). The procedure (section 7) brings it up in four stages, each
   guarded by a 30 s alarm.

## 2. The stock "hd" pipeline, call by call

Source files (paths from the assert strings): `.../app_alg/algs/anyka_av130_human_detection/src/yy_alg_ak_av130_hd.cpp`
(module 0x27dc14..0x280a04) and `yy_media_av130_video.cpp`.

| # | Where | Call | Notes (confidence) |
|---|---|---|---|
| 0 | `start.sh` (rootfs, see PLAN 2.3) | `insmod ak_npu.ko` | `ak_npu.ko` v2.0.17 depends on `ak_ion.ko`; has `ak3918av130-npu` in its device-tree alias (HIGH). Must be loaded and no other process may hold the NPU ("plat npu is already in work": `ak_npu_init` fails when `yy_cam` runs). |
| 1 | `yy_alg_init` -> `yy_alg_ak_av130_hd_init` 0x27e8a4 | read config | keys of npu-findings section 2: master switch, mode options (0x102), `[dsp] ak_av130_svp2_hd_chn`, `[alg]` model/thresholds. The context is a 0x528-byte heap block (`g_ctx` at 0x3fcbe8). |
| 2 | `..._init_interface` 0x27f9e8 | `ak_npu_init()` | `libplat_npu` V2.0.05: opens its node itself, `ioctl` init, `NNE_Init`; returns 0 (HIGH). No argument. |
| 3 | same | `ak_svp2_create_chn(handle_id, &chn_attr, &conf_info)` | `handle_id` = `[alg] ak_av130_svp2_hd_handle_id` default **0** (lib: 0..3). Structures below (HIGH). Failure: the stock logs and calls `ak_npu_deinit`. |
| 4 | `..._start` 0x27ec20 | `yy_thread_create(Y_ALG_AV130_HD, 0x27dd50)` | thread name via `prctl(PR_SET_NAME)` |
| loop | thread 0x27dd50 | `yy_media_video_snap_nv12(chn=3, &img)` | `chn` = `[dsp] ak_av130_svp2_hd_chn` default 3 = **yy media channel 3**, a table entry (152 bytes per channel) whose VI channel id is 16 (section 3). It wraps `ak_vi_get_frame(16, &frame)` and, for id 16, keeps the 40-byte frame struct in a small wrapper (no pixel copy: zero copy; the wrapper's release calls `ak_vi_release_frame(16, ..)`) (HIGH). |
| | 0x2801bc | `ak_svp2_process(handle_id, &in, &out)` | below. Non-zero return: `ak_npu_reset_hw()` and an error log. |
| | | `ak_svp2_release(&out)` | after the results were converted. |
| | 0x28062c / 0x27fd8c | convert and filter | results with class 0 are turned into percent boxes `x*100/(w-1)`, `y*100/(h-1)` (float math), the largest by area is the "main" target for tracking (`mt`), ROI overlap checks. |
| | | `usleep(100000)` | 100 ms between inferences (word at 0x27de74 = 0x186a0). |
| teardown | 0x27fb34 | `ak_svp2_destroy_chn(handle_id)`, `ak_npu_deinit()` | |

`ak_av130_svp2_hd_skip_frames` (0, ctx+0x44c) and `[dsp] ak_av130_svp2_hd_score_threshold` (750, ctx+0x454) are **stored but never
read again** in the module (only the two stores exist): they have no effect in this firmware (HIGH, exhaustive search of the
module's offsets). The effective score filter is the lib's `classify_threshold` (create argument 700, later changed by
`update_param_input` through `ak_svp2_set_param`; the sensitivity DP maps `classify = base + 100 - sensitivity`, MEDIUM,
functions 0x27ee98 / 0x27ef28). Frame skipping is simply the 100 ms sleep.

### 2.1 Structures (our transcription, `_Static_assert`ed in `poc4_detect.c`)

`chn_attr` (`ak_svp2_create_chn` arg 2, built at 0x27e240; HIGH, offsets read in the lib's own range checks):
`u32 model_num` (1), then `model_num` entries of 20 bytes `{env_mode 0 (0 day, 1 night), model_type 10 (<= 47),
classify_threshold 700 (0..1000), IoU_threshold 3 (0..10), target_type 0 (0..2)}`.

`conf_info` (arg 3, `ctx+0x4e0`): `{char *path; u32 conf_mode (0 = file path, 1 = memory buffer); void *pReadFunc (NULL = default parser)}`.
The stock path default is the model file named in npu-findings; the lib `stat()`s it ("error stat of slim file") and loads it into
DMA memory itself.

`process input` (arg 2, 40 bytes, 0x280264..0x2802cc; HIGH): `+0` 0, `+4` img_type (0 = YUV420SP, 1 = RGB line), `+8/+12` width/height,
`+16/+20/+24/+28` region of interest left/top/width/height (the stock: 0, 0, w, h), `+32` physical address, `+36` virtual
address (the lib requires `+32 == ak_mem_dma_vaddr2paddr(+36)`, so we compute it instead of trusting a frame word).
The stock fills `+32` from word 6 and `+36` from word 0 of the 40-byte `ak_vi_get_frame` struct (that struct's word 0 is the
virtual address of the NV12 data, word 1 its length; PoC-2 already relied on this).

`process output` (arg 3, 16 bytes): `+0` count, `+4` pointer to count 24-byte results (valid until `ak_svp2_release`), `+8`
result type (lib internal), `+12` second word. Result `{i32 class; i32 x1, y1, x2, y2; i32 score}` (stock reads class at +0,
the four corner coordinates at +4..+16, score at +20; HIGH).

## 3. Which frame feeds it

- Defaults of the `video_chnN_*` loader (0x29a8d8..0x29c334, one 152-byte record per channel, `video_chn_num` default 4):

| yy media chn | `vi_enable` / `ve_enable` | VI chn id | size | fps | depth | role |
|---|---|---|---|---|---|---|
| 0 | 1 / 1 | 0 | 2304x1296 | 15 | 2 or 3 (DMA pool) | main stream |
| 1 | 1 / 1 | 1 | 640x360 | 15 | 2 or 3 | sub stream |
| 2 | **0** / 1 | 1 | 640x360 | 20 | - | second encoder on the sub frames (no own VI channel) |
| **3** | **1 / 0** | **16** | **320x192** | **20** | **2** | **detector input, no encoder** |

- The init loop (0x29d780..0x29dc80) calls `ak_vi_set_chn_attr` for every record with `vi_enable` (id, fps, w, h, depth), then
  `ak_vi_enable_dev`, then `ak_vi_enable_chn(id)` for each: so the third channel is configured **together with chn 0 and 1,
  before `enable_dev`** (HIGH). `libplat_vi` has `vi_set_third_chn_attr` and "Device third channel[%d] is not disable" strings,
  which is where id 16 goes (MEDIUM).
- Format NV12 (YUV420SP semi-planar, Y then interleaved UV, 320 x 192 x 3/2 = 92,160 bytes): `snap_nv12` computes `w*h*3/2` and
  the process call is made with img_type 0 (HIGH).
- Cost of that third channel: one more DMA frame set of 92 KB x depth, negligible; the ISP scaler produces it in hardware.
- Alternatives (not needed): a resized copy of the sub stream (software, CPU cost) or the main channel (2K, too big).

## 4. Model and classes (LOW)

- File `model_0x0A00000D_V1.00.01.bin`, 985,012 bytes; header (little-endian words): parse version 2, header size 0x30, total
  size equals the file size, model label `0x0A00000D` (type 10, index 13), version 1.00.01, input 320 x 192 (0x140 x 0xc0).
  The kernel driver checks "header.model_size != model_size" and "model_id and header.model_label conflict": a wrong file is
  rejected at load, not run.
- The detector family in the driver: `HD_180P`, `HD_360P`, `FACE_DT_180P`, models `0x0A000007..0x0A000020` (several are
  face recognition / attributes). Ours is the 180p human detector. The userland lib `libakv_cnn_dt` (not loaded by `yy_cam`)
  carries the same network descriptions (`slim320`, `slim320_face`, `slim480`, `slim640`, `slim640_pets`): `slim320` train frame
  320x180, 5,875 candidate boxes, 4 score layers; a 3-way label function (background, 1, 2) with a softmax table, and a
  separate 2-class table `softmax_table_int_2class` used by the face network.
- So the generic detector reports **two foreground labels**; `target_type` selects (0 both, 1 label 1, 2 label 2), the stock
  default 0 and it only acts on class 0. **Do not assume pets or vehicles** (there is a separate `pets` network, not ours).
  What labels 1 and 2 are (full body and head/upper body? person and something else?) is **not** readable offline.
  PoC-4 prints the class of every box: walk in front, then try `-T 1` and `-T 2` to separate them, and compare box shape
  (tall = body, small and high = head).

## 5. Motion detection as a cheap fallback (MEDIUM / LOW)

The stock wrapper `ak_svp_md_*` (inside `yy_cam`, 0x27bfa4..0x27d970, thread name `svp_md` driven by `Y_ALG_AV130_MD` at 0x279634):

- `ak_svp_md_init` -> `ak_mrd_init` (`libak_mrd`: software "motion region detection"), `ak_vi_get_sensor_resolution`, set day/night
  filters (`ak_mrd_set_filters`: small/big filter per day and night) and size limits (`ak_mrd_set_motion_region_size`,
  stock `move_size_min` 4, `move_size_max` 400), sensitivity (`ak_svp_md_set_sensitivity`), start the thread (stack 200 KB).
- Per tick (`md_fps` 5): `ak_vpss_md_get_stat(dev 0, buf)` with a zeroed **1536-byte** buffer (`libplat_vpss`, returns 0 on success
  ("get 3d nr stat info fail" on error: the statistics come from the 3D noise reduction block), then `ak_mrd(handle, buf, 24,
  out)` which returns a count and fills up to 100 words/rects (the stock output buffer is 400 bytes).
- Why a fallback: no NPU, no third VI channel, a few hundred bytes per tick. Why not the main path: the grid layout of the 1536
  bytes (probably 24 columns x 16 rows of 32-bit values: 384 words) and the `ak_mrd` output layout are not decoded, and its
  sensitivity default in the stock without the cloud is 0 (no events: npu-findings hardware results). PoC-4 `-M` prints the
  non-zero word count, the maximum and the sum each loop; waving a hand then shows whether the numbers move. Decoding is a
  follow-up if the NPU path is too heavy.
- Simplest own motion if the layout stays unknown: difference of two consecutive 320x192 Y planes from the same third channel
  (CPU, about 61 K pixels, a few ms on the ARM926; no vendor call). Not implemented, noted as a fallback.

## 6. Risks and open questions

1. VI channel id 16 might need more than `set_chn_attr` (e.g. a dev attr field): stage 1 of PoC-4 isolates it. If
   `ak_vi_set_chn_attr(16)` fails, read the vendor log line (it names the cause).
2. NPU exclusivity: it must not be held by another process. The PoC-1 stub does not start `yy_cam`; check the node holder first.
3. `ak_svp2_process` blocks until the NPU finishes; the 30 s alarm covers a hang, but a hung NPU may need a power cycle.
4. Memory: the NPU model and the 2K VI buffers on 38 MiB RAM (24 MB free without `yy_cam`). Abort below 6 MB available.
5. Class meaning (LOW) and the real false-positive rate: only a walk test answers; thresholds are runtime parameters (`-c`).
6. License: the model file belongs to the vendor firmware and is only read from the device; never copy it into the repo,
   a release or a card image. Our kit installs nothing model related.
7. The NPU and the encoder share the sensor/ISP pipeline: our encoder path (PoC-3 RTSP) running at the same time was not tested;
   PoC-4 runs without an encoder (VI only), which is a smaller load than the stock.

## 7. PoC-4 program and hardware procedure

`make -C platforms/anyka-ak3918av130 poc4` builds `dist/rot2k-app/poc4_detect` (also part of `make all`). Needs
`dist/rot2k-app/vendor` from `prepare_vendor.sh` and the pinned toolchain. Link adds `-lmpi_svp2 -lplat_npu -lak_nne`. Run with
`LD_LIBRARY_PATH=/appfs/lib`. Options: `-S 1..4` stage, `-n` inferences (default 50), `-t` guard seconds (default 30, 8..120; the
loop stops 4 s earlier so the normal path tears down), `-m` model (must be under `/appfs/resource/model/`), `-c` classify
(700), `-i` IoU (3), `-T` target type (0), `-p` ms between inferences (100 like the stock), `-M` poll the motion statistics.

Preconditions (coordinator): the camera is on the PoC-1 stub (no `yy_cam`), SSH works, the card build with the watchdog feeder is
active. `grep ak_npu /proc/modules` shows the module, and no process holds the NPU node (`ls -l /proc/*/fd 2>/dev/null |
grep npu` is empty). Baseline `free`. Copy to RAM: `scp poc4_detect root@CAM:/tmp/`, `chmod 755`.
Never run two instances. Never retry in a loop after a timeout: report the step and the log first.

Stages (each `LD_LIBRARY_PATH=/appfs/lib /tmp/poc4_detect -S N ... 2>&1 | tee /tmp/poc4_sN.log; echo rc=$?`, copy the logs to
`/mnt/sdcard/akam/` afterwards):

| Stage | Command | Expected (abort criteria in the next table) |
|---|---|---|
| S1 | `-S 1` | `ak_vi_set_chn_attr(16, 320x192, 20 fps, depth 2) = 0`, `ak_vi_enable_chn(16) = 0`, 3 `frame N: ak_vi_get_frame(16) = 0 data=0x... len=92160 ... phys_word=0x...`, then `ak_mem_dma_vaddr2paddr = 0 phys 0x... (equals the frame word)`, clean teardown, exit 0 |
| S2 | `-S 2` | S1 plus `ak_npu_init() = 0`, `mem after npu init`, `ak_npu_deinit() = 0` |
| S3 | `-S 3` | S2 plus `model file ...: 985012 bytes`, `ak_svp2_create_chn(...) = 0`, RSS/MemAvailable after the model load, `ak_svp2_destroy_chn(0) = 0` |
| S4 empty | `-S 4 -n 100 -p 100 -M` (nobody in view) | 100 lines `inf N: dt=.. ms count=0 type=..` mostly count 0 (a few spurious boxes are information), `md N:` lines, summary `process() min/avg/max`, exit 0 |
| S4 walk | `-S 4 -n 250 -p 100 -t 40` and the user walks in front of the camera for the whole run (in view 10 s, out 10 s, again) | `count=1` with `class=0 score=7xx-9xx` and a box that follows the person, `count=0` when out of view, `pct=(...)` values in 0..100; summary with `cpu user .. sys ..` |
| S4 target | the same walk with `-T 1` then `-T 2` (shorter, `-n 100`) | tells which label the person is; record the class numbers and box shapes |

Record for each S4 run: dt min/avg/max (expected tens of ms, LOW), the CPU totals, `VmRSS`, `VmHWM`, `MemAvailable` (before, after
model load, after loop), number of inferences with a result, class and score range, box position versus where the person was.
Pass: S1..S3 exit 0 with clean teardown; S4 exit 0, at least one result during the walk and none (or clearly fewer) in the empty
scene, `MemAvailable` above 6 MB throughout, camera still answers SSH afterwards, no reset.

Abort criteria (stop, do not retry, report the log): the program's guard fires (`TIMEOUT ... in step: X`, exit 124: the step is the
finding; if the step is `ak_svp2_process` or `ak_npu_init`, power-cycle before any further attempt); a crash line (exit 2); a
vendor error from `ak_vi_set_chn_attr(16)` (S1 failure: report the lib line); `ak_svp2_create_chn` non-zero (report the lib
message: wrong struct layout is the likely cause, not a bad file); three consecutive `ak_svp2_process` failures (the program stops
itself); `dmesg | tail` shows NPU/ion errors or an oops; `MemAvailable` below 6 MB (the program also stops itself, checked every 20 inferences); the feeder `watchdog` is gone; any unexpected
reboot. Nothing persistent is written by the program (it opens no file of its own; the log is the shell redirect). Cleanup:
`rm /tmp/poc4_detect /tmp/poc4_s*.log`.

## 8. Hardware result (PoC-4, camera on the PoC-1 stub, coordinator run)

| Stage | Result | Confidence |
|---|---|---|
| S1 | rc 0. VI channel id 16 accepted next to chn 0/1; frames 320x192 NV12; `ak_mem_dma_vaddr2paddr(frame data)` equals the frame struct's physical word | HIGH (observed) |
| S2 | rc 0. `ak_npu_init()` / `ak_npu_deinit()` clean | HIGH (observed) |
| S3 | rc 0. Model load with the stock `model_0x0A00000D` file: about 3 MB drop in `MemFree` | HIGH (observed, single run) |
| S4 | rc 0, 50 inferences, `ak_svp2_process` min/avg/max 26.1 / 26.9 / 29.8 ms (about 10 Hz with the 100 ms pause: the NPU is busy roughly 21 % of the time); 0 results with nobody in view; clean teardown | HIGH for the timing and the empty scene (one run, 50 samples) |
| RAM | `MemAvailable` about 20.9 MB during the loop, 22.1 MB after teardown | HIGH (observed) |

Not yet known: the class meaning (person versus head/body, section 4: **LOW**, needs the walk test), true and false positive rates
(only the empty scene was seen: no result in 50 inferences says nothing about detection quality), the CPU cost of the loop and
behavior alongside an encoder (section 6, item 7). The sections above describing the call sequence and layouts are now backed by
the fact that the vendor lib accepted them (create, process, release, destroy all returned 0); the output layout itself is
unexercised until a result exists.

## 9. Walk test 2026-11-15 (hardware, akam-rot-01, one run, user in view) — DETECTION WORKS

`poc4_detect -S 4`, default thresholds (classify 700), camera in our-app mode, low indoor light. 199 inferences in
26 s (process() 26.1/27.3/29.5 ms, ~7.6 Hz with the 100 ms pacing), 158 with results:

- 0–21 s (user in view, front then moving/profile): **100 %** of inferences returned results; 21–24 s (leaving):
  4/23; 24–27 s (empty scene): 0/22 → no false positive in this run.
- Every positive inference returned **two boxes**: **class 0 = whole person/body** (large box, e.g. 182×158 px of
  320×192, score ~930–946/1000) and **class 1 = head** (small box inside the top of the class-0 box, ~35×50 px, score
  ~860–900). Confidence MEDIUM (one person, one scene, low light); `target_type` 1/2 selects one of them.
- CPU: user 230 ms + sys 1070 ms over 26 s ≈ 5 % of one core (same order as stock's HD thread).

Next: pace to ~5 Hz, expose events (MQTT/HA) with box + score, optionally drive auto-tracking with `akam_ptz` (after its
end-stop protection), and run detection alongside RTSP in the same app (shared VI device: chn 16 + chn 0/1).

## 10. Detection inside `akamd` (2026-11-15, offline, not yet run on hardware)

The walk-test pipeline of sections 7 to 9 now lives in the production app, next to both RTSP streams; the full description,
options, state file keys and the step-by-step hardware procedure are in
[archive/rot2k-own-app-feasibility.md](../../archive/rot2k-own-app-feasibility.md), section "Person detection inside `akamd`".

Decisions taken from the walk test:
- Only **class 0 (person/body)** drives presence; **class 1 (head)** is counted (`heads`) and its best score kept (`head_score`)
  as extra information. The state machine needs K=2 consecutive inferences with a class 0 box for ON and 3 s without one for OFF
  (so a single missed inference at 5 Hz, 200 ms, never ends a presence).
- Rate 5 Hz by default (1..10): at about 27 ms per inference the NPU is busy about 13 % of the time (PoC-4 measured 5 % CPU of
  one core at 7.6 Hz); the third VI channel runs at 20 fps with depth 2 and each frame is released right after `ak_svp2_process`.
- Results are validated as in PoC-4 (count at most 64, corners ordered and inside 320x192, score 0..1000); an implausible
  output is ignored, a bad box is skipped.
- Events are a log line and `/tmp/akam2k_detect.state`; MQTT/Home Assistant is published by the app itself, straight from
  the detection state module (see the "MQTT and Home Assistant" section of [archive/rot2k-own-app-feasibility.md](../../archive/rot2k-own-app-feasibility.md));
  the file stays for SSH debugging.

Open until the hardware run: CPU and RSS of the whole app with both encoders at 5 Hz, `MemAvailable` cost of the model next to
two encoders, and whether chn 16 coexists with chn 0/1 under load (it did in PoC-4 with the encoders absent).

## 11. In-app detection misses a still person (hardware 2026-11-15) - root cause CONFIRMED, fixed

Symptom (same scene, low light, user centered 2-3 m): inside `akamd` (dev 75313e2) detection goes ON only while the user
ENTERS the frame (score 0.80-0.87), then OFF after 3 s and stays OFF while the user stands still; ~300 inferences/min, 34 ms avg
per frame+process, `person=0`. With the app stopped, standalone `poc4_detect -S 4 -n 40` at the same position: 40/40 class 0,
score ~835, box (40,10)-(158,191), process() 27 ms. Model, NPU and call sequence are fine.

**ROOT CAUSE (HIGH, confirmed on hardware): stale queued chn 16 frames.** VI chn 16 (320x192, 20 fps, depth 2) keeps frames queued
and one `ak_vi_get_frame` per 200 ms tick returned an old one, so the NPU saw a past scene (user not yet in the picture, or only
the moving part). Verified with the drain build (`--detect-debug`): empty scene -> count 0; user standing still centered ->
continuous `person=1` for the whole stay, score 939-946, box ~50,17,206,191 (+ a head, count 2); user leaves ->
`person OFF after 30.5s (no person for 3.0s)`. Stats per minute: 645 stale frames skipped, 0 boxes rejected, frame age 3-33 ms avg.

The box-edge clamp (earlier suspect, MEDIUM) was NOT the cause (0 rejected); it is kept, it is harmless.

**Side effect of the first drain (found by the same run):** it looped "get until the queue answers empty (max 4)", but
`ak_vi_get_frame` BLOCKS on an empty queue until the next frame (~50 ms at 20 fps), so every tick burned the 3 extra gets:
`frame+process` 279-281 ms avg (556 max) = ~3.6 Hz instead of 5 Hz (CPU idle still ~91 %).

Final fix (all in `detect.c`; 1080p untouched, `git diff dev -- src/` empty):
- `fetch_newest()`: at most `MAX_DRAIN` = 2 gets per tick (the queue depth, so nothing older than the newest can remain), and no
  second get after one slower than `SLOW_GET_MS` = 10 ms (it waited for a new frame, which is thus already the freshest). The
  older frame is released at once; never more than 2 frames held; the block is bounded to one get of ~50 ms. Chosen over lowering
  the chn 16 rate: that would change the stock-like channel setup (20 fps depth 2) and still block when the queue is empty.
  Expected: at 5 Hz the queue is full at each tick, both gets are immediate, so the cycle stays ~200 ms.
- `collect()` clamps boxes into the image; only unordered corners or a score outside 0..1000 are rejected (counted).
- `--detect-debug` (off by default): at most one line per second, `detect-debug: #N ts= dts= age=<ms> drained=<n> count=<n>
  rejected=<n> person= score= box= heads=`; `age` is relative to the freshest frame seen. The 60 s stats line has positives,
  frame age, stale frames skipped, boxes rejected.
- Test `tests/rot2k_detect_fresh_test.c` (stubbed depth-2 queue): NPU on the newest, older released, get/release balanced; after a
  slow first get the thread pulls only once that tick.

To verify on hardware: with `--detect-debug`, the stats `frame+process` average should fall back to ~35-70 ms (inference rate
~300/min instead of ~215) while `drained` stays 0-1 per tick and the still-person case stays `person=1`.
