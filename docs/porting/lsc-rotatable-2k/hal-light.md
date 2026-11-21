# hal_light on the Rotatable 2K

First portable module of the HAL pattern in [../../hal-architecture.md](../../hal-architecture.md): pure logic, a HAL interface with a
descriptor, a generic driver, and a per-model binding. It lives in `platforms/anyka-ak3918av130/` under generic file names
(no `akam2k_` prefix) so it can move to `src` unchanged in a later phase-B step. The 1080p keeps its own `leds.c`; nothing under
`src/` is touched.

| File | Layer | Role |
|---|---|---|
| `hal_light.h` | interface | `hal_light_emitter_t` `{name, kind GPIO/LEDCLASS, gpio, active_high, led_dir}`, `hal_light_desc_t` `{emitters, count, excl_a, excl_b}`, `hal_light_ops_t` `{int set(const emitter*, int on)}` |
| `light_logic.c/.h` | pure logic | resolve by name, `light_set()` off / on / blink-fast / blink-slow, `light_tick(now_ms)`, optional exclusion pair (unused on the 2K). Ops and time are injected: no I/O, no clock |
| `hal_light_sysfs.c` | generic driver | GPIO through `akam2k_pin` (whitelist, export, direction out, polarity here); LED class: `<led_dir>/trigger` = `none` once, then `<led_dir>/brightness` |
| `hal.c/.h` | 2K binding | `a2k_light_desc`, the only place the emitters are listed |
| `led.c` | PoC tool | `akam2k_led`, see below |

## 2K descriptor

From the stock map ([stock-firmware-reference.md](stock-firmware-reference.md), "IR light": `yy_cam` `[light]` section):

| Emitter | GPIO | Polarity | Stock key |
|---|---|---|---|
| `ir` | 48 | active high | `ir_no` 48, `ir_polarity` 1 |
| `blue` | 56 | active high | the front status LED, verified on the camera (on at boot) |

The stock `white` (49) and `alarm` (22) pins are NOT lights on this board: driving them stopped the sensor frames. They are not in the
descriptor and the pin whitelist of `pin.c` refuses them (and 55, which drives nothing). Whitelist: 24, 25 (IR-cut, `akam2k_ircut`,
later `hal_ircut`), 48 (`ir`), 56 (`blue`); every other pin is refused. No exclusion pair.

Blink timing: fast 1 Hz (500 ms on / 500 ms off), slow 0.5 Hz (1000 ms / 1000 ms). The stock's own `blink_fast_ms` 250 / `blink_slow_ms` 1500
are not reproduced on purpose.

## akam2k_led

```
akam2k_led list
akam2k_led [--force] [--secs N] <name> <on|off|blink-fast|blink-slow>
```

`on` / `off` set the light and exit (the pin keeps its level). `blink-*` stays in the foreground for N seconds (default 15, 1..60), then
turns the light off; SIGINT / SIGTERM / SIGALRM do the same. `ir` is refused (exit 2) unless `--force` is given, because the running app
owns the IR LED through day/night. `blue` is the status LED, which the running app owns while it runs. The IR-cut pins are unreachable (not emitters). Exit codes: 0 ok, 1 hardware
error, 2 usage or refused.

Built by `make poc3-dist` (symcheck, strip, `led_sha256` / `led_size` / `led_source_sha256` in `BUILDINFO.txt`). A manual tool, never
installed by `build_card.sh`; copy it to `/tmp` on the camera by hand.

## Open points (not verified on hardware)

- gpio56 is exported on demand by the driver (like pin 48), accepts EBUSY, and sets `direction` `out`.
  Writing `out` makes the line low for an instant before the requested level is written, so a light that is already on blinks off once when
  a new process claims its pin.

## Tests

`tests/rot2k_light_unit.c` (host, ASan / UBSan, in `tests/rot2k_overlay_test.sh`): fake driver and fake clock for on / off, both blink
rates, errors; the 2K descriptor (ir, blue) and the pin whitelist (22 / 49 / 55 refused); the sysfs driver on fake LED-class and GPIO directories. The overlay test also audits
the module (pure logic has no I/O, no pin number or path outside the descriptor and the pin helper, write-only `open` in the driver only, the
tool's `ir` guard and bounded blink) and compiles `akam2k_led` on the host.

## Status LED policy (`akam2k_rtsp`, the app owns `blue`)

Engine `leds.c/.h` (pure, host-tested: `tests/rot2k_led_unit.c`), thread `leds_run.c` (50 ms tick, 32 KiB stack). The LED
is only written when its level changes; while the wanted pattern is `on` and the engine never wrote the LED, it is left as the system set it
(default behaviour unchanged, no pin touched). On exit the engine puts the LED back on.

**Pattern**: `on` | `off` | `N:on_ms:off_ms:pause_ms`: N blinks (lit `on_ms`, dark `off_ms`), then `pause_ms` dark, repeat. Integers only
(digits and colons, no sign, no space), N 1..10, `on_ms` and `off_ms` 50..60000, `pause_ms` 0 or 50..60000, whole cycle
`N * (on_ms + off_ms) + pause_ms` <= 120000. `3:250:250:5000` = three 250 ms flashes, 5 s dark, repeat (6.5 s cycle).

**Rules**, first true wins; each has a pattern:

| akam2k.conf key | Rule | Default |
|---|---|---|
| `led_boot` | first 20 s after start, until the Wi-Fi has had an IPv4 address once | `1:100:100:0` |
| `led_nowifi` | no IPv4 address on `wlan0` (a non-multicast route exists in `/proc/net/route` only with an address) | `1:250:250:0` |
| `led_person` | person detection state on | `on` |
| `led_stream` | at least one RTSP client in PLAY (viewing or an NVR recording) | `on` |
| `led_night` | night mode active (day/night engine) | `on` |
| `led_idle` | none of the above | `on` |

**Master key** `led=on|off|auto` (default `on`, today's behaviour): `on` / `off` force the LED, `auto` evaluates the rules. The rule keys
only act with `led=auto`. A bad value is logged by `own_app_run.sh` (`akam2k.conf line N ignored`) and the default stays; the shell only
lets `on`, `off` or digits-and-colons through, the app validates the numbers and keeps the default for a bad pattern.

**Home Assistant** (MQTT, `mqtt_cfg.c`, all `entity_category: config`, command topic `BASE/<id>/set`, state `BASE/<id>/state`; the
discovery documents are retained, states are not, like every state of the 2K app): `select` Status LED `status_led` (on / off / auto),
`select` IR LED `ir_led` (auto / off), and one `text` per rule: `led_boot`, `led_nowifi`, `led_person`, `led_stream`, `led_night`,
`led_idle`. The app subscribes to the single filter `BASE/+/set`; only these eight topics are obeyed, a payload must be an exact option or
`[0-9a-z:]` text of at most 31 bytes, at most 20 commands per 10 s are applied. An invalid pattern keeps the old one and the real value is
published back. `ir_led` off: the IR LEDs never light, the IR-cut filter and the night image still switch (the same as `ir_led=off` in
akam2k.conf). **Runtime only**: nothing is written to the card or the flash (the 2K app has no persistence), a restart returns to the
akam2k.conf values.

## Hardware result 2026-11-15 - gpio49 and gpio22 are not lights on this board

Driving them (stock [light] defaults white/alarm) stopped the sensor frames (vi get frame failed) until a reboot. They are out of the descriptor
and refused by the pin whitelist.

## Hardware result 2026-11-15 - the front status LED is gpio56 (blue)

gpio56: export, out, 1 = blue LED on, 0 = off (active high), user confirmed; no effect on video. gpio55 (stock default red) drives nothing visible on this unit (released back to input). The 2K lights are therefore: ir = gpio48 (owned by day/night), blue status LED = gpio56. gpio49/22 must not be driven.

## Hardware result 2026-11-15 - the bottom button is gpio71 (active low)

Read-only sampling (export as input, 100 ms poll) while the user pressed the bottom button: gpio71 goes 1 -> 0 while pressed, back to 1 on release (short presses 0.2-0.3 s seen). gpio50 (stock "reset" key) never changed. Both left exported as inputs (harmless).

## Hardware result 2026-11-15 - pattern engine

New app run from RAM with `--led auto --led-idle off --led-stream 3:250:250:5000`: LED off at idle, 3 blinks then 5 s dark in a loop while an RTSP client played (20 s), off again after; user confirmed by eye. Rebooted back to the flashed build.
