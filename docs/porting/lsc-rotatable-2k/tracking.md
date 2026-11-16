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

| field | default | meaning |
|---|---|---|
| deadband_pct | 8 | central band, pan: no move while the box centre is within +-8 % of the width (26 px = 80 steps = 7 deg) of the centre (`track_band_pct`) |
| tband_pct | 15 | central band, tilt (% of the height) |
| gain_pan / gain_tilt | 1000 / 600 | steps per frame width / height (derivation below) |
| gain_pct | 85 | damping: one nudge corrects 85 % of the measured offset |
| max_step | 700 | clamp of one nudge, per axis (was 300) |
| min_interval_ms | 300 | rate limit between two nudges (was 1500) |
| confirm_ticks | 2 | ticks before the FIRST move on a new target; a target that was followed within the grace needs 1 |
| settle_ms | 220 | a frame captured less than this after the end of the motion, or before the last nudge started, is dropped (see below) |
| fast_pct / speed_fast | 25 / 2 | an offset beyond 25 % of the frame uses speed divisor 2 (~500 steps/s) instead of the default 3 (~333) |
| lead_pct / lead_max | 70 / 500 | velocity lead: % of the predicted target travel added to the nudge, cap in steps (`track_lead_pct`, 0 = off) |
| tilt_enable | **0** | tilt tracking, opt-in (`track_tilt=1`) until the tilt direction (`track_invert_tilt`) is verified on the camera |
| lost_grace_s | 3 | no person for this long = lost; nothing is decided before (`track_grace_s`) |
| return_home / return_delay_s | 0 (stay) / 30 | return policy, below |
| sticky_gate_pct | 35 | the previous target is kept if a box lies within 35 % of the frame width of it in WORLD position (the camera may have turned) |

`tl_cfg_legacy()` is the 0.1.0 policy (12 % band, 1.5 s, 300 steps, no lead, home after 10 s): the "before" of the simulation and of the old unit cases.

### Image offset -> motor steps (px per step, FOV)

The motor counts 0..~4000 steps over about 350 deg of pan (`ptz-findings.md`: stops at 0 and 3932..4151), i.e. 11.4 steps/deg. The lens is about 90 deg
wide (assumption from the lens class, not measured), so one frame width (320 px of the detection frame) is about 1000 steps: **3.1 steps per px**
(0.32 px per step). Tilt: 192 px at the same pixel pitch = 600 steps (about 54 deg). The step needed to centre a box at `c` px from the centre is
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
  measured on a settled frame, never integrated over a motion; the band (8 %) is larger than the detector jitter (a few px).
- Review fixes: a correction smaller than 20 steps is dropped AFTER the soft-range clip (a person 10 steps beyond a limit no longer loops tiny motions, scene timers and MQTT updates); the velocity samples are reset when the person is lost or the picked box is not the sticky match; a new box outside the sticky gate needs 2 ticks even inside the grace; a frame is also stale while the last nudge has no reported motion end (at most 2 s: queued, refused or not started); at most 2 direction reversals in 6 s, the third is held back and the tracker stays still for 3 s.
- Tilt (off by default): same mechanism with its own band; the vertical offset of the box centre is used. At 1-2 m the box fills the frame vertically and its centre stays near
  the middle, which is why tilt rarely moves in practice. `track_tilt=1` enables it. The tilt soft range is respected (the policy clips the nudge to it; it was
  clipped with the wrong sign before: image "below" is tilt DOWN = motor -).
- Speed: the nudge carries a speed divisor (2 for a large correction, 0 = the configured speed 3 otherwise) through `a2k_ptz_nudge_sp` ->
  `hal_ptz_nudge_sp` -> the safe-motion layer `ptzs_goto(speed)`, which refuses anything outside 2..20. No new ioctl, no new motor command: the same timed polled
  goto, soft range, stall and wrong-way checks. A nudge is accepted only while the controller is idle.

### Safety (unchanged)

Safe-motion layer only; soft range (clipped in the policy, in the tracking areas and in `hal_ptz_nudge`); never while parked, calibrating, unknown, privacy,
or the position is outside every enabled tracking area; rate limit 300 ms between motion commands (the motor itself runs one command at a time); a person
beyond the soft range just pins the camera at the limit without further commands.

## Return policy (what the camera does when nobody is there)

`track_return=home|stay` (default **stay**), `track_return_delay_s` (default 30), plus the lost-person grace `track_grace_s` (default 3).

- A person is *lost* after `track_grace_s` seconds without a box (detector blur and dropouts of up to that length change nothing).
- `stay`: nothing happens, the camera stays where the person was last seen, forever.
- `home`: `track_return_delay_s` seconds after the end of the grace the head goes home, once, only if tracking moved it. `0` = right after the grace.
  A person back before that cancels it; the delay restarts from the next loss.
- `akam2k.conf`: `track_return`, `track_return_delay_s`, `track_grace_s` (-> `--track-return`, `--track-return-delay-s`, `--track-grace-s`).
- Control API (live, persisted in `/config/akam/settings`): `track.return` (`stay|home`) and `track.return_delay_s` (0..86400) next to `track.enabled`.
  The grace is conf-only.
- 0.1.0 behaviour (home 10 s after the last box) = `track_return=home`, `track_grace_s=10`, `track_return_delay_s=0`.

Other conf keys: `track_band_pct` (2..40, default 8), `track_tilt` (0|1, default 0), `track_lead_pct` (0..100).

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
- Cost of the policy: with `track_return=stay` (the default) the head stays where the person was last seen. A power cut while it is away from home means a full calibration at the next
  cold boot (the home view does not match, so home cannot be claimed). `track_return=home` returns it home after the delay and avoids that.
- Travel budget: the steps travelled since the last calibration are counted (`ptz.travel_since_cal` in `/state`; an app start without a calibration counts as
  half the budget). Past `ptz_travel_budget_k` thousand steps (default 200, akam2k.conf, 10..99999) the home reference is no longer checked / rewritten in this run (RAM-only counter,
  lost with a power cut).
- A nudge is accepted by the PTZ controller only while it is idle: it never queues behind, replaces or outlives a step, home or park (no un-park after a
  park). While a command runs `a2k_ptz_nudge` returns -1 (the control API `ptz.move` too).

## API

- `tl_cfg_defaults(&cfg)`, `tl_cfg_legacy(&cfg)`, `tl_cfg_validate(&cfg)` (0 / -1), `tl_init(&t, &cfg)` (-1 on invalid cfg).
- `tl_update(&t, now_ms, boxes, n, frame_w, frame_h, &ptz, &out)`.
- Box: `struct tl_box {x1,y1,x2,y2}` pixels in the detector frame (same as `a2k_det_box`, 320x192 on the 2K; clamped
  by the detector). Up to 8 boxes; empty/inverted boxes are ignored. Call it every tick, with `n = 0` when nobody is seen.
- `struct tl_ptz {moving, pan, tilt, pan_min, pan_max, tilt_min, tilt_max, cap_ms, motion_end_ms}` (motor steps, soft range inclusive; the two times in the clock
  of `now_ms`, 0 = unknown).
- `out.kind`: `TL_NONE`, `TL_NUDGE` (`dpan`, `dtilt`, `speed`, already clipped to the soft range) or `TL_HOME` (caller goes home).
- Sign: `dpan > 0` = person right of centre, `dtilt > 0` = below centre. `invert_pan` / `invert_tilt` flip it for the motor.

## Unit tests

```
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o /tmp/rot2k_track_unit tests/rot2k_track_unit.c firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c \
    && /tmp/rot2k_track_unit
```

Expected: `track: all tests passed` (the 0.1.0 cases through `tl_cfg_legacy`, then the responsive policy: band, proportional step, fast speed, rate limit, stale
frames, tilt, lead, return policy). `tests/rot2k_track_wire_unit.c` (wiring on a fake motor) and `tests/rot2k_track_sim.c` (above) run from `tests/rot2k_overlay_test.sh`.

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
3. `track_return=stay` (default): leave the room, the head must stay put; `home` + `track_return_delay_s=30`: it returns ~33 s after the last sighting.
4. The speed divisor 2 (500 steps/s) was verified for pan only, in one run; check it with the tracker. Tilt direction is measured (above), try `track_tilt=1` with a live person.
