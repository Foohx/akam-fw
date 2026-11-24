# Rotatable 2K: front button (hal_input) and runtime image zones

Both are in the own app `akamd`. Nothing here was run on a camera yet (host-tested with fake gpio, clock and motor).

## The front button

**Hardware (verified):** stock `[key]` gpio 71 (50 is the other key of the stock map and is NOT used), an INPUT, **active low**: reads `1` released, `0`
pressed. See `stock-firmware-reference.md`.

**Layers (docs/hal-architecture.md).**

- `src/hal/hal_input.c` and `src/hal/hal_input.h` portable: descriptor `{name, gpio, active_low, debounce_ms}`, the debouncer, the driver interface (`setup`, `read`). The
  generic sysfs driver is `firmware/lsc-rotatable-2k-ak3918av130/hal_input_sysfs.c`; the 2K descriptor is in `hal.c` (`button`, 71, active low, 50 ms).
- `src/features/button/button_gesture.c`, `button_cfg.c`, `button_logic.c`, `button_rt.c` (below): the gesture engine, the bindings, the policy and the runtime.
- `firmware/lsc-rotatable-2k-ak3918av130/button.c`: the thread (20 ms poll, 32 KiB stack), the real actions.
- `firmware/lsc-rotatable-2k-ak3918av130/pin.c` stays the ONLY file that touches the GPIO subsystem. Pin 71 is an **input-only** pin: `a2k_pin_allowed(71)` is still 0, the output
  whitelist stays 24 / 25 / 48 / 56, `a2k_pin_open_in(71)` writes `in` to the direction file and opens the value file O_RDONLY, nothing is ever written to it.

**Programmable.** Every press pattern is a gesture bound to up to four whitelisted actions; the factory behaviour is just the default binding. Layers:
`button_gesture.c/.h` (pure: the gesture engine), `button_cfg.c/.h` (pure: bindings, action whitelist, modes, dispatch through an ops table),
`button_logic.c/.h` (pure: debounce + gesture engine + LED cues), `button_rt.c/.h` (the live config, its persistence, the HTTP handlers, the glue to the
API event ring and MQTT; no hardware) and `button.c` (the thread and the real actions). All host-tested by `tests/rot2k_button_unit.c` and, through the
API, by `tests/rot2k_control_api_http_test.sh`.

**Gestures.** Each press is classified by how long it was held; a gesture is a sequence of up to 6 presses:

| element | held |
|---|---|
| `S` short | < `short_max_ms` (default 1 s) |
| `L` long | >= `long_ms` (default 2 s; `button_long_ms` of `akam.conf` seeds it) |
| `V` very long | >= `very_long_ms` (default 5 s); only when some binding contains a `V`, otherwise such a hold is just a long press |
| between `short_max_ms` and `long_ms` | dead zone: the whole interaction is dropped, nothing happens |

Names: `short` = `S`, `double` = `S S`, `triple` = `S S S`, `long` = `L`, `very_long` = `V`; anything else is its tokens joined by spaces (`S S L`, up to 6).
A gesture ends when the next press does not come within `gap_ms` (default 400 ms) of a release. **Exactly one gesture is reported per interaction**, and the engine
only waits when waiting can matter (longest match): a press is reported at its release, with no delay, unless a longer bound gesture starts with it (bind `double`
and a lone short press is reported `gap_ms` later). A sequence that is not bound and cannot grow (`S L` with only `S S L` bound) is reported at once as an unbound
gesture; at 6 elements the sequence ends. Timing, all configurable and validated together: `debounce_ms` 10..200 (default 50), `short_max_ms` 200..1500 (1000),
`gap_ms` 150..1000 (400), `long_ms` 1500..10000 (2000, above `short_max_ms`), `very_long_ms` long+500..30000 (5000).

**Bindings.** Up to 16 `{gesture -> [actions]}`, up to 4 actions each, run in order (a failing action does not stop the next ones). Only this whitelist exists:

| action | effect |
|---|---|
| `none` | nothing (the gesture is still an event) |
| `ptz.home`, `ptz.park`, `ptz.recalibrate` | like the API / MQTT buttons |
| `ptz.toggle_park` | parked -> home, anything else -> park (the factory long press) |
| `ptz.privacy on\|off\|toggle` | on = park, off = home, toggle = `ptz.toggle_park`. Privacy in this app IS the parked head: the video is withheld while parked only with `ptz_privacy=1`; there is no other run-time privacy switch |
| `ptz.goto_preset NAME` | a patrol preset (a manual move: it stops a patrol) |
| `track on\|off\|toggle` | person tracking, through the registry (`track.enabled`: same setter, same persistence as over the API) |
| `track.lost_policy VALUE` | the `track.lost_policy` setting (the tracker validates the value; refused at run time on a build without that row) |
| `patrol start ROUTE`, `patrol stop` | the PTZ patrol |
| `led pattern P` | the status LED shows pattern P for 3 s: `on`, `off`, `N:on_ms:off_ms:pause_ms` or a name (`ack` `blink` `fast` `slow`) |
| `mode NAME` | apply a named mode |

**Nothing destructive can be bound**: no reset, no key or network change, no reboot; the parser accepts exactly this list (a unit test feeds it the lookalikes and
the chained forms).

**Modes.** A mode is a named set of up to 8 `key=value` settings (at most 8 modes), applied through the settings registry as ONE patch: every pair is validated
before any is applied (a bad value or a failed cross-field rule applies nothing), and if a setter then fails the ones already applied are put back. Only rows that
are writable, not secret, applied live and shown over HTTP can be in a mode, and never `net.*` / `api.*`. The same mode is applied by the action
`mode.apply {"name":".."}`.

**Every gesture, bound or not,** emits the API event `button`
`{"gesture":"double","actions":["mode night","ptz.home"],"result":"ok"}` (`result`: `ok`, `partial`, `failed`, `unbound`; a long action list is cut with
`"more":N` to fit the 144-byte event) and an MQTT publish of the same text on `BASE/button` (not retained), and sets the state `button.last_gesture`. While the PTZ
calibration runs nothing is dispatched or published (the LED cues only). The LED confirmation blink (`led_ack`, default on) is the double blink `2:150:150:0` after
a bound gesture that ends with a short press; the hold cues are unchanged (fast blink from 500 ms, confirm double blink at `long_ms`, triple blink at
`very_long_ms` when a very long gesture is bound, then solid until the release).

**Defaults reproduce the factory button exactly** (the unit test replays the old policy on them): `short -> none` (the event and the double blink, no delay) and
`long -> ptz.toggle_park`. With only these two bound nothing waits, so every press is reported at its release as before.

**API** ([control-api.md](control-api.md), `specs/control-api.md` 4.2): `GET /akam/v1/button` (read) and `POST /akam/v1/button` (write) take and return the whole
config `{"timing":{..},"led_ack":true,"bindings":[{"gesture":"double","actions":["mode night","ptz.home"]}]}`, validated as a whole, nothing applied on any error
(400 `syntax`, 422 `value` `limit` `no_such_mode`, 413 `too_big`); `GET /modes`, `GET|POST|DELETE /modes/<name>` (`POST` body `{"settings":{"led.mode":"off"}}`;
409 `in_use` for a mode a binding names). **Persistence:** `/config/akam/button` and `/config/akam/modes` (JSON), written only by a POST / DELETE that really
changes them (atomic through the store, an identical text is not written, a burst of 8 then one per 10 s, else 429 `rate`), counted in `sys.flash_writes_button`.
Pressing the button never writes. An invalid file is ignored as a whole (the defaults) and left alone.

**Home Assistant** (MQTT stays opt-in, the ctl group): the `event` entity "Button" (`event_type` `press`, `BASE/button/event`, discovery
`homeassistant/event/<node>/button/config`, Home Assistant 2023.8 or later) is still published for a `short` gesture, and every gesture goes to `BASE/button`. One
**device trigger** is announced per configured gesture (`homeassistant/device_automation/<node>/button_<k>/config`: `automation_type` `trigger`, `type` the gesture
in lower case with `_` for spaces, `subtype` `button`, `topic` `BASE/button`, `value_template` `{{ value_json.gesture }}`, `payload` the gesture name). A slot
without a binding is cleared with an empty retained config; a change of the bindings makes the MQTT thread announce again.

**Other behaviour.**

- During the PTZ calibration a press does nothing (no event, no motion); the LED cues are still given.
- A button already held when the app starts is ignored until released.
- The LED cue is a transient override of the status LED engine (`a2k_leds_blip`): it replaces the rules for its duration, then the rules come back. With the
  master LED mode `off` there is no cue (a dark LED stays dark).
- `button_long_ms` (`akam.conf`, 1500..10000, default 2000) is `--button-long-ms`; it only seeds the default config (a saved config has its own `long_ms`).
- Gesture messages wait for the broker at most 4 deep; a dead broker drops them.
- Cost (ARM uClibc `-Os`, stripped `akamd`): 360,552 -> 381,352 bytes (+20,800) for the gesture engine, bindings, modes, API handlers and MQTT triggers; about 30 KB of
  static RAM (live config, modes and the dispatch snapshot); no new thread. Not run on a camera yet (host-tested with a fake line, clock and actions).

## Runtime image zones and ignore zones

The detection zones of `archive/rot2k-own-app-feasibility.md` ("Detection zones") are now editable at run time, and a second kind exists.

**Format** (the card file `akam/zones`, extended; the runtime file has the same syntax):

```
porch=0,40,50,100        a detection zone (percent of the 320x192 detector frame), up to 8
zone_mode=any            any | all
ignore_door=10,10,30,60  an IGNORE rectangle (same syntax, up to 8; names unique among the ignores)
sky=0,0,100,20@screen    optional anchor suffix on any zone or ignore rectangle: @screen | @position (see "When they apply")
```

A person whose **feet point** (bottom centre of its box) lies in an ignore rectangle is dropped before anything else: no presence, no zone sensor, no
tracking. Zone names starting with `ignore_` are reserved. The file holds at most 1024 bytes.

**Frame (flip / mirror; host-tested only, not verified on the camera).** The rectangles of the card file and of `/config/akam/zones` are in the **sensor frame** (the picture with `image.flip` and `image.mirror`
off), so a zone keeps covering the same part of the room when you turn the orientation on or off (`a2k_zones_orient`, isp-controls.md 3.3). The two Home Assistant
`text` entities show and accept the **displayed** frame, i.e. what the picture shows now: with the mirror on, a zone drawn at the left of the picture is typed with
x near 0 and stored mirrored. With the orientation off nothing changes. After an orientation change made over the API the text entities show the new values at the
next announcement (change them from Home Assistant, or restart the integration, to refresh at once).

**Source at start:** `/config/akam/zones` (jffs2, written by the edits below) if it exists and is valid (an EMPTY runtime file is valid and means "no zone": it
wins over the card), else the card `akam/zones`. Delete `/config/akam/zones` to go back to the card file.

**Home Assistant:** two `text` entities, **Detection zones** (`zones`: `porch=0,0,50,50;drive=50,0,100,100;zone_mode=all`) and **Ignore zones**
(`ignore_zones`: `door=10,10,30,60;sky=0,0,100,20`, names WITHOUT the prefix). Entries are separated by `;`, `none` or an empty text clears. At most 255
characters (a HA limit), characters `0-9 a-z _ = , ; : . @ -`. A text is validated as a whole: if it is invalid the old value stays and is published back. A valid text
is applied at once (the per-zone sensors `person_<name>` are announced again, the removed ones cleared) and stored atomically in `/config/akam/zones`
(skipped when unchanged: flash wear).

**When they apply: the anchor.** The rectangles are percent of the image, so a rectangle only means something where the camera looked when it was drawn,
unless it is meant to follow the image. Each zone and each ignore rectangle therefore takes an optional suffix after the four numbers:

| suffix | meaning |
|---|---|
| `@screen` | image-relative: ALWAYS active, whatever the camera position (a sky strip to ignore, a timestamp corner, a "centre of the image" zone while tracking moves the camera) |
| `@position` | active only at the position it belongs to: the camera at HOME (ptz state `home`), or its position not managed (`unknown`, `ptz_cal=off`: the motor is never moved); SUSPENDED while `moved`, `parked` or `calibrating` |
| no suffix | exactly `@position` (the behaviour before anchors existed; the text keeps no suffix so it round-trips) |

A suspended zone never matches and its `person_<name>` sensor reads off (the zone indexes do not shift); a suspended ignore rectangle ignores nothing. When no
zone is in force (only position zones, camera moved) the whole image counts, as without zones. `zone_mode=all` then requires a person in every zone IN FORCE only.
Examples: `zones` = `door=0,40,50,100;path=50,40,100,100@screen`; `ignore_zones` = `sky=0,0,100,20@screen;lamp=70,10,80,30`. A bad or repeated suffix makes the
whole text (or file) invalid. The position the zone "belongs to" is home only: zones tied to other named positions are not supported.

## Video settings (encoder, per stream)

The two RTSP streams (0 = main 2304x1296 H.264 Main, 1 = sub 640x360 H.264 Main, Baseline if the encoder refuses Main) each have seven settings. The **defaults are unchanged** (what the app always ran:
15 fps, VBR, QP 32..51 main / 26..44 sub, target `main_kbps` 2000 / `sub_kbps` 800, peak 1.25 x the target, GOP `gop` 30). The stock firmware uses AVBR 630/800 kbit/s on the
main stream (archive/rot2k-porting-plan.md "Point 4"); `video0_rc=avbr` with `video0_kbps=630` reproduces it.

| setting | `akam.conf` key | Home Assistant entity (`number` / `select`, category config) | range main / sub |
|---|---|---|---|
| target bit rate | `video0_kbps` / `video1_kbps` (win over `main_kbps` / `sub_kbps`) | `video0_kbps` "Video main bit rate" | 64..8000 / 32..1000 kbit/s |
| peak bit rate | `video0_max_kbps` / `video1_max_kbps` | `video0_max_kbps` | 0 = automatic (1.25 x target), else 64..12000 / 32..1500; never below the target |
| frame rate | `video0_fps` / `video1_fps` | `video0_fps` | 1..15 (the sensor runs at 15) |
| min QP, max QP | `video<N>_min_qp`, `video<N>_max_qp` | `video<N>_min_qp`, `video<N>_max_qp` | 10..51, min <= max (higher QP = coarser picture) |
| GOP | `video<N>_gop` (wins over `gop`) | `video<N>_gop` | 1..300 frames |
| rate control | `video<N>_rc` | `video<N>_rc` select | `vbr` (default), `cbr`, `avbr` (the vendor also has constant QP and low latency, not offered) |

14 entities in all (`video0_*`, `video1_*`), with the same command topic `BASE/<id>/set` / state topic `BASE/<id>/state` pattern as the other controls: numbers are plain
digits (anything else is refused), the select takes exactly one option, retained commands are ignored, the rate limit is shared. A set that is inconsistent (min QP
above max QP, peak below the target) is refused whole and the real value is published back. `akam.conf` values are validated twice, by the supervisor
(`own_app_run.sh`: digits only, per-stream ranges, rc exact word, a bad line is logged and ignored) and by the app (an inconsistent stream keeps its defaults).

**Live or restart?** The stock `yy_cam` never calls an `ak_venc_set_*` function; `libmpi_venc` exports `ak_venc_set_fps`, `set_qp`, `set_rc_param`, `set_rc_type` but their
signatures are unknown and nothing here may be called on guess without the camera. So **no parameter is applied live**: every change (bit rate, peak, fps, QP, GOP, rc)
closes and re-opens THAT stream's encoder (`ak_venc_close` + `open_ex` + keyframe request) in its own capture thread. Expected effect: that stream has no new frames for about
1 to 2 s (VI frames queue and are dropped, the clients stay connected and the picture resumes on a keyframe); the other stream, the detection channel and the JPEG snapshot
encoder are not touched. A burst of changes is one re-open. The frame rate is additionally enforced by the capture thread (it encodes every 15/fps-th VI frame), so a rate
below 15 also lowers the load. If the re-open fails the previous settings come back (and are re-opened), the entity shows the previous value; only if that fails too the capture
thread gives up like any dead channel (the supervisor restarts the app). Candidates for a later live path (to test on the camera): `ak_venc_set_rc_param` /
`set_qp` / `set_fps`.

**Persistence:** runtime changes are stored atomically in `/config/akam/video` (jffs2), as the keys that differ from the conf-derived base (a value set back to the base leaves
the file; an empty file is valid). Source order at start: built-in defaults, `akam.conf`, then the runtime file (it wins; an invalid file is ignored as a whole and logged).
If the encoder refuses the runtime values at boot they are forgotten (file emptied) and the conf / default values are retried once, so a bad value never causes a boot loop.
Delete `/config/akam/video` to go back to `akam.conf`. Not validated on hardware: QP below 32, CBR / AVBR on the main stream and fps below 15 (the open_ex parameter checks of the
vendor lib are only partly known; a refusal is handled as above).
