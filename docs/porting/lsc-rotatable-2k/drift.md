# Drift reference (Rotatable 2K): finding and correcting lost motor steps by the picture

Status 2026-11-19: host-tested (synthetic scenes, 10 real 2K frames), NOT yet run on the camera. Design review: `.local/reviews/fable-drift-reference-20261118.md`.

The stepper is open loop. After minutes of tracking the counters can say home (1275/1300) while the head is a few pixels or steps away. The drift check
looks at the picture when the head is back home and idle, measures by how many pixels the view moved against the view taken at home, and corrects the
counters by that. This page covers the matcher, the reference, the correction policy, the settings and the measurements.

## What the corpus told us (test-images/lsc-rotatable-2k/drift/, 320x180 luma)

* The day case "6 px" of the earlier log was 6 px of the 80x48 THUMBNAIL. At 320x180 the matcher and a brute-force ZNCC of the whole frame both find
  **dx +1, dy -28 px** (content moved up) between `day_before` and `day_recal`: the ceiling left the frame.
* Steps per pixel (night frames, tilt moved by a known number of steps, `night_results.txt`): 50 steps = 21 px up, 53 steps = 33 px down, 100 steps = 44 to
  52 px: **about 2.4 steps per px of the 320 px frame (9.6 per thumbnail px)**, with the usual backlash asymmetry. The lens model said 14.6 per
  thumbnail px (3.65 per frame px): wrong. The defaults `px_pan_x10` / `px_tilt_x10` are now 96 / 96 (they were 125 / 146). Pan was not measured separately
  (`dx` moves in the log were mostly ignored by the driver): tune `px_pan_x10` in `ptz.conf` if the first field logs say so. The controller closes
  the loop: after a correction it re-checks, and one refinement by the residual is made when the residual is at most half of the first error.
* Range: the head moving +-100 tilt steps shifts the content 40 to 64 px; a patch needs overlap, so the matcher range is +-48 x +-32 px (+-115 x +-77
  steps) and a correction beyond 30 px (about 72 steps) is a recalibration, not a declare.

## The matcher (`src/features/drift_match.{h,c}`, pure, no libm, ~600 lines)

* Feature: per pixel the Sobel gradient as a **doubled-angle vector** `(gx^2-gy^2, 2gxgy)/(|g|+k)`, length saturated. Doubling the angle makes it
  polarity invariant (a window frame dark by day and light under IR points the same way); saturation and the noise floor `k` (twice the median gradient
  magnitude of the image) take out contrast, gain and most sensor noise. The vectors are summed over 2x2 blocks (half resolution, "L1") so the fine plaster
  relief survives; a full resolution refinement uses them unsummed.
* Reference: up to 16 patches of 16x16 (L1) ranked by structure energy, at most 3 per cell of a 4x3 grid, no overlap; a patch is refused when, inside its own
  search window of the reference frame, a second peak at distance > 3 px scores more than 85 % of its best (bare straight edge, repeated pattern).
  Cost: ~4 ms on the PC.
* Match: each patch is searched over +-24 x +-16 L1 px (cosine similarity of the vector fields, window energy from an integral image); a patch is valid
  with a score of at least 0.25 and a second peak below 92 % of the best; a **consensus vote** (the seed with most patches agreeing within 1 + |d|/8 px)
  needs at least 4 patches and 60 % of the valid ones; the inliers are refined at full resolution (+-3 px, parabolic sub-pixel, 1/4 px units) and must
  agree within 2 px of their median and span a fifth of the frame. If the full resolution does not agree (day against night: the fine relief shading
  differs, the edges do not) the half resolution estimate is returned with `fine = 0` (good to about 3 px).
* Refuses (never guesses): too few valid patches (flat view, a person over the patches, another scene), no consensus, flat live view, wrong frame size.
  Cost: ~3 ms on the PC per check.

Results on the corpus (`tests/rot2k_drift_unit.c`, run with the frames in `tests/fixtures/drift/`, git-ignored: no camera image is committed; the test
prints SKIPPED when they are absent, the synthetic part always runs):

| Pair | Result | Expected |
|---|---|---|
| day_before -> day_recal | 1.5, -28.0 px | brute force ZNCC 1, -28 |
| any frame against itself (day, night) | 0, 0 exactly | 0 |
| night_home0 -> the 4 "back home" night frames | within 1 px (e.g. -13.0, 2.0 against -13, 2) | ZNCC of the frames |
| night_home0 -> night_dy_50 | 0, 33.0 | 33 (thumbnail correlation of the log) |
| day_before against another scene (day or night) | refused (inconclusive) | refuse |
| **day_home reference -> night frames** (day and night, same view) | consistent with the night-only numbers within 2 px (fine) or 3 px (half), no wrong correction | 1 to 2 px |
| night_home0 reference -> day_home | -24, -1.5 (the mirror of day -> night +24..26, +2) | consistent |

Cross-mode honesty: with the day reference the night views were matched in 6 of 6 cases, 3 at full resolution (error under 1 px) and 3 at half resolution
(error up to 2.25 px, which is why a half resolution cross-mode result is only ever used for "no displacement", never for a correction).
Synthetic day against night (inverted polarity, other relief, gamma, noise): within 2 px for shifts up to 40 px.

CPU: ~3 ms per check and ~4 ms per reference build on the PC (-O2). The single-core ARM9 has no vector unit; an estimate of 50 to 100 times slower gives
about 0.15 to 0.35 s per check and 0.2 to 0.4 s per reference build; the check runs only at home after a return, idle, rarely (once per 1500 weighted
steps of travel at most). Measure with the `ptz: drift check` log lines on the camera. RAM (static BSS): work buffers ~55 KB, two references ~85 KB, two
frame copies 120 KB, about 260 KB in total; the frame copy is only made while `ptz_drift_ref` is on.

## Reference capture (RAM only)

* The reference has its OWN capture rule, separate from the flash reference of the boot proof (`dref_capture` hook, `dref_*` in `ptz.c`). It is wanted at a
  PROVEN position: a proven cold boot (the scene check said "home", no calibration), a restart that adopted the driver position, after a calibration (which
  first DROPS both modes: the old picture belongs to the old counters), after a tilt re-home followed by a matching picture, and as a refresh after a proven
  zero when the reference is older than 1 h. It is taken once the head has been quiet at home for 6 s and nobody was seen (no 10 min tracker-free spell: that
  rule belongs to the multi-view excursion; no travel budget either: the position is proven by the picture or by the calibration, not by the odometer). Up
  to 3 tries, 30 s apart (no steady frames, too little structure); a person arriving during the capture keeps it wanted. State: `ptz.drift_ref` (`none` |
  `day` | `night` | `both`) and `ptz.drift_ref_age_s` (-1 when none). Before this rule (found 2026-11-21) a normal boot never captured it and every check
  said "skipped: no home reference yet" for the whole run. `dm_capture` takes two frames 2 s apart (AE settled, nobody walking) and keeps the reference only if the second frame matches it at zero within
  half a pixel on at least 90 % of the valid patches, with at least 6 patches on 2 quadrants or more. No IR-off probe may be running.
* One reference per day / night mode, **RAM only: nothing is written to the flash, ever** (the old 80x48 thumbnail reference of the boot proof is
  unchanged and keeps its own 24 h write rule). The reference is rebuilt after each boot.
* **Mode bridge**: when the live mode has no reference but the other mode's reference matched at zero with a full resolution agreement, the proven view is
  built into a reference for the live mode (no night calibration needed). The matcher itself already works across modes; the bridge is the backup that
  makes later checks same-mode.
* The IR-off probe gate (review finding 4) is `a2k_night_probe_busy()`: the capture and the check refuse while it is true and again after the frames were read.

## Correction policy (ptz.c, `drift_run_locked` / `drift_correct_locked`)

The scene hook `drift4` returns A2K_DRIFT_* and the displacement in quarter px:

* OK: both axes within 1 px (full resolution) or 3 px (half): counters right.
* SHIFT: confirmed by a second frame at least 0.5 s later within 1 px; the controller declares the counters at home plus the measured steps (tilt and pan),
  moves home (a few steps), checks again; a residual converging (at most half) gets one refinement; anything else: `ptz_drift_correct=full` schedules a
  recalibration (rules: urgent tilt re-home and a second picture first, idle time, two per sliding hour, cumulative cap 120 steps per axis), `adjust` only logs.
* RECAL: a displacement above 30 px, or a shift of the other mode's reference at half resolution is INCONC (never a correction).
* INCONC / SKIP: retry 30 s up to 4 times per return; 3 returns in a row that all give up inconclusive enter the ladder below (urgent re-home, picture, recalibration); a skip changes nothing.
* Backlash: `hal_ptz_home` (tracker return, correction, home button) always makes the last leg from BELOW (increasing counters, like the last leg of a
  calibration): an axis above home first goes 10 steps under it, then up. The landing is within the goto tolerance (3 steps), not exactly on the counter.
* The tilt-only re-home (`ptz_tilt_rehome`, below) is the non-visual fallback against tilt slip of any cause: it does not need the scene to match.

## Escalation ladder, interruptions and verification (2026-11-21 review: fable-ptz-drift-20261121, F1 to F16)

Every doubt of the position follows ONE ladder with no terminal give-up state: picture check (free) -> urgent tilt re-home (cheap, exact, needs no scene) ->
the picture again -> recalibration. Tests: the `f1_cases` ... `f13_cases` of `tests/rot2k_ptz_cont_unit.c` (each asserts the final pose and the state, not only counters).

* **Never parked by accident.** "parked" is the state of an explicit park command (goto / park / step / session), not of a pose: a re-home cancelled below the
  soft range settles at the park pose (tilt 300) and used to read PARKED (tracker off, privacy on). It reads moved now (HAL `explicit_pose`).
* **A cancelled re-home returns home by itself.** After an interrupted (or failed with the position still valid) re-home the controller returns the head home
  once quiet (`go_home`, an ordinary K_HOME flagged automatic: a tracker command preempts it, a manual command cancels the intent), keeps the re-home owed
  (`rh_owed`, `rh_urgent` survive the cancel), restores the gap and retries after 2 min (`A2K_PTZ_REHOME_RETRY_MS`; after 5 cancelled attempts in a row the normal gap).
* **A flat view is no loop.** FLAT needs a second frame 0.5 s later (else inconclusive) and a dark frame (mean luma < 8) is a skip. FLAT right after a completed
  re-home (no structured verdict since) does nothing more: `ptz.drift_last = flat-persist` (a covered lens, a dark room, a wall: one re-home, then a visible state).
* **One ladder.** A RECAL verdict (also a tilt-only one), an unconfirmed or impossible correction, a failed re-home with the position still valid, or inconclusive
  checks first request the URGENT tilt re-home (rung 1, the checks wait for it), then the picture is checked again whatever the travel (rung 2): ok clears
  `untrusted` (and takes a fresh drift reference), still wrong -> the recalibration. The re-home stays allowed while untrusted. The recalibration limit is a
  sliding window (`A2K_PTZ_RECAL_MAX` = 2 per `A2K_PTZ_RECAL_WINDOW_MS` = 1 h); a held-back one runs by itself when the window has passed; with the HAL position
  unknown there is no limit, and a failed recalibration of a doubted or unknown position is retried after 5 min. `ptz_drift_correct` below full keeps its meaning
  (no recalibration by a verdict) except when the position is unknown.
* **Inconclusive is not "done".** The 4 tries of a return home that all stay inconclusive count; 3 returns in a row enter the ladder (urgent re-home, picture, recalibration).
* **Looked at on a timer and by travel.** The picture is also checked while the head idles at home (`A2K_PTZ_DRIFT_PERIODIC_MS`, 30 min), and when more than
  `A2K_PTZ_VERIFY_TRAVEL` (30000 weighted steps) were travelled since it was last looked at the controller goes home by itself and verifies, only when nobody is in
  view (a person who keeps the camera away is never interrupted while tracked).
* **Abort on arrival.** A person (a detection box) or a tracker command during a check, a re-verify or a reference capture sets an abort flag the scene hooks poll
  while they wait for frames (`set_abort`): they return at once (skip), the verdict is discarded, the check is repeated once quiet. The tracker gets the head within
  about 0.1 s instead of up to 7 s. Not changed: the driver inits of the re-home (2 s each) are not cancellable; shortening them needs the hardware (F9).
* **Less grinding.** A routine re-home is skipped when the picture said ok within the gap and less than the minimum travel followed. Every re-home that runs walks the FULL stroke: a shorter walk that stopped before the real stop would declare a wrong position as proven.
* **Lows.** The counter after a cancelled re-home is read after the stop settled (two equal reads); the HAL log line buffer is 256 bytes (the re-home start line was cut);
  the RAM log segments carry a sequence header and a restarted app continues on the newer one (`ramlog.h`, `tests/rot2k_ramlog_unit.c`); `ptz.drift_last_px` is in
  quarter pixels of the 320 px frame (the key name stays).

## Settings (akam2k.conf, passed as flags by `own_app_run.sh`, validated there and in the app; nothing is persisted by the app)

| Key | Values | Default | Effect |
|---|---|---|---|
| `ptz_drift_ref` | 0, 1 | 1 | 0: no frame copy, no reference capture, no drift check, no correction, no drift recalibration |
| `ptz_drift_correct` | off, adjust, full | full | off: the displacement is measured and logged (and shown by `ptz.drift_last_px`), never corrected; adjust: a confident shift re-declares the counters and goes home, a big error is only logged; full: adjust plus recalibration on big errors, unconfirmed corrections or cumulative drift (the behaviour before the setting existed) |
| `ptz_boot_calibrate` | auto, always | auto | auto: at a cold boot the saved-view proof (`ptz_scene_check`) may skip the long calibration when it proves the head did not move; always: the proof is never used, every boot calibrates |
| `ptz_tilt_rehome` | off, idle | idle | idle: once the head is back HOME, quiet (no person seen, no tracker or user command, no motion) for 10 s and at least 3000 weighted steps were travelled since the last re-home or calibration, TILT walks DOWN into the bottom stop (the full stroke, like the boot calibration, configured calibration speed), the tilt counter is re-declared as the stop (real 1) and the head goes back UP to the tilt home (last leg from below). The pan is never commanded. RAM only, no flash write. A person detected, a tracker command or a user PTZ command cancels it: the believed position of before is kept (the slip is what it was, nothing worse). Skipped while the position is untrusted, parked, `ptz_cal=off`, and while a drift check is pending. off: never |
| `ptz_tilt_rehome_gap_min` | 1..1440 | 30 | at most one re-home per this many minutes (counted from the start of the last attempt, an interrupted one included) |

These are akam2k.conf keys like the other `ptz_*` ones (this is where the ptz settings live; the settings API and MQTT do not carry them). `never` is
not offered for `ptz_boot_calibrate`: after a power cycle the driver's counters are unknown and a head that was moved by hand cannot be told from one that
was not without the proof or a calibration, so "never" would be unsafe. The default keeps the current behaviour.

## Tests

`tests/rot2k_drift_unit.c` (matcher: synthetic sub-pixel shifts to 40 px, 20 noise draws at zero, day against night, a person over the patches, a covered
view, another room, flat, repeated stripes, bad sizes, then the real frames), `tests/rot2k_scene_glue_unit.c` (`drift_ref_tests`: capture in RAM, shift in
quarter px, other-mode reference, mode bridge, probe gate), `tests/rot2k_ptz_cont_unit.c` (`drift_ref_cases`: quarter px correction landing on the home
counters, off / adjust / full, `ptz_drift_ref=0`, boot_calibrate). One intended change to existing tests: the fake camera of `rot2k_ptz_cont_unit.c`
converts steps to thumbnail px with 9.6 instead of 12.5 / 14.6, because the default scale changed to the measured one.

Blind check (2026-11-20): a check at home on a featureless view (the head slipped to the ceiling) is no longer a silent skip. When a reference with detail exists and the live view has none (`A2K_DRIFT_FLAT`, `ptz.drift_last = skip-flat`), the controller schedules the tilt re-home at the next quiet moment (the 30 min gap does not apply, at most once per 5 min for this reason; the ordinary minimum travel neither), or a full recalibration when `ptz_tilt_rehome=off`. Since 2026-11-21 a flat view right after a completed re-home is `flat-persist` and schedules nothing (see the ladder above). Any skip (no reference, no frame, probe) is logged and keeps the travel mark: the check stays owed for the next return home. Tests: `flat_cases` in `tests/rot2k_ptz_cont_unit.c`.

Visibility of the tilt re-home (2026-11-20): read-only state keys `ptz.rehome_count`, `ptz.rehome_cancel`, `ptz.rehome_last_s` (uptime seconds of the last completed one, or `none`) and `ptz.rehome_last_reason` (`gap` | `flat` | `none`), see control-api.md. Every drift check and every re-home start / end / cancel is an `a2k_log` line ("ptz: drift check ...", "ptz: tilt re-home starting / done / cancelled"). The app writes those lines to stdout, which the supervisor appends to `akam/akam2k.log` on the card (absent without a card: the lines were lost on the card-less flash boot). `a2k_log` now also keeps every line in the RAM file `/tmp/akam2k_main.log` (64 KiB, two 64 KiB segments `akam2k_main.log` and `akam2k_main.log.1` written in turn, the older one truncated when the other is full; tmpfs: never flash), readable over SSH with `cat /tmp/akam2k_main.log /tmp/akam2k_main.log.1` (every line starts with its ms clock). No Home Assistant discovery entity was added (the discovery table is built around the sys / detection values, an extra ptz sensor is not trivial there).
