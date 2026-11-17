# Rotatable 2K person tracking: policy (track_logic)

`firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c/.h` is a pure policy (no I/O, no clock, no vendor call, integer
math): per detection tick (~5 Hz) it takes the person boxes, the capture time of the frame and the PTZ state and returns at
most one pan/tilt nudge (with a speed), a home request, or nothing. The wiring to the detector and the motor is
`app/akam2k_track.c` (below).

## Why 0.1.0 reacted every 2-10 s: the latency chain

Hardware test of 2026-11-16 (person walking at 1-2 m, pan only, speed 3): the NPU ran at 5 Hz (300 inferences per minute, 63-110 ms
each, "300 stale frames skipped" per minute) and the pan target changed only every 2-10 s. From the 0.1.0 code, per stage:

| stage | 0.1.0 | cost |
|---|---|---|
| detection cadence (`detect_hz` 5) | one inference per 200 ms; the freshest frame is taken, the stale ones skipped | 0-200 ms wait (mean 100) + 63-110 ms inference = 160-310 ms from capture to the tracker |
| detector ON debounce (`on_count` 2) | only the ON/OFF events (snapshot, zones, LED), NOT the tracker, which gets the boxes of every tick | 0 for tracking (400 ms for the event) |
| tracker acquire (`confirm_ticks` 2) | two consecutive ticks with a target before the first move; ONE missed tick (blur, partial box) resets it | 200-400 ms, repeated after every dropout |
| dead band (`deadband_pct` 12) | no move while the box centre is within 12 % of the width = 38 px = 120 steps (10 deg) of the centre | the person walks 10 deg before anything happens |
| rate limit (`min_interval_ms` 1500) | at most one nudge per 1.5 s | **the dominant term**: 1.5 s between two corrections |
| step (`gain_pan` 1000 steps per frame width, `max_step` 300) | the nudge is the offset x gain, clamped to 300 steps (26 deg) | a person at the edge needs 490 steps: only 300 are sent |
| motor (`speed` 3 = 333 steps/s) | 300 steps = 0.9 s, + 30 ms start + 70 ms stop and position read | 0.1-1.0 s of motion; no tick can nudge meanwhile (`ptz.moving`) |
| after the motion | the first frames are blurred / the AE moves: the person is often not detected, the streak and the sticky target are lost | +0.2-0.6 s (acquire again) |
| PTZ queue | one pending command; a nudge is accepted only while idle | not a limit (the tracker never nudges while moving) |

So one correction costs `max(1.5 s rate limit, acquire + motion)` = 1.5-2.4 s and moves at most 300 steps: the sustained following speed is
at best 300 steps / 1.5 s = **200 steps/s = 17 deg/s** (a person crossing at 1.5 m walks 600 steps/s = 53 deg/s), and every detector dropout adds
0.4 s or more. The measured 2-10 s intervals are this chain plus dropouts (the person ON/OFF flapping seen in the log). The simulation below
reproduces the 0.1.0 limit of 200 steps/s; it does not model the real detector's missed detections, so the field numbers are worse than the simulated ones.

## Responsive policy (defaults)

> Since the targeting work (next section) the defaults are `track_zone_pct` 40 / `track_motion` continuous: the band, upper-third and nudge rules below are the policy of `track_zone_pct=0` + `track_motion=steps` (= `tl_cfg_dev()`, pinned by `rot2k_track_unit.c` and `rot2k_track_sim.c`). The nudge machinery (stale frames, rate limit, reversal caps, lead) is also the `track_motion=steps` fallback of the new targeting.

| field | default | meaning |
|---|---|---|
| deadband_pct | 6 | central band, pan: no move while the box centre is within +-6 % of the width (19 px = 60 steps = 5 deg) of the centre (`track_band_pct`; was 8, tightened 2026-11-16: the sims keep 100 % in frame, end offsets 2..18 px, no hunting) |
| tilt_return_s | 0 (off) | stay mode: seconds after the lost-person grace before the TILT goes back to the home tilt (pan stays; `track_tilt_return_s`, API `track.tilt_return_s`, 0 = never) |
| tband_pct | 10 | tilt band (% of the height; `track_tilt_band_pct` 2..40, API `track.tilt_band_pct`): the dead band of the centre rule (`head_pct` 0) and, HALF of it, the head band: **+-5 %** (about +-10 px of 192) around the head row. Was 15 (head band +-7.5 %): a top 16 px too low was never corrected (hardware 2026-11-17) |
| gain_pan / gain_tilt | 1000 / 700 | steps per frame width / height (`track_gain_pan` / `track_gain_tilt` 200..4000, API `track.gain_pan` / `track.gain_tilt`, live; derivation below). gain_tilt was 600 |
| gain_pct | 85 | damping: one nudge corrects 85 % of the measured offset |
| max_step | 700 | clamp of one nudge, per axis (was 300) |
| min_interval_ms | 300 | rate limit between two nudges (was 1500) |
| confirm_ticks | 2 | ticks before the FIRST move on a new target; a target that was followed within the grace needs 1 |
| settle_ms | 220 | a frame captured less than this after the end of the motion, or before the last nudge started, is dropped (see below) |
| fast_pct / speed_fast | 25 / 2 | an offset beyond 25 % of the frame uses speed divisor 2 (~500 steps/s) instead of the default 3 (~333) |
| lead_pct / lead_max | 70 / 500 | velocity lead: % of the predicted target travel added to the nudge, cap in steps (`track_lead_pct`, 0 = off) |
| head_pct | 15 | tilt: the vertical reference is the TOP of the box (the head), kept 15 % of the frame height below the frame top (`track_head_pct` 0..50, 0 = centre of the box); see Tilt |
| tilt_enable | **0** | tilt tracking, opt-in (`track_tilt=1`) until the tilt direction (`track_invert_tilt`) is verified on the camera |
| lost_grace_s | 3 | no person for this long = lost; nothing is decided before (`track_grace_s`) |
| return_home / return_delay_s | 1 (home) / 30 | return policy, below |
| sticky_gate_pct | 35 | the previous target is kept if a box lies within 35 % of the frame width of it in WORLD position (the camera may have turned) |

`tl_cfg_legacy()` is the 0.1.0 policy (12 % band, 1.5 s, 300 steps, no lead, home after 10 s): the "before" of the simulation and of the old unit cases.

### Image offset -> motor steps (px per step, FOV)

The motor counts 0..~4000 steps over about 350 deg of pan (`ptz-findings.md`: stops at 0 and 3932..4151), i.e. 11.4 steps/deg. The lens is about 90 deg
wide (assumption from the lens class, not measured), so one frame width (320 px of the detection frame) is about 1000 steps: **3.1 steps per px**
(0.32 px per step). Tilt: 192 px at the same pixel pitch = 600 steps (about 54 deg) nominal, **700 by default since 2026-11-17**: the hardware log (close person, top at 59 px of 192) showed the nominal tilt gain under-correcting (a 16 px excess gave `want` 0 / 40 steps once, then nothing). At 700 the correction of a measured offset of `c` px is `c x 700 / 192 x 0.85` = 3.1 steps per px, i.e. exactly the nominal geometry (3.1 steps/px) after the 85 % damping, so a head 30 px off comes back in ONE nudge of about 93 steps when the nominal geometry is right and in two when the real image shift per step is lower (the second one measures the remainder on a settled frame); a higher gain (800, 900) overshoots in the closed-loop sim (reversals, `close_range`). Retune live with `track.gain_tilt`. The step needed to centre a box at `c` px from the centre is
`dx = (c / 320) x 1000`; the code works on `d2 = 2 x centre - width` in half pixels, integers only. To measure the real value on the camera: park the
head, note a fixed landmark at the edge of the frame, nudge by N steps with the HA buttons and read the landmark shift in the snapshot (px per step = shift / N);
`gain_pan` / `gain_tilt` are the only numbers to change.
The simulation shows the loop does not need it to be exact: with the true gain 30 % off in either direction (700..1500 steps per frame) the person stays
in the frame at 200 steps/s without hunting, because every correction is re-measured on a settled frame and damped to 85 %.

### Acting early, acting once (frames vs motion)

- The policy acts as soon as the box centre leaves the band (two detections for a new target), proportionally to the offset: a person at 100 px gets
  265 steps now, not 300 steps 1.5 s later.
- **Frame time against the motion.** The detector passes the capture time of the frame (`now - queueing age`, the freshest frame of the queue has age 0) and
  the PTZ controller reports when the last motion ended (`a2k_ptz_info_t.motion_end_ms`, CLOCK_MONOTONIC, the clock of the detector). A frame is dropped for
  the correction (but still counts as "the person is there") when it was captured while the motor ran, less than `settle_ms` (220 ms) after the motion end
  (the unknown video latency of about 40-60 ms, the AE and the rolling shutter), or less than 80 ms after a nudge was sent (the motor takes a moment to start).
  Blurred frames without a person do not reset the followed target. This replaces the fixed waits of 0.1.0: the first settled frame corrects at once.
- **Velocity lead.** The world position of the person along pan, `W = pan + offset steps`, is recorded for the last 3 settled frames (pose independent). With 2-3
  samples spanning 150 ms to 3 s the speed is `(W_new - W_old) / dt`; below 40 steps/s it is noise and ignored. The nudge aims at
  `W + v x (frame age + motion time + 150 ms) x lead_pct`, capped at `lead_max`. If the aim would reverse the direction (the person is coming back to the
  centre by itself) nothing is sent.
- **No hunting.** Damping (85 %); a correction below 20 steps is not sent; a nudge against the previous one within 1.5 s is halved; the correction is always
  measured on a settled frame, never integrated over a motion; the band (6 %) is larger than the detector jitter (a few px).
- Review fixes: a correction smaller than 20 steps is dropped AFTER the soft-range clip (a person 10 steps beyond a limit no longer loops tiny motions, scene timers and MQTT updates); the velocity samples are reset when the person is lost or the picked box is not the sticky match; a new box outside the sticky gate needs 2 ticks even inside the grace; a frame is also stale while the last nudge has no reported motion end (at most 2 s: queued, refused or not started); at most 2 direction reversals in 6 s, the third is held back and the tracker stays still for 3 s.
- Tilt (off by default, `track_tilt=1`): same mechanism with its own band (`tband_pct`, 10 %), no lead. Hardware test 2026-11-16 (user at ~1 m): the box spans the full height (`0,0,102,191`, `16,13,264,191`), so its centre never moves and a centred policy never tilts. Tilt is therefore **head-anchored** (`head_pct`, default 15): the reference is the TOP of the box, aimed at 15 % of the frame height from the top, with a +-5 % head band around it (`tband_pct` / 2: top between 10 and 20 % of the height, rows 19..39 of 192: nothing).
  Box top at the frame edge (<= 2 px) = head out of the frame = tilt UP by `gain_tilt * box height / frame height / 3` steps (about 200 for a full-height box), at least 20, at most `max_step`; a box top below the band = tilt DOWN in proportion to the distance (crouching). A box 55 % of the frame height or taller, or cut by the bottom edge, is a near person and uses this rule; a shorter box (far person, fully inside) is centred on its UPPER THIRD, and a top already inside the head band counts as in band for the DOWN direction only (else the camera tilting down, which shrinks a cut box into the far rule, would be pulled back up: hunting); the UP direction is symmetric: an upper third above the band tilts up in proportion even when the head is inside the head band, and a far box clipped at the top edge gets at least the up step of a 40 % tall box (a clipped short box says little about how far the head is out). Hardware report 2026-11-17: far person crouches, the camera follows down, stands up, the camera stays low. Sims (`rot2k_track_unit.c` `crouch_stand_cases`: far 4 m / 5 m / 2.6 m and near 0.9 m, crouch then stand, with and without a 4 s detector loss, motor image shift 0.32 / 0.2 / 0.45 px per step): the camera follows down and ends back within 25 steps of its standing tilt (the 0.2 case at 2.6 m ends 43 steps lower with the head at row 20, inside the +-5 % band: by design), at most 3 reversals, no pan nudge. Anti-hunting: a tilt nudge against the previous one inside 1.5 s is halved, a 3rd tilt reversal inside 6 s holds only the tilt for 3 s (pan keeps its own counter). The soft range clips the nudge (no push at a limit). `head_pct=0` and `tl_cfg_legacy` keep the centre-of-box behaviour. Tests: `rot2k_track_unit.c` (`head_tilt`: closed loop, close person crouching and standing, head clipped, far person, range).

  **Never up to the sky (hardware report 2026-11-17: a far person, the camera tilted up to the ceiling).** (1) The head-clip rule (up, no matter how far the head is) and the UP exit search need a box at least 40 % of the frame height (`CLIP_MIN_PCT`): a small box touching the top edge is a far person on a ledge / stairs or a false positive, it takes the far rule (its upper third goes to the centre, in proportion). (2) The cumulative tilt UP of an episode is capped at +400 steps (`TILT_UP_CAP`) relative to the tilt at the first fresh frame of the episode, unless the head clip is confirmed on 3 consecutive fresh frames (`CLIP_CONFIRM`: a tall box really cut by the top edge). An episode ends after a loss of more than 60 s (`EPISODE_MS`) or a return home, so alternating detections (present 1 s, absent 4 s) do not get +400 each time. Sims (`rot2k_track_unit.c` `sky_cases`, motor 0.32 px/step): far 50 px box 4 px from the top, world-fixed: tilt 1300 -> 1700 at most (it stops at the cap); static false positive at the top of the image: 1700; alternating 1 on / 6 off and 3 on / 23 off for 60 s: 1535; a real clip (tall box, head 250 px above the view): followed to 1995 (confirmed clip, beyond the cap).
  **Hardware test of abafb75 (2026-11-16): the camera went up (1300 -> 1682) and never came down.** Causes found by replaying the logged boxes: the head band was too wide (top rows 0..30 % counted as fine, so tops of 56 px were "in band"), the up step (1/3 box height, ~198 steps) overshot and made the opposite correction look like a reversal, a pan reversal hold and a pan reversal cap returned from the whole tick (tilt was swallowed too), a halved tilt reversal step could fall under the 20-step minimum, and a bottom-cut box of 55 % or less flipped between the near and far rules. Now: the head band is HALF of `tband_pct` (7.5 % = 14 px around the target row, so the top is kept between about 14 and 42 px), the up step is `gain_tilt * h / fh / 5` (~119 for a full-height box), the pan holds only stop the pan, a halved step never drops under the minimum, and a box cut by the bottom edge and at least 40 % tall uses the head rule. `rot2k_track_unit.c` `hw_boxes` replays the logged boxes.
  Before (centre-based): At 1-2 m the box fills the frame vertically and its centre stays near
  the middle, which is why tilt rarely moved in practice. `track_tilt=1` enables it. The tilt soft range is respected (the policy clips the nudge to it; it was
  clipped with the wrong sign before: image "below" is tilt DOWN = motor -).
- Speed: the nudge carries a speed divisor (2 for a large correction, 0 = the configured speed 3 otherwise) through `a2k_ptz_nudge_sp` ->
  `hal_ptz_nudge_sp` -> the safe-motion layer `ptzs_goto(speed)`, which refuses anything outside 2..20. No new ioctl, no new motor command: the same timed polled
  goto, soft range, stall and wrong-way checks. A nudge is accepted only while the controller is idle.

### Safety (unchanged)

Safe-motion layer only; soft range (clipped in the policy, in the tracking areas and in `hal_ptz_nudge`); never while parked, calibrating, unknown, privacy,
or the position is outside every enabled tracking area; rate limit 300 ms between motion commands (the motor itself runs one command at a time); a person
beyond the soft range just pins the camera at the limit without further commands.

## Return policy (what the camera does when nobody is there)

`track_return=home|stay` (default **home**), `track_return_delay_s` (default 30), plus the lost-person grace `track_grace_s` (default 3).

- A person is *lost* after `track_grace_s` seconds without a box (detector blur and dropouts of up to that length change nothing).
- `stay`: nothing happens, the camera stays where the person was last seen, forever.
- `home`: `track_return_delay_s` seconds after the end of the grace the head goes home, once, only if tracking moved it. `0` = right after the grace.
  A person back before that cancels it; the delay restarts from the next loss.
- `akam2k.conf`: `track_return`, `track_return_delay_s`, `track_grace_s` (-> `--track-return`, `--track-return-delay-s`, `--track-grace-s`).
- Control API (live, persisted in `/config/akam/settings`): `track.return` (`stay|home`) and `track.return_delay_s` (0..86400) next to `track.enabled`.
  The grace is conf-only.
- 0.1.0 behaviour (home 10 s after the last box) = `track_return=home`, `track_grace_s=10`, `track_return_delay_s=0`.

Other conf keys: `track_gain_pan` / `track_gain_tilt` (200..4000, default 1000 / 700), `track_tilt_band_pct` (2..40, default 10), `track_band_pct` (2..40, default 6), `track_tilt_return_s` (0..3600, default 0 = off), `track_debug` (0..2: 1 = decision log, 2 = also the raw recorder, see "Hardware calibration experiments"), `track_lag_ms` (-150..300, default 50, signed: the video latency of the pose at capture), `detect_debug` (0|1), `detect_model_thr` (300..950, off), `detect_age_window` (0|1, default 0), `track_tilt` (0|1, default 0), `track_lead_pct` (0..100), `track_head_pct` (0..50, default 15). The supervisor re-reads `akam2k.conf` before every app (re)start, so a change applies when the app restarts (no supervisor restart).

## Targeting policy, person state and continuous motion (`track_motion`)

Hardware reports of 2026-11-17: "much better, less lag, but the camera often re-centres on the middle of the body for nothing", "after a quick feint it overshoots and loses me", the camera went all the way DOWN after a partial torso box and UP when the person filled the frame. This section is the answer. Everything is in `track_logic.c` (pure policy, no I/O), the wiring in `akam2k_track.c`, the motor side in `ptz/akam_ptz_safe.h` / `hal_ptz.c` / `akam2k_ptz.c` (see `ptz-findings.md` "Continuous motion").

### Keys, API rows, fallback

| conf key (`akam2k.conf`) | API row | default | meaning |
|---|---|---|---|
| `track_motion` | `track.motion` | `continuous` | `continuous` = one ongoing move re-targeted without a stop; `steps` = the timed nudges (the fallback; also used when the motor layer has no continuous mode). Live. |
| `track_zone_pct` | `track.zone_pct` | 40 | the comfort zone, % of the frame WIDTH (40 = the central 40 %, +-20 %). `0` = the dev targeting of before (central band 6 %, upper-third rule): the whole new targeting off. Live. |
| `track_zone_y_pct` | `track.zone_y_pct` | 30 | vertical comfort band of the BODY rule, % of the height (+-15 % around the frame centre). Live. |
| `track_zone_head_pct` | `track.zone_head_pct` | 20 | vertical band of the HEAD rule around the head row (`track_head_pct`, 15 %): +-10 %. Live. |
| `track_lease_ms` | (conf only) | 450 | continuous: the dead-man lease (150..2000): the motor stops by itself when no refresh arrived this long. |

`tl_cfg` also carries (code constants, not exposed): `zone_stop_pct` 35 (the move stops when the target is within 35 % of the zone half width), `full_pct` 85 (see below), `predict_ms` 500, `rate_k` 1100 (steps/s = 1100 / divisor), `gap_predict_ms` 1500, `lag_ms` 50 (video latency subtracted from the capture time), `meas_px` 4, `accel_mms2` 4000, `steps_rad` 654, `person_h_mm` 1700, `shoulder_mm` 450 (the Kalman parameters below). `tl_cfg_dev()` = the policy before this work (the "before" of the simulation); `tl_cfg_legacy()` = 0.1.0.

### A. What the camera aims at

- **Reference point.** A box that is not cut by the frame and shorter than `full_pct` (85 %) of the height is a WHOLE BODY: the reference is the centre of the box (smoothed, see below). A close person (>= 85 % tall) or a box cut by the frame bottom (>= 40 % tall) uses the HEAD: its top is kept at the head row (`track_head_pct`). Head cut by the top edge = head-out rule (up, in proportion to the box height). Hysteresis: once on the head rule a box must be under 77 % tall to go back (no flapping near the threshold). The old "upper third" rule of far persons is gone: it was the cause of "re-centres on the middle of the body for nothing" (a far person crouching or a head swinging moved the camera).
- **Ambiguous box (rule 5).** A box cut at the top AND the bottom and wider than 80 % of the frame is a person extremely close: no tilt decision at all (never a head-clip up), pan only when exactly one side is free (the existing wide rule).
- **Comfort zone with hysteresis.** No motion while the reference stays inside the zone (+-20 % of the width, band per rule vertically) and is not heading out. The camera starts when the reference leaves the zone or when its predicted position (angular rate x `predict_ms` 500) will leave it; it stops when the reference is back within 35 % of the zone half width (about +-7 % of the frame) and the person does not walk on. A walking person is followed as long as it walks (one motion episode), a person who stands inside the zone never moves the camera.
- **Smoothing.** The reference is the output of a Kalman filter (below), not the raw box: arm swing, head bob and box jitter are ignored.

### B. Person state in 3D (monocular), small constant-velocity Kalman filters

The policy reasons in the room, not in pixels. Per fresh frame (the pose at capture comes from the driver counters, also while the head moves):

1. **Distance.** Whole person: `d = person_h * F_v / box_height` with `F_v = frame_h * steps_rad / gain_tilt` (about 175 px for the defaults: the vertical focal length in px derived from the 700 steps per frame height) and `person_h = 1.7 m`. Box cut or close: from the width, `d = shoulder * F_h / box_width` (`F_h = frame_w * steps_rad / gain_pan`, shoulder 0.45 m, only when the box is not cut at the sides and narrower than 60 %), with 40 % noise; otherwise the last estimate is carried. Error: the height cue assumes 1.7 m (children / crouching: +-20 % is the budget, the filter noise is 20 %), the width cue +-40 %; a wrong distance only scales the angular rate of the prediction, never the position of the target (angles come from the pose and the pixel).
2. **Angles.** The world azimuth / elevation of the reference = the pose at capture + the pixel offset (small-angle: 1000 steps per frame width, 700 per height, 654 steps per radian).
3. **Filters.** One 1-D constant-velocity filter per angular axis: state = angle (motor steps) and LATERAL VELOCITY IN mm/s; the angular rate is `v * steps_rad / distance`, so a person walking at the same speed turns the camera consistently near and far (sim: 1 m/s at 2 m = 331 steps/s, at 4 m = 160 steps/s, ratio 2.07 for an ideal 2.0). A third filter smooths the distance (state d, d'). Walking towards the camera along a ray changes the distance, not the angles: the angular rate stays ~0 (no useless pan; sim: < 60 steps/s for 1.25 m/s). Process noise: acceleration std `accel_mms2` 4000 mm/s^2 (walking, feints, reversals); measurement noise = `meas_px` 4 px of detector jitter x a quality factor (1 whole body, 2 body centre, 3 cut at the side, 4 wide, 6 / 8 ambiguous or head clip). The innovation gate is 5 sigma: a lateral measurement outside it is an outlier and is held; a second consecutive one is accepted (the person really jumped). Single precision, only `+ - * /` (no libm): 1.5 us per tick on the host including the test projection, an armv5 soft-float core is about 30x slower: tens of us at 5 Hz.
4. **Robust velocity for lead and prediction.** The velocity the lead, the prediction and the control use is the Kalman rate, believed only once the median slope of the last fresh samples agrees with it in direction, the last two slopes agree, the net travel is >= 60 steps and the span >= 350 ms (>= 3 consistent samples), never more than twice the measured slope, and it drops to the latest slope as soon as the person decelerates and to 0 on a reversal. A feint (1-2 ticks) therefore gives no lead. The lead of the nudge mode is also capped by half the frame minus half the box width (never lead the person out of the frame) and its horizon by 0.8 s.
5. **Outliers.** A box whose top or centre jumps by more than 22 % of the frame height (in world terms: the pose at capture is taken into account) from the followed track is held until a second consistent fresh frame confirms it (hardware log 1: a partial torso box with the top at 99 instead of 28 drove the camera 219 steps down). A rejected box is never used for the exit logic either.

### C. Never lose the person

- **Gaps (continuous).** The constant-velocity prediction continues through a gap, fading to zero in 700 ms (bounded by `gap_predict_ms` and half a frame of travel), then the camera stops.
- **Re-acquisition by world position.** The sticky match uses the predicted position OR where the target was last seen (a feint or reversal makes the prediction wrong), a gate of 35 % of the width that GROWS with the time since the last fresh sighting (600 steps/s), a size cue (6 steps per px of height difference; two whole bodies that differ by more than 12 % in apparent size are not matched while the target was seen less than 0.7 s ago), and a penalty for a candidate that would have to move against the established direction. A box that does not match is not adopted while the target was seen less than 1 s ago (people crossing: sim 85 % of the time on the person it followed, DEV 29 %).
- **Search between the poses.** The pose before every large move (> 1/3 of a frame: a nudge, or a continuous episode whose displacement exceeds that) is recorded. When the person is lost within 3 s of it and did NOT leave through an edge: after 0.5 s (continuous) / 0.7 s (steps) of fresh no-box frames the head goes back toward that pose (continuous: a controlled move at divisor 4, ~275 steps/s, re-checking detections every tick with the position-compensated boxes; steps: two nudges of 60 % of the way), never beyond it, and stops at the first detection (the normal logic takes over); then the exit search / guard return as before. Host test: `rot2k_track_cont_unit.c` `sweep_cases` (the head reaches the previous pose and stops, a detection ends the sweep, an edge exit searches that way instead).
- **Existing caps kept:** the sky-climb cap (+400 steps per episode unless the head clip is confirmed on 3 fresh frames), the search limits (2 searches, 700 ms / 200 steps), the confirm ticks, the reversal caps in nudge mode.
- **Exit search only on fresh evidence (hardware logs 1 and 2).** The DOWN exit search needs a real downward trend of the box TOP over >= 3 fresh boxes in world terms (about 12 px); a single ambiguous bottom-cut box never starts it. Every exit search and the sweep wait for `LOSS_FRESH_MS` 700 ms of fresh (non-stale) frames without a person: never right after the camera's own move.

### D. Continuous motion

- **Policy.** Per axis, every tick: `e` = predicted reference minus the camera position now (`pos` from the driver, + latency 120 ms + the video age), `v` = the angular rate. Start when `|e|` > zone half or the predicted `e` (v x 0.5 s) leaves it; desired rate `v + e / 600 ms` (feed-forward + proportional), quantised to the speed divisors 2..20 (steps/s = 1100 / divisor, floor 55 steps/s = divisor 20, ceiling 550 = divisor 2), both axes at once (8 directions); stop when `|e|` is inside the inner zone and the target does not walk on. **Overshoot guard:** the commanded rate is limited so that after 400 ms the predicted reference is not beyond the opposite half-edge of the zone (a feint stops the camera instead of carrying it past the person). A close person (box > 50 % wide) caps the pan rate at 220 steps/s. Soft range: no command toward a limit the head is within 30 steps of (the motor layer enforces it again per axis).
- **Output.** `TL_VEL` on every tick that has an ongoing move (refreshes the lease), `TL_STOP` once when the move ends, nothing otherwise. The tracker maps it to `a2k_ptz_vel(sx, sy, dx, dy)`.
- **Frames while moving.** `a2k_ptz_pos_at(capture_time - lag)` gives the driver position at the capture of the frame (polled every 50 ms, interpolated): the box is converted to world coordinates with THAT pose, so frames taken during the motion are used (the nudge mode drops them). A frame whose pose is unknown (before the first sample, a gap) is "stale": nothing is learnt, the running command is refreshed.
- **Safety summary** (details in `ptz-findings.md`): soft range per axis in the safe layer; dead-man lease (150..2000 ms, 450 default; both a polled-time lease in the safe layer and a real-time one in the controller); stall / wrong-way detection per commanded axis, restarted on every command change; an axis that moves without being commanded is an error; a dropped axis is stopped first; privacy / park / calibration / any manual command / a manual nudge / the shutdown / disabling tracking / a state that is not home or moved / a position outside every enabled area / switching to `steps` stop the move within about 50 ms; tracking areas bound the move (the range handed to the policy is the area containing the head, and the wiring refuses a direction toward its edge); the travel counted for the wear budget includes reversals; no ioctl other than move (0..7), stop and position; no flash write (the sims and the unit test count driver requests).

### Simulation, before and after (`tests/rot2k_track_cont_sim.c`)

Real wiring (`akam2k_track.c` + `track_logic.c`) against a simulated room (pinhole camera 1.0 m above the floor, persons in metres, 5 Hz detector with 63..110 ms inference + 50 ms video latency, box jitter +-3 px, arm swing, partial boxes at the edges, motion blur: a miss probability of 35 % above 100 steps/s of pan and 55 % above 300) and a fake motor (nudges: 30 ms start, 1 step per divisor ms, pan then tilt, 70 ms stop; continuous: 1100 / divisor steps/s per axis, 30 ms command latency, immediate stop, 450 ms lease, 50 ms position samples). DEV = the policy before this work with nudges, STEPS = the new targeting with nudges, CONT = the new targeting with continuous motion. In zone = the person's reference within +-20 % of the width (64 px), measured over the true geometry every 5 ms; starts = the motor going from rest to moving (a nudge or a continuous move from rest).

Walking person, 9 s (0.5 / 1.0 / 1.5 m/s at 1 / 2 / 4 m; the camera turns at most 500 steps/s: 1.5 m/s at 1 m and 1 m/s at 1 m (700 steps/s) cannot be followed by any policy):

| walk | DEV zone / frame / max off / starts | STEPS | CONT |
|---|---|---|---|
| 0.5 m/s at 1 m | 89 % / 100 % / 113 px / 9 | 86 / 100 / 113 / 4 | 100 / 100 / 48 / 2 |
| 0.5 m/s at 2 m | 100 / 100 / 41 / 8 | 96 / 100 / 71 / 3 | 93 / 100 / 78 / 3 |
| 0.5 m/s at 4 m | 100 / 100 / 28 / 6 | 90 / 100 / 74 / 2 | 89 / 100 / 74 / 2 |
| 1.0 m/s at 1 m | 10 / 16 / 320 / 3 | 10 / 13 / 320 / 2 | 91 / 100 / 92 / 1 |
| 1.0 m/s at 2 m | 92 / 100 / 86 / 5 | 94 / 100 / 79 / 3 | 100 / 100 / 47 / 2 |
| 1.0 m/s at 4 m | 100 / 100 / 41 / 7 | 88 / 100 / 79 / 3 | 90 / 100 / 75 / 4 |
| 1.5 m/s at 1 m | 50 / 74 / 320 / 4 | 8 / 11 / 320 / 2 | 86 / 98 / 320 / 1 |
| 1.5 m/s at 2 m | 78 / 100 / 136 / 5 | 79 / 100 / 118 / 2 | 98 / 100 / 69 / 3 |
| 1.5 m/s at 4 m | 100 / 100 / 64 / 5 | 86 / 100 / 74 / 4 | 95 / 100 / 69 / 3 |

Over the six crossings the motor can follow: motion starts per crossing DEV 6.7 -> CONT 2.7 (the nudge mode sends one per 0.3 s of correction, the continuous move runs while the person walks); time in zone DEV 96.7 % -> CONT 94.4 % (by design the person may reach the zone edge before the camera moves: the old policy kept it within +-6 %, at the price of the starts and stops); CONT keeps the person in frame 100 % where DEV / STEPS lose it (1.0 m/s at 1 m, 1.5 m/s at 1 / 2 m). Distances are the DEV / STEPS weakness at 1 m: the box is cut and the old pan decision on it was unreliable.

| scenario | DEV | STEPS | CONT |
|---|---|---|---|
| standing still 3 m: tilt travel / starts | 233 steps / 4 | 0 / 0 | 0 / 0 |
| crouch 1.7 -> 1.0 m and stand: tilt travel | 204 | 90 | 83 |
| approach 4 -> 1 m and leave: pan travel | 163 | 0 | 0 |
| 1 s occlusion of a 1 m/s walker: in zone, max off | 80 % / 122 px | 89 % / 75 px | 96 % / 70 px |
| two people crossing: camera on the followed person | 29 % | 80 % | 85 % |
| feint left 2 m/s 0.6 s then stop: out of frame | 0 ms | 0 ms | 0 ms (max off 73 px) |
| feint then reverse: out of frame | 3420 ms | 215 ms | 0 ms |
| sprint 2 m then stop: out of frame | 0 ms | 0 ms | 0 ms |

(The reacquisition after a 1 s occlusion is 5 ms in the table: the person is already inside the comfort zone when it shows up, because the fading prediction did not carry the camera away; the 1.4 s hidden-after-sprint / hidden-after-feint variants end the same way, the camera never went past the pose before the move. The sweep-back itself is exercised by the unit test, the sims never overshoot enough to need it, which is the point of the overshoot guard.)

### Hardware logs replayed (`rot2k_track_cont_unit.c`, `replay_cases`, both motion modes)

1. Partial torso box (`97,99,279,192` after boxes with the top at 28, tilt 1580): held as an outlier, no tilt down; no DOWN exit search from the single box (needs the downward trend); a real downward trend (tops 30, 46, 62 ... then lost) still searches down.
2. Person fills the frame (`210,9,319,192`, then stale, then no box 1.2 s later, then `0,0,297,192`, `0,0,304,192`): no exit search at the first fresh no-box frame right after the camera's own move (needs 0.7 s of fresh frames), the 95 % wide box cut top and bottom is rule 5: no head-clip up.

### To verify on the camera (nothing here was run on hardware)

1. T-diagonal-drop: the safe layer stops an axis before it leaves a command (never relies on the driver dropping it). Run `track_motion=continuous`, `track_debug=1`, walk across: the log must show no `moves although it is not commanded` stall; if it does, the driver keeps unlisted axes in a way the fake model did not cover.
2. Speed change mid-move is assumed to be honoured (the command carries both speeds); the stall detector would not notice a driver that ignores it: compare the polled pan rate with `1100 / divisor` in the debug log (`ptz: velocity` sessions are logged only when they fail).
3. `rate_k = 1100` (370 steps/s at divisor 3) and `lag_ms = 50` (video latency): wrong values show as a constant lead / lag while walking; both are code constants of `tl_cfg` (`track_lease_ms` is the only conf key of this group besides the zones and the mode).
4. Distance cue: the person height 1.7 m and the focal lengths follow `gain_pan` / `gain_tilt`; a very different lens shifts the near / far scaling of the angular rate, not the aim.

## Simulation (tests/rot2k_track_sim.c)

The real wiring (`akam2k_track.c` + `track_logic.c`) against a simulated person and a fake PTZ: detector 5 Hz with 63-110 ms inference, frames captured
60 ms before the tick (the tracker is told the tick time: 60 ms of unmodelled latency, covered by `settle_ms`), box jitter +-3 px, blur while the head
moves (the person is lost in half of those frames), PTZ motion 30 ms start + 1 step per `speed` ms + 70 ms stop, one command at a time, pan inverted like
the 2K, true gain 1000 steps per frame. A person in frame = its centre inside the 320 px. `-DSIM_LEGACY` builds it against the 0.1.0 sources.

Constant walking speed, in frame % (walk of 8 s or up to the pan limit; 100 steps/s = 8.8 deg/s = 0.23 m/s at 1.5 m = 0.31 m/s at 2 m; the camera can turn 500 steps/s at most):

| steps/s | 50 | 100 | 150 | 200 | 250 | 300 | 350 | 400 | 450 | 500 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.1.0 (same on the 0.1.0 sources) | 100 | 100 | 100 | 100 | 54 | 27 | 28 | 29 | 30 | 27 |
| responsive | 100 | 100 | 100 | 100 | 100 | 100 | 75 | 48 | 32 | 34 |
| max offset px, 0.1.0 | 45 | 56 | 88 | 135 | 397 | 608 | 601 | 690 | 683 | 704 |
| max offset px, responsive | 30 | 40 | 46 | 71 | 118 | 155 | 237 | 515 | 495 | 655 |

**X = 300 steps/s (26 deg/s)** is asserted (the person stays in frame >= 90 % of the time); 350 steps/s holds 75 %; 0.1.0 holds up to 200 steps/s. (The longer settle time of 220 ms and the wait for the reported end of motion cost the 350 steps/s of the first version.)
In metres: 300 steps/s = 0.46 rad/s = 0.7 m/s at 1.5 m, 0.9 m/s at 2 m, 1.4 m/s at 3 m (perpendicular walking; a diagonal walk is slower in angle). A person
crossing at 1.5 m at a brisk 1.4 m/s (600 steps/s) is beyond the motor itself (500 steps/s at divisor 2).

| scenario | 0.1.0 | responsive |
|---|---|---|
| person appears 100 px off-centre: first motion command / inside the band | 400 ms / 1120 ms | 400 ms / 890 ms |
| walking 200 steps/s: time to first move / max offset | 795 ms / 135 px | 605 ms / 71 px |
| walking 50 steps/s: time to first move | 2800 ms | 1590 ms |
| stop-and-go (250 steps/s, 2 s / 3 s) in frame, end offset | 100 %, -29 px | 100 %, 19 px |
| reversal (+250 / -250 steps/s) in frame, reversals | 100 %, 1 | 100 %, 1 |
| dropouts of 0.6 s, 0.6 s, 1 s at 200 steps/s: in frame | 65 % | 100 % |
| true gain 700 / 800 / 1250 / 1500 at 200 steps/s: in frame | 97 / 100 / 100 / 100 % | 100 / 100 / 100 / 100 % |
| person just outside the band (32 px) | no move, stays at 32 px | 1 correction, 6 px |
| centred person, 20 s | 0 nudges | 0 nudges |
| 200 s without a person after tracking | home after 10 s | stay: 0 requests, pan unchanged; home delay 30: 35.6 s; delay 0: 5.6 s |

The same simulation asserts: no oscillation (a centred person never moves the camera; a person just outside the band costs at most 3 corrections without
reversal), tilt tracking, the pan / tilt soft range is never left (also with the person beyond the range), only valid speed divisors, the rate limit, and that
parked / calibrating / unknown / privacy never move the camera. Limits of the model: no real detector misses (the field ON/OFF flapping), no rolling-shutter
or AE effects after a motion, no vertical walking, one person.

## Flash wear and mechanical wear

- `/config` writes since the start, by source, in `/state` (control API): `sys.flash_writes_ptzpos` (always 0: the motor position lives in RAM, there is no `ptz.pos`),
  `sys.flash_writes_scene` (home reference files), `sys.flash_writes_store` (every other store file: settings, tracking areas, zones, video, ...).
- ptz persistence (2026-11-16, "No recurring flash writes" in ptz-findings.md): a tracking nudge writes nothing, neither the position nor the scene reference; the home reference is keyed
  to home, survives every move and is only (re)written after a calibration. A person crossing every 2 min: 0 writes per day (it used to be 2 per crossing = 60 per hour for `ptz.pos`
  plus up to 12 references per hour). Host test: `tests/rot2k_hal_ptz_unit.c`, 12 crossings, zero `ptz.pos` writes, zero scene writes and drops.
- Home is the scene-check-proven position, so with the default `track_return=home` (industry "guard position" behaviour) fewer calibrations are needed after a power cut: the head is nearly always at home.
- Cost of the `stay` option: with `track_return=stay` the head stays where the person was last seen. A power cut while it is away from home means a full calibration at the next
  cold boot (the home view does not match, so home cannot be claimed). `track_return=home` returns it home after the delay and avoids that.
- Travel budget: the steps travelled since the last calibration are counted (`ptz.travel_since_cal` in `/state`; an app start without a calibration counts as
  half the budget). Past `ptz_travel_budget_k` thousand steps (default 200, akam2k.conf, 10..99999) the home reference is no longer checked / rewritten in this run (RAM-only counter,
  lost with a power cut).
- A nudge is accepted by the PTZ controller only while it is idle: it never queues behind, replaces or outlives a step, home or park (no un-park after a
  park). While a command runs `a2k_ptz_nudge` returns -1 (the control API `ptz.move` too).

## API

- `tl_cfg_defaults(&cfg)`, `tl_cfg_dev(&cfg)` (before the targeting work), `tl_cfg_legacy(&cfg)`, `tl_cfg_validate(&cfg)` (0 / -1), `tl_init(&t, &cfg)` (-1 on invalid cfg).
- `tl_update(&t, now_ms, boxes, n, frame_w, frame_h, &ptz, &out)`.
- Box: `struct tl_box {x1,y1,x2,y2}` pixels in the detector frame (same as `a2k_det_box`, 320x192 on the 2K; clamped
  by the detector). Up to 8 boxes; empty/inverted boxes are ignored. Call it every tick, with `n = 0` when nobody is seen.
- `struct tl_ptz {moving, pan, tilt, pan_min, pan_max, tilt_min, tilt_max, cap_ms, motion_end_ms}` (motor steps, soft range inclusive; the two times in the clock
  of `now_ms`, 0 = unknown).
- `out.kind`: `TL_NONE`, `TL_NUDGE` (`dpan`, `dtilt`, `speed`, already clipped to the soft range), `TL_HOME` (caller goes home), `TL_VEL` (continuous: `pan_dir` motor sign, `tilt_dir` image sign, `pan_div` / `tilt_div` speed divisors; refresh it every tick) or `TL_STOP`. `struct tl_ptz` also carries `has_cap` / `cap_pan` / `cap_tilt` (the pose at the capture time) and `cont`.
- Sign: `dpan > 0` = person right of centre, `dtilt > 0` = below centre. `invert_pan` / `invert_tilt` flip it for the motor.

## Unit tests

```
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o /tmp/rot2k_track_unit tests/rot2k_track_unit.c firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c \
    && /tmp/rot2k_track_unit
```

Expected: `track: all tests passed` (the 0.1.0 cases through `tl_cfg_legacy`, then the responsive policy: band, proportional step, fast speed, rate limit, stale
frames, tilt, lead, return policy). `tests/rot2k_track_wire_unit.c` (wiring on a fake motor) and `tests/rot2k_track_sim.c` (above) run from `tests/rot2k_overlay_test.sh`. The targeting policy and the continuous mode: `tests/rot2k_track_cont_unit.c` (policy unit test incl. the two hardware replays), `tests/rot2k_track_cont_sim.c` (DEV / STEPS / CONT on the same scenarios), `tests/rot2k_ptz_cont_unit.c` (safe layer + HAL + controller on a fake driver); `rot2k_track_unit.c` and `rot2k_track_sim.c` run the dev policy (`tl_cfg_dev`).

## Wiring in the app (akam2k_track)

`app/akam2k_track.c` (core, motor through an ops table, host-tested on a fake motor by `tests/rot2k_track_wire_unit.c`) and `akam2k_track_run.c` (the real
binding). The detection thread calls `a2k_track_feed(now, capture_time, ...)` after every inference with the person boxes that count (not ignored, in a zone when zones apply;
`n = 0` when nobody), `TL_NUDGE` becomes `a2k_ptz_nudge_sp(dx, dy, speed)`, `TL_HOME` the home command.

- **Off by default.** `track=1` in `akam2k.conf` (`--track 1`) or the Home Assistant switch **Person tracking** (runtime only: a restart goes back to the key).
- **Signs.** `hal_ptz.h`: pan + = right, tilt + = up, and `a2k_ptz_nudge` takes right / up positive. A target right of the centre: `dx = +dpan`; a target below
  the centre (`dtilt > 0`): tilt DOWN, `dy = -dtilt`. `invert_pan` defaults to 1 on the 2K (measured), `invert_tilt` to 0: measured 2026-11-16 (API `ptz.move` dy=+250 from home, user absent): the tilt counter 1300 -> 1553 made the camera look UP (the ceiling entered at the top of the image, image upright), so a person above the centre needs the counter to increase. The tracker maps image-above (`dtilt < 0`) to `dy = -dtilt > 0` = counter up, which is what `invert_tilt=0` gives: the default is correct. `track_tilt` stays 0 until a test with a live person.
- **Never** while the camera is parked, calibrating, its position unknown, the privacy mode hides the video, the motor is moving, or the position is outside
  every enabled tracking area. Leaving any of these forgets the target (no surprise home request later).

## Tracking areas

Named rectangles in **motor coordinates** (steps) inside the soft range, up to 4: `track_area_<name>=PMIN:PMAX:TMIN:TMAX` (name = the zone name rule). Each is
enabled or disabled.

- No complete area: the whole soft range. With areas: tracking is allowed only while the current position lies inside at least ONE ENABLED complete area (all
  disabled = never), and a nudge is clipped to the enabled area that contains the current position (when several overlap, the one that lets the nudge go the
  furthest). An incomplete area (no corners yet) does not count.
- `akam2k.conf`: `track_area_<name>=...` lines (up to 4), passed as `--track-area name=...`; one outside the soft range is dropped with a log line.
- **Home Assistant:** the `text` **New tracking area name** creates an (incomplete, enabled) area; per area: the switch **Tracking area <name>** (enabled),
  the buttons **Set tracking area <name> corner A** / **corner B** (they store the CURRENT camera position; any order; complete with both; two equal corners are
  refused), and **Delete tracking area <name>**; a diagnostic sensor **Tracking areas** shows all of them. Slot based entities (`tarea0` .. `tarea3`, `_a`,
  `_b`, `_del`): when the list changes the entities are announced again and the unused slots are cleared.
- Persisted atomically in `/config/akam/track_areas` (`track_area_<name>=PMIN:PMAX:TMIN:TMAX:ENABLED`, or `=none:ENABLED` while incomplete); this file wins over
  the `akam2k.conf` keys when it exists (even when empty).

Tests: `tests/rot2k_track_wire_unit.c` (sign mapping, the "never" cases, home after loss, parse / format, overlap, enable, clip, corners, persistence, the responsive wiring).

## To verify on the camera (not done: no hardware in this change)

1. Walk across at 2-3 m: the head should follow with a correction every ~0.6-1.2 s; look at the `ptz: nudge` log lines (position and result).
2. If the camera over- or undershoots a static person, tune `gain_pan` (see the measurement above); a hunting head means the gain is too high.
3. `track_return=stay`: leave the room, the head must stay put; `home` (default, `track_return_delay_s=30`): it returns ~33 s after the last sighting.
4. The speed divisor 2 (500 steps/s) was verified for pan only, in one run; check it with the tracker. Tilt direction is measured (above), try `track_tilt=1` with a live person.

## Close range (user within about 1 m, camera on a desk)

- Physical limit: the NPU person detector needs enough of the body in view. Below about 0.8 m (only head and shoulders) it may not detect at all, and between 0.8 and 1.2 m the person flaps ON/OFF every few seconds (logs: boxes `30,0,318,191`, `0,0,292,191`, ON/OFF every 3-9 s). Nothing in the policy can track a person the detector does not see; the policy only avoids making it worse.
- Wide box (> 75 % of the frame width): the horizontal centre is unreliable (arms, two persons, the body cut by both sides), so there is NO pan on it, except a clear one-sided cut (the box touches exactly one side edge), which pans on the visible centre. A box wider than 50 % caps the pan nudge at 200 steps.
- Tilt uses the head rule (above); a very tall box cut at both ends just keeps the head row.
- Detection gaps: within the lost grace (`track_grace_s`, 3 s) the target, the streak and the sticky position are kept (the velocity samples are dropped), so a flapping person that comes back needs no new 2-tick acquire; after the grace the tracker forgets it (the return policy is unchanged).
- Approaching person: when the box area grows by more than 30 % between two consecutive ticks (0.8 s at most apart) the tick is skipped ("approach"): the geometry of a box that is growing fast says nothing about where the person will be.
- Tests: `rot2k_track_unit.c` `close_range` (physical model: desk at 0.75 m, 4 m -> 0.8 m approach, standing close, crouching close, standing up; no pan on an on-axis person, no large tilt nudge, at most 2 reversals; wide boxes; the gap hold).

## Debug log and live tuning

- **No accumulating lag (hardware report 2026-11-17, checked in `rot2k_track_sim.c`, latency model: capture 60 ms before the tick, inference 63..110 ms, 220 ms settle, 5 ms motor steps).** A nudge is computed from the CURRENT pose and only from a frame captured after the last motion end + `settle_ms` and after the last nudge started (`stale` in `tl_update`); the HAL refuses a nudge while it is busy (never queues, never cancels), so nothing stacks; the velocity lead is computed from world positions (pan at the frame + the offset in it) of fresh frames only, with the horizon capped at 1.5 s and `lead_max`, and a lead against the correction is dropped. The sim now counts, for every walking speed 50..500 steps/s, the nudges sent while the motor was busy, the nudges from a stale frame, the overshoot (camera ahead of the person, max px) and the cumulative error (mean |offset| while walking): 0 busy, 0 stale, 0 px overshoot at every speed; mean |offset| 13 px at 50, 47 px at 200, 107 px at 300 steps/s (a lag, never an overshoot; the person leaves the frame above 350 steps/s because the motor cannot keep up). Nothing needed fixing; the checks are asserted.
- `track_debug=1` in `akam2k.conf` (`--track-debug 1`): one line per decision to the RAM file `/tmp/akam2k_track.log` (never the card; a rolling window: past 64 KiB the newest half is kept, never truncated to empty; at most 2 lines per second, every nudge is logged): frame time, number of boxes, the box, the rule (`centre`, `head-clip`, `near`, `far`), the target row, the offset in px, the pan/tilt position, the wanted and sent nudge, and the reason when nothing was sent (`stale`, `confirm`, `interval`, `gap-hold`, `approach`, `in-band`, `min-step`, `range`, `pan-hold`, `pan-reversal-cap`, `tilt-reversal-cap`, `no-person`, `nudge`). Read it over SSH: `cat /tmp/akam2k_track.log`.
- Control API (live, persisted): `track.band_pct` (2..40, pan band), `track.head_pct` (0..50, head row), `track.gain_pan` / `track.gain_tilt` (200..4000 steps per frame width / height) and `track.tilt_band_pct` (2..40, the vertical band, the head band is half of it), see control-api.md; the conf keys `track_band_pct` / `track_head_pct` are the defaults.

## Recovery after losing the person (hardware test of dev ca11529, 2026-11-16)

Log: the camera looked up (tilt 1866, home 1300) after following a close person; the user walked away / crouched, the box top sank (`19,55,202,190`, 70 % tall, bottom cut), the frame was stale, then `no-person` for good: the head stayed looking up. Findings and fixes:
- `rule=centre target_row=0` on those lines was a LOGGING artefact: the rule was only recorded after the stale return. The classification (`head_rule`) now runs before it. A bottom-cut box at least 40 % tall (or any box 55 % tall) keeps the head rule.
- A stale frame never touches the target any more: with a known target it only records the box (for the exit logic) and the sighting time; it is NOT matched by position (the pan read of a moving camera is 0/0, it was fed to the sticky match). The position read of a motion-time tick is logged as `unknown`.
- Exit search: the last boxes (stale frames included) give the exit direction. DOWN: the box top sank by >= 12 px to at least 20 % of the height, or the box is cut at the bottom with its top at >= 25 %. UP / LEFT / RIGHT: the box touches that edge and is narrower than 60 % of the frame (a close, wide, flapping person is not leaving). Lost inside the grace, motor idle: at most 2 nudges of 200 steps (0.9 s apart, soft range respected, pan / tilt as the direction says), then it holds. `why=exit-search` in the debug log.
- Tilt return (`stay` mode, `track_tilt_return_s`, default 0 = off): the pan stays, but grace + `track_tilt_return_s` s after the last sighting the TILT goes back to the home tilt (one nudge, only if tracking moved the tilt, only when more than 40 steps away). `home` mode (the default) returns both axes instead. 0 (the default) disables it.
- Tests: `rot2k_track_unit.c` `recover`, `down_cases`, `away_sim` replay the logged boxes (head clip, then `19,55,202,190`, stale, 28 s of nobody), crouch while detected, sitting, crouch with loss, walk-away with the camera up.

### Down vs up responsiveness (sim, close person at 0.9 m, 200 ms ticks, head row 28 settled)

| step | first nudge | settled in the head band | nudges |
|---|---|---|---|
| head DOWN 90 px (crouch) | tick 1 (0.4 s: two acquire ticks) | tick 6 | 2 |
| head UP 60 px | tick 1 | tick 5 | 2 |

Tilt uses the same stale rules as pan (`settle_ms` 220, nudge lag, motion end): there is no extra tilt-only drop. The extra dead time of a combined move is in the MOTOR layer: `ptzs_goto` moves pan FIRST, then tilt, each axis by its own polled timed move with a stop and 20 ms wait between, so a pan+tilt nudge lasts the sum of both and every frame inside it is stale.

### Smoothness: can a goto be retargeted while moving? (analysis, nothing implemented)

**Update 2026-11-17: implemented as the continuous mode** (hardware: a new direction command during a move is accepted at once without a stop, diagonals move both axes at ~370 steps/s; the analysis below is why it needed its own motion mode in the safe layer). See "Targeting policy, person state and continuous motion" above and `ptz-findings.md` "Continuous motion".


- `ptzs_goto` (akam_ptz_safe.h) is blocking: `ptzs_axis_goto` sends ONE directional timed move (commands 2/6 pan, 0/4 tilt), polls the counter every 50 ms, stops at the target, up to `PTZS_PASSES` passes. The HAL (`submit` in akam2k_ptz.c) refuses a nudge while busy or with a command pending: it never queues and never retargets. The safe layer's whole guarantee (stall detection, soft-limit clip, the driver limits as the shifted frame) is built on ONE motion owning the motor at a time.
- What the driver is documented to do (ptz-findings.md): `set_ptz_cmd` takes the direction 0..7 and the pan / tilt speed divisors, speeds are kept per axis ("w1 > 0 -> pan speed, w2 > 0 -> tilt speed"). Directions 1, 3, 5, 7 (diagonals) exist but only 0, 2, 4, 6 are hardware verified. What a second `set_ptz_cmd` does while the first still runs (retarget? restart? queue?) and whether a speed change mid-move is honoured is NOT documented and was never tested on the camera. Without that, a chained retarget could leave the counters and the stall detector disagreeing (a stop is always sent last, the position file goes to `unknown` on every move start).
- So: no retargeting was added (it would need a new motion mode in the safe layer, i.e. a motor change for review, and a hardware test first). Proposed hardware test T-retarget (camera in a safe middle position, user watching and listening): `move 6 (left) 1000 ms` in the background, 300 ms later a second `move 2 (right)` and `move 6` at another speed; read `pos` every 50 ms; success = the second command takes effect without a stop, the counters follow within +-3 steps, no stall. Until then the policy uses short nudges and the speed divisor 2 only for large offsets (> 25 % of the frame) and the default 3 otherwise.
- Policy-level smoothness / reactivity trials in the sim (settle_ms): 150 and 180 raise the in-frame ratio at 350 steps/s from 75 % to 92 % but the stop-and-go case then ends 33 px off the centre and fails its "ends centred" check; 100 passes but has more reversals. `settle_ms` stays 220 (stale-frame safety against a still-moving head).

## Hardware test of 3908f89 (2026-11-17, continuous mode): follow-ups

User: "vraiment pas mal". What was wrong and what changed (`rot2k_track_cont_unit.c`: `real_headout_cases`, `lost_chain_cases`; `rot2k_track_cont_sim.c`: the log):

- **Decision log in continuous mode** (`track_debug=1`, RAM file `/tmp/akam2k_track.log`, rolling 64 KiB, nothing on the card). One line per tick that has a box or a command, an idle tick only when its reason changed or once a second:
  `T n=1 CONT box body - ref=159,105 pan=1275 tilt=1300 | dist=2989mm v=0/0 st/s kf=ok | zone ex=0/0 ey=0/0 mv=00 | want=0/0 cmd=-0/0,-0/0 lease=0 | hold=0 search=0 sweep=0 exit=0 | why=confirm`.
  `box <rule>` (centre / head-clip / near / far / body / ambiguous, `(head-out-from-ambiguous)` when rule 5 was read as a real head-out), `ref=col,row` the reference point, `dist` the filtered distance, `v` the angular rates (steps/s, pan/tilt), `kf` the innovation gate (`ok`, `held`, `reinit` = accepted after two rejections), `zone ex/ey` the predicted offset over the zone half (steps), `mv` the axes being moved (hysteresis), `want` the desired rate, `cmd` the direction / divisor per axis (`-1/3` = pan negative at divisor 3; tilt `d` down, `u` up), `lease` the ms asked (0 = no command, -1 = stop), `hold` the outlier hold, `search` / `sweep` / `exit` the loss state, `why` the reason (`vel`, `in-zone`, `confirm`, `approach`, `outlier`, `stale`, `tilt-cap`, `predict`, `sweep-back`, `exit-search`, `gap-hold`, `no-person`, `lost-*`).
- **Approaching person, the camera stopped following the head up.** Rule 5 (box cut top AND bottom and > 80 % wide: tilt held) also caught a REAL head-out. Now the head is "seen rising" when over >= 3 fresh samples whose two older tops were not cut the top climbed by about 12 px in world terms (`up_trend`); for 1.5 s after that, a box that is ambiguous is read as rule 1 (head out, up in proportion to the box height), without clip confirmation: the sky-climb cap (+400 steps per episode) bounds it. No rising head before (hardware log 2: the person simply filled the frame) = rule 5, tilt held, as before. Sim (`real_headout_cases`): a person with arms out walking up to 0.38 m: the box becomes 100 % wide and cut top and bottom, the camera goes on up to the cap (1300 -> 1721) instead of stopping at 1575.
- **Camera tilted up, the person goes down so that only the head is left at the bottom of the frame.** (a) The DOWN exit search is allowed by the Kalman state as well: the vertical reference moving down at >= 60 steps/s (`kf_down`) is enough, the 3-fresh-box trend is no longer the only way (a partial box that appears suddenly with a quiet track is still held: hardware log 1). (b) A small box cut at the bottom edge that is smaller than the followed box is accepted as the head when the filtered state was moving down (size penalty and outlier hold do not apply to it). Sim: tilt 1406, the person sits down in 0.8 s, the detector loses the head crop at the bottom: the camera follows down 1406 -> 1001 through the prediction and two exit searches until it sees the head again, never an upward command. Limit: an ABRUPT single-frame drop of the top (28 -> 81 px at once, then nothing) stays held as an outlier, because it cannot be told from the partial torso of log 1; it needs a second consistent frame or a downward motion before.

### Lost-target policy (`track_lost_policy`, API `track.lost_policy`, `track.lost_step`)

What the tracker does when the person is lost: an ordered chain of optional steps, or nothing.

| step | meaning |
|---|---|
| `hold:N` | stay still N s (1..3600) |
| `backsweep` | between the pose before the last large move and the current pose (the overshoot search above; skipped after an edge exit, without a large move, or 3 s after the move) |
| `exit` | search toward the exit direction (two moves of 0.7 s, as above; skipped when no exit direction is known) |
| `local:W:D:N` | oscillate around the last seen position: width W steps (40..1500), speed divisor D (2..20), N s (1..600); the pan only |
| `route:<name>[:N]` | run the stored patrol route `<name>` (patrol engine, `ptz.patrol.*`) for N s (1..3600, default 60) or until it ends; a person seen again stops it |
| `home` | go to the guard position (home) |
| `stay` | end: remain (only as the last step) |

`none` = do nothing at all (no search, no home). Up to 8 steps, separated by commas, no spaces: `backsweep,exit,local:300:4:20,home`. **The default `backsweep,exit,hold:30,home` is the legacy behaviour** (the old return policy with `track_return`, `track_return_delay_s` and `track_grace_s`, unchanged); any other value switches the chain on and replaces those three keys. A detection of the person at ANY step re-acquires it and ends the chain (a route the chain started is stopped, a move is taken over by the normal logic). The chain needs `track_motion=continuous` (the steps mode keeps the legacy recovery). All or nothing: an invalid string is refused as a whole (API 422-style check error, the supervisor ignores the conf line, the app refuses the option).

Bounds: every move is a velocity command of the continuous mode (soft range per axis, lease, stall detection, privacy / park / calibration / manual preemption as for any tracking motion); a route is the patrol engine's (its own travel budget, privacy and manual preemption; `a2k_patrol_note_person` pauses it when the person is back); `home` is the usual tracker home (not sent while a patrol runs or waits to resume); nothing is written to the flash. `track.lost_step` (read only, in `/state`) is `idle` while the person is followed or nobody was lost, then `backsweep`, `exit`, `hold`, `local`, `route:<name>`, `home`, `stay` or `done`; every change is also an event `{"type":"track","data":{"lost_step":"exit"}}` (`GET /events`) and a log line. Examples:

```
track.lost_policy = "backsweep,exit,hold:30,home"        # the default (legacy)
track.lost_policy = "backsweep,exit,local:300:4:20,home"  # look around the last position for 20 s before going home
track.lost_policy = "exit,route:entrance:90,stay"          # follow the exit, then patrol the route "entrance" for 90 s, then stay
track.lost_policy = "none"                                 # never move on a loss
```
Conf: `track_lost_policy=backsweep,exit,local:300:4:20,home` (same syntax, validated word by word by the supervisor). Tests: `rot2k_track_cont_unit.c` `lost_chain_cases` (syntax incl. 25 invalid strings leaving the config untouched, hold + home, none, the order exit / local / home with the local sweep width, a route started once and stopped by a detection, a route that ends by itself), `rot2k_track_wire_unit.c` `test_lost_policy` (all or nothing through the tracker, the step for /state), `rot2k_overlay_test.sh` (the supervisor accepts / ignores conf values).

### Review round 2 (Fable on 9eaf596)

- The Kalman-down bypass (accepting a small bottom-cut box, and allowing the DOWN exit search from the filtered motion) is limited to a genuinely small head crop (box height < 40 % of the frame) and needs >= 180 steps/s of downward rate; a tall partial torso never uses it (replay of hardware log 1 unchanged).
- After an ambiguous head-out climb the clip stays unconfirmed (`amb_latch`) until a box with an uncut top is seen again: the sky-climb cap (+400) holds even if the top stays cut; a new target clears the rising-head memory and the latch.
- Lost chain: the `local` window is clamped inside the soft range minus the 45-step margin (skipped when it cannot fit, and when the travel budget is used up: `tl_ptz.over_budget`, from `a2k_ptz_travel()` vs the patrol travel budget); the `route` step is skipped when a patrol already runs (the user's one is never started again nor stopped), `lost_route_on` is set only when `route_start` returned 0, and a route the chain started is stopped when the tracker is disabled or parked. `tl_lost_format` can no longer truncate (the row accepts 255 characters, the longest valid chain is 223).

## Close range (0.6 m): the camera went down / stayed up (hardware logs of dev 462356f)

1. **Descent for nothing at 0.6 m.** The log: the head row jumped 44 -> 56 while the camera panned diagonally, the vertical Kalman rate read -122..-294 steps/s, the head was corrected down, the detections stopped, and the DOWN exit search (built on that rate) drove the tilt 1152 -> 968. Causes: (a) at 0.6 m the person fills the frame, the head row is unreliable and the head-row zone (+-10 %) too tight; (b) a vertical rate computed from frames captured while the camera moved is mostly compensation error: the pose at capture comes from 50 ms driver samples and a video latency constant (`lag_ms` 50), an error of delta ms in it shifts a measurement by camera rate x delta and a change of rate makes a velocity pulse.
   Fixes: the tracker passes the camera rate at the capture instant (`cap_rate_*`, from the driver samples 25 ms either side); the Kalman measurement noise grows with it (rate x 70 ms); the vertical rate used for control is zeroed when the tilt moved in the last 2 frames; `kf_down` / `down_trend` need the last 3 frames captured with the camera still, or a genuinely small head crop (< 40 % tall) at the bottom; below 0.8 m the DOWN search needs a small crop, the vertical dead band is doubled, a tilt starts only on 2 consecutive fresh frames, and the prediction through a gap never STARTS a tilt move (it only continues one that was on).
   Audit (`rot2k_track_cont_unit.c` `close_down_cases`): a static person at 0.6 m, the camera moved by a script (pan +-370 steps/s with reversals every 0.7 s, tilt +-150 steps/s), video latency 0 / 40 / 80 / 120 ms longer than the model: residual rate of the compensated person (raw Kalman, steps/s) before -> after: vertical 10 / 89 / 164 / 260 -> 9 / 84 / 157 / 253, lateral 52 / 272 / 474 / 656 -> 49 / 180 / 380 / 518. With the exact latency the residual is small (9 and 49 steps/s: ring interpolation only); it grows linearly with the latency error, which is why the rate used for control is gated, not trusted. Calibrating the constant on the camera: person standing still, pan the camera by hand with the buttons, `track_debug=1`: the `v=` field must stay near 0; `lag_ms` (code constant, 50) is the knob if it does not.
   Replay of the sequence (person static at 0.6 m, 60 ms of unmodelled latency, off-centre, detections stop after 4 s): before, the tilt went 1405 -> 1290 (a correction of a merely low head row, then the prediction kept going); now it stays at 1405 and no DOWN search runs.
2. **Climb for the head at 0.6 m, then the detections stop and the camera stays up.** The log also showed a zone band of 1048576: the "tilt not decided" sentinel (ambiguous box / approach hold) leaked into the logged band; the tilt axis is now skipped explicitly and the real band is logged. The distance dropped from 600 mm to the 3 m default on a cut box: a new target (or one after a gap) with no distance cue now keeps the last estimate (60 s, wide variance). After a head-clip episode (the last two fresh boxes top-clipped) a loss runs a bounded UP exit search (1-2 moves, never beyond the sky-climb cap, even for a wide box). And `track_tilt_back_s` (conf, API `track.tilt_back_s`, default 5, 0 = off): when the person is lost with the tilt more than 250 steps above home, the tilt alone goes back to the home tilt after that delay (moderate speed, at most 20 s, once per loss, not into a soft limit), whatever the return policy does for the pan.
   Replay (`close_up_cases`): whole person at 2 m (estimate kept), head-clip boxes `90,0,150,191` climb 1300 -> 1580, ambiguous `10,0,315,191` (band 70, no sentinel), then nothing: three UP searches within the cap, then the tilt is back within 60 steps of home after 32 ticks; `tilt_back_s=0`, 200 steps above home, and a person seen again are handled.
3. **Close range design review (Fable): low crop, close-loss machine, measurement model, jump threshold.**
   - Rule 7, "low crop": top uncut, bottom cut, height < 40 %: the box top is the reference (row `head_pct`, band `zone_head_pct`), no doubled band, no two-frame start, never rule 4.
   - Close-loss machine (a near target: bottom cut and >= 40 % tall, a low crop, or closer than 1200 mm), timed from the first fresh no-box frame, replacing the exit search and the tilt-back for it: HOLD 0-700 ms (pan prediction only); then SEARCH when the last accepted frames give evidence (E_UP: the last 2 tops clipped; E_DOWN: the last frame was rule 7, a downward jump is being held, or the sigma-aware down trend; E_SIDE: a box at a side edge): 2 moves of 700 ms at `rate_k`/4, 900 ms apart, 400 steps at most from the loss pose, clipped to the soft range; or WAIT without evidence until 2500 ms (no motion); then WIDEN: the tilt goes toward the home tilt, either direction, at `rate_k`/6 while it is more than 150 steps away, stops within 40 steps or after 20 s, the pan is not touched; then the existing lost chain. Any accepted box ends it at once. Far targets keep the old logic.
   - Measurement model: tilt sigma max(`meas_px`, 5 % of the box height), centre sigma max(`meas_px`, 4 % of its width) x the rule factor, plus (camera rate x 70 ms)^2; the angular process noise is capped at 2000 steps/s^2 (effective acceleration min(`accel_mms2`, 2000 x D / `steps_rad`)); every box bounds the distance (D <= `person_h_mm` x f / box height and D <= 700 mm x f / box width, applied to the filter at every frame: a box cut at both ends reads at most ~1.6 m, never the 3 m default); the distance floor is 300 mm. The detector score is only logged.
   - Jump threshold clamp(0.6 x head height (250 mm at D), 10 %, 30 % of the frame height) instead of 22 %. A downward top jump of a near box with the bottom still cut freezes the tilt and the vertical filter until 2 further frames agree within a third of the threshold (a return to the old top discards it); a near target does not start a tilt while the camera pans faster than 200 steps/s (rules 1 and 7 excepted). The steps path holds an approach like the continuous one (rule-4 tilt decision and DOWN only).

## Hardware calibration experiments

Instrumentation for the tuning that cannot be done on the host (it changes no policy: `tl_update` ignores head boxes, see E1). Everything below runs on the camera with a person, RAM only.

**The raw recorder (`track_debug=2`).** `akam2k.conf` `track_debug=2` (restart the app after editing the card file) writes `/tmp/akam2k_track.raw` (tmpfs, rolling, 128 kB: past that the newest half is kept and the `C` line is written again). Four kinds of lines:

| Line | Content |
|---|---|
| `C key=value ... lost=<chain>` | the policy configuration (every scalar of `struct tl_cfg`), written first and after every change or roll |
| `I now cap pan tilt has_cap cap_pan cap_tilt rate_p rate_t moving tilt_home pmin pmax tmin tmax n {cls,score,x1,y1,x2,y2}... me=<motion_end> cr=<cap_rate_ok> ob=<over_budget> rb=<route_busy> ct=<cont>` | EVERY `tl_update` input: both clocks, the pose, the pose at capture, the camera rate, the soft range, all boxes (class 0 person, 1 head) with their score; the trailing `key=value` are the remaining `struct tl_ptz` fields |
| `R t pan tilt` | every position-ring push of a continuous motor session (one per ~50 ms, monotonic ms: the clock of `now` / `cap`) |
| `D <decision line>` | the continuous decision line of `track_debug=1`, extended with `rule=`, `dcue=` (distance cue: `h` height, `w` width, `carried` the last estimate, `none`; `head` is reserved), `inn=x/y` (normalised innovation of the pan / tilt filter before the update, in 1/100 sigma), `rs=x/y` (measurement noise used, steps), `st=` (near mode, filter state, velocity command, lost step) and `bud=` (travel budget used up, consecutive gate rejections, age of the last fresh frame) |

`track_debug=1` alone keeps the old log (`/tmp/akam2k_track.log`, now with the extended fields) and writes no raw file. Fetch with `ssh root@$CAM cat /tmp/akam2k_track.raw > run.raw`.

**Replay (`tests/rot2k_track_replay.c`).** Reads the `C` and `I` lines and runs `tl_update` with the recorded configuration, one decision per input (`O now kind pan=dir/div tilt=dir/div nudge=dp/dt why=... rule=... dist=... cue=...`):

```sh
cc -std=gnu99 -O1 -Ifirmware/lsc-rotatable-2k-ak3918av130/app -o /tmp/track_replay tests/rot2k_track_replay.c firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c -lm
/tmp/track_replay run.raw                                         # the decisions of this checkout on the recorded inputs
/tmp/track_replay --cfg lag_ms=100 --cfg zone_x_pct=30 run.raw    # what-if: override any field (names of track_cfgkv.h)
/tmp/track_replay --twice --heads -q run.raw                      # determinism; head boxes added to every frame change no decision
```

Comparing the output of two checkouts on the same recording shows what a policy change does on a real session. `tests/fixtures/track/` holds the two hardware logs of `rot2k_track_cont_unit.c` (`torso_down.raw`, `fills_frame.raw`) with `# expect` lines; `rot2k_overlay_test.sh` replays them (twice, with heads added), and `rot2k_track_rec_unit.c` checks that the replay of a recorder output decides like the live tracker.

### E1. Does the model see heads, and how well (class 1)

Is the head class (class 1 of the stock model, `ai-own-app.md` section 9, `npu-models.md`) reliable enough to become a distance or vertical cue? The detector passes up to 4 head boxes (class, score) next to the 8 persons; the policy ignores them.
1. `akam2k.conf`: `detect_debug=1`, and to see weak candidates `detect_model_thr=450` (the model returns boxes down to this score; persons below `detect_threshold` are logged but never counted or tracked).
2. Stand at 0.6, 1, 2, 3, 4, 6 m facing the camera, sideways, with a cap, the head partly cut by the frame top, sitting; 20 s each.
3. `grep 'detect-debug: all' /mnt/sdcard/akam/akam2k.log` (the app log): one line per second, every box as `cls:score:x1,y1,x2,y2`. Read at which distances and poses class 1 appears, its score, and whether its box sits at the top of the person box (head y1 within a few px of the person y1, centred in x).
4. Decision: a head box in more than 80 % of the frames at 1..4 m with a stable size gives a third distance cue (`dcue=head`) and a better vertical reference; otherwise drop it.

### E2. Lag and gain sweep (the latency of the pose at capture, the pan / tilt gains)

`lag_ms` (default 50) and the gains (1000 / 700 steps per frame width / height) are estimates. Measure them with a STATIC target while the head sweeps in a continuous session. Only the tracker moves the head in a continuous session, so the sweep is made by the tracker following a WALKER while a STATIC DECOY (a person standing still, or a life-size poster / mannequin, away from the walker's path) stays in view.
1. `akam2k.conf` on the card: `track=1`, `track_tilt=1`, `track_motion=continuous`, `track_debug=2`; restart the app.
2. Put the decoy in view at 2 to 3 m. The walker walks left / right (and sits / stands for the tilt) in front of the decoy so that the head sweeps +-300 steps or more on pan and +-100 on tilt several times, for 60 s. The decoy must stay in the frame (stay within +-35 deg of it).
3. `ssh root@$CAM cat /tmp/akam2k_track.raw > sweep.raw`
4. `tools/track_lagfit sweep.raw` (Python, standard library only; `--invert-pan` / `--invert-tilt` when there is no `C` line, `--window` for the decoy selection). It picks the static box (the world position that appears in most frames) and, for every lag of -150..300 ms in 5 ms steps, fits the gains in closed form by minimising the variance of the compensated world position `pose(cap - lag) + offset x gain`; it prints the best lag, each axis' own lag, the gains, the residual (steps rms) and the `akam2k.conf` lines.
5. Apply `track_lag_ms=<fit>`, `track_gain_pan=<fit>`, `track_gain_tilt=<fit>` in `akam2k.conf` (or live with `PATCH /settings {"track.lag_ms":<fit>,"track.gain_pan":<fit>}`, control-api.md), record again: the residual must be smaller (a few steps rms is a good fit). A negative gain means the invert flag is wrong. Pan and tilt lags that differ by more than 30 ms point to a clock problem rather than to a latency: report both.

Expected: a lag between 50 and 150 ms.

### E3. Clock (20 min static scene, raw age drift)
A static scene for 20 minutes with `detect_debug=1` and `track_debug=2`: the `age=` field of the `detect-debug:` lines and the frame stamps of the `I` lines must stay flat. A drift (the age grows or jumps) is a clock problem, not a latency: compare `detect_age_window=0` with `1` (the reference is the minimum over the last 60 s) and check that the window does not hide a real stall (an age above 500 ms must still appear when the NPU is loaded).

### E4. Distance (tape at 0.6 / 1 / 1.5 / 2 / 3 m vs estimate and cue)
Stand still at 0.6, 1, 1.5, 2 and 3 m (tape measure), whole body when it fits and cut by the frame otherwise, `track_debug=2`. In the `D` lines compare `dist=` and `dcue=` (`h`, `w`, `carried`) with the tape: the height cue should be within +-20 %, the width cue within +-40 %, and a box cut at both ends must never read farther than about 1.6 m (the distance bound of every box). Adjust `person_h_mm` / `shoulder_mm` when the bias is systematic.

### E5. Jitter and dropouts (sit still 30 s at 0.6 / 1 / 2 m, camera still)
Sit still for 30 s at 0.6, 1 and 2 m with the camera still. From the `I` lines: the standard deviation of the box top and of the centre in px (the model assumes max(`meas_px`, 5 % of the box height) for the top and max(`meas_px`, 4 % of its width) for the centre), the number of frames without a box and the length of the longest gap, and the `inn=` statistics (68 % of the values within +-100 in 1/100 sigma for a well tuned filter). Replay with `--cfg meas_px=N` / `--cfg accel_mms2=N` to compare.

### E6. Blur (pan at divisors 3 and 2 across a static person, day and low light, detection rate while moving)
Pan the camera across a static person at divisors 3 and 2, by day and in low light: the detection rate while the camera moves (frames with a box over frames taken, from the `I` lines with `cap_rate` above 60 steps/s) against the rate when still. It decides whether a moving camera can be trusted for decisions or whether the loss logic must expect blur dropouts.

### E7. Motor (steps/s per divisor, single axis and diagonal, command-to-motion delay, from the R lines)
Command constant velocities at divisors 2, 3, 4, 6, 10, 20, on one axis and on the diagonal: from the `R` lines the slope of pan / tilt against time gives the steps per second (`rate_k` = rate x divisor, default 1100, about 370 steps/s at divisor 3; a diagonal at divisor 2 was never measured). The delay between the `cmd=` of a `D` line and the first moving `R` line is the command-to-motion delay (model: 30 ms) and the lease (450 ms) must stop the head within 100 ms of the last refresh.

### E8. Scenarios (approach, sit / stand at 0.6 m, crouch under a high camera, feint; saved as replay fixtures)
Record with `track_debug=2`: an approach from 4 m to 0.6 m, sitting down and standing up at 0.6 m, a crouch under a high camera, a feint. Replay each with the old and the new checkout (`rot2k_track_replay`, same file), compare the `O` lines (moves, direction reversals, histogram of `why=`), and save every recording that makes the policy do something it should not as a fixture under `tests/fixtures/track/` with an `# expect` line.

### E9. Tilt geometry (the tilt value that is horizontal, whether 2100 is reached standing at 0.6 m)
Find the tilt value at which the camera looks horizontally (a level and a distant horizontal line at the frame centre) and whether the soft limit 2100 is reached when a person stands at 0.6 m and the head is aimed at the head row. It fixes the home tilt, the head-clip climb budgets (400 / 700 / 300) and the close-loss search budget (400 steps).
