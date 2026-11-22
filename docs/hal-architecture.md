# Akam HAL architecture — portable modules + per-model hardware layer

Goal: one Akam firmware whose **feature modules are model-agnostic** and whose **per-model specifics (GPIOs,
device nodes, vendor libs) live in a thin descriptor + driver**. Adding a camera = fill a descriptor and provide
the vendor glue, not rewrite the features.

This file sets the convention. It is derived from the two patterns already in the tree that work well:
`src/features/night/night_logic.c` (a pure policy engine fed samples, reused by both models) and `platforms/anyka-ak3918/leds.c`
(an engine with an injectable `cfg` + `ops`).

## The three layers

1. **Pure logic (portable, no I/O).** State machines and policy: night/day, LED behaviour, detection
   hysteresis, MQTT/HA entity model, metrics formatting. Pure functions + small structs, host-testable without a
   camera. Lives in shared code (`src/`). Already true for `night_logic.c`.

2. **HAL interfaces (portable contracts).** Small C interfaces the logic calls to touch hardware, each a struct
   of function pointers + a model-filled descriptor. No GPIO number or device path appears in layer 1 or in the
   interface — only in the descriptor a model fills:
   - `hal_light`  — LEDs/IR: a list of named emitters, each `{how, gpio|led_dir, active_high}`; set on/off/blink.
   - `hal_ircut`  — IR-cut filter: `{p_gpio, m_gpio, pulse_ms}`; go day/night.
   - `hal_video`/`hal_isp` — streams, encoder, ISP day/night, WDR, image settings.
   - `hal_ptz`    — pan/tilt (2K only): device node, limits, direction map. The patrol / scan engine on top of it (`ptz_patrol.c`,
     presets, routes, sweeps, priorities with tracking) is pure logic with its own ops table: see `specs/ptz-patrol.md`.
   - `hal_audio`  — mic/speaker: device nodes, rate, gain.
   - `hal_sys`    — metrics: CPU/load/mem/uptime/temp/wifi from `/proc` (generic; one implementation fits all).

3. **Per-model binding.** One file per model that provides the descriptors and wires the drivers:
   `firmware/<model>/hal.c` plus the descriptors next to it (the 2K: `hal.c`, `ptz_desc.c`, `pin.c`). The 1080p binding stays as it is today (its facts are
   still inside `platforms/anyka-ak3918/`: `ircut.c`, `ledsys.c`) until it is migrated module by module (phase B); the 2K binding consolidates onto the shared modules.

## Where each layer lives in the tree

| Layer | Directory | Content |
|---|---|---|
| 1 pure logic and protocol code | `src/features/<x>/`, `src/net/{rtsp,onvif,mqtt,http,crypto}/`, `src/api/`, `src/core/` | state machines (night, light, button, PTZ patrol, tracking, scene), RTSP/ONVIF/MQTT/HTTP cores, the signed API, config, logs, registry, control gate, version |
| 2 HAL interfaces and generic drivers | `src/hal/` (`hal_light.h`, `hal_input.*`, `hal_ptz.h`, `hal_sys.*`) | the contracts and the drivers that only need sysfs |
| 3 hardware access of one SoC family | `platforms/<soc-family>/` | the raw-ioctl drivers (`anyka-ak3918`) or the vendor-library glue (`anyka-ak3918av130`), the process, the Makefile and toolchain pins |
| 3 descriptors of one camera | `firmware/<model-id>/` | pins, motor descriptor, settings rows, API binding, service table, `model.mk` (features and ordered object list), boot scripts, image builders |

`src/build/sources.mk` names every shared module once (module -> directory); `src/build/features.mk` gates modules per model. A shared file may include only
shared headers: a file that needs a platform or model header stays in `platforms/` or `firmware/` until its hardware part is split off.

## The module pattern (how every module is written)

```
logic:   <feature>_logic.c/.h   pure: nl_step()-style, takes samples/cfg, returns actions; host-tested
hal:     hal_<feature>.h         struct of ops + a descriptor struct; no paths/pins
driver:  generic + per-model     e.g. a generic sysfs-gpio driver reused by every model via its descriptor
bind:    <model>_hal.c           fills the descriptor, picks the driver, hands them to the logic
```

A model that drives a LED over raw GPIO and a model that drives a kernel LED-class device use the **same logic
and the same `hal_light` interface** — only the descriptor and the driver function differ.

## No-regression rule (unchanged, §8ter)

- `git diff dev -- src/` stays empty for 1080p-shared files, OR a change is proven byte-identical for the 1080p
  binary **and** validated on 1080p hardware. Since no 1080p hardware is on hand now, **new shared modules are
  added without editing the 1080p's current files**: the 1080p keeps `leds.c` etc.; the 2K adopts the new
  portable module; the 1080p converges later as a dedicated phase-B step with the byte-identical proof.
- Every extraction: full 1080p host tests green, and the 2K change validated on the camera (now card-less).

## Feature selection (build time)

Portable modules are compiled into a model only if that model enables them, so no model ships another's dead code.
`src/build/features.mk` knows the features `API PTZ BUTTON SCENE PATROL TRACK SYS LIGHT ONVIF` (`WIFI5G` gated nothing and was removed). A model's
`model.mk` sets `AKAM_FEAT_<NAME> := 1` or `0` (unset = off; a typo or a value other than 0/1 is a build error) and adds the module's objects through
`$(call feat_objs,<NAME>,<objects>)`; `feat_on <NAME>` tests a feature. `make features` prints what is on and off. Both models use it: the 1080p
(`firmware/lsc-indoor-1080p-ak3918/model.mk`, `SRCS`) and the 2K (`firmware/lsc-rotatable-2k-ak3918av130/model.mk`, `POC3_ORDER` with `c:<name>` for a `src`
object). With the feature on the mechanism changes no binary (both golden hashes are identical with it). What a 0 really does: on the 1080p `ONVIF=0` drops
every ONVIF object and links `onvif_off.c` (the answers of an ONVIF that is off); on the 2K `PTZ`, `TRACK`, `SCENE` and `PATROL` drop their objects (and
the shared modules only they use) and link `firmware/<2k>/<feature>_off.c`, whose stand-ins answer "not available" and never move a motor or write a file
(`tests/feature_switch_test.sh` links each variant). `ONVIF=0` does not link on the 2K yet (its `main.c` calls the ONVIF server). The 2K gates: `API`
(`api_events`, `settings_reg`, the security files), `BUTTON` (`hal_input`, `button_logic`, `button_gesture`, `button_cfg`), `SYS` (`hal_sys`), `LIGHT`
(`light_logic`, `led_pat`; `hal_light.h` is its interface), `ONVIF`, and the four above. The sysfs drivers `hal_light_sysfs.c` and `hal_input_sysfs.c` stay in the
model directory because they call the model pin helper; the common code includes no model header (enforced by `tools/ci/layering_lint.sh`).

Code shared by both platforms beyond the cores: the Annex B NAL splitter (`src/net/rtsp/h264_nalu.c`), the RTCP Sender Report and interleaved-frame writer
(`src/net/rtsp/rtcp_sr.c`), the ONVIF handlers whose answer is the same on every camera (`src/net/onvif/onvif_common.c`: missing profile or configuration
faults, refused set/create/delete, empty audio lists, discovery mode), `mqtt_connack_reason` (`src/net/mqtt/mqtt.c`) and the AKPCM capture ioctl numbers
(`src/hal/akpcm.h`). The RTSP session loops (UDP and TCP on the 1080p, TCP only on the 2K), the camera-specific ONVIF answers (profiles, stream URI,
capabilities, device information) and the MQTT receive loops stay per platform.

## First module: `hal_light` (the worked example)

- **Descriptor** (`hal_light_desc`): an array of emitters `{ name, kind (GPIO|LEDCLASS), gpio, active_high,
  led_dir }`, e.g. the 2K: `ir`=gpio48 active-high, `white`=gpio49 active-high, `alarm`=gpio22 active-high.
- **Driver**: a generic sysfs driver — GPIO (`/sys/class/gpio/gpioN/value` with export + direction, polarity
  applied) and LED-class (`<led_dir>/brightness`, `trigger`=none once). One driver, selected per emitter kind.
- **Logic** (`light_logic.c`): set a named emitter to on/off/blink(fast|slow); optional "status LED auto" policy
  (starting/no-wifi = 1 Hz, client playing = 0.5 Hz, else solid) mirroring the 1080p `leds.c`; mutual exclusion
  rules (white vs IR) as the stock did.
- **2K binding**: `hal.c` provides the descriptor; the app exposes control (first over a tiny mechanism to
  prove it, then through MQTT/HA and the control port in later increments).
- **Test**: a host unit test of `light_logic` (fake driver), plus a camera check — toggle `white`/`alarm` and see
  it light, IR follows night.

Later modules (`hal_sys` metrics, `hal_ircut`, image settings, PTZ, audio) follow the same pattern. Once a few
are in, the 1080p migrates onto them in phase B.
