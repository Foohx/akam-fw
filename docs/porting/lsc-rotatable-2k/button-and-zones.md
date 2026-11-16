# Rotatable 2K: front button (hal_input) and runtime image zones

Both are in the own app `akam2k_rtsp`. Nothing here was run on a camera yet (host-tested with fake gpio, clock and motor).

## The front button

**Hardware (verified):** stock `[key]` gpio 71 (50 is the other key of the stock map and is NOT used), an INPUT, **active low**: reads `1` released, `0`
pressed. See `stock-firmware-reference.md`.

**Layers (docs/hal-architecture.md).**

- `app/hal_input.c/.h` portable: descriptor `{name, gpio, active_low, debounce_ms}`, the debouncer, the driver interface (`setup`, `read`). The
  generic sysfs driver is `app/hal_input_sysfs.c`; the 2K descriptor is in `akam2k_hal.c` (`button`, 71, active low, 50 ms).
- `app/button_logic.c/.h` portable and pure: the press policy (below), host-tested.
- `app/akam2k_button.c`: the thread (20 ms poll, 32 KiB stack), the actions.
- `app/akam2k_pin.c` stays the ONLY file that touches the GPIO subsystem. Pin 71 is an **input-only** pin: `a2k_pin_allowed(71)` is still 0, the output
  whitelist stays 24 / 25 / 48 / 56, `a2k_pin_open_in(71)` writes `in` to the direction file and opens the value file O_RDONLY, nothing is ever written to it.

**Policy (release decides).**

| press | action |
|---|---|
| < 1 s | short press: Home Assistant `event` entity "Button" (`event_type` `press`) + blue LED double blink (`2:150:150:0`, 0.7 s) |
| 1 s .. `button_long_ms` | nothing (dead zone) |
| >= `button_long_ms` (default 2 s) | long press: `a2k_ptz_toggle_park()` (parked -> home, anything else -> park). The LED blinks fast (`1:100:100:0`) from 500 ms of hold ("keep holding"); at the threshold it shows a double blink then stays solid until the release ("release now") |

- `button_long_ms` (`akam2k.conf`, 1500..10000, default 2000) is `--button-long-ms`; it is always above the 1 s short limit.
- During the PTZ calibration a press does nothing (no event, no motion); the LED cues are still given.
- A button already held when the app starts is ignored until released.
- The LED cue is a transient override of the status LED engine (`a2k_leds_blip`): it replaces the rules for its duration, then the rules come back. With the
  master LED mode `off` there is no cue (a dark LED stays dark).
- The HA entity is the MQTT `event` platform: discovery `homeassistant/event/<node>/button/config`, state topic `BASE/button/event`, payload
  `{"event_type":"press"}`, not retained. It needs a Home Assistant that has the MQTT event platform (2023.8 or later). On an older one the same topic can still be
  used by hand as an MQTT trigger (a device_trigger discovery is not published). Presses wait for the broker at most 4 deep; a dead broker drops them.

## Runtime image zones and ignore zones

The detection zones of `own-app-feasibility.md` ("Detection zones") are now editable at run time, and a second kind exists.

**Format** (the card file `akam/zones`, extended; the runtime file has the same syntax):

```
porch=0,40,50,100        a detection zone (percent of the 320x192 detector frame), up to 8
zone_mode=any            any | all
ignore_door=10,10,30,60  an IGNORE rectangle (same syntax, up to 8; names unique among the ignores)
sky=0,0,100,20@screen    optional anchor suffix on any zone or ignore rectangle: @screen | @position (see "When they apply")
```

A person whose **feet point** (bottom centre of its box) lies in an ignore rectangle is dropped before anything else: no presence, no zone sensor, no
tracking. Zone names starting with `ignore_` are reserved. The file holds at most 1024 bytes.

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

The two RTSP streams (0 = main 2304x1296 H.264 Main, 1 = sub 640x360 H.264 Baseline) each have seven settings. The **defaults are unchanged** (what the app always ran:
15 fps, VBR, QP 32..51 main / 32..50 sub, target `main_kbps` 2000 / `sub_kbps` 512, peak 1.25 x the target, GOP `gop` 30). The stock firmware uses AVBR 630/800 kbit/s on the
main stream (PLAN.md "Point 4"); `video0_rc=avbr` with `video0_kbps=630` reproduces it.

| setting | `akam2k.conf` key | Home Assistant entity (`number` / `select`, category config) | range main / sub |
|---|---|---|---|
| target bit rate | `video0_kbps` / `video1_kbps` (win over `main_kbps` / `sub_kbps`) | `video0_kbps` "Video main bit rate" | 64..8000 / 32..1000 kbit/s |
| peak bit rate | `video0_max_kbps` / `video1_max_kbps` | `video0_max_kbps` | 0 = automatic (1.25 x target), else 64..12000 / 32..1500; never below the target |
| frame rate | `video0_fps` / `video1_fps` | `video0_fps` | 1..15 (the sensor runs at 15) |
| min QP, max QP | `video<N>_min_qp`, `video<N>_max_qp` | `video<N>_min_qp`, `video<N>_max_qp` | 10..51, min <= max (higher QP = coarser picture) |
| GOP | `video<N>_gop` (wins over `gop`) | `video<N>_gop` | 1..300 frames |
| rate control | `video<N>_rc` | `video<N>_rc` select | `vbr` (default), `cbr`, `avbr` (the vendor also has constant QP and low latency, not offered) |

14 entities in all (`video0_*`, `video1_*`), with the same command topic `BASE/<id>/set` / state topic `BASE/<id>/state` pattern as the other controls: numbers are plain
digits (anything else is refused), the select takes exactly one option, retained commands are ignored, the rate limit is shared. A set that is inconsistent (min QP
above max QP, peak below the target) is refused whole and the real value is published back. `akam2k.conf` values are validated twice, by the supervisor
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
the file; an empty file is valid). Source order at start: built-in defaults, `akam2k.conf`, then the runtime file (it wins; an invalid file is ignored as a whole and logged).
If the encoder refuses the runtime values at boot they are forgotten (file emptied) and the conf / default values are retried once, so a bad value never causes a boot loop.
Delete `/config/akam/video` to go back to `akam2k.conf`. Not validated on hardware: QP below 32, CBR / AVBR on the main stream and fps below 15 (the open_ex parameter checks of the
vendor lib are only partly known; a refusal is handled as above).
