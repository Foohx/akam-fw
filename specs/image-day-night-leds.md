# Image, day/night and LEDs

This file covers what shapes the picture and the lights: the ISP configuration and the picture sliders, the auto
exposure (AE), the day/night state machine with the IR-cut filter and IR LEDs, the image-quality switches, and the
front status LED policy.

Status: **hardware-validated** for the day/night state machine (including the night probe and the IR-back-on logic,
checked with a covered lens), the front LED (`on`, `off`, `auto`) and the IR LED `auto` mode. **Validated in RAM on one
camera**: picture-setting persistence across a daemon restart and a forced night-to-day switch. **Not yet hardware
checked**: strict `SET` ranges of `ae.*` and `night.*`, behaviour over a whole dusk and dawn cycle, `img.*` debug keys
beyond the first comparisons.

## 1. ISP configuration

- The ISP blocks (30 blocks from 24 modules of the sensor file `/tmp/sensor_isp.conf`, which has a day sub-file and a
  night sub-file) are sent unmodified, exactly as the stock application sends them. A comparison of a stock capture
  with `ak_rtsp` found no block-level difference at start-up or after a day/night switch, except the white-balance block
  (next paragraph).
- **White balance (`img.wb`).** `conf` (default): keep the file's WB block, as stock. `manual`: additionally send a
  hand-made manual WB block (type and three gains, rest zero) after the file's block, and re-apply it after every
  night-to-day reload. `conf` was chosen after a side-by-side comparison on a camera (better colours).
- **Gain limits.** The exposure block limits the sensor gain to 1.0x and the ISP gain to 1.5x (day) or 3.0x (night). The
  longest exposure is the frame time. `ak_rtsp` keeps those limits.
- All ISP register writes that touch more than one ioctl run under `isp_cfg_lock()`, the same lock the night thread
  holds for a whole day/night reload. Order of locks: control `SET` lock, then ISP lock; the night thread never takes the
  `SET` lock.

## 2. Picture sliders

| Key | Range | Neutral | Saved |
|---|---|---|---|
| `isp.brightness`, `isp.contrast`, `isp.saturation`, `isp.sharpness` | -50 to 50 (integer) | 0 | `/etc/config/akam/picture` |

- The value is a delta from the hardware baseline captured at the first call. `0` leaves the file's block unchanged.
- Saved file: four lines `brightness=`, `contrast=`, `saturation=`, `sharpness=`; written after every successful
  loopback `SET` of any of the four; atomic; all four at 0 removes the file. A remote `SET` (debug mode) changes the
  picture in RAM only.
- Applied at every start after the ISP runs (`control_apply_saved_isp`). A value the ISP refuses is logged and the others
  still apply.
- The same four settings are exposed through ONVIF Imaging ([onvif.md](onvif.md)), MQTT ([mqtt.md](mqtt.md)) and the
  installer's `picture` command.

## 3. Auto exposure

`ae.c` polls the ISP exposure state about every 100 ms and adjusts the exposure block.

| Key | Range | Default | Meaning |
|---|---|---|---|
| `ae.stable_range` | 0 to 100 | 4 | Dead-band (exposure block word 9) |
| `ae.hold_range` | 0 to 100 | 4 | Hold range (word 10) |
| `ae.speed` | 0 to 100 | 7 | Convergence aggressiveness (word 11) |
| `ae.exp_max` | 0 to 32767 | 2 | Shortest-exposure word (word 1). The name is historical; the stock ISP value is 2 |
| `ae.enabled` | 0 or 1 | 1 | 0 freezes the exposure (AE tick skipped) |

The stock file values are 4 / 5 (night 6) / 16; Akam's 4 / 4 / 7 came from a fix for exposure hunting. `ae.*` values live
in RAM only; they are lost at restart. Night mode re-applies the AE tuning after its reload.

## 4. Day/night state machine

`night_logic.c` is a pure state machine (no ISP, no GPIO, no clock); `night.c` feeds it one AE sample per poll
(100 ms, `NL_POLL_MS`) and performs the action it returns. Time is counted in polls, so a bad clock cannot shorten the
lock. States: `day`, `night`, `night-probe` (reported as `night.state=night` while probing).

### Modes

`night.mode` is `auto` (default), `day` or `night`. A forced mode applies once per change and then pins the state; a
change of mode resets the state machine. A forced `day` or `night` is saved in `/etc/config/akam/night`
(`mode=day|night`; `auto` removes the file) by a loopback `SET`, and applied at start before the monitor thread runs, so
a forced mode never flips at boot. A remote `SET` is RAM only.

### Day to night

Only when the AE is saturated for `night.confirm_samples` consecutive polls (default 30 = 3 s):

- exposure `hw_exp >= night.trigger_hw_exp` (default 15500, 95 % of the 16384 ceiling), and
- total gain (sensor gain x ISP gain) `>= night.gain_pct` % (default 95) of the maximum the AE is allowed, read at run
  time from the exposure block.

A dim but usable room never saturates: the AE reaches its target before the ceiling.

### Night to day

With the IR LEDs on the camera lights its own scene, so exposure says nothing about the room. Instead:

1. After the minimum night dwell `night.min_night_ms` (default 120000 ms; doubled up to x8 when the camera falls back to
   night within 10 minutes of a day switch), the IR LEDs are switched off for a few seconds (a probe) while the IR-cut
   filter stays in the night position, and the AE is read in ambient light.
2. Dark room: exposure climbs back above `night.day_hw_exp` (default 2048, 12 % of the ceiling) within about 2 s. The
   probe ends at once, the LEDs come back on, and the next periodic probe waits `night.probe_ms` (default 300000 ms,
   minimum 10000), doubling after each failure (up to x8).
3. Lit room: exposure stays below `night.day_hw_exp` for 10 samples after `night.probe_settle_ms` (default 4000 ms): the
   camera goes to day (the LEDs are already off; no flash).
4. The first probe starts as soon as the minimum dwell is over; later ones every `night.probe_ms`, or at once when a lamp
   is suspected (exposure below half of its slow average for 3 s, latched until the dwell is over).
5. With `night.ir_led=off` there is no feedback loop: exposure is read directly and the return to day needs
   `night.day_confirm_samples` (default 50) samples below the threshold. No probe runs.

An anti-flap lock `night.lock_ms` (default 60000) follows every switch. Every switch and probe is logged with the values
that triggered it.

### Effect of a switch

`ircut_switch(night)` (called by `night.c`) moves the IR-cut filter, reloads the day or night ISP sub-file and sets the
frame rate (night always 10 fps, VTS 3426; day uses the configured rate). The sequence follows stock: IR LED GPIO first,
then the ISP blocks, then the IR-cut pulse.

### Keys

| Key | Access | Range | Default | Saved |
|---|---|---|---|---|
| `night.mode` | rw | `auto`, `day`, `night` | `auto` | loopback SET |
| `night.state` | ro | `day`, `night` | | |
| `night.trigger_hw_exp` | rw | 0 to 16384 | 15500 | no |
| `night.day_hw_exp` | rw | 0 to 16384 | 2048 | no |
| `night.confirm_samples` | rw | 1 to 36000 | 30 | no |
| `night.lock_ms` | rw | 0 to 3600000 | 60000 | no |
| `night.gain_pct` | rw | 1 to 100 | 95 | no |
| `night.day_confirm_samples` | rw | 1 to 36000 | 50 | no |
| `night.min_night_ms` | rw | 0 to 86400000 | 120000 | no |
| `night.probe_ms` | rw | 10000 to 86400000 | 300000 | no |
| `night.probe_settle_ms` | rw | 500 to 60000 | 4000 | no |
| `night.ae` | ro | | | live `lum`, `hw_exp`, gains, limits, `saturated=0\|1` |
| `night.logic` | ro | | | mode, confirmations, lock, probes, failures, next probe, last decision |

Tuning keys other than `night.mode` live in RAM only and return to the defaults at restart.

## 5. IR-cut filter and IR LEDs

| Item | Interface |
|---|---|
| IR-cut filter | GPIO 65 (`ircut_gpio_a`) and GPIO 66 (`ircut_gpio_b_en`), dual-coil solenoid, a 100 ms pulse, reset pulse once per boot (the two-click sound) |
| IR LEDs | GPIO 70, active high |

`night.ir_led` is `auto` (default: the IR LEDs light in night mode) or `off` (the filter and the night image still
switch, the LEDs never light). Changing it while it is night lights or darkens the LEDs at once. A mutex orders the night
thread and the control thread so the GPIO never stays lit against the policy. At start `off` also drives GPIO 70 low if an
earlier run left it exported and lit. `night.state` and `night.mode` are not affected.

## 6. Status LED

| Item | Interface |
|---|---|
| Front blue LED | `/sys/class/leds/state_led`: `brightness` 0 or 1 (on/off only), `trigger` one of `none`, `timer`, `mmc0` |

The module writes `none` to `trigger` once (best effort) before its first write, then `0` or `1` to `brightness`. It
writes only when the level changes. A sysfs failure is logged once and retried quietly at the next change.

`led.status` is `on` (default), `off` or `auto`:

| Value | Behaviour |
|---|---|
| `on` | The LED is left as the system sets it; the module does not write to it until the setting was changed once since boot. After a change back to `on` it writes 1 |
| `off` | Written 0 at once |
| `auto` | State machine below |

`auto`, evaluated every 500 ms (the LED thread wakes at once on a setting change); first match wins:

| Condition | Pattern | `led.state` |
|---|---|---|
| At least one RTSP client is playing | Slow blink, 1 s on and 1 s off (privacy indicator) | `blink_stream` |
| Camera still starting, or Wi-Fi not connected | Blink, 500 ms on and 500 ms off | `blink_wifi` |
| Otherwise | Solid on | `on` |

- "Starting" lasts from the start of `ak_rtsp` until the RTSP server listens.
- "Wi-Fi connected" means `wlan0` has an IPv4 address and `/sys/class/net/wlan0/operstate` is not `down`, `dormant`,
  `lowerlayerdown` or `notpresent` (a missing or odd file is accepted).
- "Playing" counts a client from PLAY until TEARDOWN or the end of the connection, through a counter updated only on
  those transitions; the streaming loop never waits for the LED thread.
- `led.state` reports `system` (not managed), `on`, `off`, `blink_wifi` or `blink_stream`; it is read-only.

**Saved file** `/etc/config/akam/leds` (directory 0700, file 0600), written atomically and only when a value changes;
lines it does not own are kept:

```
status=auto
ir_led=off
```

An absent file, an unknown line or a bad value keeps the default. Both settings are applied before the camera is
initialised, so in `auto` the LED blinks during the whole start of `ak_rtsp`. A `SET` answers `OK key=value`, or
`OK key=value (not saved: reason)` when the file cannot be written (the change is live, but lost at the next boot).

## 7. Image-quality switches (debug)

For A/B tests against the stock firmware. Options come, in order, from the environment, then `/tmp/akam_image` (RAM),
then `/etc/config/akam/image` (saved), as `key=value` lines with `#` comments.

| Switch | Values | Default | Effect |
|---|---|---|---|
| `venc_rc` (`AKAM_VENC_RC`) | `legacy`, `stock` | `legacy` | `CREATE_ENC` rate-control sub-struct: `legacy` keeps the values `ak_rtsp` always sent; `stock` sends exactly what the stock application sends. `stock` has not been tried without risk: other rate-control words crashed the kernel twice. Keep it in `/tmp/akam_image` (a crash then self-heals at reboot), never in the saved file, until a hardware test has passed |
| `wb` (`AKAM_IMG_WB`) | `conf`, `manual` | `conf` | See section 1 |
| `poll` (`AKAM_IMG_POLL`) | 0, 1 | 0 | 1 polls the 3D-NR statistics (about 1.2 Hz) and sensor fps (about 2 Hz) like stock |

Keys (read with `GET`, written with `SET` from loopback or in debug mode):

| Key | Access | Meaning |
|---|---|---|
| `img.venc_rc` | ro | Active choice (fixed once the encoder exists) |
| `img.wb` | rw | `conf` or `manual`, applies at once (needs the ISP pipeline to be initialised: otherwise `ERR ISP not initialised yet`). A loopback `SET` that changes the value is saved to the `image` file (the `wb=` line is replaced in place) |
| `img.poll` | rw | 0 or 1, applies at once, not saved |
| `img.reload` | wo | `1`: re-send the whole file for the current mode, reset picture baselines, re-apply the AE tuning |
| `img.dump` | wo | `confirmed` or `all`: read the live ISP blocks, log one line per block against the file's day and night sub-files, write `/tmp/isp_live/isp_blocks.bin` |

`tools/hw_tests/image_noise.py` (temporal and spatial noise of two RTSP captures) and `tools/hw_tests/isp_block_diff.py`
(block-by-block diff against a stock capture) support the comparisons.

Open question, never proven: the remaining grain difference against stock. Candidates are the encoder rate-control
sub-struct and sharpening that stock re-applies after a day/night switch.

## 8. Tests

`tests/night_logic_test.sh`, `leds_test.sh`, `isp_tune_test.sh`, `isp_blocks_test.sh`, `isp_effects_test.sh`,
`isp_block_diff_test.sh`, `imgq_test.sh`, `image_noise_test.sh`, `control_test.sh`, `wizard_led_test.sh`,
`wizard_picture_test.sh`.
