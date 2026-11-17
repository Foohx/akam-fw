# Settings registry and control API of the 1080p (`src/ak_rtsp`)

The LSC Smart Indoor 1080p has the same remote control as the Rotatable 2K, within its own hardware: one **settings registry**
(`src/ak_rtsp/ak1080_settings.c`, the generic engine is `src/common/api/settings_reg.c` shared with the 2K) in front of the
modules that own the settings, reachable through two doors:

| Door | Port | Who | Rules |
|---|---|---|---|
| Text control port (the one the assistant and `./akam` use) | 8091 | LAN, read-only unless loopback or `DEBUG on` | unchanged: `LIST GET SET DEBUG ONVIF MQTT QUIT`, old key names, old replies. Keys it does not know are offered to the registry (section 2) |
| Signed HTTP API (same protocol, keys, scopes, audit and rate limits as the 2K, see [../lsc-rotatable-2k/control-api.md](../lsc-rotatable-2k/control-api.md) and [specs/control-api.md](../../../specs/control-api.md)) | 8092 | LAN, HMAC key | opens only while an API key exists (section 3) |

The registry has no file of its own (the 2K's `/config/akam/settings` does not exist here): every row has `persist=0` in the
registry sense and the module of the row keeps its own file under `/etc/config/akam`, written only when a user changes the value
(skip-if-equal in each module). There is no periodic write.

## 1. Rows

Keys follow the 2K wherever the concept is the same. A different scale is told by the schema (`GET /schema`: bounds, options,
default), not by a different name. "8091 name" is the key the text port has always had for it (kept, byte for byte).

### Settings (writable, `write` scope)

| Key | Type, range | Default | Apply | 8091 name | Owner and file |
|---|---|---|---|---|---|
| `led.mode` | `auto\|on\|off` | `on` | live | `led.status` | `leds.c`, `/etc/config/akam/leds` |
| `led.stream`, `led.boot`, `led.nowifi`, `led.night`, `led.idle` | pattern `on\|off\|N:on_ms:off_ms:pause_ms` (N 1..10, times 50..60000 ms, pause 0 or 50..60000, cycle at most 120 s) | `1:1000:1000:0`, `1:500:500:0`, `1:500:500:0`, `on`, `on` | live | (new: registry name only) | `leds.c`, `led_<rule>=` lines of the same file, only while not the default |
| `night.ir_led` | `auto\|off` | `auto` | live | `night.ir_led` | `leds.c` (`ircut.c` applies it) |
| `night.mode` | `auto\|day\|night` | `auto` | live | `night.mode` | `night.c`, `/etc/config/akam/night` |
| `image.brightness`, `image.contrast`, `image.saturation`, `image.sharpness` | int -50..50 | 0, 0, 0, -15 | live | `isp.brightness` ... `isp.sharpness` | `control.c`/`isp_tune.c`, `/etc/config/akam/picture` |
| `image.denoise` | int 0..4 (3D-NR bias, 0 = stock tables) | 2 | live | `isp.denoise` | same |
| `video0.kbps` | int 500..3000 (VBR+ ceiling, kbit/s) | 1536 | restart_component | `video.bitrate_cfg` | `video_ctl.c`, `/etc/config/akam/video` |
| `video0.fps` | `10\|15\|20\|25` (30 is refused: dark, grey picture) | 15 | restart_component | `video.fps_cfg` | same |
| `video0.gop` | int 1..4 (key frame interval in SECONDS; the 2K row is in frames) | 2 | restart_component | `video.gop_cfg` | same |
| `video0.min_qp`, `video0.max_qp` | int 10..51, `min_qp < max_qp` over the whole PATCH | 28, 43 | restart_component | `video.minqp_cfg`, `video.maxqp_cfg` | same |
| `onvif.enabled` | bool | 0 | live (the service is reloaded) | `ONVIF reload` after editing the flag file | `onvif_cfg.c`; keeps the saved port (default 80); enabling is refused unless the service can start (the stream password is set, as `akam_onvif.sh` refuses) and the previous file is put back |
| `mqtt.enabled` | bool | 0 | live (the service is reloaded) | `MQTT reload` | `mqtt_svc.c`; flips `enabled=` in an existing valid `/etc/config/akam/mqtt` only: no file = refused (configure the broker with `akam_mqtt.sh` first); the password never goes through the API |

`restart_component` means: saved now, read by `ak_rtsp` at its next start (the kernel encoder has no proven live setter yet,
convergence P10). The state row `video0.cfg_pending` is 1 while the saved file differs from what runs. There is no restart action.

### State (read only, `read` scope)

| Key | Meaning |
|---|---|
| `sys.cpu_pct`, `sys.load1_x100`, `sys.mem_total_kb`, `sys.mem_avail_kb`, `sys.uptime_s` | `hal_sys` (same code and meaning as the 2K), sampled when read, at most once a second; `-1000000` = not known |
| `net.wifi_rssi` (dBm), `net.wifi_lq` | Wi-Fi level of `wlan0` |
| `rtsp.clients`, `rtsp.auth` (`off\|on\|broken`), `rtsp.skipped_frames` | RTSP slot and credentials state; no secret, no user name |
| `video0.fps_x10`, `video0.bitrate_kbps` | measured over the last 5 s while a client plays (`-1000000` / 0 otherwise) |
| `video0.cfg_pending` | 1 when the saved video settings are not what runs |
| `night.active` | 1 in night mode (IR-cut on) |
| `onvif.state`, `mqtt.state` | `off`, `on:<port>`, `no_password`, `error:...` / `off`, `connecting`, `connected`, `error:...` |
| `sys.time_set` | 1 when the wall clock is believable (the camera has no RTC) |
| `sys.version` | firmware identity (`b<build>-<git>`) |
| `net.hostname` | host name (read only here: change it with `akam_hostname.sh`) |
| `net.ntp` | the servers of `/etc/config/akam/ntp` ("" while NTP is off; change with `akam_ntp.sh`) |
| `api.keys` | number of API keys |

Identity data (`net.ssid`, `net.mac`, `net.ip`) is NOT in the registry: it stays on the 8091 port with its own remote-peer rule.

### Absent on purpose (hardware or radio the 1080p does not have)

`ptz.*`, `track.*`, `detect.*`, `zones.*`, `button.*`, `net.wifi_band`, `net.wifi_country` (2.4 GHz only), `image.wdr`,
`image.mains_hz`, `video1.*` (one encoder channel). They are not stubs: the 1080p binary has none of that code
(`src/common/features.mk`: `AKAM_FEAT_PTZ/BUTTON/SCENE/PATROL/WIFI5G/TRACK = 0`). `ae.*`, the night thresholds and the `img.*`
tuning keys stay on the 8091 port only.

## 2. The 8091 adapter

`control.c` answers every key it knows exactly as before (names, values, error texts, the read-only rule, `LIST`). A key it does
not know is offered to the registry under its registry name: `GET led.mode`, `SET led.stream 3:250:250:2000`, `GET video0.kbps`,
`SET image.contrast -5`, `SET onvif.enabled 1`. Replies: `led.mode=auto`, `OK image.contrast=-5`, `OK video0.kbps=2000 (restart to apply)`,
`ERR invalid led.stream value x (check)`, `ERR sys.cpu_pct is read-only`, else `ERR unknown param <key>`. A loopback write is saved; a
network write (only with `DEBUG on`) keeps the picture in RAM exactly as the old keys do. `LIST` is unchanged (the six `sys.*` keys of
step P4 are the only addition, at the end of the metrics). Honest scope of "adapter": the old 8091 code is still the writer of the
keys it knows and the registry rows call into it (`control_apply_setting`), so a value has one writer whichever door it came through;
`control.c` was not rewritten onto the table, which would have changed its error texts for nothing.

## 3. The signed API on port 8092

Same protocol as the 2K ([the 2K reference](../lsc-rotatable-2k/control-api.md) sections 2, 3 and 6, [specs/control-api.md](../../../specs/control-api.md)):
HMAC-SHA256 signature of the request, replay cache, per-peer lockout, global accepted-request limit (burst 14 then 0.4 a second), scopes
`read` / `write` / `admin`, up to 8 keys, `GET /hello /info /schema /settings /settings/<key> /state /events /audit /keys`,
`PATCH /settings`, `POST /actions/<name>`, `POST /keys`, `DELETE /keys/<id>`, JSON errors, `tools/akam-api-sign.sh` as the reference signer.
It is the SAME code: `src/common/api/api_core.c` (routing, verification order, endpoints, keys, audit), `api_httpd.c` (the socket side),
`net/http_req.c` (the strict head parser), `api_sig.c api_keys.c api_limit.c api_events.c settings_reg.c json_min.c`. The 2K binds it in
`akam2k_apibind.c`, the 1080p in `src/ak_rtsp/ak1080_api.c` (+ `ak1080_settings.c`).

What is specific to the 1080p:

| | 1080p |
|---|---|
| Opens | **only while a key exists** in `/etc/config/akam/api_keys` (mode 0600). No key: no listener. A key file that others can read, or an invalid one, keeps it closed (the file is kept). Created or revoked keys open and close the port within about a second, without a restart |
| First key | over SSH: `ak_rtsp --api-key new --scope admin --name assistant` (prints the id and the 64 hex secret ONCE), also `list` and `revoke ID\|NAME [--force]`. Runs before any hardware is touched, so it is safe while the camera streams. The assistant gets `akam api-key` later (the tool is the on-camera half) |
| Threads | ONE HTTP thread (64 KiB stack, 1 s tick that stats the key file). No worker thread: a PATCH or an action runs inline on the HTTP thread (the setters are the 8091 ones). Connection buffers (12 KiB) are malloc'd at accept and freed at close; 4 connections, 3 per peer |
| Flash | settings are saved by the modules, only when a value changes. A PATCH or `settings.reset` spends one token of a flash limiter (burst 6, then one per 20 s): when empty it is `429 flash_rate` with `Retry-After`, nothing applied (a refused PATCH gives its token back). The audit file `/etc/config/akam/audit` holds the last 20 WRITES, written at most twice at once then one per 30 s, never for reads. No periodic write of any kind |
| Settings | the rows of section 1; `GET /schema` lists them with bounds, options, defaults, apply mode and scope |
| Actions | `led.blip {"pattern":"3:200:200:500","ms":5000}` (find the camera: a transient LED pattern for 1..60 s, ignored while `led.mode=off`), `led.blip_stop`, `settings.reset {"key":"led.mode"}` or `{"prefix":"led."}` (back to the defaults as ONE patch). No `ptz.*`, no `mode.apply`, no `settings.save` (the modules save), no reboot action |
| Routes | no `/ptz/...`, `/button`, `/modes`: they are 404 `unknown_path` (that code is not compiled) |
| Events (`GET /events`) | `night` `{"night":true,"ir_on":true}` (every day/night switch), `setting` `{"keys":"a,b"}` or `{"n":3}` (a PATCH or reset changed something). There is no `person`/`detections`/`button`/`ptz` event: the 1080p has no detector, button or motor |
| Info | `GET /info` says `"model":"lsc-smart-indoor-1080p"` and the firmware identity |

```sh
# over SSH, as root (no helper script on the 1080p yet):
ak_rtsp --api-key new --scope admin --name assistant
export AKAM_KEY_ID=… AKAM_KEY_SECRET=… AKAM_URL=http://192.0.2.10:8092
H=$(tools/akam-api-sign.sh GET /akam/v1/state)   # then curl -H, see the 2K reference section 2
```

Budget measured on the pinned zig build (stripped `ak_rtsp`, against `dev` 639d44a): see the golden file `tests/golden/ak_rtsp_1080p.sha256`
(every step records its delta). The RSS on a camera is **not measured yet** (checklist B in `docs/hardware-test-checklists.md`).

## 4. Tests

`tests/ak1080_settings_test.sh` (registry against stubs and the real LED engine, 163 checks), `tests/ak1080_api_http_test.sh` (the real server
on loopback with fake hardware, driven by curl and the reference signer, ASan/UBSan, 119 checks: closed without a key, key tool, scopes,
schema and the 41 rows, PATCH reaching the setters, actions, events, flash limiter, audit file, port follows the key file, no 2K-only code),
`tests/control_test.sh` (the 8091 contract plus the registry adapter), `tests/leds_oracle_test.sh` (LED engine against the old one),
`tests/ctl_sys_test.sh`. The generic protocol (hygiene, replay, lockout, slow clients, caps, challenge flow, 357 checks) is covered once, on
the shared code, by `tests/rot2k_control_api_http_test.sh` and `tests/rot2k_control_api_test.sh`.
