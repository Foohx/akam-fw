# Local control API of the Rotatable 2K (port 8092)

Signed, plain-HTTP JSON API inside the own app `akam2k_rtsp`, for scripts and for a native Home Assistant integration
**without an MQTT broker**. Design and threat model: [specs/control-api.md](../../../specs/control-api.md). This page is
the user and integrator reference of what phase 2 ships.

## 1. Provisioning: the first key

With **no key installed nothing listens on the port**. Create the first (admin) key from SSH:

```sh
sh /mnt/sdcard/akam/akam_api_key.sh new --scope admin --name assistant     # card build
akam2k_rtsp --api-key new --scope admin --name assistant                   # what the script runs (RAM copy in /tmp)
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
| `image.brightness`, `image.contrast`, `image.sharpness` | 0..100 | ISP picture, RAM only (reset by a restart) |
| `image.mains_hz` | `50\|60` | RAM only |
| `track.enabled` | bool | person tracking; persisted |
| `video0.*`, `video1.*`: `kbps`, `max_kbps` (0 = auto), `fps`, `min_qp`, `max_qp`, `gop`, `rc` (`vbr\|cbr\|avbr`) | int / enum | ranges from `akam2k_video.h`; min QP <= max QP and peak >= target are checked for the whole PATCH; the capture thread applies live or re-opens that one channel; stored by the video module in `/config/akam/video` |
| `net.hostname`, `net.wifi_country`, `net.wifi_band` | string / string / `2.4\|5` (default 2.4); country `none` (default) or two capitals | see below |

Read-only state: `sys.cpu_pct`, `sys.load1_x100`, `sys.mem_total_kb`, `sys.mem_avail_kb`, `sys.uptime_s`, `net.wifi_rssi`,
`net.wifi_lq` (`-1000000` = not available yet; the sample is refreshed every 30 s), `rtsp.clients`, `detect.person`,
`night.active`, `net.apply`, `ptz.state` (`home|parked|moved|calibrating|unknown`), `ptz.pan`, `ptz.tilt` (`-1` unknown), `api.keys`.

Persistence: `/config/akam/settings` (`key=value`, only keys that differ from the default), saved by the settings worker within a second of a
change, limited to a burst of 4 writes then one per 30 s (flash wear), loaded at boot after `akam2k.conf`
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
is not running); `settings.reset {"key":"led.mode"}` or `{"prefix":"led."}` (back to the defaults); `settings.save` (save now).
There is **no reboot action**.

## 6. Events (push without MQTT)

`GET /events` without a cursor returns the current sequence number and no events. `GET /events?since=N&timeout=20` returns at once when
there are events with `seq > N`, else **holds the connection** (up to `timeout` seconds, max 25) and answers when one arrives; at the deadline
it answers with an empty list. Reply: `{"seq":N,"missed":false,"events":[{"seq":..,"up_s":..,"type":"button","data":{..}}]}`; `missed:true` when
more than 32 events were lost between two polls (re-read `/state`). A cursor newer than the camera's (after an app restart) is clamped
to now.

| `type` | `data` | when |
|---|---|---|
| `button` | `{"press":"short"}` | short press of the front button |
| `person` | `{"present":true,"score":812}` | presence changed (person detection is the motion source) |
| `night` | `{"night":true,"ir_on":true}` | day/night switch |
| `ptz` | `{"state":"parked","known":true,"pan":..,"tilt":..}` | motor state change |
| `setting` | `{"keys":"led.mode,..."}` or `{"n":5}` | a PATCH changed something (re-read `/settings`) |

A Home Assistant integration needs one connection per camera in a loop on `/events` (about 1 accepted request per 20 s with the
sustained limit of 0.4/s) plus `GET /state` every 30 s.

## 7. Resource cost (measured, ARM uClibc, `-Os`)

Stripped binary `akam2k_rtsp`: 211,712 -> 265,024 bytes (+53,312; the spec estimate was +25 KB, its phase gate +40 KB, see the
spec section "Phase 2"). `size`: text +54 KB, bss +101 KB (mostly untouched pages: 4 x 12 KiB connection buffers, the
12 KiB reply buffer, the audit ring, the job slot), two threads (HTTP 64 KiB, settings worker 32 KiB stacks, a few KiB touched). The
port is closed and the HTTP thread sleeps in `poll` (1 s tick) without a key; the worker wakes once a second.

## 8. Tests

`tests/rot2k_control_api_test.sh` (building blocks, HTTP parser, limits, event ring, fuzz), `tests/rot2k_control_api_http_test.sh`
(the real server on loopback with fake hardware, driven by curl and the reference signer, under ASan/UBSan: auth, replay, hygiene,
slow client, caps, scopes, keys, settings, actions, events, persistence, lockout, challenge flow), plus the static scans of
`tests/rot2k_overlay_test.sh`. Camera checks to run once: [hardware-test-checklists.md](../../hardware-test-checklists.md).
