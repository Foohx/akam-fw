# Rotatable 2K person tracking: policy (track_logic)

`firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c/.h` is a pure policy (no I/O, no clock, no vendor call, integer
math): per detection tick (~5 Hz) it takes the person boxes and the PTZ state and returns at most one pan/tilt nudge, a
home request, or nothing. The wiring to the detector and the motor is `app/akam2k_track.c` (below).

## API

- `tl_cfg_defaults(&cfg)`, `tl_cfg_validate(&cfg)` (0 / -1), `tl_init(&t, &cfg)` (-1 on invalid cfg).
- `tl_update(&t, now_ms, boxes, n, frame_w, frame_h, &ptz, &out)`.
- Box: `struct tl_box {x1,y1,x2,y2}` pixels in the detector frame (same as `a2k_det_box`, 320x192 on the 2K; clamped
  by the detector). Up to 8 boxes; empty/inverted boxes are ignored. Call it every tick, with `n = 0` when nobody is seen.
- `struct tl_ptz {moving, pan, tilt, pan_min, pan_max, tilt_min, tilt_max}` (motor steps, soft range inclusive).
- `out.kind`: `TL_NONE`, `TL_NUDGE` (`dpan`, `dtilt`, already clipped to the soft range) or `TL_HOME` (caller goes home).
- Sign: `dpan > 0` = person right of centre, `dtilt > 0` = below centre. `invert_pan` / `invert_tilt` flip it for the motor.

## Policy and defaults

| field | default | meaning |
|---|---|---|
| deadband_pct | 12 | no move while the box centre is within 12% of width / height from the centre |
| gain_pan | 1000 | steps per frame width (4000 steps ~ 350 deg, ~90 deg HFOV) |
| gain_tilt | 600 | steps per frame height (~50 deg VFOV; an assumption, tune on the camera) |
| max_step | 300 | clamp per nudge and axis |
| min_interval_ms | 1500 | between two nudges |
| confirm_ticks | 2 | consecutive ticks with a target before the first move (a gap resets it) |
| lost_s | 10 | no person for this long: `TL_HOME`, once, only if a nudge was sent since the last home |
| home_enable | 1 | 0 disables the home request |
| sticky_iou_pct | 10 | the previous target is kept if a box overlaps it by at least this IoU, else the largest box |

Step = offset fraction x gain, clamped. Never a nudge while `ptz.moving`. The nudge is clipped so pan/tilt stay inside the
soft range; nothing is sent if the clip leaves 0 on both axes.

## Unit test

```
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o /tmp/rot2k_track_unit tests/rot2k_track_unit.c firmware/lsc-rotatable-2k-ak3918av130/app/track_logic.c \
    && /tmp/rot2k_track_unit
```

Expected: `track: all tests passed`.

## Wiring in the app (akam2k_track)

`app/akam2k_track.c` (core, motor through an ops table, host-tested on a fake motor by `tests/rot2k_track_wire_unit.c`) and `akam2k_track_run.c` (the real
binding). The detection thread calls `a2k_track_feed()` after every inference with the person boxes that count (not ignored, in a zone when zones apply;
`n = 0` when nobody), `TL_NUDGE` becomes `a2k_ptz_nudge(dx, dy)`, `TL_HOME` the home command.

- **Off by default.** `track=1` in `akam2k.conf` (`--track 1`) or the Home Assistant switch **Person tracking** (runtime only: a restart goes back to the key).
- **Signs.** `hal_ptz.h`: pan + = right, tilt + = up, and `a2k_ptz_nudge` takes right / up positive. A target right of the centre: `dx = +dpan`; a target below
  the centre (`dtilt > 0`): tilt DOWN, `dy = -dtilt`. `invert_pan` / `invert_tilt` stay 0 (to be checked on the camera).
- **Never** while the camera is parked, calibrating, its position unknown, the privacy mode hides the video, the motor is moving, or the position is outside
  every enabled tracking area. Leaving any of these forgets the target (no surprise home request later).
- Home after loss: `lost_s` (10 s) without a person, once, only after a nudge.

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

Tests: `tests/rot2k_track_wire_unit.c` (sign mapping, the "never" cases, home after loss, parse / format, overlap, enable, clip, corners, persistence).
