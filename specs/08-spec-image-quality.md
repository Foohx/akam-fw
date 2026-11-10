# Akam: specification, image quality (grain) against the stock firmware

Status: written 2026-11-10 (branch `feature/image-quality-stock`, from `dev`) from a block-by-block comparison of a stock day / night capture with what `ak_rtsp` sends. English, like the code. Everything in sections 3 and 4 is derived from a private stock capture and from the code; nothing has run on an Akam camera yet (update 2026-11-10: the decisions of section 11 changed two defaults and rewrote the night switch): the switches of section 5 and the dump tool of section 6 exist so that section 7 can close the open points on hardware. Defaults are unchanged.

## 1. Goal

The image of an Akam camera is grainier than the stock firmware's on the same model (LSC camera, Anyka AK3918AV130, GC20C3 sensor), see `docs/upstream_background.md`. Find the causes, make every plausible fix testable on a camera without reflashing, and fix what is certain.

Out of scope: a new ISP tuning, AE rewrite, WDR, new encoder features.

## 2. Material and handling

- A stock capture of camera #3 (`tools/hw_tests/stock_capture.sh run --daynight`, 300 s: day, lens covered at about 51 s on the hook clock, uncovered, night until 175 s, then day again): every ISP parameter block written or polled by the stock `anyka_ipc` in full with its time stamp, the AE state, the `CREATE_ENC` payload, the kernel log lines of the encoder.
- The `sensor_isp.conf` (`isp_gc20c3.conf`) of the same firmware, and the code of `src/ak_rtsp`.
- The published screenshots `images/video_out/` (stock and Akam, same scene).

Vendor data stays private: this spec quotes counts, times and the few parameters Akam itself sends, never a table. `tools/hw_tests/parse_stock_capture.py` (summary, `--export-isp`) reads the capture.

## 3. Findings

### 3.1 The ISP blocks are the same: stock sends the file, unmodified

The stock application writes 30 ISP blocks (24 modules of `sensor_isp.conf`, some with 2 or 3 sub-blocks). Compared byte by byte with the two subfiles of the file (mode 0 = day, mode 1 = night):

| State | Result |
|---|---|
| Stock at start-up (T = 2.0 s) vs day subfile | 30 of 30 blocks identical |
| Stock after the day to night switch vs night subfile | 30 of 30 blocks identical |
| Stock after the night to day switch vs day subfile | 30 of 30 blocks identical |

So stock never edits a block (no strength, gamma or sharpness computed at run time) and `isp_pipeline_init()` / `isp_pipeline_init_night()` send exactly what stock sends. 23 of the 30 blocks differ between day and night; the 7 identical ones are RAW_LUT, GB, DEMO, SHARP_EX, RGB2YUV, MISC and AWB_CALIB. Day to night changes, among others, NR1 (240 of 766 words), NR2, UVNR, 3D_NR (822 of 1769), WDR, SHARP (2709 of 11646), RGB_GAMMA, Y_GAMMA, CCM, SATURATION, CONTRAST, BLC, LSC, WB, AWB_EX, the exposure block and the frame rate.

### 3.2 No dynamic "auto ISP" linkage in user space

Each block is written 3 times in 300 s: start-up, night switch, day switch (the 7 identical ones once). Between the switches nothing is written but the OSD text (about 8 writes per second, the time stamp). All 23 changed blocks are rewritten within 0.1 s of the switch (51.497 to 51.585 s), after the IR LED GPIO moved (51.46 s) and before the IR-cut GPIOs (51.61 to 51.72 s), in a fixed order (BLC, LSC, NR1, NR2, UVNR, 3D_NR, RGB_GAMMA, CCM, FCS, WDR, SHARP, SATURATION, CONTRAST, EFFECT, DPC, LCE, AF, WB, AWB_EX, exposure, frame rate, sensor fps, Y_GAMMA, HUE).

One thing is NOT in the file: after each switch the stock application calls SHARP once more and EFFECT three more times (the picture settings are re-applied; EFFECT repeats the content it just got, SHARP's content changes again). The hook rate limit did not store that last SHARP content, so the sharpness the stock really runs with after a switch is unknown (it comes from the stock's own sharpness setting, like `isp_set_sharpness()` here, whose neutral value leaves the file's block unchanged). Sharpening amplifies grain, so this is the one open point of 3.1; section 6 closes it by reading the live blocks of the stock camera itself.

The per-gain tables are inside the blocks: NR1, NR2, UVNR, SHARP and others carry several gain levels (NR1 has 8 gain thresholds), and the kernel driver selects the level from the AE gain. `ak_rtsp` sends the whole blocks too, so it already "uses" them; there is nothing to add in user space. What stock polls while running: the AE state (`GET_EXPOSURE`, `GET_AE_RUN_INFO`, 10 to 14 Hz, like `ae.c`), the 3D-NR statistics (`GET_3D_NR_STAT`, 401 reads in 330 s), the sensor fps (`GET_SENSOR_FPS`, 636 reads), and, in night mode only, two 256-byte statistics blocks (14 Hz). None of them is followed by a write.

### 3.3 Gain and exposure limits are the same, and low

The exposure block of the file limits the sensor gain to 1.0x in both modes and the ISP gain to 1.5x (day) or 3.0x (night); the longest exposure is the frame time (VTS 2284 lines at 15 fps, 3426 at 10 fps). In the capture the sensor gain never left 1.0x, even with the lens covered (exposure at its maximum 16384 within 2 s, ISP gain 1.5x). `ak_rtsp` keeps these limits. So the grain does not come from a higher analog gain, and an Akam camera in the same light has the same exposure and gain unless its AE decides otherwise (to verify, section 7).

Differences in the AE settings: the file has dead-band 4, hold range 5 (night 6), speed 16 and `ae.c` overrides them with 4 / 4 / 7 (a past fix for hunting). This changes how fast the AE settles, not the level it settles at. Note: what `ae_tuning_t` calls `exp_max` (word 1 of the block's public part) is really the SHORTEST exposure (2 lines in the file, as in stock); the longest is word 0. The 8091 key keeps its name.

### 3.4 Day / night switching differs

Stock switches to night only when the AE is saturated: in the capture, 1 to 2 s after the exposure reached 16384 with the ISP gain at its 1.5x maximum (10 confirming samples). Its configured threshold, 6 300 000, is the product exposure x ISP gain (Q8): 16384 x 384 = 6 291 456. `night.c` used to trigger at `hw_exp > 3000` for 10 samples, about 18 % of the exposure range: in a lit room (seen on a camera) the Akam camera was in night mode (IR LEDs, 10 fps, night tables, grainy and grey) while stock stays in day mode. This is a different picture, not noise by itself. Reworked, see section 11.

### 3.5 Encoder: wire parameters equal, one sub-struct differs

From the stock `CREATE_ENC` and the kernel log of the stock encoder: H.264 Main, 1920x1080, 15 fps, GOP 30, rate-control mode 5 which the library calls AVBR, target 1520 / max 1536 kbit/s, min QP 28, max QP 43, initial QP 99 (the library logs 38), `smart_goplen` 100, `smart_quality` 50, smart mode 0, MMA on, IP delta 6. `venc.c` sends the same enc config and smart words (confirmed by capture on 2026-07-04).

The only difference is the `rc` sub-struct (bytes 0x40 to 0x5f):

| Field | Stock | `ak_rtsp` |
|---|---|---|
| `rc_flag` (0x3c) | 1 | 1 |
| `rc.flag` (0x58, MMA) | 1 | 1 |
| `rc.minqp`, `rc.maxqp` | 0, 0 | 28, 43 |
| `rc.srd_threshold` | 0 | 128 |
| `rc.flag2` (MVV range) | 0 | 40 |

The stock log says `bEnableSRD = FALSE`, `MVVRange_factor = 40` (the library default), `MinQP = 28(28)`, `MaxQP = 43(38)`. What the library does with non-zero `rc.minqp/maxqp/srd_threshold` is unknown; `srd_threshold != 0` may enable SRD. The comment of `venc.c` records two kernel crashes with an all-zero or `rc.flag = 0` struct; both had MMA off while `VENC_IOCTL_QUERY_DMA` sizes the ring pool with MMA on (`buf[29] = 1`), which is the pool shortfall the oops shows. The stock struct (`rc.flag = 1`, all other words zero) keeps the pool consistent. Not tried on an Akam camera.

Why it matters for grain: in the stock kernel log the adaptive rate control "learns" the scene over the first 1080 frames (72 s) and reports `210 kbps and QP is 42` for a static scene (and `43 kbps, QP 42` for the 640x360 stream). A QP of 42 smooths sensor noise away, a QP near the minimum 28 encodes it. If the Akam encoder, with its different `rc` words, does not learn or holds a lower QP, the stream is grainier and heavier at the same sensor noise. Whether it does is the first thing to measure.

### 3.6 What else differs (all small)

- WB: `ae_init_isp_params()` sends a hand-made manual WB block (type and 3 gains, the rest zero) after `isp_pipeline_init()` has sent the file's block (the same type and gains, plus colour-temperature tables). Stock sends only the file's block. The published screenshots show a clearly different colour cast under the same scene (stock olive / yellow, Akam blue), so the live colour pipeline is not the stock one; the zeroed tables are the only known difference in the block. This is colour, not grain, but it says the live ISP state needs a check.
- Stock reads the 3D-NR statistics about every 0.8 s and the sensor fps about every 0.5 s; `ak_rtsp` does not. Probably harmless, but it is the only run-time ISP traffic stock has that Akam lacks.
- After `SET_FRAME_RATE` the AE tuning is re-applied by `night.c`, so night uses 4 / 4 / 7 instead of the file's 4 / 6 / 16.
- The stock encodes 2 streams (1920x1080 and 640x360), captures a third one (320x180) for the NPU and runs it; Akam encodes one. No effect on the main stream's noise.

## 4. Root-cause ranking

Honest status: no cause is proven. Ranked by (probability x size of the effect), with what would confirm each.

| # | Cause | Confidence | Confirmed by |
|---|---|---|---|
| 1 | Encoder `rc` sub-struct (3.5): different QP behaviour than stock's adaptive rate control | medium | kernel log of Akam's encoder (`eRCMode`, `MinQP`, `MaxQP`, `bEnableSRD`, the "finish the learning" line, QP 42 in stock) and bit rate on a static scene; then `AKAM_VENC_RC=stock` |
| 2 | Live ISP state is not the file's (WB block zeroed, any other drift): colour cast already differs | medium | `isp_dump --blocks` / `img.dump` compared with the file and with the stock capture (section 6); then `img.wb=conf` |
| 3 | Different exposure / mode in the same light (night trigger at 18 % of the range, AE settings) | medium for dim scenes, none for bright ones | AE log lines (`[ae] window ...`) on both cameras, `night.state` |
| 4 | The comparison itself (sub-stream, app post-processing, screenshots at different scales, two different units) | high that it matters | compare RTSP main streams with `tools/hw_tests/image_noise.py`; same camera with both firmwares if possible |
| 5 | Missing stock statistics polls (3.6) | low | `img.poll=1` |
| 6 | Sharpness / picture settings the stock re-applies after a day / night switch (3.2): unknown content | unknown | live dump of the stock camera (section 6, step 4), then `isp.sharpness` (existing key) to match |

Ruled out by the capture: block contents at start-up and at the switches (3.1), run-time per-gain updates (3.2), analog gain headroom (3.3), frame rate, GOP, bit rate and profile on the wire (3.5). Open: the sharpness stock applies after a switch (3.2).

## 5. Changes

All behind switches, defaults = previous behaviour (no flip before the hardware test of section 7). New module `src/ak_rtsp/imgq.c`.

| Switch | Values | Where | Effect |
|---|---|---|---|
| `venc_rc` / env `AKAM_VENC_RC` | `legacy` (default), `stock` | start-up only | `CREATE_ENC` `rc` sub-struct exactly as stock (3.5) |
| `wb` / env `AKAM_IMG_WB` / 8091 `img.wb` | `conf` (default), `manual` | start-up and live | `conf`: keep the file's WB block (default since the user preferred its colours); `manual`: the hand-made block, re-applied after every day switch; live `conf` re-sends the day or night block of the current mode |
| `poll` / env `AKAM_IMG_POLL` / 8091 `img.poll` | `0` (default), `1` | start-up and live | poll the 3D-NR statistics (12 AE ticks) and the sensor fps (8 ticks) like stock |
| 8091 `img.reload` | `1` | live, write only | re-send the whole file for the current mode, reset the picture baselines, re-apply the AE tuning, re-apply the manual WB if `img.wb=manual`: a known state to compare from, whatever was tried before |
| 8091 `img.dump` | `confirmed`, `all` | live, write only | read the live blocks, log one line per block against the file's day and night subfiles (8091 `LOG` lines and serial), write `/tmp/isp_live/isp_blocks.bin` |
| 8091 `img.venc_rc` | | read only | shows the active choice (it cannot change after the encoder exists) |

Start-up options are read from the environment, then `/tmp/akam_image` (RAM, gone at reboot), then `/etc/config/akam/image` (saved), as `key=value` lines (`venc_rc=stock`, `wb=manual`, `poll=1`). The existing keys cover the other experiments: `ae.hold_range`, `ae.speed`, `ae.stable_range` (stock values 5 / 16 / 4) and the `night.*` keys of section 11.

Not implemented, on purpose: a stock-like EV for the night switch (the formula is inferred from one number), per-gain updates (none in stock), any change of the `rc` words other than the stock struct.

Decisions to take after section 7: flip the `venc_rc` default if it wins; confirm the night thresholds of section 11 on a camera; keep or drop `ae.speed` 7. Taken already: section 11.

## 6. Dumping the live ISP state of an Akam camera

Goal: close the comparison on hardware, block by block, without vendor data leaving anyone's machine.

1. On the camera, with `ak_rtsp` running, one of:
   - 8091: `printf 'SET img.dump all\n' | nc <akam-ip> 8091` (the lines come back as `LOG ...`), file `/tmp/isp_live/isp_blocks.bin`;
   - `isp_dump --blocks --all --out /tmp/isp_ours` (the tool built with `ak_rtsp`; also works for `--conf FILE`). Whether a second process may open the ISP device while `ak_rtsp` runs is not known: use the 8091 route first.
   The default (`confirmed`) reads only the getters seen in a stock capture or reversed (SHARP, EFFECT, WB, exposure, frame rate); `all` also tries the others, whose command is the SET command plus one (a guess, an error is just logged).
2. Each line: `NAME size hash day=eq|diff(Nw) night=eq|diff(Nw)`; the last line says whether the live state is day, night, or neither, and how many blocks equal each subfile. SET_EXPOSURE ignores its 7 runtime words.
3. Copy the file to the PC and compare with the stock capture: `python3 -I tools/hw_tests/isp_block_diff.py <stock-run-dir> <dir-with-isp_blocks.bin> --stock-at 10 --show 4` (`--stock-at 60` for night). Same format as the capture's `isp_blocks.bin`, so the parser's export works on it too.

4. Also read the STOCK camera live, to close the open point of 3.2 (the sharpness after a switch) and to get a reference in the same format: copy the `isp_dump` binary to the stock camera (`/tmp`, RAM; the stock application keeps running) and run `/tmp/isp_dump --blocks --all --conf /tmp/sensor_isp.conf --out /tmp/isp_stock_day` (the stock firmware's copy of the file is `/tmp/sensor_isp.conf` too, or point `--conf` at the app's `isp_gc20c3.conf`), then cover the lens until it has switched to night and run it again into `/tmp/isp_stock_night`. If a second process cannot read the ISP device while the stock application runs, the tool reports the failed reads; then use `parse_stock_capture.py` numbers of 3.1 only. The stock dumps are then the `<stock-run-dir>` of step 3 (`--stock-at 1000000` takes the only record of each block).

Expected on a faithful Akam camera in day mode: every readable block equal to the day subfile and to the stock capture at T = 10 s, except SET_WB (zeroed tables, until `img.wb=conf`) and SET_EXPOSURE (the AE settings of 3.3).

## 7. Hardware test plan

Two cameras, same scene, no changes between runs except the one under test. Prefer the same physical camera running each firmware in turn (flash back and forth) over two units: unit-to-unit differences are unknown.

### 7.1 Setup

- Fixed support, side by side, same height, a static scene with a flat evenly lit area (wall) for the ROI, steady artificial light (no window), 5 minutes of warm-up. Stock streams `rtsp://<stock-ip>:554/main_ch` (1080p), Akam `rtsp://<akam-ip>:554/`. Do not compare the stock phone app (it rescales and may post-process) or screenshots.
- Capture without re-encoding: `ffmpeg -rtsp_transport tcp -i <url> -t 40 -c copy akam_a.mkv`.
- Measure: `python3 -I tools/hw_tests/image_noise.py --roi X,Y,W,H stock=stock.mkv akam=akam_a.mkv`. Columns: mean luma (must match within about 5, else the exposure differs and the numbers are not comparable), temporal noise `sigma_t`, spatial noise `sigma_s`, bit rate, I / P frame sizes.

### 7.2 Runs (in this order; settle 60 s after each change, then capture)

| Run | Change on the Akam camera | What it answers |
|---|---|---|
| A0 | none | baseline numbers; stock capture B0 |
| A1 | note the kernel log: `dmesg \| grep -E 'eRCMode\|MinQP\|MaxQP\|bEnableSRD\|SmartMode\|learning'` (after 80 s of streaming, the learning takes 72 s) | does Akam's encoder show AVBR, QP 28 to 43, SRD, and a learned QP near 42 on a static scene? (stock: AVBR, `MaxQP = 43(38)`, SRD FALSE, learning `210 kbps and QP is 42`) |
| A2 | `printf 'SET img.dump all\n' \| nc <akam-ip> 8091`, then diff (section 6) | is the live ISP state the file's? |
| A3 | `printf 'SET img.wb manual\n' \| nc <akam-ip> 8091` (conf is the default now), dump again | colour difference between the file's WB block and the hand-made one |
| A4 | `printf 'SET img.poll 1\n' \| nc <akam-ip> 8091` | does polling the statistics change the noise? (expected: no) |
| A5 | `echo venc_rc=stock > /tmp/akam_image` then `killall ak_rtsp` (the supervisor respawns it); watch the log for `venc rc sub-struct: stock` and for a kernel oops on the UART | does the stock `rc` struct run, and does it lower `sigma_t` / bit rate / raise `q_ratio`? Keep the UART attached |
| A6 | A5 + A3 + A4, plus `SET ae.hold_range 5`, `SET ae.speed 16` | all stock-like |
| A7 | day / night switch, see the procedure of section 11.4 (lit room stays day, lens covered switches to night after about 3 s, no oscillation, lamp at night returns to day) | the switch point against stock, no flapping |

To go back: `rm /tmp/akam_image`, `killall ak_rtsp`, `SET img.wb manual`, `SET img.poll 0`, or `SET img.reload 1` for the ISP part.

### 7.3 Acceptance

A change is worth keeping when, with equal luma, `sigma_t` and the bit rate drop towards the stock values and the picture is not worse by eye (no blocking, no pumping at the GOP). Target: `sigma_t` of the Akam stream within +15 % of stock, bit rate within +30 %. Record the numbers of every run in `docs/akam_v1_test_plan.md`. If no switch changes anything, the next suspect is the sensor / lens unit and the in-camera ISP state of 3.1 should be re-checked with `img.dump all` after a cold boot.

## 8. Risks

| Risk | Mitigation |
|---|---|
| `venc_rc=stock` raises a kernel oops (the encoder crashed twice with other `rc` words) | start-up option only, default off; put it in `/tmp/akam_image` (RAM: a crash self-heals at the reboot), never in the saved file before a test has passed (a warning is logged if it is); UART attached for the first try; the struct is the stock one of channel 0 and keeps the MMA flag the DMA pool was sized for. Still a hypothesis: minqp = maxqp = 0 (suspected once of a zero divisor) and the stock sub-stream sends `rc.flag = 0` without crashing |
| `img.wb=conf` (now the default) brings back the blue cast the manual block was meant to fix (the file's block carries the same gains) | the user compared both on a camera and prefers `conf`; live and reversible (`SET img.wb manual`, or `wb=manual` in `/etc/config/akam/image`) |
| `img.dump all` sends a guessed getter that is not one | only reads are tried; an unknown command returns an error; `confirmed` is the default |
| `img.reload` re-sends every block: a visible flicker, picture settings lost | documented; for tests only |
| The stock-like night trigger leaves the camera in day mode in very dim light (dark, but coloured picture); the IR probe of the return to day blinks the IR LEDs for 2 to 3 s every 5 minutes at the start | the thresholds are 8091 keys (section 11.3), `night.mode night` forces night; probes back off exponentially, `night.ir_led off` removes them |
| Measurements on two units mix unit and firmware effects | same camera for both firmwares where possible; otherwise repeat with the two cameras swapped |

## 9. Files

`src/ak_rtsp/imgq.c`, `imgq.h` (switches, 8091 keys), `isp_blocks.c`, `isp_blocks.h` (block table, conf loader, comparison), `isp.c` (`isp_reapply_wb_conf`), `ae.c` (WB option, statistics poll), `venc.c` (rc sub-struct), `isp_dump.c` (`--blocks`), `control.c` (3 lines), `night.c`, `night_logic.[ch]` (section 11), `ircut.[ch]`, `ae.[ch]` (gain limits), `tools/hw_tests/image_noise.py`, `tools/hw_tests/isp_block_diff.py`, tests `isp_blocks_test.sh`, `imgq_test.sh`, `night_logic_test.sh`, `image_noise_test.sh`, `isp_block_diff_test.sh`.

## 10. Concurrency and 8091 model

- The `img.*` SETs are debug keys. Spec 07 makes 8091 read-only remotely (loopback writes only): `img.wb`, `img.poll`, `img.reload`, `img.dump` follow whatever rule that step applies to SET (they stay usable from the loopback, i.e. from `./akam` over SSH).
- `img.wb`, `img.reload` and `img.dump` send several ioctls: they run under `isp_cfg_lock()`, the lock the night thread takes for a whole day / night switch (`g_is_night` changes before it is released), so they never interleave with one. They refuse (`ERR ISP not initialised yet`) until the first `isp_pipeline_init()` has completed, because the 8091 server starts before the ISP is set up. Single-ioctl reads (AE thread, `img.poll`) stay lock-free like the existing ones.
- Nothing here writes to flash: `/etc/config/akam/image` is only read (it is edited by hand); `img.dump` writes at most about 110 KiB to `/tmp/isp_live/isp_blocks.bin` (RAM), the dump buffer is on the heap (64 KiB, the largest block is 46584 bytes), the conf file (282 KB) is read into the heap, twice during `img.dump` (day and night), under 1 MiB at the peak, freed before the key returns.
- The `night.*` keys of section 11 follow the same rule: reads stay open, writes are loopback only once spec 07 applies it to SET. They are live only (lost at the next boot); the decision state is touched only by the night thread, the keys change a snapshot under `g_night_lock`, a change of `night.mode` resets the state.

## 11. Decisions of 2026-11-10 and the night switch rework

### 11.1 Decisions

- `img.wb=conf` is the default (user decision after comparing both on a camera: better colours). `manual` stays selectable, and is now re-applied after each night to day reload.
- `venc_rc` stays `legacy` (the stock `rc` struct is untested and risks a kernel oops).
- The night switch is reworked as below. Defaults aim at: colour (day) in a normally lit room, night only in real darkness, no oscillation.

### 11.2 Rule (`night_logic.[ch]`, pure state machine, host-tested; `night.c` is the glue)

Day to night, like stock: the AE must be saturated for `night.confirm_samples` consecutive polls (30 x 100 ms = 3 s): `hw_exp >= night.trigger_hw_exp` (15500, 95 % of the 16384 ceiling) AND exposure x ISP gain >= `night.gain_pct` % (95) of the maximum the AE itself is allowed, read at run time from the exposure block (`ae_get_limits()`: sensor gain max x ISP gain max; 1.0x x 1.5x = 6.29 M in Q8 in day, the stock threshold is 6.3 M). A dim but usable room never reaches it: the AE finds its target before the ceiling.

Night to day is the hard part. With the IR LEDs on, the camera lights its own scene, so the exposure says nothing about the room (stock capture: exposure at its floor with the lens covered), and "exposure is low, go to day" would flip every few minutes. The stock avoids it with AWB statistics (a "non infrared ratio") which are not decoded here. Instead, after the minimum night dwell (`night.min_night_ms`, 120 s, doubled up to x8 when the camera returns to night within 10 minutes of a day switch) the IR LEDs are switched off for a few seconds (probe) while the filter stays in night position, and the AE is read in the real ambient light:

- dark room: the exposure climbs back above `night.day_hw_exp` (2048, 12 % of the ceiling) within about 2 s: the probe ends, IR back on, the next periodic probe waits `night.probe_ms` (300 s) and doubles after each failure (up to x8);
- lit room: the exposure stays below it for 10 samples after `night.probe_settle_ms` (4 s): back to day, the LEDs are already off.

A probe starts every `night.probe_ms`, or at once when a lamp is suspected (exposure below half of its slow average for 3 s, latched until the dwell is over). With `night.ir_led=off` there is no feedback loop: the exposure is read directly (`night.day_confirm_samples`, 50 samples) and there is no probe. An anti-flap lock of `night.lock_ms` (60 s) follows every switch. `night.state` stays `night` during a probe. Every switch and probe is logged with the values that triggered it (`[night] switching to NIGHT mode: AE saturated: hw_exp=16384 ... gain=384/256`).

Not changed: `night.mode auto|day|night`, `night.ir_led`, the LED code, the ircut mutex (the probe goes through it: `ircut_ir_led_probe()`; a switch ends a probe), the night / day ISP reload.

### 11.3 8091 keys

Existing (control.c): `night.mode`, `night.state`, `night.trigger_hw_exp` (default 15500, was 3000), `night.day_hw_exp` (default 2048, was 1200, now measured with the IR off), `night.confirm_samples` (30, was 10: 3 s instead of 1 s, the poll is 100 ms), `night.lock_ms` (60000). New (night.c): `night.gain_pct` (1..100, 95), `night.day_confirm_samples` (50), `night.min_night_ms` (120000), `night.probe_ms` (300000, min 10000), `night.probe_settle_ms` (4000), and read-only `night.ae` (live `lum`, `hw_exp`, sensor and ISP gain, the AE limits, `saturated=0|1`) and `night.logic` (mode `day` / `night` / `night-probe`, confirmations, lock, probes, failures, next probe, last decision with its values).

### 11.4 Hardware test procedure

`nc` here is `printf '<cmd>\n' | nc -w 2 <akam-ip> 8091`.

1. Lit room, 10 minutes: `GET night.ae` repeatedly (`hw_exp` well below 15500, `saturated=0`); `GET night.state` stays `day`; the picture is in colour.
2. Dusk: dim the room; `night.ae` shows `hw_exp` rising to 16384 while the gain stays below `max_isp`; the camera stays in day until both are at their ceiling.
3. Lens covered (or a dark room): `saturated=1` within 2 s, `night.state=night` about 3 s later, the log shows `switching to NIGHT mode: AE saturated: ...`.
4. Stay dark 10 minutes: `GET night.logic` (probes counted, `next_probe_s`); every 5 minutes the IR LEDs go dark for 2 to 3 s (the log: `ambient light probe`, then `probe failed ... IR back on`); `night.state` stays `night`, no oscillation.
5. Lamp at night (after 2 minutes of night): uncover the lens or switch the room light on: within about 15 s the log shows a probe and `lit room, back to day`.
6. `night.ir_led off`: in the dark the camera is in night without LEDs and no probe; light returns it to day after 5 s.
7. Tuning: `SET night.trigger_hw_exp 3000` / `SET night.gain_pct 1` restore the old behaviour for comparison; `SET night.mode day|night|auto` as before.
