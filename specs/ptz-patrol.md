# PTZ patrol / scan engine

A generic, programmable pan/tilt patrol: named presets, routes (lists of timed steps), generated sweep and raster
patterns, an idle trigger, and clear priorities against person tracking and manual control. The engine is pure logic
plus a thin binding, following `docs/hal-architecture.md` (logic, interface, per-model binding), so another PTZ camera
reuses it by providing its own limits and motor calls.

Status: **host-tested only** (never run on a camera). The patrol is **off by default**: nothing moves until a route is
started through the API or `ptz.patrol.idle_start_s` is set.

## 1. Layers

| Layer | File (2K app dir for now, candidate for `src/features`) | Role |
|---|---|---|
| Pure logic | `ptz_patrol.c/.h` | Data model, validation, pattern expansion, text and JSON forms, the run state machine. No I/O, no clock, no threads, no hardware. |
| Core binding | `patrol.c/.h` | Thread, locking, persistence, API handler, hooks. Talks to the motor only through an ops table (`a2k_patrol_ops_t`). |
| Model binding | `patrol_run.c` | Fills the ops table with the 2K controller (`ptz.h`), the event ring and the store. |

The patrol never calls a driver. Every move goes through the PTZ controller (`ptz.c`), that is through the safe
motion layer (soft range, polled moves, stall detection, cancel).

## 2. Data model

Limits come from the model descriptor (`pp_limits_t`): soft range `pan_min..pan_max`, `tilt_min..tilt_max`, allowed
speed divisors `speed_min..speed_max` (2..20 on the 2K) and the default speed. Everything is in motor steps (`pan +` is
right, `tilt +` is up), the numbering of `hal_ptz.h`.

Names: `[a-z][a-z0-9_]{0,15}` (same rule as the zones). Presets and routes have separate namespaces.

| Object | Fields | Limits |
|---|---|---|
| preset | `name`, `pan`, `tilt` | up to 16; inside the soft range |
| step | target: `preset` (a preset name) or explicit `pan` + `tilt`; `speed`; `dwell_ms` | `speed` 0 (default) or `speed_min..speed_max` (divisor: one step per `speed` ms, so a **smaller number is faster**; 3 is about 333 steps/s, 20 about 50 steps/s); `dwell_ms` 0..3600000 |
| route | `name`, `mode` (`once`, `loop`, `pingpong`), 1..32 steps | up to 8 routes |

A step that names a preset follows the preset: the position is resolved when the route starts. A preset that a route
references cannot be deleted (409 `in_use`).

Generated patterns are accepted in place of `steps` and are **expanded into plain steps when the route is stored**
(the stored route is always a step list; the pattern parameters are not kept):

| `pattern.type` | Parameters | Expansion |
|---|---|---|
| `sweep_h` | `pan_from`, `pan_to`, `tilt`, `step`, `speed`, `dwell_ms` | pan from `pan_from` to `pan_to` in increments of `step` steps (the last step lands exactly on `pan_to`), tilt fixed |
| `sweep_v` | `tilt_from`, `tilt_to`, `pan`, `step`, `speed`, `dwell_ms` | the same on the tilt axis |
| `raster` | `pan_from`, `pan_to`, `tilt_from`, `tilt_to`, `rows`, `cols` (default 2), `speed`, `dwell_ms` | `rows` evenly spaced tilt rows, each with `cols` evenly spaced pan stops, the pan direction alternating row by row |

An expansion that gives more than 32 steps is refused (`too_many_steps`). `step` must be at least 1 and the range must
not be empty. `mode` applies to the expanded route (`pingpong` suits a sweep, `loop` a raster).

Run rules (`mode`): `once` stops after the last step (`reason: done`); `loop` goes back to step 0; `pingpong` reverses at
both ends (the end steps are visited once per pass). Each step: move to the target at its speed, then wait `dwell_ms`
after the arrival. First-release bounds on unattended motion: the dwell is floored at 1500 ms for `loop` and `pingpong` (250 ms for `once`);
the patrol speed is never faster than the default (on the 2K `speed_min` = 3, the descriptor's 2 is for manual moves); a step whose target is
within 5 steps of the known position is reached without a move; and a patrol stops with reason `travel` once `a2k_ptz_travel()` passes
`ptz.patrol.travel_budget` (default 200000 steps, the controller's travel budget; settable 1000..2000000000), so the next cold boot
recalibrates; it does not start again until then.

The park position (2K: pan 1275, tilt 300, +-15 steps) is never a target (the head would count as parked and the privacy mode would
withhold the video): a preset, route step or generated step near it is refused with 422 `range`, a stored one is refused at start, and the
controller refuses a patrol move there.

Defaults of the settings rows: `ptz.patrol.idle_start_s = 0` (off), `ptz.patrol.route_default = ""`.

## 3. Triggers

- **manual**: `POST /actions/ptz.patrol.start {"route": "<name>"}` and `ptz.patrol.stop`. Starting while another route runs
  replaces it (`stopped` with `reason: replaced`, then `started`).
- **idle**: with `ptz.patrol.idle_start_s = N > 0` and a `ptz.patrol.route_default` that names a route, the route starts
  after N seconds without a person detection (the tracker feeds it, tracking enabled or not) and without a manual PTZ
  command, provided the PTZ is ready (state `home` or `moved`, privacy off). A stop, a finished `once` route and a manual
  command restart the idle clock. The clock only runs while the PTZ is ready: it is held while the PTZ is calibrating, unknown, parked, private
  or still saving its home scene reference, and a value under 30 s counts as 30 s. Set `idle_start_s` to 0 to disable. A route that does not exist is not started (logged once).

## 4. Priorities

Highest first:

1. **Shutdown** cancels the patrol (`reason: shutdown`) before the motor stops.
2. **Privacy, park, calibration** stop the patrol: the `ptz.park` command or button, privacy becoming active, a
   recalibration, a lost or unknown position (`reason`: `park`, `privacy`, `calibration`, `state`). It does not restart by
   itself except through the idle trigger.
3. **Manual PTZ command** (step, home, goto preset, move, MQTT buttons, front button) stops the patrol (`reason: manual`).
4. **Person tracking** (only a person the tracker has CONFIRMED and acts on: tracking enabled, allowed at the position, target held for the confirm streak, fresh frame): that person **pauses** the
   patrol (`interrupted`), cancelling the running move at once. The patrol resumes (`resumed`) **at the interrupted step**
   (it moves back to that step and starts its dwell again) once no person has been seen for the tracking return time:
   `track lost grace + track return delay`. While a patrol is running or paused the tracker does not send the head home
   (the resume replaces it); with no patrol the tracker behaves as before.
5. **Patrol moves** never preempt anything: a move is submitted only when the controller is idle; otherwise it is retried
   on the next tick (100 ms). A patrol move never fires the manual-command hook.

## 5. Safety

- Every target is clamped to the soft range at run time (config input outside the range is refused at the API, so what is
  stored is what runs); every speed is validated against the descriptor (a speed outside it is clamped at run time).
- Three failed moves in a row (refused, stall, I/O) stop the patrol (`reason: move_failed`).
- The patrol uses only `a2k_ptz_patrol_goto` (absolute move through the safe layer); the position stays in RAM.
- The controller state is checked every tick: unknown, calibrating or privacy stops the patrol.

## 6. Flash policy

- While patrolling nothing is written: the run state is RAM only.
- Presets and routes are one file, `/config/akam/ptz_patrol` (atomic replace through `a2k_store`, skipped when the content is
  unchanged), written once per accepted change and counted in `sys.flash_writes_patrol` and `sys.flash_writes_store`.
- The whole file must fit 4096 bytes (the store limit): about 150 steps in all, refused with 413 `too_big` beyond.
- Config writes are rate limited: a burst of 8, then one per 10 s (429 `rate`).
- `ptz.patrol.idle_start_s` and `ptz.patrol.route_default` are ordinary settings rows (saved by the settings worker with its
  own limiter).
- Text format (LF lines): `p <name> <pan> <tilt>`; `r <name> <mode>` followed by its `s <preset-name|pan:tilt> <speed> <dwell_ms>`
  lines. A file that does not parse is ignored (logged) and the config starts empty; nothing is rewritten at boot.

## 7. Control API

All under `/akam/v1`, signed like every endpoint. Reads need a read key, changes a write key.

| Request | Scope | Result |
|---|---|---|
| `GET /ptz/presets` | read | `{"presets":[{"name","pan","tilt"}],"max":16}` |
| `GET /ptz/presets/<name>` | read | one preset, 404 `not_found` |
| `POST /ptz/presets` | write | body `{"name","pan","tilt"}` or `{"name","here":true}` (current position). Creates or replaces. `{"ok":true,"created":bool,...}` |
| `POST /ptz/presets/<name>` | write | same with the name from the path |
| `DELETE /ptz/presets/<name>` | write | 404, 409 `in_use` |
| `GET /ptz/routes` | read | `{"routes":[{"name","mode","steps","running"}],"max":8,"default":""}` |
| `GET /ptz/routes/<name>` | read | the full route: `{"name","mode","steps":[{"preset"\|"pan","tilt","speed","dwell_ms"}]}` |
| `POST /ptz/routes` | write | body: a route (`steps` or `pattern`). All or nothing. Creates or replaces. |
| `DELETE /ptz/routes/<name>` | write | 404, 409 `running`, 409 `in_use` (it is the default route) |

Errors are `{"error":code,"detail":text}`: 400 `bad_json`, 404 `not_found`, 409 `in_use`/`running`/`full`, 413 `too_big`,
422 `bad_name` `range` `speed` `dwell` `bad_mode` `no_preset` `too_many_steps` `empty` `bad_pattern`, 429 `rate`. A route being
replaced while it runs is refused (409 `running`).

Actions (`POST /actions/<name>`, write scope): `ptz.patrol.start {"route"}`, `ptz.patrol.stop`, `ptz.goto_preset {"name"}`
(a manual move: stops a running patrol). Unavailable motor or unknown route: 409 `unavailable` / 404 `not_found`.

Settings rows: `ptz.patrol.idle_start_s` (0..86400), `ptz.patrol.route_default` (a route name or empty), `ptz.patrol.travel_budget` (steps). State rows (read only):
`ptz.patrol.state` (`idle`, `running`, `paused`), `ptz.patrol.route` (empty when idle), `ptz.patrol.step` (-1 when idle),
`sys.flash_writes_patrol`. Patrol events have a ring of their own (8 entries) so they never evict button, person or night events. The resume delay after a person is floored at 3 s.

Events (`GET /events`, type `patrol`, data `{"event":...,"route":...,"step":N[,"reason":...]}`): `started`, `step` (a step
reached; at most one per second), `interrupted`, `resumed`, `stopped` (with `reason`).

## 8. Reuse on another model

Provide: the soft range and speed limits (`pp_limits_t`), an absolute move at a speed with a completion result, a
cancel, the controller state and the persistence. `ptz_patrol.c` has no other dependency.

## 9. Tests

`tests/rot2k_ptz_patrol_unit.c` (pure logic, timing sim), `tests/rot2k_patrol_wire_unit.c` (core with a fake motor,
tracker preemption, flash policy), controller integration in `tests/rot2k_hal_ptz_unit.c`, and the HTTP checks in
`tests/rot2k_control_api_http_test.sh`.
