# Settings registry and control API of the 1080p (`platforms/anyka-ak3918`)

The LSC Smart Indoor 1080p has the same remote control as the Rotatable 2K, within its own hardware: one **settings registry**
(`firmware/lsc-indoor-1080p-ak3918/settings.c`, the generic engine is `src/core/settings_reg.c` shared with the 2K) in front of the
modules that own the settings, reachable through two doors:

| Door | Port | Who | Rules |
|---|---|---|---|
| Text control port (on-camera scripts, the PC tuning tool, monitoring) | 8091 | **127.0.0.1 only** by default; the saved opt-in `control.lan` opens it to the LAN (then read-only from the network unless `DEBUG on`, a loopback command) | unchanged: `LIST GET SET DEBUG ONVIF MQTT QUIT`, old key names, old replies. Keys it does not know are offered to the registry (section 2). The assistant does not use it |
| Signed HTTP API (same protocol, keys, scopes, audit and rate limits as the 2K, see [../lsc-rotatable-2k/control-api.md](../lsc-rotatable-2k/control-api.md) and [specs/control-api.md](../../../specs/control-api.md)) | 8092 | LAN, HMAC key | opens only while an API key exists (section 3). **The assistant (`./akam`) uses only this door**, for both cameras |

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
| `img.wb` | `conf\|manual` (white balance: the stock block of `sensor_isp.conf`, or Akam's own) | `conf` | live | `img.wb` | `imgq.c`, `/etc/config/akam/image` |
| `audio.out_volume` | int 0..100 | 100 | live | `audio.out_volume` | `audio_out.c`, `/etc/config/akam/audio_out` |
| `audio.out_enable`, `audio.talk_enable` | bool | 1, 1 | live (`audio.out_enable` 1 answers `restart_component` when the device cannot be opened now) | `audio.out_enable`, `audio.talk_enable` | same |
| `audio.mic_enable` | bool (off = no audio track in the stream) | 0 | restart_component (read at start) | `audio.mic_enable` | same |
| `control.lan` | bool: the text port 8091 listens on the LAN too | 0 | restart_component (bound when `akamd` starts) | none (registry only) | `control.c`, `/etc/config/akam/control_lan` (file present = on; written only on a change) |
| `image.flip`, `image.mirror`, `image.ceiling` | bool 0\|1 (vertical, horizontal, both; ceiling reads 1 only while both are on) | 0 | live (sensor readout flip through ISP tunnel command `0x40044979`, no restart) | the same names (rows with no old 8091 name; local only, a network write in DEBUG mode is refused) | `image_orient.c`, `/etc/config/akam/orient` (written only on a change, removed when both are off). **Not yet hardware-validated** |
| `video0.kbps` | int 500..3000 (VBR+ ceiling, kbit/s) | 1536 | restart_component | `video.bitrate_cfg` | `video_ctl.c`, `/etc/config/akam/video` |
| `video0.fps` | `10\|15\|20\|25` (30 is refused: dark, grey picture) | 15 | restart_component | `video.fps_cfg` | same |
| `video0.gop` | int 1..4 (key frame interval in SECONDS; the 2K row is in frames) | 2 | restart_component | `video.gop_cfg` | same |
| `video0.min_qp`, `video0.max_qp` | int 10..51, `min_qp < max_qp` over the whole PATCH | 28, 43 | restart_component | `video.minqp_cfg`, `video.maxqp_cfg` | same |
| `onvif.enabled` | bool | 0 | live (the service is reloaded) | `ONVIF reload` after editing the flag file | `onvif_cfg.c`; keeps the saved port (default 80); enabling is refused unless the service can start (the stream password is set, as `akam_onvif.sh` refuses) and the previous file is put back |
| `mqtt.enabled` | bool | 0 | live (the service is reloaded) | `MQTT reload` | `mqtt_svc.c`; flips `enabled=` in an existing valid `/etc/config/akam/mqtt` only: no file = refused (configure the broker with `akam_mqtt.sh` first); the password never goes through the API |

A key whose value took effect but whose file could not be written answers `"saved":false` in the PATCH reply (the value is lost at the next reboot; the assistant says so, as the text port's "not saved" note did). The 2K saves its registry file later from a worker and retries, so its replies never carry the flag.

`restart_component` means: saved now, read by `akamd` at its next start (the kernel encoder has no proven live setter yet,
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
| `net.ssid` | the name of the Wi-Fi network the camera is on (what any radio in range sees; the MAC and the address are not in the API) |
| `net.wifi_rate` | link rate, Mb/s (`-1000000` = not known) |
| `led.state` | what the front LED engine does now (`idle`, `solid`, ...) |
| `audio.out_state`, `audio.out_underruns`, `audio.out_drops` | the speaker's state and counters |

Identity data other than the network name (`net.mac`, `net.ip`) is NOT in the registry: it stays on the 8091 port with its own remote-peer rule.

### Absent on purpose (hardware or radio the 1080p does not have)

`ptz.*`, `track.*`, `detect.*`, `zones.*`, `button.*`, `net.wifi_band`, `net.wifi_country` (2.4 GHz only), `image.wdr`,
`image.mains_hz`, `video1.*` (one encoder channel). They are not stubs: the 1080p binary has none of that code
(`src/build/features.mk`: `AKAM_FEAT_PTZ/BUTTON/SCENE/PATROL/TRACK = 0`). `ae.*`, the night thresholds and the `img.*`
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
It is the SAME code: `src/api/api_core.c` (routing, verification order, endpoints, keys, audit), `api_httpd.c` (the socket side),
`net/http_req.c` (the strict head parser), `api_sig.c api_keys.c api_limit.c api_events.c settings_reg.c json_min.c`. The 2K binds it in
`api_bind.c`, the 1080p in `firmware/lsc-indoor-1080p-ak3918/api_bind.c` (+ `firmware/lsc-indoor-1080p-ak3918/settings.c`).

What is specific to the 1080p:

| | 1080p |
|---|---|
| Opens | **only while a key exists** in `/etc/config/akam/api_keys` (mode 0600). No key: no listener. A key file that others can read, or an invalid one, keeps it closed (the file is kept). Created or revoked keys open and close the port within about a second, without a restart |
| First key | over SSH: `akamd --api-key new --scope admin --name assistant` (prints the id and the 64 hex secret ONCE), also `list` and `revoke ID\|NAME [--force]`. Runs before any hardware is touched, so it is safe while the camera streams. The assistant runs exactly this over SSH for you (`./akam api-key`, also offered at the end of an install and at the first settings command of an existing install; `replace` revokes `assistant` first) and keeps the secret in its state folder (0600). It never prints it. An update does not touch `/etc/config/akam/api_keys` |
| Threads | ONE HTTP thread (64 KiB stack, 1 s tick that stats the key file). No worker thread: a PATCH or an action runs inline on the HTTP thread (the setters are the 8091 ones). Connection buffers (12 KiB) are malloc'd at accept and freed at close; 4 connections, 3 per peer |
| Flash | settings are saved by the modules, only when a value changes. A PATCH or `settings.reset` spends one token of a flash limiter (burst 6, then one per 20 s): when empty it is `429 flash_rate` with `Retry-After`, nothing applied (a refused PATCH gives its token back). The audit file `/etc/config/akam/audit` holds the last 20 WRITES, written at most twice at once then one per 30 s, never for reads. No periodic write of any kind |
| Settings | the rows of section 1; `GET /schema` lists them with bounds, options, defaults, apply mode and scope |
| Actions | `led.blip {"pattern":"3:200:200:500","ms":5000}` (find the camera: a transient LED pattern for 1..60 s, ignored while `led.mode=off`), `led.blip_stop`, `settings.reset {"key":"led.mode"}` or `{"prefix":"led."}` (back to the defaults as ONE patch). No `ptz.*`, no `mode.apply`, no `settings.save` (the modules save), no reboot action |
| Routes | no `/ptz/...`, `/button`, `/modes`: they are 404 `unknown_path` (that code is not compiled) |
| Events (`GET /events`) | `night` `{"night":true,"ir_on":true}` (every day/night switch), `setting` `{"keys":"a,b"}` or `{"n":3}` (a PATCH or reset changed something). There is no `person`/`detections`/`button`/`ptz` event: the 1080p has no detector, button or motor |
| Info | `GET /info` says `"model":"lsc-smart-indoor-1080p"` and the firmware identity |

```sh
# over SSH, as root (no helper script on the 1080p yet):
akamd --api-key new --scope admin --name assistant
export AKAM_KEY_ID=… AKAM_KEY_SECRET=… AKAM_URL=http://192.0.2.10:8092
H=$(tools/akam-api-sign.sh GET /akam/v1/state)   # then curl -H, see the 2K reference section 2
```

Budget measured on the pinned zig build (stripped `akamd`, against `dev` 639d44a): see the golden file `tests/golden/lsc-indoor-1080p-ak3918.sha256`
(every step records its delta). The RSS on a camera is **not measured yet** (checklist B in `docs/hardware-test-checklists.md`).

## 4. Tests

`tests/ak1080_settings_test.sh` (registry against stubs and the real LED engine, 163 checks), `tests/ak1080_api_http_test.sh` (the real server
on loopback with fake hardware, driven by curl and the reference signer, ASan/UBSan, 119 checks: closed without a key, key tool, scopes,
schema and the 41 rows, PATCH reaching the setters, actions, events, flash limiter, audit file, port follows the key file, no 2K-only code),
`tests/control_test.sh` (the 8091 contract plus the registry adapter), `tests/leds_oracle_test.sh` (LED engine against the old one),
`tests/ctl_sys_test.sh`. The generic protocol (hygiene, replay, lockout, slow clients, caps, challenge flow, 357 checks) is covered once, on
the shared code, by `tests/rot2k_control_api_http_test.sh` and `tests/rot2k_control_api_test.sh`.
