# Hardware test checklists

Tickable checklists for people who test Akam on real hardware. Tick a box only after you have seen the result on
your own camera. What was already tested, and where: [testing status](testing-status.md).

Check ids are unique: `API-` (2K control API), `O2K-` and `O1080-` (picture orientation), `L2K-` (2K latency), `SPK-`, `TALK-` and `PIC-` (2K
speaker, talk and picture), `M` (MQTT), `B` and `C` (1080p remote control), `R` (rename). Camera checks are on this page; the checks of the
`akam` program per operating system are in [assistant checks](hardware-checks-assistant.md).

Send results as an issue or a pull request. Remove Wi-Fi names, passwords, MAC addresses and real IP addresses from
anything you post. **Never attach a backup, a dump or an image.**

## LSC Indoor 1080p: camera checklist (acceptance)

| # | Criterion | How to check | Done |
|---|---|---|---|
| 1 | Hardware reference confirmed (`3215672.3`, SI `D26228`) | Label; `/proc/mtd`; the wizard's hardware verdict ([backup guide](manual/backup-uart.md)) | [x]|
| 2 | 8 partitions backed up with SHA-256 | `akam backup`, or the [backup guide](manual/backup-uart.md) checklist | [x]|
| 3 | Boots without a microSD | Remove the card after provisioning, power cycle, `ssh` in, `akam_hwcheck.sh` | [x]|
| 4 | Wi-Fi connects automatically | Camera appears on the network after power-on; `wpa_supplicant` and `udhcpc` PASS in `akam_hwcheck.sh` | [x]|
| 5 | H.264 1080p stream | `ffprobe rtsp://<camera-ip>:554/` and a viewer, 1920x1080 | [x]|
| 6 | Microphone audible (the L16 track exists; listening still to do) | Listen to the stream audio | [ ] |
| 7 | Night mode works | Cover the lens / darken the room: IR-cut switches, image stays usable | [x]|
| 8 | Port 8091 works (read-only from the network) | **Old image only, ticked before the port was closed:** `nc <camera-ip> 8091` `GET` from the PC worked and `SET` was refused. The current behaviour (8091 on 127.0.0.1 only, the `control.lan` opt-in) is checks C1 and C7 below | [x] old image |
| 9 | Dropbear starts by itself | `dropbear` PASS and port 22 PASS in `akam_hwcheck.sh`; `/tmp/akam_ssh.log` | [x]|
| 10 | SSH root by key | `ssh -i ~/.ssh/akam root@<camera-ip>`; a password attempt must fail | [x]|
| 11 | Persists after a power cut | Cut power, reconnect: same key accepted, same host fingerprint | [x]|
| 12 | Telnet unreachable and never re-enabled (gone at boot ; the Wi-Fi reconnect case is still to check) | Ports 23 and 24 closed in `akam_hwcheck.sh` after boot, `--boot-window`, and again after a Wi-Fi reconnect; `nmap` from the PC | [ ] |
| 13 | FTP off | Port 21 closed (same checks) | [x]|
| 14 | No Internet dependency | Put the camera on a segment with no route out; everything above still works | [ ] |
| 15 | Memory acceptable | Free memory and RSS in `akam_hwcheck.sh` (section 4) | [x]|
| 16 | Image fits in APP | `build_report.txt` free bytes, and a successful flash | [x]|
| 17 | Restore documented and (your decision) rehearsed | [Flash and restore](manual/flash-and-restore.md) | [ ] documented, not rehearsed |


### MQTT (optional, needs a camera and a broker)

Not run on a camera yet. Use a test broker (see [MQTT and Home Assistant](mqtt.md#5-try-it-without-a-broker-a-temporary-mosquitto-on-your-computer)) or Home
Assistant with its Mosquitto add-on.

| # | Check | How | Done |
|---|---|---|---|
| M1 | Off by default: no thread, no socket | Fresh image, no MQTT file: `akam_hwcheck.sh` says `MQTT is off: akamd holds no outbound TCP connection`; `for p in $(pidof akamd); do grep -qa akam_klog /proc/$p/cmdline || ls /proc/$p/task; done` (pidof also lists the kernel log filter) has the same number of threads as an image without MQTT (record both) | [ ] |
| M2 | `akam mqtt set` then `mqtt test` | Wrong password: test says refused; right password: test says ok; `mqtt status` shows `state: connected` | [ ] |
| M3 | Home Assistant entities | One device `akam-XXXX` with all entities of [the table](mqtt.md#3-what-home-assistant-gets); the MAC merges it with the ONVIF device if both are on | [ ] |
| M4 | Controls work both ways | Each select, number, switch and button in Home Assistant changes the real LED, IR LEDs, night mode, volume (hear it), picture (see it), talk switch, beep; a change made with `akam led status off` shows up in Home Assistant within about 2 s | [ ] |
| M5 | Sensors plausible | Compare CPU load with `top`, Wi-Fi signal with `iwconfig` or `akam status`, frame rate and bitrate with `akam video show`; temperature present or absent as the camera reports | [ ] |
| M6 | Availability | Kill `akamd` on the camera (the wrapper restarts it) or unplug it: the device shows unavailable within about 90 s (last will), then available again | [ ] |
| M7 | Broker restart | Stop and start the broker: the camera reconnects (delay 5 s growing to 60 s) and publishes discovery again | [ ] |
| M8 | One outbound connection, video untouched | `akam_hwcheck.sh` says `MQTT is on: akamd holds one outbound TCP connection`; frame rate (`video.fps_actual`) and bitrate unchanged compared with MQTT off, over a 10 minute stream; free memory and `akamd` VmRSS recorded with MQTT off and on (budget: the delta must stay under 100 KB) | [ ] |
| M9 | Hostile input | `mosquitto_pub` to `BASE/volume/set` with `101`, `-1`, `5;reboot`, an empty and a 10 KB payload, to `BASE/reboot/set`, `BASE/uptime/set` and a retained `BASE/beep/set PRESS`: nothing changes, the camera stays connected | [ ] |
| M10 | Broker down at boot | Enable MQTT, power cycle with the broker stopped: the stream and ONVIF come up as usual, MQTT state is `error:connect`, and it connects once the broker is back | [ ] |


### Rotatable 2K control API (port 8092, own app; needs the camera and SSH)

Host tests (`tests/rot2k_control_api_*_test.sh`) cover the whole API. On the camera, signed requests were used (settings changes, `ptz.recalibrate`, state reads, and on 2026-11-23 the speaker and latency settings written with a curl-based signed helper, on the image that runs now); none of the rows below is ticked yet. Reference: [control-api.md](porting/lsc-rotatable-2k/control-api.md).

| # | Check | How | Done |
|---|---|---|---|
| API-1 | Closed without a key | Fresh `/config` (no `api_keys`): `ss -ltn` (or `netstat -ltn`) shows no listener on 8092; the log says `no API key installed` | [ ] |
| API-2 | First key opens the port | `akam_api_key.sh new --scope admin --name t` over SSH: key id and secret printed once; port 8092 listens within ~2 s without a restart; `ls -l /config/akam/api_keys` is `-rw-------` | [ ] |
| API-3 | Signed calls | `tools/akam-api-sign.sh` + curl from a PC: `/hello` unsigned, `/info`, `/state`, `/schema` signed answer 200; a wrong secret is 401 `bad_signature`; the same request twice is 401 `replay` | [ ] |
| API-4 | Settings take effect | PATCH `led.mode` off/on (LED), `night.ir_led`, `image.brightness` (see it), `video1.fps` (stream re-opens, other stream untouched), `track.enabled`; read back with GET; reboot by power cut and check `led.mode`/`track.enabled` persisted, `image.*` did not | [ ] |
| API-5 | Actions | `ptz.home`, `ptz.step`, `ptz.move`, `ptz.park`, `ptz.recalibrate` move the head; a second `ptz.move` within 150 ms is 429 | [ ] |
| API-6 | Events | `GET /events?since=N&timeout=25` held; press the front button: the `button` event arrives at once; walk in front of the camera: `person` present true then false; PTZ and day/night events appear | [ ] |
| API-7 | net.* | PATCH `net.hostname`: `/tmp/akam_net.req` disappears within ~5 s, `akam_net.sh` log says set, `GET /settings/net.hostname` shows it; `net.apply` goes queued, pending, ok; `net.wifi_band` 5 with country `none` is refused (422), else keeps the Wi-Fi up (dead-man); do not change the band while the router restarts | [ ] |
| API-8 | Cost | `a2k_mem_report` (log) VmRSS/MemAvailable before the first key, with a key and idle, and after a 1 minute burst of requests (gate: +64 KB RSS, +40 KB binary; the binary is +53 KB, see the spec); `video.fps_actual` and CPU unchanged under a flood of bad requests (they are rejected before any crypto once locked) | [ ] |
| API-9 | Hostile input | `curl --data` of 9 KB, a 5 KB header, chunked upload, `GET /akam/v1/../x`: 413, 431, 411, 400/404; nothing crashes (`akam.log` has no CRASH line) | [ ] |
| API-10 | Fail closed | `chmod 644 /config/akam/api_keys`: the API closes (`keys_perm`); `chmod 600` and it reopens; `echo x >> /config/akam/api_keys`: closes (`keys_invalid`) and the file is kept | [ ] |
| PARK-1 | Long press: closed, then back | Long press the button: the lens is fully hidden in the base (not looking down out of it), no grinding or clicking, the video stops (`ptz_privacy=1`); long press again: back to the home view. If the lens is not hidden, try `ptz_park 1275:60` | [ ] |
| PARK-2 | The closed pose is not against the stop | Close the camera ten times: smooth moves, no skipping sound at the end; `ptz.stall_steps_est` in `GET /state` does not grow by a close | [ ] |
| PARK-3 | Boot after a close | Close the camera, power cycle: it boots closed with no head motion (log `the head was last left parked`, `ptz.cal_full` 0); a long press opens it at the home view | [ ] |
| PARK-4 | Motor skipping at boot | Cold boot with the head at home: does it calibrate? Read `ptz.stall_steps_est`, `ptz.cal_full`, `ptz.cal_short`; the default threshold is now 0.85 (`ptz.scene_min`, API or assistant; was 0.90): the boot that read `score 0.899, needs 0.900` should now say MATCH and skip the calibration; if it still calibrates, read the score in the log. Also try `ptz.scene_min` 0.95 then 0.85 and check the next boot line names the new threshold | [ ] |


### 2K picture orientation (flip, mirror, ceiling mount)

O2K-2 to O2K-4 were done on 2026-11-22; O2K-1 and O2K-5 to O2K-10 are open. Reference:
[ISP controls, sections 3.2 and 3.3](porting/lsc-rotatable-2k/isp-controls.md). Use a stream player on `/stream0` and `PATCH` the settings
with the signed API (or the Home Assistant switches **Image flip**, **Image mirror**, **Ceiling mount**).

| # | Check | How | Done |
|---|---|---|---|
| O2K-1 | Default is unchanged | Both off (`image.flip` 0, `image.mirror` 0): the picture is exactly the one of the earlier builds, colour correct; the log has no orientation call | [ ] |
| O2K-2 | `image.flip` is vertical | `image.flip` 1: the picture is upside down (top and bottom swapped, left and right unchanged); no stream restart; back to 0 restores it | [x] 2026-11-22 |
| O2K-3 | `image.mirror` is horizontal | `image.mirror` 1: left and right swapped, top and bottom unchanged; back to 0 restores it | [x] 2026-11-22 |
| O2K-4 | `(0, 0)` leaves the picture unchanged | After a 1 then 0 round trip of both switches, the picture is identical to O1 (same framing) | [x] 2026-11-22 |
| O2K-5 | Colour stays right | With flip, with mirror and with both on, skin and sky colours look natural (no swapped red and blue, no green or magenta cast) | [ ] |
| O2K-6 | Survives day/night | With `image.ceiling` 1, force `night.mode` night then day (and let it switch by itself once): the orientation is the same after each switch | [ ] |
| O2K-7 | Survives a restart | Set flip, power cycle: the orientation is still applied (it is saved only when you change it); the stream, the zones and the tracker agree with it | [ ] |
| O2K-8 | Ceiling mount, tracker | Camera mounted upside down (or the camera turned over on the desk) with `image.ceiling` 1: with `track.enabled` on, the head pans toward the person, not away, and tilts the right way with `track.tilt` on | [ ] |
| O2K-9 | Zones and PTZ follow | A zone drawn in the displayed picture covers the same part of the scene before and after a change of orientation; the ONVIF or API pan and tilt directions match the picture | [ ] |
| O2K-10 | Home Assistant | The three switches show the real state; `Ceiling mount` reads ON only while flip and mirror are both on | [ ] |

### 2K motion detection and the lost-target search (host-tested only, nothing here was run on a camera)

Settings: `motion.enabled` (default on), `motion.sensitivity` (1-10, 5), `track.search` (`motion` default, `legacy`), `ptz.scene_min` (0.80-0.99, 0.85); read-only rows `motion.active`, `motion.dir`, `motion.x`, `motion.y`, `motion.source`. [How it works](porting/lsc-rotatable-2k/tracking.md#motion-detection-and-the-motion-aware-search)

| # | Check | How | Done |
|---|---|---|---|
| MOT-1 | Motion row follows a hand | Wave in front of the camera (head still): `motion.active` 1, `motion.x` / `motion.y` where you are in the picture; still: 0 within a second. Note `sys.cpu_pct` with `motion.enabled` 1 and 0 | [ ] |
| MOT-2 | Direction is the picture's | Walk left to right in the picture: `motion.dir` right; back: left; with `image.mirror` 1 the same physical walk reads the other way; with `image.flip` 1 up / down swap; ceiling both. Change the orientation while walking: nothing false for about 3 s | [ ] |
| MOT-3 | Nothing false | The head moving (a patrol or `ptz.vel`), a day/night switch (force `night.mode`), an RTSP client connecting / leaving, a `video0.fps` change: `motion.active` stays 0 with nobody there. If it fires, set `motion_source` or lower `motion.sensitivity` | [ ] |
| MOT-4 | Home Assistant | The binary sensor `Motion` and the sensor `Motion direction` appear (MQTT on) and follow the rows | [ ] |
| MOT-5 | Pass-by, left to right | `track.enabled` 1, `track.search` motion, walk across the view in 3 s and leave: one short pan look toward where you left, then home within about 3 s of the look; no scan, no sweep | [ ] |
| MOT-6 | Pass-by, right to left | Same, the other way | [ ] |
| MOT-7 | Leave and come back | Leave through one edge, come back after 5 s: at most one look, the head follows you again; leave and come back twice within 30 s: only the first leave gets a look | [ ] |
| MOT-8 | Standing still | Stand still in view, then hide behind something: no search move at all, the head goes home after `track.return_delay_s` | [ ] |
| MOT-9 | Steer toward motion | Leave the view, then wave at one side within 8 s: one short move toward the motion, the detector takes over | [ ] |
| MOT-10 | Motor travel | `ptz.travel_since_cal` before and after ten pass-bys: with `track.search` motion about a fifth of the legacy travel | [ ] |
| MOT-12 | Defaults and options | A fresh install: `track.search` reads `motion` and `ptz.scene_min` reads `0.85` with no setting saved; set each through the assistant (Tracking / Patrol options) or `PATCH /settings`, reboot: the value is kept, the boot log names the threshold, and `track.search legacy` brings the sweeps back | [ ] |
| MOT-11 | Vendor statistics (optional) | `motion_source=md`: `motion.source` reads `md` and the direction is right in the four orientations; if it reads `md>diff` the statistics are not usable (log). Try `motion_md_frame=sensor` when the direction is mirrored under flip / mirror | [ ] |

### 2K video latency

Written 2026-11-23. Reference: [video latency](porting/lsc-rotatable-2k/video-latency.md). Before the fix: about 2 s glass to glass on `/stream0` (2304x1296, 10 fps, 3000 kbit/s, Wi-Fi -67 dBm, ffplay over TCP with `-fflags nobuffer -flags low_delay -framedrop`).

| # | Check | How | Done |
|---|---|---|---|
| L2K-1 | Glass-to-glass latency under 500 ms (stopwatch) and the camera-side frame age in the stats line | Phone stopwatch in front of the camera, ffplay beside it, photograph both (several times); the app log shows `stream0: frame age at send avg ... max ... ms` every 10 s: note avg, max and the unsent backlog max (measured on 2026-11-23: avg about 5 ms, max about 20 ms at send; the stopwatch glass-to-glass part is still open). Repeat on a weak spot of the Wi-Fi: the log may show `client ... frames behind` or `backlog standing too long` (a short freeze to the next keyframe is expected there, a growing delay is not) | [x] partly 2026-11-23: frame age at send avg about 5 ms, max about 20 ms (camera side); glass-to-glass not yet measured with a stopwatch |

### 2K speaker, two-way audio and picture settings

Written 2026-11-23. **SPK-1 and TALK-2 (partly) ran on the 2K on 2026-11-23; the rest is host-tested only.** Reference: [audio findings section 10](porting/lsc-rotatable-2k/audio-findings.md#10-speaker-and-two-way-audio),
[using the 2K, section 7](porting/lsc-rotatable-2k/using-the-2k.md#7-speaker-two-way-audio-and-picture-settings), [ISP controls](porting/lsc-rotatable-2k/isp-controls.md). Have the UART attached for the first
speaker start (a kernel fault in the playback driver would reboot the camera; the crash guard then keeps the speaker off at the next start). Use the signed API (`akam settings set`, or a curl helper as on 2026-11-23) and the app log.

| # | Check | How | Done |
|---|---|---|---|
| SPK-1 | The speaker starts and the amplifier opens | `audio.out_enable` 1: `audio.out_state` becomes `idle`; `audio.beep`: the one-second beep (three tones) is audible and clean. No sound: note it, then try GPIO 9 by hand (audio findings section 10) | [x] 2026-11-23, on the image that runs now: `audio.out_state` idle, beep and several minutes of melodies audible and clean at volume 70 |
| SPK-2 | No crash, no loop | After A1 the camera keeps running 10 minutes, `/config/akam/audio_out.trying` is gone (written only by the enable of A1, removed 5 s after the start), the video is unaffected, RSS rise is below 300 kB | [ ] |
| SPK-3 | Volume and gain | `audio.out_volume` 100, 50, 10, 0 (0 is silent), `audio.out_gain` 0 to 5: the level follows, no click at each change, no hiss at rest (the amplifier is off two seconds after the last sound) | [ ] |
| SPK-4 | Survives a restart | Power cycle with `audio.out_enable` 1: the speaker comes back by itself, volume and gain as saved; `killall akamd` restarts the app: no `audio_out.trying` appears at any moment (watch `/config/akam` with `ls` during the restart) | [ ] |
| SPK-5 | Crash guard | With the speaker off, create `/config/akam/audio_out.trying` by hand and set `audio.out_enable` 1: refused, `audio.out_state` is `disabled_crash`; remove the file, enable again: it works | [ ] |
| SPK-6 | Capture and playback together | `audio=1` (microphone track on): enable the speaker while a player reads the stream with sound: no glitch in the microphone track, no error in the log | [ ] |
| TALK-1 | The backchannel is offered | With a stream password and `audio.out_enable` 1: go2rtc (or any ONVIF backchannel client) over TCP shows the send-only G.711 track; without the password, or with `audio.talk_enable` 0, the camera answers 551 to a client that requires it | [ ] |
| TALK-2 | Talk works | go2rtc or Frigate (TCP): speak, hear yourself on the camera speaker, clear and without a large delay; `audio.talk_state` is `active` while talking and `idle` after | [x] 2026-11-23, partly: with `tools/akam-talk` over TCP (Digest), six sessions up to 40 s played clean and complete; go2rtc/Frigate and `audio.talk_state` not checked |
| TALK-3 | Half duplex | `audio.talk_duplex` half: the microphone track is silent while speech plays; `full`: it is not | [ ] |
| TALK-4 | One talker | A second client that sets up the backchannel while the first talks is refused; when the first leaves, the second can talk | [ ] |
| TALK-5 | Cut off | `audio.talk_enable` 0 (or removing the stream password) ends a running talk within about a second; the video of the same connection goes on | [ ] |
| TALK-6 | Quality and load | Ten minutes of talk: CPU stays low, `MemAvailable` stable, no underrun counter growing (`audio.out_state` stays `talking`/`idle`) | [ ] |
| PIC-1 | Saturation | `isp.saturation` -30, 0, 30 on a daylight scene: the colour intensity follows, 0 is the earlier picture | [x] 2026-11-24, current dev build: saturation -30 dulls, +30 over-saturates (orange bleeds into red), +15 looks natural, 0 gives back the earlier picture; live, no stream restart (2K, daylight, a Rubik's cube) |
| PIC-2 | Saturation survives day/night | Set 20, force `night.mode` night then day: the saturation is the same after the switch (log line `picture: replayed`) | [ ] |
| PIC-3 | Denoise | `isp.denoise` 0, 2, 4 in a dim room: less grain and less pulsing at the key frames as it rises, with some loss of detail; the log has no `refused`; 0 gives the first picture back | [ ] (2026-11-24, current dev build: applied without errors; on a bright static scene 2 smooths fine texture slightly compared with 0, nothing else visible; the dim-room grain check is still to do) |
| PIC-4 | Denoise table check | With `--isp-test 1`, `get nr3d` before and after `isp.denoise` 2: record 1 of the table equals record 3 of the first read (the shift rule, `picture.h`); `get all` shows `tnr_enable` never lowered | [ ] |
| PIC-5 | White balance | `img.wb` manual in daylight: the picture stops following the light (move a lamp or a coloured card); `conf` brings the automatic one back within seconds. Note which `wb_type` value (0 or 1) the log shows as automatic | **FAILED** 2026-11-24, current dev build: `img.wb manual` gives a strong blue cast right away (fixed gains that do not match the light); `conf` restores it at once |
| PIC-6 | White balance and night | (dropped for the 2K: `img.wb manual` is refused, see PIC-15) | n/a |
| PIC-7 | Persistence | `isp.saturation` and `isp.denoise` survive a power cycle (isp.nr2 and isp.wdr: PIC-12); `img.wb` stays `conf` | [x] 2026-11-24, current dev build: after a reboot `isp.saturation` 15 and `isp.wdr` 60 read back as set; `img.wb` is `conf` |
| PIC-8 | Install-time questions | `akam prepare-2k-sd` setup questions, then `seed-2k`: answer yes, speaker on, volume 60, saturation 10: after the flash and the first start, `akam settings get audio.out_volume isp.saturation` read 60 and 10 | [ ] |
| PIC-9 | Sharpness | `image.sharpness` 50, 70, 90 on a daylight scene: the edges get crisper as it rises | [x] 2026-11-24, current dev build: 70 is visibly crisper than 50 with no halo; 90 is crisper still but edges start to darken |
| PIC-10 | Bitrate | `video.kbps` 3000 against 4500 on a static scene | [x] 2026-11-24, current dev build: no visible difference |
| PIC-11 | Backlight strength (WDR) | A scene with a bright window, `isp.wdr` 0, 50, 90, 100 (live, no stream restart): the dark areas brighten as it rises, 90 is the picture the camera had before this setting existed, 0 the flattest; `--isp-test 1` `get fx` shows effect 5 = value - 50 and `get wdr` sum differs between 0 and 100. Note whether 91..100 is visibly stronger than 90 | [ ] |
| PIC-12 | Spatial noise reduction (nr2) | Same backlit scene, `isp.nr2` 0, 2, 4 (live): the grain in the dark areas drops as it rises (this is the unproved direction: a HIGHER nr2_k / ynr_intensity should filter MORE), with some loss of detail; bit rate lower; the log has no `refused`; 0 gives the first picture back. Before the first test run `--isp-test 1` `rt nr2` (get / set / get identical) | [ ] |
| PIC-15 | Image rows saved, white balance refused (fix of 2026-11-24) | (a) `image.sharpness` 70, `image.brightness` 60, `image.contrast` 40: power cycle: the three read back as set (`akam settings get image.`) and the picture shows them; then force `night.mode night` and back to `day`: values and picture unchanged (replayed if the switch reset them). No flash write while idle (`sys.flash_writes_store` unchanged over a few minutes). (b) `akam picture wb manual` (or PATCH `img.wb` manual) is refused with a clear message, the picture does not change; `wb conf` is accepted | [x] partly, 2026-11-24, current dev build: `image.sharpness` 70 survives a reboot (brightness and contrast not re-set); `img.wb manual` answers 422 `range` |
| PIC-13 | nr2 table check | `get nr2` (nr2_enable, nr2_k, both intensities of the 17 records) before and after `isp.nr2` 2: k and intensities grew by 1 on record 1 up to 5 on record 16 (clamped at 15), enables and mode unchanged. If the enables of the high-gain records are 0 the filter is off there and nr2 does nothing: report the dump | [ ] |
| PIC-14 | WDR / nr2 survive day/night and restart | `isp.wdr` 100 and `isp.nr2` 2, force night then day: both are back after the switch (`get fx`, `get nr2`); both survive a power cycle | [ ] |

### 1080p picture orientation (flip, mirror, ceiling mount)

Run on a 1080p on 2026-11-24 for flip, mirror and colour in daylight (O1080-2, 3, 5); the rest is host tests only. Do these on the first flash of a build that has `image.flip`. Reference: [video spec, section 5.7](../specs/video-and-streaming.md#57-flip-and-mirror-reverse-engineering-of-2026-11-23-not-yet-hardware-validated-on-the-1080p)
(what the code does and what is unknown). Have UART or SSH open and a stream player on `/stream0`. Set the values with `akam picture flip on|off`,
`mirror on|off`, `ceiling on|off`, the camera options screen, or `akamd ctl SET image.flip 1` over SSH. Also compare `/stream1` (same picture as `/stream0`, on purpose).

| # | Check | How | Done |
|---|---|---|---|
| O1080-1 | Default is unchanged | No `/etc/config/akam/orient`: the picture and the log are exactly those of the earlier build (no `[orient]` line, colour correct); `GET image.flip` 0, `image.mirror` 0 | [ ] |
| O1080-2 | `image.flip` is vertical | `SET image.flip 1`: the picture is upside down (top and bottom swapped, left and right unchanged), no stream restart (the player keeps playing; one or two frames may drop); the log shows `isp flip_mirror(1, 0) = 0`; back to 0 restores it | [x] 2026-11-24, current dev build: applied live from the assistant (`akam picture flip on|off`), vertical, back to normal with off |
| O1080-3 | `image.mirror` is horizontal | `SET image.mirror 1`: left and right swapped, top and bottom unchanged; back to 0 restores it | [x] 2026-11-24, current dev build: applied live from the assistant (`akam picture mirror on|off`), horizontal, back to normal with off |
| O1080-4 | Round trip | After flip 1 then 0 and mirror 1 then 0, the framing is identical to O1 | [ ] |
| O1080-5 | Colour stays right | With flip, with mirror and with both on, skin and sky colours look natural (no swapped red and blue, no green or magenta cast, no 2-pixel shifted fringe). This is the Bayer order the sensor driver is expected to keep | [x] 2026-11-24, current dev build, in daylight only: colours fine with flip and with mirror (the combination and the sensor Bayer order were not looked at separately) |
| O1080-6 | Survives day/night | With `image.ceiling` 1, `night.mode` night then day (and one automatic switch): the orientation is the same after each switch (the call is sent again after each switch: look for `isp flip_mirror(1, 1) = 0` in the log after the switch; a short frame drop at the switch is expected) | [ ] |
| O1080-7 | Survives a restart | Set flip, then kill `akamd` (the wrapper restarts it) and power cycle: the orientation is applied at start-up (log `[orient]`), the file `/etc/config/akam/orient` holds `flip=1` and `mirror=0`; with both off the file is gone; `md5sum` of the file is unchanged after 30 minutes idle (no recurring write) | [ ] |
| O1080-8 | Assistant | `akam picture flip on` shows `Flip (vertical) 1`; the Camera options > Picture screen lists Flip, Mirror and Ceiling mount and changes them live; `akam config save-global` records them and `akam config apply` on another 1080p sets them | [ ] |
| O1080-9 | API and 8091 | A signed `PATCH /akam/v1/settings {"image.ceiling":1}` turns both on (`GET` shows flip 1, mirror 1, ceiling 1); `{"image.flip":2}` is 422 `format`; a network `SET image.flip 1` in DEBUG mode is refused (local only) | [ ] |
| O1080-10 | RTSP and audio unaffected | While flipped, one RTSP client for 15 minutes: `video.fps_actual` 14.9 to 15, no skipped frames beyond the first change, CPU of `akamd` unchanged, audio in sync | [ ] |
| O1080-11 | Home Assistant (needs a broker) | With MQTT on, the switches `Image flip` and `Image mirror` appear; ON turns the picture over / swaps left and right, OFF restores it; the state follows a change made with `akam picture flip on` | [ ] |

### 1080p backlight (WDR) and 2D noise reduction (`isp.wdr`, `isp.nr2`)

Written 2026-11-24. **Not run on a camera: host-tested only (fake ISP).** Reference: [video-and-streaming.md](../specs/video-and-streaming.md) section 5.8. Use a scene with a bright window behind a darker room, the camera on a fixed mount, `akam picture` to set the values, and the app log (`[isp.wdr]`, `[control]`). Look at the same scene at each step.

| # | Check | How | Done |
|---|---|---|---|
| WDR-1 | The default sends nothing | A camera with no `wdr=` / `nr2=` line in `/etc/config/akam/picture`: `akam picture` shows `Backlight (WDR) strength: 90` and `Spatial noise reduction: 0`; the log has one `[isp.wdr] ev=load ... note=stock-nothing-sent` line at the start (since 2026-11-24) and one per day/night ISP load (a read of the WDR and Y_GAMMA blocks, nothing sent) and no `wdr` / `nr2` "not applied" line | [ ] |
| WDR-2 | The block can be read and written | `akam picture wdr 20`: answers Done, no `ERR isp.wdr`. If it answers `the ISP refused the block` or `live table not as decoded`, note the log and stop: `0x8004491c` is not the getter (section 5.8) | [ ] |
| WDR-8 | Self-diagnosis, on the 1080p after a no-change result (partly done 2026-11-24: write, read-back and ISO level confirmed, see below) | Backlit scene, `akam picture wdr 0` then `wdr 90`, after each `WDRDUMP` on port 8091 (or the `[isp.wdr]` lines of the log). Read: `get=ok mode=1`; `sw` has bits 1..10 (0x007fe) and `lift` bits 1..4 and 10 (0x0041e) in the day subfile; `changed` is hundreds of bytes and `csum_after` differs from `csum_before` at `wdr 0`; `set=ok readback=match`. Then compare `iso_prod` of the scene with the lift entries: if `lift` has no bit for the live level (entries 5..9 are the identity) the slider cannot act at that light, and a darker scene (more gain) is the next test. `readback=DIFFERS` or a `set=errno` means the kernel does not keep the block | [~] 2026-11-24, LSC Indoor 1080p, backlit living room in daylight: `get=ok mode=1 sw=0x007fe prog=0x007fe lift=0x0041e`; `wdr 0` `changed=938 set=ok readback=match`, `wdr 90` `changed=0` (block restored); `iso_prod=671`, `sgain=256 ispgain=256` (lowest ISO level, level 1 = entry 2, a lifting entry): GET, SET and read-back work, the WDR lift was live and its removal did not clear the haze. Still open: the missing `ev=load` line at start (fixed in this build, check WDR-1) and the darker-scene test |
| WDR-9 | Y_GAMMA toe flattening (`isp.wdr` below 90) | Same backlit scene: `akam picture wdr 90` (reference snapshot), then `wdr 45`, `wdr 0`; after each `WDRDUMP` on port 8091 reads `gamma=ok glift=N ... gset=ok greadback=match` (`gchanged` > 0 below 90, `gchanged=0`/`gnote=stock-nothing-to-send` at 90 and above). The shadows and the haze: the lifted dark part should look deeper and more contrasty as the number falls, `wdr 0` the flattest; `wdr 100` must look like 90 (no more gamma lift). Force night then day: the toe is back, no `not re-applied` line. Note whether the haze is gone and whether the toe at 0 looks crushed | [ ] |
| WDR-3 | The picture follows | Same scene, `wdr` 90, 45, 0: the dark part of the room gets darker and the contrast against the window rises as the number falls; `wdr` 100 lifts the shadows a little more than 90. Note which number looks best and whether the haze is gone | [ ] |
| WDR-4 | 0 gives the night-like curves, no artefacts | `wdr 0`: no colour shift, no banding in the shadows, no flicker while the light changes (walk a hand in front of the lens) | [ ] |
| WDR-5 | Back to stock | `wdr 90`: the picture is the first one again (compare a snapshot) | [ ] |
| WDR-6 | Survives day/night | `wdr 10`, force `night.mode` night then day: the same picture as before the switch, the log has no `[isp.wdr] ... not re-applied` line | [ ] |
| WDR-7 | Survives a power cycle | `wdr 10` and `nr2 3` are still set after a power cycle (`akam picture`); `akam picture reset` puts 90 and 0 back and removes the file | [ ] |
| NR2-1 | 2D noise reduction | In a dim room `nr2` 0, 2, 4: less grain in flat areas as it rises, with some loss of fine detail; the log has no `refused`; 0 gives the first picture back | [ ] |
| NR2-2 | The same rows through the API and Home Assistant | `akam settings get isp.wdr isp.nr2` answers 90 and 0; with MQTT on, the Home Assistant entities "Backlight (WDR)" and "2D noise reduction" follow the numbers | [ ] |

### 1080p remote control (convergence P4-P8; akam-01 first, then akam-02; UART attached, run from /tmp before flashing)

Not run on a camera yet; host tests only (`tests/ak1080_*`, `tests/leds_oracle_test.sh`, `tests/ctl_sys_test.sh`, `tests/control_test.sh`).
Reference: [control-api.md](porting/lsc-1080p/control-api.md). Baseline first: run the previous `akamd` 15 minutes with one RTSP client and record
`video.fps_actual`, `ctl_sys` skipped frames, CPU of `akamd` and `VmRSS`/`MemAvailable`; every item below must keep them within noise.

| # | Check | How | Done |
|---|---|---|---|
| B1 | Build and boot | `make repro` hash matches the golden; copy the binary to `/tmp/akamd`, stop the wrapper's copy, run it: stream up, `GET onvif.state`, `GET mqtt.state` as before, no new log errors, no port besides 22 and 554 listening (8091 only on 127.0.0.1) | [ ] |
| B2 | sys.* (P4) | `akamd ctl GET sys.cpu_pct`, `sys.load1_x100`, `sys.mem_total_kb`, `sys.mem_avail_kb`, `sys.uptime_s`, `net.wifi_lq`: plausible against `top`, `free`, `iwconfig`; `-1000000` is never shown once the camera is up; the older keys (`sys.cpu`, `sys.mem_free`...) unchanged | [ ] |
| B3 | LED, defaults (P6) | Power cycle with `status=auto` saved: the LED blinks 1 Hz while starting, solid when ready; unplug the router: 1 Hz again; start VLC: 0.5 Hz (1 s on, 1 s off), stop it: solid; `led.status off/on` and `night.ir_led off` as before; `led.state` names `blink_wifi`/`blink_stream`/`on`/`off`/`system` as before. Same on akam-02. This is the visual proof that completes `tests/leds_oracle_test.sh` | [ ] |
| B4 | LED, patterns (P6/P7) | `akamd ctl SET led.nowifi 3:250:250:2000` then unplug the router: 3 quick blinks, pause; `GET led.nowifi`; `cat /etc/config/akam/leds` shows the `led_nowifi=` line only while not the default; set it back to `1:500:500:0`: the line is gone | [ ] |
| B5 | 8091 unchanged on loopback (P7) | The text port still answers on the camera itself (`nc 127.0.0.1 8091`; the assistant now uses the API, see the checks C below); `akamd ctl LIST` order unchanged (six `sys.*` keys appended in the metrics); `akamd ctl SET image.contrast -5` from the camera saves `/etc/config/akam/picture` | [ ] |
| B6 | Closed without a key (P8) | No `/etc/config/akam/api_keys`: `netstat -ltn` shows no listener on 8092 (only 22, 554 and 127.0.0.1:8091); the akamd log has no API error line | [ ] |
| B7 | First key opens the port | `akamd --api-key new --scope admin --name t` over SSH while streaming: id and secret printed once; 8092 listens within ~2 s without a restart; `ls -l /etc/config/akam/api_keys` is `-rw-------`; the stream does not hiccup (`video.fps_actual`) | [x] 2026-11-24, current dev build, partly: the key was made with `akam api-key --yes` (not with `akamd --api-key new` by hand), 8092 opens once a key exists; the stream during the creation was not measured |
| B8 | Signed calls | `tools/akam-api-sign.sh` + curl from a PC: `/hello`, `/info` (model `lsc-smart-indoor-1080p`), `/state`, `/schema` 200; wrong secret 401 `bad_signature`; the same request twice 401 `replay`; `/ptz/presets` and `/button` are 404 | [ ] |
| B9 | Settings take effect | PATCH `led.mode`, `night.ir_led`, `night.mode` (force night: IR-cut click, IR LEDs per `night.ir_led`), `image.brightness`/`sharpness`/`denoise` (see it), `video0.kbps`/`fps`/`gop` (file written, `video0.cfg_pending` is 1, applied after `kill` + wrapper respawn), `onvif.enabled` (port opens, ODM sees it; refused without the stream password), `mqtt.enabled` (with a broker file); read back; power cut: persisted values survive | [ ] |
| B10 | Actions and events | `led.blip` (find the camera), `led.blip_stop`, `settings.reset`; `GET /events?since=N&timeout=25` held; force night: the `night` event arrives at once; a PATCH: the `setting` event | [ ] |
| B11 | Flash discipline | `md5sum /etc/config/akam/*` before and after 30 minutes of idle API polling (state every 30 s): identical (no periodic write); 7 PATCHes in a row: the 7th is 429 `flash_rate`; `audit` appears only after a write request | [ ] |
| B12 | Cost | `VmRSS`/`MemAvailable` of `akamd` with no key, with a key idle, and after a 1 minute burst of requests (gate of the spec: +64 KB RSS; the binary is over its +56 KB gate, see the golden file); `video.fps_actual` 14.9-15, `ctl_sys` skipped 0 and CPU unchanged under a flood of bad signatures from a PC (they are rejected before any crypto once the peer is locked) | [ ] |
| B13 | Hostile input and fail closed | As A9 and A10 of the 2K list (9 KB body 413, 5 KB header 431, chunked 411, `..` path 400/404; `chmod 644 api_keys` closes the API, `chmod 600` reopens, garbage in the file closes it and keeps it) | [ ] |
| B14 | Rollback | Copy back the previous `akamd` to `/tmp`, restart the wrapper: video identical; `rm /etc/config/akam/api_keys` closes the port; deleting `led_*` lines of `/etc/config/akam/leds` restores the default patterns | [ ] |

### 1080p: the assistant through the signed API, text port 8091 closed to the network (checks C; run after B7)

Not run on a camera yet; host tests only (`tests/control_test.sh`, `tests/ak1080_settings_test.sh`, `tests/ak1080_api_http_test.sh`, the Go tests of
`tools/assistant`). Needs a 1080p with the new firmware, a PC with the new `akam`, SSH access. Use a camera that already runs an older Akam image for C9 and C10.

| # | Check | How | Done |
|---|---|---|---|
| C1 | 8091 closed to the network | `ss -ltn` on the camera shows `127.0.0.1:8091`, not `0.0.0.0:8091`; from the PC `nc -vz CAMERA 8091` is refused; on the camera `printf 'LIST\nQUIT\n' \| nc -w 2 127.0.0.1 8091` still answers; the akamd log says `listening on port 8091 (loopback only)` | [x] 2026-11-24, current dev build: hwcheck shows `127.0.0.1:8091` and "8091 answered LIST" on the camera; the PC sees only 22, 554, 80 (ONVIF on) and 8092 (key present) |
| C2 | Open ports after a boot | From the PC: only 22 and 554 (and 8092 once a key exists) answer; `akam step verify` agrees ("only 22 and 554 are open") | [x] 2026-11-24, current dev build: after the hand power cycle, `step verify` lists 22 554 80 (80 because ONVIF was on) and 8092 answers once the key exists |
| C3 | Key made at the end of an install | After `akam step verify` on a fresh install: asked "Create one over SSH?"; yes: the state folder has `api_key` (0600), the camera has `/etc/config/akam/api_keys` (0600), 8092 listens within seconds, the secret is nowhere on screen or in `akam.log` | [ ] |
| C4 | Every command through the API | `akam led`, `led status off`, `audio`, `audio volume 40`, `audio beep` (a beep is heard), `picture`, `picture contrast 5`, `picture wb manual`, `video`, `video bitrate 1200` (asks to restart), the options screens (all six groups), `config save-global` (the Wi-Fi name is offered) and `config apply`: all work and the values are the camera's. `akam picture flip on` flips (see the 1080p orientation checks) | [ ] |
| C5 | No traffic on 8091 | During C4, on the camera: `netstat -tn` shows no connection to 8091 from the PC (nothing came from the PC at all) and `akamd` logs no `tuning client connected` line without `(local)` | [ ] |
| C6 | Speaker and microphone rows | `akam audio mic on` answers "restart"; after a power cycle the stream has an audio track; `audio off` then `audio on`: the speaker works again (a restart note only when the device cannot open now); `GET /state` shows `audio.out_state` and the counters move during `talk` | [ ] |
| C7 | The LAN opt-in | `akam control-port open` (asks, default No), restart the video service when asked: from the PC `nc CAMERA 8091` then `LIST` answers, `SET isp.contrast 3` is refused (read-only); `control-port close` and a restart: refused again; `md5sum /etc/config/akam/control_lan` exists only while open; the file is written once per change (look at its mtime) | [ ] |
| C8 | Flash discipline | `/etc/config/akam/*` unchanged after 30 minutes of the options screen left open and reloaded 10 times (reads write nothing); a change spends one PATCH (one flash token); 7 settings changes in a row from the assistant: the 7th says it is rate-limited and nothing is applied twice | [ ] |
| C9 | Existing install: enrol over SSH | An older image with no key and 8091 open: run `akam led` in a terminal: "This PC has no API key... Create one?" answer n: nothing changes; answer y: the key is made, the LED values show. The old image has no audio, `img.wb` or `control.lan` rows: they show n/a until `akam update` | [ ] |
| C10 | Update keeps the keys | `akam update` on that camera: `/etc/config/akam/api_keys` and the PC's `api_key` unchanged (checksums), the next settings command works without asking | [ ] |
| C11 | Lost or refused key | Delete the PC's `api_key`: `akam api-key` says the camera already has `assistant`, asks before revoking it, makes a new one; `rm /etc/config/akam/api_keys` on the camera: the next command says the camera does not answer its API and `akam api-key` offers to replace the key | [ ] |
| C12 | Discovery without 8091 | `akam find-camera` (typed address and the network scan): the camera is "Akam" from SSH + video + the Dropbear banner; the firmware shows once the key exists; no connection to 8091 is made | [ ] |

### R - Rename to `akamd` (update from an old image, old config, old names)

Written 2026-11-23; R1, R2 (1080p) and R4 (2K) ran on 2026-11-24, the rest is **host tests only.** The camera program is `akamd` on both cameras (1080p: was `ak_rtsp`; 2K: was
`akam2k_rtsp`, helpers now `akam-sntp`, `akam-mic`, `akam-led`; `akam2k.conf` is now `akam.conf`). Needs one camera of each
model still running an image of the old names, a PC with the new `akam`, SSH access. Nothing here is validated on a camera yet.

| # | Check | How | Done |
|---|---|---|---|
| R1 | 1080p: update from an old image | `akam update` from the old image; after the reboot `pidof akamd` answers, the stream runs on 554, `akam step verify` is green | [x] 2026-11-24, current dev build: update over SSH from the previous image OK, `akamd` running; `step verify` green after the hand power cycle (30 PASS, 0 FAIL). The API key was made with `akam api-key --yes`; 8091 listens on 127.0.0.1 only, 8092 is open once a key exists; the microphone is off by default after the update and the speaker setting was kept |
| R2 | 1080p: `ak_rtsp` link | `ls -l /usr/bin/ak_rtsp` is a link to `akamd`; `/usr/bin/ak_rtsp ctl` works like `/usr/bin/akamd ctl` | [x] 2026-11-24, current dev build: the `/usr/bin/ak_rtsp` link to `akamd` is present |
| R3 | 2K: update from an old image (hot update and reboot) | `flash_appfs.sh` or `akam update` from an image with the old names: the hot update stops the old `akam2k_rtsp`, the new app runs as `/tmp/akamd`, stream on 554 | [ ] |
| R4 | 2K: old config migrated | With only `/config/akam/akam2k.conf` present before the update: after it `/config/akam/akam.conf` exists with the same lines, `akam2k.conf` is still there, the settings (LED, audio, tracking keys) behave as before | [x] 2026-11-24, current dev build: on the 2K `akam2k.conf` was migrated to `akam.conf`, identical, the old file kept (the 2K part only) |
| R5 | 2K: log and helpers | The app log is `akam.log` (card; RAM copy `/tmp/akam2k_main.log` unchanged); a leftover `akam2k.log` is ignored; `akam-sntp` syncs the clock when a server is set; `akam-led list` and `akam-mic` run | [ ] |
| R6 | Assistant against OLD firmware | New `akam` against a camera still on the old names: `api-key` makes the key (falls back to `/usr/bin/ak_rtsp`), `video` restart works, `hwcheck` reports the program running | [ ] |
| R7 | Assistant against NEW firmware | Same commands against the updated camera: the key is made with `/usr/bin/akamd`, restart and `hwcheck` work | [ ] |
| R8 | Old kit still readable | `akam get-kit --kit` with a 0.1.x kit (`kit_format=1`) still builds an image whose `bin/akamd` is the old binary plus the `ak_rtsp` link | [ ] |

### Talk through the speaker, hands-on (1080p)

Replace `<cam>` by the camera address and `viewer` by the stream user. Prerequisite: `akam rtsp-auth set` done. Written for the 1080p (the 2K's results are in the checks above).

```bash
# 1. The plain stream is unchanged (no Require header): same SDP, no talk track
ffprobe -rtsp_transport tcp rtsp://viewer:PASSWORD@<cam>:554/ 2>&1 | grep -E 'Stream|Audio|Video'

# 2. Without the password: the camera refuses a talk request (551) and still serves video
akam rtsp-auth disable && akam talk test            # expect exit 3: "does not offer talk"
akam rtsp-auth set                                    # back on

# 3. Tone, file, microphone (listen at the camera)
akam talk test
akam say hello.wav
akam talk 20

# 4. Counters during and after a 10 s talk (loopback port, over SSH)
ssh root@<cam> "printf 'LIST\n' | nc -w 2 127.0.0.1 8091 | grep -E 'audio\.(talk|out)_'"
#    expect packets about 500 per 10 s, dropped 0, lost 0, underruns 0, state idle afterwards,
#    and the amplifier off 2 s after the end (no hiss)

# 5. Half duplex: while talking, the camera's own microphone (ffplay of the stream) goes silent only
#    while you speak; with audio.talk_duplex=full it does not.
ffplay -rtsp_transport tcp rtsp://viewer:PASSWORD@<cam>:554/      # in another terminal, mind: one client at a time

# 6. go2rtc / Frigate / Home Assistant: the snippets above; note what works and the failure mode of the
#    single RTSP slot when go2rtc and another client are connected.
```

What to report: the exit codes, the `audio.talk_*` and `audio.out_*` lines after a talk, whether the voice
is clear, any pop when the amplifier switches, and `top` on the camera during a talk (CPU of `akamd`).

## Post-flash check script

`tools/hw_tests/akam_hwcheck.sh` is a read-only POSIX `sh` script. It never writes to flash or CONFIG,
never kills or starts anything, and never prints key material. It prints `PASS`, `FAIL`, `WARN`,
`UNKNOWN` and `INFO` lines, then facts, and exits 0 if there is no `FAIL`. It is **not** installed in
the image, and it has run on the 1080p cameras. Run it without copying anything:

```bash
ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh | tee hwcheck-1.txt
```

or copy it to `/tmp` first with `ssh ... 'cat > /tmp/hwcheck.sh' < tools/hw_tests/akam_hwcheck.sh`, or
from the UART console off the card: `sh /mnt/akam_hwcheck.sh`.

It checks, in sections: listening sockets (22 and 554 expected; 8091 only on 127.0.0.1 (a LAN address is a finding); 21, 23, 24 must be closed; 80
reported as WARN if open), processes (`akamd`, `dropbear`, `wpa_supplicant`, `udhcpc` present;
`anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd` absent), resident memory, mounts and `/proc/mtd`, Dropbear
files and modes, whether root's shell is in `/etc/shells`, the maintenance flag, a `LIST` to port 8091,
the debug flag of 8091 (`debug:` info line, WARN while it is on), entropy, the Akam logs, and system facts.

Details worth knowing: the Dropbear key path is judged from **root's HOME in `/etc/passwd` as seen at
runtime** (`akam_ssh.sh` moves it to the private `/tmp/akam_home`), so `/` and `/etc` being 0775 is
only `INFO`; a group/other-writable HOME or key file is a `FAIL`. The `akamd` CPU use is the delta of
`/proc/PID/stat` over ~2 s (no `top`). The night check reads `night.mode` and samples `night.state` four
times over ~20 s through 8091 (each sample is a short connection; 8091 serves 4 clients at once, so
no other client is disturbed) and WARNs if it changed more than once; `HWCHECK_NIGHT_SAMPLES=0` disables it.

Run order (the first run is expected to WARN about the maintenance flag if you left it in place):

1. Right after the first boot: `akam_hwcheck.sh`. Save the output.
2. **Exposure window**: reboot, and start the sampler as early as you have a shell (UART console), with
   the output on the card, not in RAM:
   ```sh
   sh /mnt/akam_hwcheck.sh --boot-window 180 > /mnt/bootwindow.log 2>&1 &
   ```
   It samples ports 21, 23 and 24 every second (default 120 s if no number is given) and prints the
   uptime of each sample plus first/last seen per port. A forbidden port open on the first line means
   the window began before you started measuring: the result is then a lower bound.
3. After removing `AKAM_MAINTENANCE`: power cycle, run `akam_hwcheck.sh` and `--boot-window` again.
   Ports 21, 23 and 24 must be closed.
4. Power-cut test, then repeat step 3 without the card inserted.
5. Reconnect test: restart your router or Wi-Fi and run the port check again to see that nothing reopens.
6. Manual checks the script only reminds you of: microphone, night mode, 8091 refused from the PC (it listens on 127.0.0.1 only; with `control.lan` the reads work and `SET` is refused), a stream viewer for 1080p.

Compare the port check from outside too: `nmap -p 21,22,23,24,80,554,8091 <camera-ip>` (8091 must show closed unless you opened it with `control-port open`).
