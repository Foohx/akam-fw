# Akam: specification, front LED and IR LED settings

Status: written 2026-11-10 from the user's decisions (branch `feature/led-controls`, from `dev`). English, like the code. The kernel LED interface was checked on a real camera before this spec (`echo 0 > brightness` turns the front LED off and the video is unaffected); the rest (auto-mode behaviour, IR GPIO policy) is covered by host tests and still to be seen on hardware (section 7).

## 1. Goal

Two independent settings, both saved on the camera and applied at start-up, set through the 8091 control port and the wizard command `./akam led`:

- **`led.status`**: what the front blue LED does. `on` | `off` | `auto`. **Default `on`**: the LED is left exactly as the system sets it today (the module does not even write to it).
- **`night.ir_led`**: whether the infrared LEDs may light at night. `auto` | `off`. **Default `auto`**: unchanged behaviour. `off` keeps the day/night switching (IR-cut filter, night ISP and colour mode) but never lights the IR LEDs.

There is a single LED and no colour: patterns are the only way to tell states apart.

## 2. Hardware

| Item | Interface |
|---|---|
| Front blue LED | kernel LED class device `/sys/class/leds/state_led`: `brightness` 0 or 1 (max 255, on/off only), `trigger` one of `none`, `timer`, `mmc0` |
| IR LEDs | GPIO 70, active high (`_ht_hw_settings.ini`: `ir_led_gpio=70`, `ir_led_level=1`), driven by `ircut.c` |
| IR-cut filter | GPIO 65 and 66 (`ircut_gpio_a`, `ircut_gpio_b_en`), driven by `ircut.c` |

The module writes `"none"` to `trigger` once (best effort, before its first write) so a timer or mmc trigger cannot fight it, then `"0"` or `"1"` to `brightness`. A write is made only when the level changes. A sysfs failure is logged once (`[leds] cannot write ...`) and retried quietly at the next change; the log line is armed again after a successful write.

## 3. `led.status`

| Value | Behaviour |
|---|---|
| `on` | Default. As long as the setting was never changed since boot the LED is not touched (system behaviour). After a change back to `on` the module writes 1. |
| `off` | LED written 0 at once, whatever else happens. |
| `auto` | See below. |

### `auto` state machine

Evaluated every 500 ms tick (the LED thread wakes twice a second in `auto`, four times less in `on`/`off`, and at once when a setting is changed through 8091). First match wins:

| Condition | Pattern | `led.state` |
|---|---|---|
| at least one RTSP client is playing | slow blink: 1 s on, 1 s off (privacy indicator) | `blink_stream` |
| camera still starting, or Wi-Fi not connected | blink: 500 ms on, 500 ms off | `blink_wifi` |
| otherwise | solid on | `on` |

- **Starting** lasts from the moment `ak_rtsp` starts until its RTSP server listens (`ledsys_boot_done()`).
- **Wi-Fi connected** means: `wlan0` has an IPv4 address (`SIOCGIFADDR`, one socket and one `ioctl` per tick) and `/sys/class/net/wlan0/operstate` is not `down`, `dormant`, `lowerlayerdown` or `notpresent` (a missing or odd file is accepted). Not verified on hardware whether the vendor driver reports `down` after the access point disappears; if it does not, a stale address keeps the LED solid until the interface is reconfigured.
- **Playing** is the RTSP PLAY state: `rtsp.c` counts a client from PLAY until TEARDOWN or until the connection ends, through `ledsys_stream_delta(+1/-1)` (a mutex-protected counter, taken only on these transitions; the streaming loop never waits for the LED thread). The counter never goes below 0.

## 4. `night.ir_led`

`ircut_switch(night)` (called by `night.c` on every day/night change) moves the IR-cut filter as before, then drives GPIO 70 to `night && allowed`. `allowed` is 1 for `auto` and 0 for `off`. Changing the setting while it is night lights or darkens the IR LEDs at once. A mutex orders the night thread and the control thread so the GPIO never stays lit against the policy. Applying `off` at start-up also drives GPIO 70 low if it was left exported and lit by an earlier run, before `ircut_init()`. `night.state` and `night.mode` are not affected.

## 5. 8091 keys

Same line protocol as the other keys (`LIST`, `GET`, `SET`). Spec 07 (standard protocols) makes port 8091 read-only for remote clients (writes only from loopback, or from a remote client while a RAM-only debug flag is on) and drops the planned HMAC; until that lands, 8091 has no authentication, like every other key. `./akam led` is unaffected: it sets the keys from the camera itself (`127.0.0.1`), which stays allowed.

| Key | Values | Writable | Notes |
|---|---|---|---|
| `led.status` | `on`, `off`, `auto` | yes | lower case only |
| `led.state` | `system`, `on`, `off`, `blink_wifi`, `blink_stream` | no | what the LED does now (`system` = not managed) |
| `night.ir_led` | `auto`, `off` | yes | |

`SET` answers `OK key=value`, or `ERR invalid led.status value X (want on|off|auto)` / `ERR invalid night.ir_led value X (want auto|off)` / `ERR led.state is read-only`, and changes nothing on error. If the settings file cannot be written the answer is `OK key=value (not saved: reason)`: the change is live but lost at the next boot. `LIST` shows the three keys.

## 6. Persistence

File `/etc/config/akam/leds` (directory 0700, file 0600), key=value lines, same method as the audio out file:

```
status=auto
ir_led=off
```

Written only when a value changes (a `SET` with the current value does not touch the flash), atomically (temporary file, `fsync`, `rename`), keeping lines it does not own. Read at start-up: an absent file, an unknown line or a bad value keeps the default. At start-up `ak_rtsp` applies the settings before it initialises the camera, so in `auto` the LED blinks during the whole boot of `ak_rtsp`.

## 7. Wizard command

`./akam led [show | status on|off|auto] [ir auto|off]` (default `show`). The wizard connects over SSH and, on the camera, sends `GET`/`SET` lines to `127.0.0.1:8091` with the camera's `nc` (the same method as `akam_hwcheck.sh`). Because the connection comes from loopback on the camera, it keeps working when 8091 becomes read-only for remote clients (spec 07, section 4). Words are validated on the PC before anything is sent; no confirmation is asked (nothing destructive, nothing secret). The 8091 port is single-client: the command briefly replaces any other connected client, for example the tuning tab. Messages are English.

## 8. Tests

- `tests/leds_test.sh` (native, AddressSanitizer and UBSan): state machine table, defaults untouched, 8091 get/set/list and validation, saved file (CRLF, garbage, foreign lines kept, modes, no temporary file, failed save reported), write only on change, sysfs failure logged once, built-in network check against a fake `operstate`, thread (blinks, stops at once, no network checks in `on`, bounded wakeups, concurrent counter), all against a fake sysfs directory.
- `tests/wizard_led_test.sh`: show, set, both words in any order, refusals that never reach the camera, old firmware, no answer, camera that cannot save, unreachable camera, dry run.

To check on hardware (not done): `SET led.status off` and `on` with video running; `auto` through a boot, a Wi-Fi loss (router off) and an RTSP session; `SET night.ir_led off` in the dark (filter clicks, image switches to night mode, IR LEDs stay dark); persistence across a power cycle; CPU use of the LED thread (should be negligible).
