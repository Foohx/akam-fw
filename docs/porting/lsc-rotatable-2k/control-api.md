# Local control API of the Rotatable 2K (port 8092)

Signed, plain-HTTP JSON API inside the own app `akamd`, for scripts and other local tools. Home Assistant uses MQTT discovery ([MQTT](../../mqtt.md#lsc-rotatable-2k)), not this API. Design and threat model: [specs/control-api.md](../../../specs/control-api.md). This page is
the user and integrator reference of what phase 2 ships.

## 1. Provisioning: the first key

With **no key installed nothing listens on the port**. Create the first (admin) key from SSH:

```sh
sh /appfs/akam/akam_api_key.sh new --scope admin --name assistant         # card-less (flash build)
sh /mnt/sdcard/akam/akam_api_key.sh new --scope admin --name assistant     # with the first-install card inserted
akamd --api-key new --scope admin --name assistant                   # what the script runs (RAM copy in /tmp)
```

It prints the key id and the 64-hex secret **once**. The running app notices the file within about a second and opens
port 8092 (no restart). More keys: `new --scope read|write|admin --name TOOL` (one key per tool, up to 8, names 1..24 of
`A-Za-z0-9 ._-`), `list` (ids, names, scopes; never a secret), `revoke ID|NAME` (the last admin key is refused unless `--force`).
Keys can also be created and revoked over HTTP by an admin key (`POST /keys`, `DELETE /keys/<id>`).

The key file is `/config/akam/api_keys`, mode 0600. A file that is group/world readable, or invalid, **closes the API**
(state `keys_perm` / `keys_invalid`); the file is kept, never wiped. A factory reset (which wipes `/config`) closes the API again.

## 2. Requests

`http://<camera>:8092/akam/v1/<endpoint>`, `Connection: close`, JSON bodies, `Content-Length` required for PATCH and POST
(no chunked uploads, no `Expect`). Every endpoint except `hello` is signed (HMAC-SHA256, spec section 5): use
[`tools/akam-api-sign.sh`](../../../tools/akam-api-sign.sh) (shell + openssl) or implement the canonical string of the spec.
The raw request target is signed as sent: a literal `+` in a query is NOT a space (sign it as `%2B`).

```sh
export AKAM_KEY_ID=3fa91c07 AKAM_KEY_SECRET=<64 hex> AKAM_URL=http://192.0.2.10:8092
H=$(tools/akam-api-sign.sh GET /akam/v1/state)
set --; while IFS= read -r l; do set -- "$@" -H "$l"; done <<EOF
$H
EOF
curl -sS "$AKAM_URL/akam/v1/state" "$@"
```

If the camera clock is not set (no NTP yet) `GET /hello` returns `clock_set:false` and a one-shot `challenge`; sign with
`AKAM_CHALLENGE=<hex>` instead of a timestamp.

## 3. Endpoints

| Method and path | Scope | What |
|---|---|---|
| `GET /hello` | none | `{"api":1,"clock_set":bool,"challenge":"<32 hex>"}`; 2/s per peer, 10/min |
| `GET /info` | read | model, app version, uptime, clock, caller key id and scope, event cursor, limits |
| `GET /schema` | read | every setting (type, bounds, options, default, apply, persist, access, scope) and every action with its arguments |
| `GET /settings[?prefix=]` | read | writable values `{"key":"value"}` (all values are strings in canonical form: `0`/`1`, decimals, enum words) |
| `GET /settings/<key>` | read | one setting with metadata and value |
| `GET /state` | read | read-only values (metrics, PTZ position, RTSP clients, person, night, key count) |
| `PATCH /settings` | write | `{"led.mode":"off","video0.fps":10,"track.enabled":true}`. JSON integers and booleans become canonical text. All-or-nothing validation: 200 all applied, 207 a setter failed after others succeeded, 422 nothing applied. Reply `{"results":{key:{"ok":true,"apply":"applied","value":".."}\|{"ok":false,"error":"range"}}}` |
| `POST /actions/<name>` | write | see section 5 |
| `GET /events[?since=N&timeout=S]` | read | event feed with long poll (section 6) |
| `GET /button`, `GET /modes[/<name>]` | read | the programmable push button: the whole config (timing, LED confirmation, bindings) and the named modes (section 5.2) |
| `POST /button`, `POST /modes[/<name>]`, `DELETE /modes/<name>` | write | replace the whole button config; create or replace a mode; delete a mode no binding uses (section 5.2) |
| `GET /audit` | admin | last 64 requests (ring); the last 20 writes survive a restart (`/config/akam/audit`) |
| `GET /keys`, `POST /keys`, `DELETE /keys/<id>` | admin | list; create `{"name","scope"}` (the secret is in the reply, once); revoke (409 `last_admin` for the last admin) |

Errors: `{"error":"code","detail":"text"}`. Status codes: 400 malformed (`bad_request`, `bad_json`, `bad_query`, `bad_path`),
401 (`bad_signature` for an unknown key and a wrong signature alike, `clock_skew` with the camera time, `replay`,
`bad_challenge`, `missing_auth`), 403 `scope`, 404 `unknown_path` / `unknown_key` / `unknown_action`, 405, 408 (head not
received in 5 s), 409, 411, 413 (body over 8 KiB), 414, 417, 422, 429 (`rate`, `locked`, `busy`; with `Retry-After`),
431 (head over 4 KiB), 503 (`busy`, `api_closed`), 504 (`busy`: the change is still being applied).

Limits: 4 connections (3 per peer), head 4 KiB, body 8 KiB, 5 s without progress. Accepted requests: burst 14 then 0.4 per second
across all keys (the nonce cache holds 64 entries, spec 5.5); send several settings in ONE PATCH. Per peer: 5 failed
authentications lock it for 1 s doubling to 5 min (a peer that proved itself in the last 10 minutes is exempt from the global
cap of 20 failures per minute, so a flood cannot lock the owner out).

## 4. Settings (the registry rows of the 2K)

| Key | Type | Notes |
|---|---|---|
| `led.mode` | `auto\|on\|off` | status LED; persisted |
| `led.boot`, `led.nowifi`, `led.person`, `led.stream`, `led.night`, `led.idle` | pattern `on\|off\|N:on_ms:off_ms:pause_ms` | LED rule patterns; persisted |
| `night.ir_led` | `auto\|off` | IR LED policy; persisted |
| `image.brightness`, `image.contrast`, `image.sharpness` | 0..100 | ISP picture; saved (written only when you change one, never at start or while idle), applied at start and replayed after a day/night switch (the ISP keeps none of them) |
| `isp.saturation` | -50..50, default 0 | saturation, the 1080p's key and range (ISP effect type 2); saved; replayed after a day/night switch; host-tested only |
| `isp.denoise` | 0..4, default 0 | denoise, the 1080p's rule (3D NR record N levels higher); saved; refused when the live table is not as decoded; host-tested only |
| `isp.nr2` | 0..4, default 0 | spatial (luma 2D) noise reduction: 0 sends nothing (the camera's own table); N raises `nr2_k` and `ynr_intensity1/2` of every record (more at high gain), from the pristine table, so it is never cumulative; saved; replayed after a day/night switch; refused when the live table is not as decoded; host-tested only |
| `isp.wdr` | 0..100, default 90 | backlight (WDR) strength = ISP effect type 5 with the value N - 50 (the stock's 40 is 90): 0 identity curve (off), 100 the ISP file's curves in full; saved; replayed after a day/night switch; host-tested only |
| `img.wb` | `conf` only | the automatic white balance. `manual` is refused (422 `range`) on the 2K: the camera gives no way to freeze the current gains, and the vendor manual gains give a blue picture (see [isp-controls](isp-controls.md) section 4) |
| `audio.out_enable` | 0/1, default 0 | the speaker (opt-in on the 2K); turning it on starts the playback at once; saved |
| `audio.out_volume`, `audio.out_gain` | 0..100 (default 100), 0..5 (default 2) | speaker volume (software, ramped) and hardware gain step; saved |
| `audio.out_amp` | `auto`, `on`, `off` | amplifier policy (auto: on while there is sound); RAM only |
| `audio.talk_enable`, `audio.talk_duplex` | 0/1 (default 1), `half`/`full` (default `half`) | the ONVIF backchannel switch; whether the microphone is silenced while the speaker plays speech; saved |
| `audio.out_state`, `audio.talk_state` | read only | `off`, `idle`, `talking`, `test`, `disabled_crash`, `error`; `idle` or `active` |
| action `audio.beep` | | queues the 1 s test beep; 409 while the speaker is not running |
| `image.mains_hz` | `50\|60` | anti-flicker; saved like the three rows above |
| `image.flip` | bool (default 0) | flip the picture vertically (`ak_vpss_switch_flip_mirror`, isp-controls.md 3.2; host-tested only, not verified on the camera); live, no restart; saved in `/config/akam/settings` only when the user changes it (never rewritten while running); Home Assistant switch **Image flip** |
| `image.mirror` | bool (default 0) | mirror the picture horizontally (not verified on the camera either); same rules; Home Assistant switch **Image mirror** |
| `image.ceiling` | bool (default 0) | convenience for a camera mounted upside down: 1 = flip and mirror both on, 0 = both off; reads 1 only while both are on; not saved itself (the two rows above are); Home Assistant switch **Ceiling mount**. The orientation also applies to the zones, the tracker, the PTZ directions and the scene reference: see isp-controls.md 3.3 |
| `detect.publish_hz` | 0..10 (default 2) | how often the person boxes are published (event `detections`, `detect.boxes`, MQTT `BASE/detections`); 0 = off; never more than 5 a second; live, persisted |
| `track.enabled` | bool | person tracking; persisted |
| `track.return` | `stay\|home` (default `home`) | what the camera does when the person is gone: stay where it was last seen, or go home; live, persisted (see tracking.md) |
| `track.return_delay_s` | 0..86400 (default 30) | home mode: seconds after the lost-person grace (3 s, `track_grace_s`) before going home, 0 = right after the grace; live, persisted |
| `track.band_pct` | 2..40 (default 6) | pan band: no pan while the box centre is within +-this % of the frame width from the centre; live, persisted |
| `track.tilt_return_s` | 0..3600 (default 0 = off) | stay mode: seconds after the lost-person grace before the TILT (not the pan) goes back to the home tilt, 0 = never; live, persisted |
| `track.head_pct` | 0..50 (default 15) | tilt: the row (% of the frame height from the top) where the top of the person box (the head) is kept, 0 = centre of the box; live, persisted |
| `track.gain_pan` | 200..4000 (default 640) | pan: motor steps per frame width of offset (measured 2 steps per px of the 320 px detector frame = 640; `TL_2K_STEPS_PER_PX_X10` in track_logic.h); live, persisted |
| `track.gain_tilt` | 200..4000 (default 384) | tilt: motor steps per frame height of offset (measured 2 steps per px of 192 px); raise it if the head is brought back too slowly, lower it if the camera overshoots; live, persisted |
| `track.tilt_band_pct` | 2..40 (default 10) | the vertical dead band, % of the frame height; the head band around `track.head_pct` is half of it (default +-5 %); live, persisted |
| `track.zone_pct` | 0..90 (default 40) | the comfort zone, % of the frame width (40 = the central 40 %): no motion while the person's reference point stays inside it and is not heading out; 0 = the old targeting (central band, upper-third rule); live, persisted |
| `track.zone_y_pct` | 0..90 (default 30) | vertical comfort band of the body rule (whole person visible: the centre of the box), % of the frame height; live, persisted |
| `track.zone_head_pct` | 0..90 (default 20) | vertical band of the head rule (close or cut person) around the head row `track.head_pct`; live, persisted |
| `track.motion` | `continuous`, `steps` (default `continuous`) | `continuous`: one ongoing move re-targeted without a stop (dead-man lease, frames positioned by the driver counters); `steps`: timed nudges (the fallback); switching to `steps` stops a running move; live, persisted |
| `track.lost_policy` | string, `none` or up to 8 steps joined by `,` (default `backsweep,exit,hold:30,home` = the legacy return policy) | what the tracker does when the person is lost: `hold:N`, `backsweep`, `exit`, `local:W:D:N` (oscillate around the last position), `route:<name>[:N]` (a stored patrol route), `home`, `stay` (last); a detection at any step ends the chain; all or nothing (an invalid string is refused). Needs `track.motion=continuous`; live, persisted. Examples: `backsweep,exit,local:300:4:20,home`, `exit,route:entrance:90,stay`, `none` |
| `track.lost_step` | read only string | the current step of the lost chain: `idle` (followed / nobody lost), `backsweep`, `exit`, `hold`, `local`, `route:<name>`, `home`, `stay`, `done`; every change is also an event `{"type":"track","data":{"lost_step":"..."}}` |
| `track.tilt_back_s` | 0..600 (default 5; 0 = off) | continuous mode: this many seconds after the person was lost with the tilt more than 250 steps above home, the tilt goes back to the home tilt (the pan follows the return policy, not this); live, persisted |
| `track.tilt` | 0\|1 (default 0, `track_tilt` in akam.conf) | 0 = pan only tracking: the tracker never moves the tilt (no follow, no close scan, no tilt-back, no vertical search; the tilt stays where it is); live, persisted; Home Assistant switch **Pan only tracking** (ON = 0) |
| `track.close_scan` | 0\|1 (default 1) | continuous mode: a close person lost with no direction evidence is searched up and down around the loss pose before the lost policy runs; 0 = the old WAIT 2.5 s then tilt back to home; live, persisted |
| `track.close_scan_up` | 0..600 (default 250) | steps above the loss tilt the close-scan goes (clipped to the soft range; 0 = skip); live, persisted |
| `track.close_scan_down` | 0..600 (default 300) | steps below the loss tilt the close-scan goes (clipped to the soft range; 0 = skip); live, persisted |
| `track.lag_ms` | -150..300 (default 50) | continuous mode: the video latency subtracted from the capture time when the pose at capture is looked up; negative allowed; calibrate with `tools/track_lagfit` (tracking.md, E2); live, persisted |
| `track.search` | `motion` (default), `legacy` | the lost-target search: `motion` = at most two short looks toward the exit / new motion, no scans, sweeps or stops; `legacy` = the backsweep, close scan, exit search and sweep (legacy lost policy only); live, persisted |
| `ptz.scene_min` | string `0.80`..`0.99` (default `0.85`) | the boot scene check threshold; persisted only on a user change, takes effect at the next boot check. Once set it overrides the `ptz_scene_min` key of `akam.conf` (which still sets it when the row was never set; the saved file is loaded after the app started, before the check settles). A head shifted by 3 px or more is refused whatever the value. Below 0.80 is refused (the screening floor) |
| `motion.enabled` | 0/1 (default 1) | motion detection on the picture; live, persisted |
| `motion.sensitivity` | 1..10 (default 5) | live, persisted |
| `motion.active`, `motion.dir`, `motion.x`, `motion.y`, `motion.source` | read only | motion now (0/1), `none|left|right|up|down` in the picture you see, centroid in percent (-1 idle), `diff|md|md>diff|off` |
| `video0.*`, `video1.*`: `kbps`, `max_kbps` (0 = auto), `fps`, `min_qp`, `max_qp`, `gop`, `rc` (`vbr\|cbr\|avbr`) | int / enum | ranges from `video.h`; min QP <= max QP and peak >= target are checked for the whole PATCH; the capture thread applies live or re-opens that one channel; stored by the video module in `/config/akam/video` |
| `audio.mic_enable` | 0\|1 (default 0) | the microphone track (`audio=` in `akam.conf`); admin key; read at the next start of the app |
| `mqtt.enabled`, `mqtt.host`, `mqtt.port`, `mqtt.user` | 0\|1 (0); host name or IPv4 (1..63 of `A-Za-z0-9.-`, empty = none); 1..65535 (1883); 1..32 of `A-Za-z0-9._@+-` or empty | the files `mqtt` (`enabled=` needs a host); admin key; next start of the app |
| `mqtt.password` | **write-only** string, at most 128 printable characters | the file `mqtt_pass`; needs a user; empty removes it; never read, listed or echoed |
| `onvif.enabled`, `onvif.port` | 0\|1 (0); 1024..65535 (8000) | `onvif=` / `onvif_port=` in `akam.conf`; enabling needs a stream password (stored or in the same PATCH); admin key; next start of the app |
| `rtsp.user` | string, 1..32 of `A-Za-z0-9._@+-`, or empty | the user of `rtsp.conf`; empty removes the stream password; another user needs `rtsp.password` in the same PATCH; admin key; applies at once |
| `rtsp.password` | **write-only** string, 8..128 printable characters | stored only as the Digest hash `user:akam:HA1` in `rtsp.conf`; never read, listed or echoed |
| `ntp.servers` | 1..3 host names or IPv4 addresses separated by spaces, or empty (off) | the file `ntp`; admin key; next start of the app |
| `net.hostname`, `net.wifi_country`, `net.wifi_band` | string / string / `2.4\|5` (default 2.4); country `none` (default) or two capitals | see below |

The user-file rows above are not saved by the registry: a PATCH of them is queued in RAM as ONE request (`/tmp/akam_cfg.req`, mode 0600) which the supervisor
validates again and writes into the files (all or nothing, only when the content changes, atomically; in `/config/akam` with a link in the card-less tree, or on the card).
`cfg.apply` reads `idle`, `queued` or the outcome (`ok`, `refused KEY`, `failed`). A write-only row is writable over HTTP with an admin key and is never readable, listed, in the schema or echoed.

Read-only state: `rtsp.auth` (`off|on|invalid`), `cfg.apply`, `sys.cpu_pct`, `sys.load1_x100`, `sys.mem_total_kb`, `sys.mem_avail_kb`, `sys.uptime_s`, `net.wifi_rssi`,
`net.wifi_lq` (`-1000000` = not available yet; the sample is refreshed every 30 s), `rtsp.clients`, `detect.person`, `detect.boxes` (the latest list of person boxes, JSON text, see section 6),
`night.active`, `net.apply`, `ptz.state` (`home|parked|moved|calibrating|unknown`), `ptz.pan`, `ptz.tilt` (`-1` unknown), `ptz.untrusted` (0 / 1, HTTP only: the position is doubted, a drift recalibration is due or running; moves are still allowed), `ptz.travel_since_cal` (weighted steps since the last calibration: fast steps and reversals count more, ptz-findings.md "Drift"), `ptz.drift_last_px` (`none`, then the last displacement `dx,dy` of the home view against the home reference, in QUARTER pixels of the 320 px detection frame: 4 per px), `ptz.drift_ref` (`none` | `day` | `night` | `both`: the drift reference held in RAM) and `ptz.drift_ref_age_s` (its age, `-1` when none), `ptz.drift_last` (`none`, then the verdict of the last home-view check: `ok`, `shift`, `recal`, `inconc`, `skip` (nothing to compare), `skip-flat` (flat view where the reference had detail: a tilt re-home, or a recalibration when the re-home is off, is scheduled), `flat-persist` (still flat after a completed re-home: not a slip, nothing more is done)), `ptz.drift_corrections` (counters corrected by the scene since the start), `ptz.recal_reason` (`none`, or why the last drift-triggered recalibration was scheduled), `ptz.rehome_count` / `ptz.rehome_cancel` (tilt-only re-homes completed / interrupted since the start; a person or a command cancels one), `ptz.rehome_last_s` (`none`, or the uptime seconds at which the last re-home completed) and `ptz.rehome_last_reason` (`none`, `gap` = enough travel since the last one, `flat` = scheduled by a flat home view), `sys.flash_writes_ptzpos` (always 0: the position lives in RAM), `sys.flash_writes_scene` (home reference files), `sys.flash_writes_store` (`/config` writes since the start, by source), `sys.flash_writes_button` (writes of `/config/akam/button` and `/config/akam/modes`), `button.last_gesture` (`none` until the first press, then the gesture name: `short`, `double`, `S S L`, ...), `api.keys`.

Persistence: `/config/akam/settings` (`key=value`, only keys that differ from the default), saved by the settings worker within a second of a
change, limited to a burst of 4 writes then one per 30 s (flash wear), loaded at boot after `akam.conf`
(default < conf < settings file; an invalid file is ignored as a whole and logged).

`net.*`: the app never spawns a process, so a change writes one request line into the RAM file `/tmp/akam_net.req`; the supervisor
(`own_app_run.sh`) hands it to `akam_net.sh` (validation, persistence, Wi-Fi dead-man) within about 5 s. The reply says `applied` when the
request was queued, NOT that it succeeded: `GET` shows the new value when the script finished (Wi-Fi: after the dead-man confirmed it,
at most 60 s), and the read-only state `net.apply` says what became of the last request (`queued`, `pending`, `ok`, `refused`,
`failed`, `reverted`, else `idle`). The API refuses at once (422 `check`) what `akam_net.sh` refuses: band `5` while the country is
`none`, and country `none` while on band 5. Defaults without a net file: country `none`, band `2.4`. One request at a time (a
second one is refused until the first was taken). Do not change the band while the router is restarting.

Not in the registry yet (their live setters do not exist, phase 6): `mqtt.*`, `rtsp.streamN.enabled`, `audio.enabled`, `detect.enabled`,
`night.mode`, `ptz.park/privacy/cal`, `zones.*`, tracking areas, `button.long_ms`. No secret setting exists; a secret would be
refused over HTTP (spec, "Deferred: payload encryption").

## 5. Actions (`POST /actions/<name>`, write scope)

`ptz.home`, `ptz.park`, `ptz.recalibrate` (no arguments); `ptz.step {"dir":"left|right|up|down"}` (one step, like the MQTT buttons);
`ptz.move {"dx":-400..400,"dy":-400..400}` (relative motor steps, at most 6 per second, else 429 `rate`; 409 `unavailable` when the motor
is not running); `ptz.vel {"pan":-1..1,"tilt":-1..1,"lease_ms":150..2000}` (a held CONTINUOUS move, the same as the ONVIF ContinuousMove: pan / tilt are the generic
velocity, fractions must be JSON **strings** (`"0.5"`, the API's flat JSON has integers and strings only), the speed is mapped to the motor divisor (dead zone 0.05; effective per axis after the safe layer: pan alone 2, tilt alone at least 3, diagonal at least 3 per axis, started by a speed ladder; see docs/onvif.md),
`lease_ms` default 450 is the dead-man time: the head stops by itself that long after the last call, a repeat refreshes it, both axes 0 = stop; 409 `unavailable` while parked, calibrating or the motor is down;
soft limits as for tracking; it preempts a tracking move like a manual nudge). **Own rate budget**, outside the 0.4/s of the other signed requests: 5 per second per peer, 8 per second overall (429 `rate`, `Retry-After`), because a held move needs a call every
150..450 ms and the nonce cache of the normal budget (sized for 0.4/s) could not hold them. It is still signed (a valid key with the write scope, scope 403), the per-peer lockout and flood rules apply, the bucket is spent only after the signature
passed (a flood without a key costs the others nothing), and it has its **own nonce cache and the shortest timestamp window (10 s)**: the camera clock must be set (the challenge mode of `/hello` is for the slow budget only) and a full cache refuses (429 `busy`), it never evicts a live nonce.
A working call is not written to the audit log (it would flood the ring and the flash); refusals are. Compile time (`A2K_API_FAST_ACTION`, 2K only: the 1080p binary does not have it).
ONVIF is the preferred way (no Akam change needed); the same motion over MQTT: `BASE/ptz_vel/set` (ptz-findings.md, MQTT). `onvif.enabled` / `onvif.port` (settable, see above) and `onvif.state` (read only) report the ONVIF service, see [docs/onvif.md](../../onvif.md);
`settings.reset {"key":"led.mode"} or `{"prefix":"led."}` (back to the defaults); `settings.save` (save now).
PTZ patrol (see [specs/ptz-patrol.md](../../../specs/ptz-patrol.md)): `ptz.patrol.start {"route":"<name>"}` (404 `not_found`, 409 `unavailable` when the
position is unknown, calibrating or the video is withheld), `ptz.patrol.stop` (idempotent), `ptz.goto_preset {"name":"<preset>"}` (a manual move: it stops a
running patrol).
`mode.apply {"name":"<mode>"}` applies a named mode (section 5.2): 404 `not_found` (no such mode), 422 `args`, 422 `mode_refused` (the registry refused a pair: nothing was applied).
There is **no reboot action**.

### 5.2 The programmable button: `/akam/v1/button` and `/akam/v1/modes` (read key to read, write key to change)

The front button turns every press pattern into a gesture (`short`, `double`, `triple`, `long`, `very_long` or a sequence of up to 6 presses such as `S S L`;
timing and rules: [button-and-zones.md](button-and-zones.md)) and runs the actions bound to it.

| Request | Effect |
|---|---|
| `GET /button` | the whole config: `{"timing":{"debounce_ms":50,"short_max_ms":1000,"gap_ms":400,"long_ms":2000,"very_long_ms":5000},"led_ack":true,"bindings":[{"gesture":"short","actions":["none"]},{"gesture":"long","actions":["ptz.toggle_park"]}]}` (this is the default: today's button) |
| `POST /button` (the same JSON) | replaces the whole config, validated as a whole: up to 16 bindings, 4 actions each, no gesture twice, every action from the whitelist, every `mode` it names must exist, timing in range; any error applies nothing. `timing` and `led_ack` may be left out (defaults), `bindings` is required |
| `GET /modes`, `GET /modes/<name>` | the modes (up to 8): `{"name":"night","settings":{"led.mode":"off","image.brightness":"30"}}` |
| `POST /modes/<name>` `{"settings":{"key":value,...}}` (or `POST /modes` with `"name"` in the body) | create or replace; 1 to 8 pairs; values are strings, integers or booleans |
| `DELETE /modes/<name>` | 404, or 409 `in_use` while a binding names it |

Actions (text, one per array element): `none`, `ptz.home`, `ptz.park`, `ptz.toggle_park`, `ptz.recalibrate`, `ptz.privacy on|off|toggle` (privacy = the head parked), `ptz.goto_preset <name>`,
`track on|off|toggle`, `track.lost_policy <value>`, `patrol start <route>`, `patrol stop`, `led pattern <on|off|N:on_ms:off_ms:pause_ms|ack|blink|fast|slow>`, `mode <name>`. Nothing else is
accepted: there is no reset, key, network or reboot action. A mode may contain only registry rows that are writable, not secret, applied live and listed by `GET /schema`
(never `net.*` or `api.*`); it is applied all or nothing (every pair is validated first, a failed setter puts the earlier ones back).
Errors: 400 `syntax`, 405, 409 `in_use`, 413 `too_big` (the text must fit 3900 bytes), 422 `value` `limit` `no_such_mode`, 429 `rate` (config writes: burst 8, then one per 10 s).
Persistence: `/config/akam/button` and `/config/akam/modes`, written only when a request really changes them (`sys.flash_writes_button`); pressing the button never writes.
Every gesture is an event `button` (section 6) and an MQTT message on `BASE/button`.

### 5.1 PTZ presets and patrol routes (`/akam/v1/ptz/...`, read key to read, write key to change)

| Request | Effect |
|---|---|
| `GET /ptz/presets`, `GET /ptz/presets/<name>` | list (max 16), one |
| `POST /ptz/presets` `{"name","pan","tilt"}` or `{"name","here":true}`; `POST /ptz/presets/<name>` `{"pan","tilt"}` | create or replace (`here` records the current position) |
| `DELETE /ptz/presets/<name>` | 409 `in_use` while a route uses it |
| `GET /ptz/routes`, `GET /ptz/routes/<name>` | summaries (max 8, with `running` and the default route), the full step list |
| `POST /ptz/routes` `{"name","mode":"once\|loop\|pingpong","steps":[{"preset"\|"pan","tilt","speed","dwell_ms"}]}` | create or replace, all or nothing (max 32 steps) |
| `POST /ptz/routes` `{"name","mode","pattern":{"type":"sweep_h\|sweep_v\|raster",...}}` | a generated pattern, expanded into steps when stored |
| `DELETE /ptz/routes/<name>` | 409 `running`, 409 `in_use` (the default route) |

Positions are motor steps inside the soft range (422 `range` outside), `speed` is a divisor 3..20 for a patrol (smaller = faster, 0 = the default 3; the park position +-15 is refused), `dwell_ms` 0..3600000.
Errors: 400 `bad_json`, 404 `not_found`, 409 `in_use` `running` `full`, 413 `too_big` (the whole config must fit 4096 bytes), 422 `bad_name` `range` `speed` `dwell`
`bad_mode` `no_preset` `too_many_steps` `empty` `bad_pattern`, 429 `rate` (config writes: burst 8, then one per 10 s). A change writes `/config/akam/ptz_patrol`
once (counted in `sys.flash_writes_patrol`); a running patrol writes nothing. Settings: `ptz.patrol.idle_start_s` (0 = off, the default; under 30 s counts as 30), `ptz.patrol.route_default` and `ptz.patrol.travel_budget` (steps; a patrol stops with reason `travel` past it); state: `ptz.patrol.state` (`idle|running|paused`), `ptz.patrol.route`, `ptz.patrol.step`. Events: type `patrol`, data
`{"event":"started|step|interrupted|resumed|stopped","route":..,"step":N[,"reason":..]}`.

## 6. Events (push without MQTT)

`GET /events` without a cursor returns the current sequence number and no events. `GET /events?since=N&timeout=20` returns at once when
there are events with `seq > N`, else **holds the connection** (up to `timeout` seconds, max 25) and answers when one arrives; at the deadline
it answers with an empty list. Reply: `{"seq":N,"missed":false,"events":[{"seq":..,"up_s":..,"type":"button","data":{..}}]}`; `missed:true` when
more than 32 events were lost between two polls (re-read `/state`). A cursor newer than the camera's (after an app restart) is clamped
to now.

| `type` | `data` | when |
|---|---|---|
| `button` | `{"gesture":"double","actions":["mode night","ptz.home"],"result":"ok"}` | every gesture of the front button, bound or not (`result` `ok`, `partial`, `failed`, `unbound`; an action list that would not fit gets `"more":N`). With the default bindings a short press is `{"gesture":"short","actions":["none"],"result":"ok"}` |
| `person` | `{"present":true,"score":812}` | presence changed (person detection is the motion source) |
| `night` | `{"night":true,"ir_on":true}` | day/night switch |
| `ptz` | `{"state":"parked","known":true,"pan":..,"tilt":..}` | motor state change |
| `setting` | `{"keys":"led.mode,..."}` or `{"n":5}` | a PATCH changed something (re-read `/settings`) |

### Person boxes: the `detections` event

| `type` | `data` | when |
|---|---|---|
| `detections` | `{"ts_ms":1700000000123,"w":320,"h":192,"pan":1200,"tilt":340,"boxes":[{"x":312,"y":104,"w":125,"h":520,"score":900,"id":0,"target":true,"ignored":false}]}` | the person boxes of the detector changed, or every second while a person is in view; one `"boxes":[]` when the last person has left |

- `x y w h`: the box in per-mille of the frame (0..1000; `w` and `h` are the 320x192 detector frame, which has the full field of view). `score` 0..1000.
  `id` stays the same while the person stays in view (a new number for a new person, 0..999, wraps). `target` is true for the person the tracker follows
  (`track.enabled`). `ignored` is true when the person stands in an ignore zone: the box is still published, the flag lets the client decide.
  `ts_ms` is the camera's wall clock in ms (a small number while the clock is not set); `pan` and `tilt` (motor steps) are those of the moment, and absent while unknown.
- Rate: `detect.publish_hz` (default 2, 0 = nothing at all, never above 5 a second), only on a change (a box moved by more than 1.5 % of the frame,
  another number of persons, a flag) or every second while somebody is there. Nothing is published, and no empty list either, while the privacy mode hides the video
  or the camera is parked.
- Detections have their **own ring of 4**: a burst of them never pushes a `button`, `ptz` or `setting` event out of the 32-event ring, and a slow poller sees at
  most the 4 newest lists (`missed:true` when it lost more). RAM only, nothing is written to flash.
- The signed-request budget (0.4 requests a second after a burst of 14) also applies to the long poll, so a client reading `/events` sees a list about every
  2.5 s at most, with the up to 4 newest in each reply. For every list use MQTT (`BASE/detections`, [mqtt.md](../../mqtt.md)); to poll without a cursor read the state key
  `detect.boxes` (the latest list, a JSON text inside the JSON string).

Example client, a long poll with `curl` and `jq` (the key variables and the signer are those of section 2):

```sh
akam_call() { # akam_call PATH [QUERY]: a signed GET, prints the body
  H=$(tools/akam-api-sign.sh GET "$1" "${2:-}") || return 1
  set --; while IFS= read -r l; do set -- "$@" -H "$l"; done <<EOF
$H
EOF
  curl -fsS --max-time 40 "$AKAM_URL$1${2:+?$2}" "$@"
}
since=$(akam_call /akam/v1/events | jq .seq)
while :; do
  r=$(akam_call /akam/v1/events "since=$since&timeout=20") || { sleep 3; continue; }
  since=$(echo "$r" | jq .seq)
  echo "$r" | jq -c '.events[] | select(.type=="detections") | .data
      | {t: .ts_ms, persons: [.boxes[] | select(.ignored | not) | {id, x, y, w, h, score, target}]}'
done
```

A Home Assistant integration needs one connection per camera in a loop on `/events` (about 1 accepted request per 20 s with the
sustained limit of 0.4/s) plus `GET /state` every 30 s.

## 7. Resource cost (measured, ARM uClibc, `-Os`)

Stripped binary `akamd`: 211,712 -> 265,024 bytes (+53,312; the spec estimate was +25 KB, its phase gate +40 KB, see the
spec section "Phase 2"). `size`: text +54 KB, bss +101 KB (mostly untouched pages: 4 x 12 KiB connection buffers, the
12 KiB reply buffer, the audit ring, the job slot), two threads (HTTP 64 KiB, settings worker 32 KiB stacks, a few KiB touched). The
port is closed and the HTTP thread sleeps in `poll` (1 s tick) without a key; the worker wakes once a second.

## 8. Tests

`tests/rot2k_control_api_test.sh` (building blocks, HTTP parser, limits, event ring, fuzz), `tests/rot2k_control_api_http_test.sh`
(the real server on loopback with fake hardware, driven by curl and the reference signer, under ASan/UBSan: auth, replay, hygiene,
slow client, caps, scopes, keys, settings, actions, events, persistence, lockout, challenge flow), plus the static scans of
`tests/rot2k_overlay_test.sh`. Camera checks to run once: [hardware-test-checklists.md](../../hardware-test-checklists.md).
