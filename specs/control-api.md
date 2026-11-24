# Control API: settings registry, signed local HTTP API, generated MQTT entities

Status: **built.** Both cameras ship the signed API (port 8092) and the settings registry behind it, and the assistant uses it for every setting. **Hardware-used:** on the 2K, signed requests on the camera (settings changes, flip and mirror, speaker and latency settings, `ptz.recalibrate`, state reads; 2026-11-22 and 2026-11-23, the last ones with a curl-based signed helper); **host-tested only:** the rest of the endpoints, the 1080p side (convergence P7 and P8) and the MQTT entities generated from the registry. The sections 1 to 13 and the phase sections below are the design and its history, kept as written (the first adopter was the 2K; the 1080p adopted the same modules later, section 11). Per-camera reference: [2K](../docs/porting/lsc-rotatable-2k/control-api.md), [1080p](../docs/porting/lsc-1080p/control-api.md).

## 1. Goals and non-goals

Goals:
- One **settings registry** describes every user-visible setting once. Everything else (MQTT/HA entities, HTTP API,
  `akam.conf` parsing, persistence, docs) is derived from it.
- Feature toggles (MQTT, RTSP streams, audio, detection, tracking, LEDs) change **live** where physically possible,
  without restarting the app.
- A **local, authenticated, signed HTTP JSON API** that works without SSH and without MQTT. It is the recovery
  channel: it stays up when MQTT is disabled or broken.
- Small: fits the 38 MB camera budget (section 9), no TLS dependency in v1.

Non-goals (hard limits of the API surface, enforced by the registry having no such entries):
- No flash access (no appfs/kernel/rootfs write, no firmware update, no reboot into recovery).
- No raw GPIO/pin, device-node or register access. Only named, bounded settings and named actions.
- No command execution, no file read/write by path, no shell. It is not a root shell.
- No Wi-Fi credential, RTSP/ONVIF/MQTT password or API key is ever readable through it. Passwords may be **set**
  only if a later phase adds write-only secret settings (not in v1).

## 2. Settings registry

Portable pure C (target `src/core/settings_reg.[ch]`; 2K first as `akam2k_` files until moved). No I/O of its own.

```c
typedef enum { ST_BOOL, ST_INT, ST_ENUM, ST_STRING, ST_PATTERN } st_type_t;
typedef enum { AP_LIVE, AP_RESTART_COMPONENT, AP_REBOOT } st_apply_t;
typedef struct {
  const char *key;          /* "rtsp.stream1.enabled": [a-z0-9_.]+, <= 48 bytes, unique */
  st_type_t   type;
  long lo, hi;              /* ST_INT bounds; ST_STRING: max length in hi */
  const char *const *opts;  /* ST_ENUM options, NULL-terminated; ST_PATTERN grammar id */
  const char *def;          /* default, in canonical text form */
  st_apply_t  apply;
  uint8_t     persist;      /* 1 = saved in /config/akam/settings, 0 = RAM only */
  uint8_t     access;       /* SC_READ | SC_WRITE (a read-only state value has no WRITE) */
  uint8_t     expose;       /* bit0 HTTP, bit1 MQTT; MQTT: component (switch/number/select/sensor/...), icon, unit */
  uint32_t    model_mask;   /* models that have it; filtered against the HAL descriptor at boot */
  int (*get)(const char *key, char *out, size_t cap);                    /* canonical text */
  int (*set)(const char *key, const char *val, st_apply_t *effective);   /* re-validates, applies */
  int (*check)(const char *key, const char *val);                        /* optional cross-field rule (min_qp <= max_qp) */
} st_def_t;
```

Rules:
- **Values are text in canonical form** (`0`/`1` for bool, decimal ints, enum word, pattern
  `on|off|N:on_ms:off_ms:pause_ms`). One strict parser per type (whole string, no trailing text, no overflow), shared
  by HTTP, MQTT and conf so the three can never disagree. It generalises `a2k_vid_parse`.
- **Availability**: the per-model HAL descriptor (hal-architecture layer 3) provides a capability set; a setting whose
  `model_mask` or capability is absent does not exist (HTTP 404 `unknown_key`, no MQTT entity, an empty retained
  discovery payload cleans old ones). Example: `ptz.*` only on the 2K.
- **Persistence**: one file `/config/akam/settings` (key=value lines, only keys that differ from the default, LF,
  max 4 KiB), written with the existing atomic store (`store.c`: temp file, fsync, rename). Precedence:
  default < `akam.conf` < `/config/akam/settings`. The existing `/config/akam/video` file keeps its format and is
  read as registry keys `video<N>.*` (no regression).
- **Apply modes** appear in every write reply: `applied` (live), `restart_component` (the app restarted the component
  itself, section 6), `pending_device_restart` (saved, effective at next boot; the API never reboots).
- **PATCH is validated as a whole** (each key, then each `check`) before any setter runs, then applied in registry
  order. A setter that fails after others succeeded yields per-key results; failed keys keep their old value
  (already-applied live keys are not rolled back; the video re-open rolls itself back, 6.4).

### 2.1 Initial key set (2K)

| Key | Type | Values / bounds | Apply | Persist |
|---|---|---|---|---|
| `mqtt.enabled` | bool | default 0 | live (start/stop thread) | yes |
| `mqtt.host`, `.port`, `.user`, `.base_topic`, `.interval` | string/int | as [mqtt.md](mqtt.md) section 2 | restart-component | yes |
| `rtsp.stream0.enabled`, `rtsp.stream1.enabled` | bool | default 1 | live | yes |
| `audio.enabled` | bool | default 0 | live | yes |
| `detect.enabled`, `track.enabled` | bool | track needs detect | live | yes |
| `track.return`, `track.return_delay_s` | `stay|home` (default home) / int 0..86400 (default 30) | return policy of the tracker | live | yes |
| `track.band_pct`, `track.head_pct` | int 2..40 (default 6) / int 0..50 (default 15) | pan dead band, head row (% of the frame) | live | yes |
| `track.gain_pan`, `track.gain_tilt`, `track.tilt_band_pct` | int 200..4000 (default 1000) / 200..4000 (default 700) / 2..40 (default 10) | motor steps per frame width / height of offset; the vertical dead band (the head band is half of it) | live (applied to the running tracker under its lock) | yes |
| `track.zone_pct`, `track.zone_y_pct`, `track.zone_head_pct` | int 0..90 (default 40 / 30 / 20) | comfort zone of the person tracker, % of the frame width / the height (body rule) / the height (head rule); 0 = the old targeting | live | yes |
| `track.motion` | enum `continuous\|steps` (default `continuous`) | continuous move re-targeted without a stop, or timed nudges (fallback) | live | yes |
| `track.lost_policy` | string (`none` or up to 8 of `hold:N`, `backsweep`, `exit`, `local:W:D:N`, `route:<name>[:N]`, `home`, `stay`) | what the tracker does when the person is lost; default = the legacy return policy; all or nothing | live | yes |
| `track.lost_step` | string, read only | current step of the lost chain (`idle`, `backsweep`, `exit`, `hold`, `local`, `route:<name>`, `home`, `stay`, `done`); changes are events of type `track` | live | no |
| `track.tilt_back_s` | int 0..600 (default 5) | seconds after a loss before the tilt goes back to home when it is > 250 steps above it; 0 = off | live | yes |
| `track.close_scan` | int 0..1 (default 1) | close person lost without direction evidence: scan up and down around the loss pose, then the lost policy; 0 = old WAIT then tilt back to home | live | yes |
| `track.close_scan_up` | int 0..600 (default 250) | steps above the loss tilt the close-scan goes; 0 = skip | live | yes |
| `track.close_scan_down` | int 0..600 (default 300) | steps below the loss tilt the close-scan goes; 0 = skip | live | yes |
| `track.lag_ms` | int -150..300 (default 50) | video latency of the pose-at-capture lookup (negative allowed) | live | yes |
| `led.mode`, `led.boot/nowifi/person/stream/night/idle` | enum / pattern | `auto|on|off`; patterns | live | yes |
| `video0.kbps/max_kbps/fps/min_qp/max_qp/gop/rc` (and `video1.*`) | int/enum | `video.h` ranges | live, else restart-component | yes |
| `ptz.park`, `ptz.privacy`, `ptz.cal` | pattern/bool/enum | `PAN:TILT`, 0/1, `auto|off` | live | yes |
| `zones.<name>` | pattern | `PMIN:PMAX:TMIN:TMAX`, max 4 | live | yes |
| `night.mode`, `night.ir_led`, `image.brightness/contrast/saturation/sharpness/wdr/mains_hz` | enum/int | as image spec | live | yes (picture sliders RAM by default, 7.4) |
| `button.long_ms` | int | 1500..10000 | live | yes |
| `sys.*`, `net.wifi_rssi`, `rtsp.clients`, `detect.state`, `video.fps_actual` | state | read-only | - | no |

Actions (a separate table `act_def_t {name, args schema, hook, scope}`; not settings): `ptz.home`, `ptz.park`,
`ptz.recalibrate`, `ptz.move` (bounded relative step, rate limited), `ptz.vel` (2K: a held continuous move with its own 5/s budget outside the 0.4/s one, see the 2K control-api notes), `ptz.patrol.start`, `ptz.patrol.stop`, `ptz.goto_preset` ([ptz-patrol.md](ptz-patrol.md)), `audio.beep`, `snapshot.refresh`, `mqtt.test`,
`settings.reset` (one key or group to default; never a factory reset), `settings.save`. No `reboot` action in v1: a
wedged camera is power-cycled or reached over SSH. It may be added later as a separate high-scope action if needed.

## 3. Channel A: MQTT / Home Assistant generated from the registry

- Today the entity table is hand-written (`akam2k_mqtt*.c`, [mqtt.md](mqtt.md) section 5). Each registry entry carries
  an `expose` descriptor (HA component, name, icon, unit, category); `mqtt_gen.c` derives discovery documents,
  command topics `BASE/<entity>/set`, state publishing and payload validation (the strict parser).
- Entity id = key with `.` replaced by `_`. **Existing entity ids stay as aliases** (`alias="status_led"`) so current
  HA installs keep history; discovery uses the alias when set.
- Hand-written tables are replaced group by group, each step proven by a golden-file test (byte-identical discovery
  documents for existing entities).
- MQTT commands call the same `set()` path as HTTP (source `mqtt`) with the limits of mqtt.md section 6.
  `mqtt.enabled` and `api.*` are **not** exposed over MQTT (switching a channel off from itself is a footgun).
- Adding a setting costs one table row, no HA code.

## 4. Channel B: local HTTP JSON API

### 4.1 Transport

- Plain HTTP/1.1 on TCP **8092** by default (`api.port` 1024..65535; 8091 stays the 1080p control port). Its own
  listener thread: it must not depend on ONVIF or MQTT. With **no key installed the port is closed** (nothing
  listens on an unprovisioned camera).
- Limits: 4 concurrent connections, request line plus headers at most 4 KiB, body at most 8 KiB, 5 s read timeout,
  10 s per request, `Connection: close` (no keep-alive, no pipelining, no chunked uploads: 411), one request served at
  a time by one thread (requests are tiny; revisited in phase 4 if latency matters).
- JSON in and out (`application/json`). Errors: `{"error":"code","detail":"text"}`. Unknown method 405, unknown path
  404.
- Patterns reused from the 1080p (`src/net/onvif/http_srv.c` parsing and limits, `cred.c` constant-time compare and
  per-peer lockout, nonce issuing). Those files are **not edited**; the 2K gets its own small copy or a `src`
  move in a dedicated step.

### 4.2 Endpoints (`/akam/v1`)

| Method and path | Scope | Effect |
|---|---|---|
| `GET /hello` | none | Unsigned: `{"api":1,"clock_set":bool,"challenge":"<32 hex>"}` (5.4). Rate limited |
| `GET /info` | read | model, firmware, api version, uptime, clock state, caller `key_id` |
| `GET /schema` | read | The registry as JSON (key, type, bounds, options, default, apply, persist, access) plus the action list, so the assistant builds forms without hard-coded knowledge |
| `GET /settings[?prefix=]` | read | All values `{"key":"value"}` |
| `GET /settings/<key>` | read | One value with metadata |
| `GET /state` | read | Read-only state keys (metrics, component states, `pending_device_restart`) |
| `PATCH /settings` | write | Body `{"video0.fps":"15","rtsp.stream1.enabled":false}`; JSON numbers/bools are converted to canonical text. Reply `{"results":{key:{"ok":true,"apply":"applied|restart_component|pending_device_restart","value":".."}\|{"ok":false,"error":"range","detail":"[1,15]"}}}`; 200 all ok, 207 mixed, 422 none |
| `POST /actions/<name>` | write | Body = argument object; reply `{"ok":true,...}` |
| `GET /audit` | admin | Recent audit entries (7.5) |
| `GET/POST/DELETE /keys[/<key_id>]` | admin | List (ids, scopes, labels, dates; never secrets), add, revoke (section 8) |
| `GET /ptz/presets[/<name>]`, `GET /ptz/routes[/<name>]` | read | PTZ presets and patrol routes ([ptz-patrol.md](ptz-patrol.md) section 7) |
| `POST /ptz/presets[/<name>]`, `POST /ptz/routes`, `DELETE /ptz/presets/<name>`, `DELETE /ptz/routes/<name>` | write | Create or replace, delete; JSON bodies, validated, all or nothing; one flash write per change, rate limited |

| `GET /button`, `GET /modes[/<name>]` | read | The programmable push button: its whole config (gesture timing, LED confirmation, bindings `{gesture -> [actions]}`) and the named modes |
| `POST /button`, `POST /modes[/<name>]`, `DELETE /modes/<name>` | write | Replace the whole button config (validated as a whole, all or nothing); create or replace a mode (up to 8 `key=value` pairs, only registry rows that are writable, non-secret, live, not `net.*` / `api.*`); delete a mode no binding uses (409 `in_use`). One flash write per real change (`/config/akam/button`, `/config/akam/modes`), burst of 8 then one per 10 s (429 `rate`), counted in `sys.flash_writes_button` |

Scopes: `read` < `write` < `admin`. Each setting/action has a minimum scope (default `write`; `api.*` need `admin`).

**Programmable button (2K, implemented 2026-11-17).** The front button is a gesture source: short / double / triple / long / very long presses and
user sequences of up to 6 presses (`S S L`) are bound to up to 4 actions from a fixed whitelist (`none`, `ptz.home`, `ptz.park`, `ptz.toggle_park`,
`ptz.recalibrate`, `ptz.privacy on|off|toggle`, `ptz.goto_preset <name>`, `track on|off|toggle`, `track.lost_policy <value>`, `patrol start <route>`, `patrol stop`,
`led pattern <pattern|name>`, `mode <name>`); nothing destructive (no reset, key, network or reboot) can be bound. Every gesture emits the event `button`
`{"gesture","actions","result"}` and an MQTT publish on `BASE/button` (plus one Home Assistant device trigger per configured gesture, MQTT staying opt-in); state key
`button.last_gesture`; action `mode.apply {"name":".."}` applies a mode through the registry all or nothing (every pair is validated before any is applied). The
defaults are the factory behaviour. Details, timing and limits: [docs/porting/lsc-rotatable-2k/button-and-zones.md](../docs/porting/lsc-rotatable-2k/button-and-zones.md).

### 4.3 What the API cannot do

No endpoint takes a path, command line, pin number, device node, URL to fetch or firmware image. The registry is the
whole surface; a new capability must be added to the firmware registry, reviewed and tested.

## 5. Request signing (HMAC-SHA256)

### 5.1 Keys

- A key = `{key_id (8 lowercase hex), secret (256 bit random), scope, label, created}`.
- Secret from `/dev/urandom`; the install refuses (`entropy`) if the pool is not seeded rather than make a weak key.
- Stored in `/config/akam/api_keys` (0600, directory 0700), one line per key
  `key_id:scope:created:hex(secret, 64 chars):label`, written atomically. Refused when readable by others (like
  `mqtt_pass`). Secrets are never logged, never in argv, never in any API reply after creation, never on MQTT.
- The secret is shown once to the installer (section 8). The assistant keeps it in its state dir (0600) or the OS
  keyring.

### 5.2 Request format

```
X-Akam-Key:       <key_id>
X-Akam-Timestamp: <unix seconds, decimal>        (or X-Akam-Challenge, 5.4)
X-Akam-Nonce:     <16 random bytes, 32 hex>
X-Akam-Signature: <hex(HMAC-SHA256(secret, canonical_string))>
```

Canonical string (UTF-8, `\n` separators, no trailing newline):

```
AKAM1-HMAC-SHA256
<METHOD, upper case>
<path exactly as sent>
<canonical query>
<hex(SHA256(body))>          (SHA256 of the empty string when no body)
<timestamp>
<nonce>
<key_id>
```

- Canonical query: split on `&`; each `name=value` percent-decoded then re-encoded (unreserved set `A-Za-z0-9-._~`,
  upper-case hex); sorted by name, then value, bytewise; joined with `&`; empty when no query. Repeated names kept. A
  malformed escape is 400.
- Path is not normalised: `..`, `//`, `%2f` or an unexpected trailing slash is rejected (400), so client and server
  cannot disagree.
- The server hashes the **received body bytes** (`Content-Length` required).
- Method/path are checked against the endpoint table before any HMAC work.
- Replies are not signed in v1 (see 5.6).

### 5.3 Verification order (cheap first)

1. Size limits, method/path whitelist. 2. Per-peer lockout check (no crypto when locked). 3. Key lookup: an unknown
key follows steps 4 to 6 exactly like a known one (same errors, same work) with a dummy fixed secret and always ends in
401 `bad_signature`, so neither the answer nor the timing reveals whether a key id exists. 4. Timestamp
window. 5. Nonce cache lookup (read only). 6. Recompute HMAC, compare in **constant time** (fixed 32 bytes).
7. Scope check (403 `scope`). Only a request that passes 6 inserts its nonce and clears the peer's failure count, so
failed requests never consume cache slots (an attacker cannot evict real nonces).

### 5.4 Clock window and unset-clock fallback

- Normal: `|now - timestamp| <= 60 s` (`api.clock_window_s`, 10..300). The camera clock comes from opt-in NTP
  ([network-time-security.md](network-time-security.md) section 4). Outside the window: 401 `clock_skew`, reply carries
  the camera time so the assistant can show the difference.
- **Clock unset** (`sys.time_set == 0`): timestamps are meaningless. The client calls `GET /hello`; the camera
  returns a **challenge** (16 random bytes) kept in a table (8 entries, 30 s TTL on the **monotonic** clock,
  single use). The client signs with `X-Akam-Challenge: <hex>` instead of timestamp and nonce (canonical lines 6
  and 7 become `challenge:<hex>` and the same `challenge:<hex>`). The server accepts it once and deletes it. Replay
  protection without any clock.
- The challenge mode is always accepted, even with a set clock (a client with a bad clock still works, one extra round
  trip). `hello` is rate limited (10/min global, 2/s per peer) and returns no secret.
- Nonce retention uses the **monotonic** clock, so a wall-clock step (NTP) cannot make a seen nonce acceptable again.

### 5.5 Nonce cache and rate

Fixed 64 entries `{key_idx, nonce[16], expires_mono_ms}` (about 2.5 KB), retention `2 x window + 5 s` (125 s at the
default). Invariant, unit-tested: `capacity >= accepted_rate x retention + burst`. With 64 entries and 125 s the
sustained accepted rate is capped at **0.4 request/s per camera, burst 14** (0.4 x 125 + 14 = 64) (token bucket across keys; configurable
together with the window by the same invariant). That is ample for a human-scale assistant; bulk callers must PATCH many
keys in one request. When the cache is full, new requests get 429 (fail closed, never evict a live nonce).

### 5.6 Threat model

| Threat | Mitigation |
|---|---|
| LAN attacker without key | HMAC; unknown key and bad signature look identical; lockout |
| Replay of a captured request | Window + nonce cache, or single-use challenge; signature covers method, path, query, body |
| Brute force | 256-bit secret; lockout/backoff and global cap bound CPU use |
| Timing attack | Constant-time compare; dummy HMAC for unknown key id |
| Eavesdropping | **Not mitigated**: settings travel in clear. The API carries no secret in v1 |
| Active MITM altering a reply | Not mitigated in v1 (replies unsigned); a MITM cannot forge requests |
| Key theft from the camera | `/config` is 0600 root; whoever has SSH or physical access already owns the camera |
| Key theft from the PC | 0600 / keyring; scoped keys; revoke by id |
| Compromised script key | Give scripts `read` or `write` only, label it, revoke it |
| Denial of service | Connection cap, small buffers, lockout, 429; own low-priority thread, no lock shared with video |
| Bad values | Every value bounded by the registry; flash writes rate limited |

**TLS cost/benefit.** TLS (mbedTLS or wolfSSL, ECDHE + ChaCha20, self-generated certificate pinned by the assistant)
would add roughly 150 to 300 KB of binary and 40 to 80 KB of heap per handshake plus connect-time CPU and a pinning
story (figures to be measured). It buys confidentiality and authenticated replies. Since v1 exposes no secret and
requests are authenticated, v1 ships without TLS. The design allows `api.tls=1` on a second port later without
changing the signature scheme (kept as defence in depth). Decision gate: add TLS before any secret-carrying endpoint
(write-only passwords) is added.

## 6. Live toggles: component lifecycle

A **component controller** per feature owns a desired state (from the registry), an actual state, one mutex and
`start()`/`stop()` hooks. Setting a toggle writes the desired state and the controller reconciles. Idempotent: setting
the current value is a no-op without a write.

**Threading.** One **settings worker** thread (32 KiB stack) serialises every registry `set()` from HTTP, MQTT, conf
and buttons through a queue of 8. Setters are never concurrent with each other. HTTP and MQTT post and wait at most 3 s;
on timeout the reply is 504 `busy`, the change still completes, and state shows `applying`, so a slow component cannot
wedge a network thread.

### 6.1 MQTT (`mqtt.enabled`, `mqtt.*`)
`a2k_mqtt_start/stop` already exist (stop publishes `offline`, joins the thread). The registry becomes the config source
(the card file `akam/mqtt` stays as a seed). Off = close socket, join, free buffers; the API is unaffected. A config
key change = stop + start (`restart_component`). The password stays in its 0600 file, outside the registry.

### 6.2 RTSP streams (`rtsp.streamN.enabled`)
- Off, **new sessions**: `DESCRIBE`/`SETUP` for that path answers `404`; the stream disappears from ONVIF profiles.
- Off, **existing sessions**: mark disabled, stop RTP and close the connections of that stream within 1 s, then
  release the stream. The encoder channel is stopped only when no other consumer needs it:
  `channel_needed = rtsp_wanted || detect_wanted || snapshot_wanted` (the sub stream feeds detection, so
  `rtsp.stream1.enabled=0` with detection on stops publishing, not encoding).
- On: reverse; serving starts at the next IDR.
- Both streams off and MQTT off is allowed: the API remains.

### 6.3 Audio, detection, tracking
- `audio.enabled`: opens/closes the microphone and adds/removes the audio track. The SDP changes, so existing sessions
  keep the old SDP; with `audio.reconnect_clients=1` (default) the controller closes them so players renegotiate. The
  mic closes only when no session needs it.
- `detect.enabled`: stops the NPU thread (frees its buffers and sub-stream tap); tracking is forced off; detection state
  reads `unknown`.
- `track.enabled`: starts/stops the tracker; stopping holds the motor in place. Cross-field `check`: needs
  `detect.enabled`.
- `track.return`, `track.return_delay_s`: what the camera does after the person is lost (3 s grace): `stay` where it was last seen (opt-in,
  never returns) or `home` (default) after the delay (0 = right after the grace). Applied to the running tracker at once.

### 6.4 Video parameters
Reuse `video.h`: live setters first, else a **re-open of that channel only** with automatic rollback on failure;
bursts coalesce into one re-open. The reply says `restart_component` when a re-open happened.

### 6.5 Safe stop rules
1. `stop()` blocks new work (flag), waits for in-flight work with a bound, then joins. A component that misses its bound
   is **left running**; the setting returns `stop_timeout` and keeps its old value (never half stopped).
2. No stop blocks a video thread or holds a lock they take.
3. Lock order: settings worker -> controller mutex -> component mutex. A component never calls the registry while
   holding its own mutex (checked by a debug mutex wrapper in tests).
4. Exit order: HTTP and MQTT front-ends first (no remote command may reach a stopping setter, as `a2k_mqtt_stop()`
   already guarantees), then the worker, then components.
5. A setting that disables a component persists only after the live apply succeeded.

## 7. Security operations

7.1 **Rate limit and lockout.** Per peer IP (16 entries, LRU): 5 failures, then 1 s lock doubling to 5 min, reset by a
success or 10 quiet minutes; a locked peer gets 429 `Retry-After` with no crypto. Global cap of 20 failed auths/min.
7.2 **Constant time.** HMAC compare and the unknown-key dummy path; a unit test counts that both paths run one HMAC.
7.3 **Atomic persistence.** `settings`, `api_keys`, `audit` all use the `akam2k_store` write (same-directory temp, fsync,
rename, 0600). An invalid `settings` file is ignored as a whole (logged; defaults apply). An invalid `api_keys` file is
**kept**, the API stays closed with `error:keys_invalid` in state; it is never silently wiped.
7.4 **Flash wear.** Persistent writes burst 4 then one per 30 s across all sources; a refused write still applies live,
is flagged `persist_deferred` and saved by one coalescing timer. `image.*` sliders are RAM-only unless saved with
`settings.save`.
7.5 **Audit log.** RAM ring of 64 entries, a trimmed line log `/tmp/akam_api.log` (pattern of `akam_ssh.log`), and the
last 20 **write** events persisted in `/config/akam/audit` on the persistence timer. Entry: time (wall, or `mono+N`
when the clock is unset), key_id, scope, peer IP, method and path, keys changed, result. Never the secret, signature,
nonce or any secret setting value; failed auths log the key_id as sent (truncated to 8 characters) and the peer.

## 8. Key management

- **Install (bootstrap).** A camera with no key has no API port. The assistant installs the first (`admin`) key over
  the **existing SSH channel**, like `authorized_keys`: it generates 32 random bytes locally and runs a RAM-only
  `akam_apikey.sh add admin <label>` (secret on stdin, never argv). The script writes `/config/akam/api_keys`
  atomically and asks the app to reload keys through the loopback socket, which opens the port. Alternative without
  SSH: `akam/api_key` on the card, validated, imported once and remembered by hash (as `authorized_keys`), never
  deleted from the card.
- **Rotate.** `POST /keys` (admin) returns a new secret once; the client switches, then `DELETE`s the old id. Deleting
  the **last admin key** is refused (`last_admin`), as `AUTHKEYS=REFUSED_LAST_KEY` does for SSH.
- **Revoke.** `DELETE /keys/<id>` (admin) or `akam_apikey.sh remove <id>` over SSH. Effective immediately (key table
  swapped under a mutex, that key's nonces dropped).
- **Multiple keys and scopes.** Up to 8 keys: the assistant `admin`, a user script `write` or `read`, a dashboard
  `read`. Labels 1..24 of `[A-Za-z0-9 ._-]`.
- **Factory reset** wipes `/config`: keys, settings, audit are gone and the API returns to closed and unprovisioned;
  the assistant re-bootstraps over SSH or the card. An appfs/app update does **not** touch `/config/akam/api_keys`. A
  camera answering `bad_signature` to a stored key is marked stale in the assistant and re-provisioned after user
  confirmation.
- **Lost key on the PC.** Recovery is SSH (`akam-key`) or the card file; the API cannot recover a lost admin
  secret, by design (but any `admin` key, or SSH, can mint a new one; see 8.1).

### 8.1 Getting a key easily (for scripts, HA custom integrations, curl)

Principle: **one named key per tool**, with its own scope, so revoking a tool never touches the others. The secret is
stored as 64 hex characters so any tool (including `openssl`) can use it directly.

**Via the assistant** (the user's normal route; talks to the camera with an `admin` key or over SSH):

| Command | Effect |
|---|---|
| `akam api-key new --camera <name> --scope read\|write --name <tool>` | Creates a key (name 1..24 chars), prints the copy block below once, stores a copy in the assistant state dir (0600) |
| `akam api-key show --camera <name> [--name <tool>]` | Lists keys (id, name, scope, created, last used; never secrets). With `--name`, re-prints that key's copy block when the assistant holds the secret |
| `akam api-key export --camera <name> --name <tool> [--format env\|json\|curl\|python]` | Copy-ready output for one key; refuses (exit 2) if the assistant never held the secret, and says to create a new key |
| `akam api-key revoke --camera <name> --name <tool>` | Revokes that key only (last admin refused) |
| `akam api-key qr ...` | Optional: the TUI renders the `export --format json` payload as a QR code for a phone/HA companion; off by default, shown on screen only, never written to a file or log |

`export`/`new` print to the terminal only (stdout); secrets are never written to the assistant log, shell history
suggestions or process arguments of child processes. The assistant warns that the output is a secret.

**Copy block** (what `new`/`export` print):

```
key_id:   3fa91c07
secret:   9b1d...e4 (64 hex)
scope:    read
url:      http://akam-12ab.local:8092   (or the IP)

# env snippet
export AKAM_URL=http://192.0.2.10:8092
export AKAM_KEY_ID=3fa91c07
export AKAM_KEY_SECRET=9b1d...e4
```

**Reference signing helpers** (shipped in the repo under `docs/` and printed by `export --format curl|python`), so any
tool signs without the Go code. Shell + openssl (POSIX sh, no query string, JSON body in `$BODY`):

```sh
akam_call() { # usage: akam_call METHOD PATH [BODY]
  m=$1; p=$2; b=${3:-}
  ts=$(date +%s); nonce=$(openssl rand -hex 16)
  bh=$(printf '%s' "$b" | openssl dgst -sha256 | sed 's/.* //')
  s=$(printf 'AKAM1-HMAC-SHA256\n%s\n%s\n\n%s\n%s\n%s\n%s' "$m" "$p" "$bh" "$ts" "$nonce" "$AKAM_KEY_ID")
  sig=$(printf '%s' "$s" | openssl dgst -sha256 -mac HMAC -macopt "hexkey:$AKAM_KEY_SECRET" | sed 's/.* //')
  curl -sS -X "$m" "$AKAM_URL$p" -H "X-Akam-Key: $AKAM_KEY_ID" -H "X-Akam-Timestamp: $ts" \
    -H "X-Akam-Nonce: $nonce" -H "X-Akam-Signature: $sig" ${b:+-H 'Content-Type: application/json' --data-binary "$b"}
}
# akam_call GET /akam/v1/settings
```

Python (stdlib only) is the same ten lines with `hmac`, `hashlib`, `secrets`, `urllib.request`; both helpers are
tested in CI against the shared vector file (section 12) so they cannot drift from the firmware. The query-string rule
(5.2) is documented next to them; the helpers support queries by sorting in the caller.

**Without the assistant (SSH on the camera)**: `akam-key` (the `akam-key` of 8, a BusyBox shell script, root
only, installed read-only in the image), writing `/config/akam/api_keys` atomically and asking the app to reload:

| Command | Effect |
|---|---|
| `akam-key new --scope read\|write\|admin --name <tool>` | Generates the key from `/dev/urandom`, prints the copy block once (secret on stdout only) |
| `akam-key list` | id, name, scope, created, last used; no secrets |
| `akam-key show <name>` | Prints the secret of that key (the camera holds it, 0600 root: SSH access already implies it), so a lost copy is recoverable over SSH |
| `akam-key revoke <name\|id>` | Revokes; refuses the last admin unless `--force` (SSH is the recovery route) |

The same keys appear in both worlds: a key made over SSH is visible to `akam api-key show` (secret unknown to the
assistant until `import`, `akam api-key import --camera <name> --name <tool>` pulls it over SSH), and vice versa.

**Never in logs, MQTT or replies**: the secret appears only in the output of `new`/`show`/`export` on a terminal the
user ran the command in. It is never in `/tmp/akam_api.log`, the audit log, MQTT topics or discovery, `/akam/v1/keys`
replies (which carry ids, names, scopes only), crash output, or argv. `akam-key` reads no secret from argv and writes
none to syslog. Tests: a canary secret is created and the whole log/audit/MQTT output of a session is grepped for it.

## 9. Resource budget (38 MB camera)

| Item | Estimate |
|---|---|
| Registry table (about 120 entries x 56 B, `.rodata`) | 7 KB flash, 0 heap |
| SHA-256 + HMAC (no dependency) | 3 KB code |
| JSON writer + flat strict parser | 4 KB code |
| HTTP server (one connection at a time) | 6 KB code, one 4 KiB request buffer and one 8 KiB body/response buffer, static |
| Nonce cache, peer table, challenge table, audit ring | about 14 KB bss |
| API thread (64 KiB reserved, about 10 KB touched) + worker (32 KiB reserved) | about 20 KB RSS |
| Generated MQTT replaces hand-written tables | net code saving expected |
| **Total** | about +25 KB flash, +45 KB RSS: estimates, to be **measured** on the camera. Phase gate: binary +56 KB (raised from +40 KB after the measurement below: +53.3 KB, the registry table, schema and JSON code are larger than guessed; nothing cheap left to trim, `-Os` and `--gc-sections` are on) and RSS +64 KB at most |

No `malloc` or `fork` on the request path; no disk read on it except persistence writes. One HMAC over a few hundred
bytes is microseconds; an idle API costs one blocked `accept`. The API thread runs below the video threads and takes no
lock a capture thread holds across I/O.

## 10. Relation to the 2K today

The 2K has no 8091 port; it is configured by `akam.conf` and MQTT. v1 adds the API only; no legacy text port is added.

## 11. Portability to the 1080p

- `settings_reg` and `mqtt_gen` have no model knowledge: the 1080p supplies its own `st_def_t` table (the keys of
  [control-port.md](control-port.md) section 6) filtered by its HAL capabilities.
- The 1080p control port (8091) becomes an **adapter**: `GET`/`SET key` map onto the registry `get`/`set`, keeping the
  text protocol, reply forms and write rule (loopback or debug flag). `LIST` walks the registry. Existing key names
  stay (`night.mode`, `isp.brightness`, ...), differing ones get an `alias`.
- The HTTP API reaches the 1080p only after the 1080p adoption phase (hal-architecture phase B), with the byte-identical
  proof and a hardware pass. Until then no 1080p file changes. **Done on the host (2026-11-17), see "Phase 3" at the end of this
  file:** the 8091 adapter is a fallback in front of the registry (not a rewrite, and `LIST` does not walk the registry); the
  hardware pass is open.
- A shared key name keeps one meaning, range and unit on both models; a divergence is a per-model `lo/hi` override.

## 12. Test plan

Host unit tests (`tests/native/`, no camera):
- **Canonicalization vectors**: a committed JSON file of (method, path, query, body, timestamp, nonce, key_id, secret)
  with expected canonical string and HMAC hex, consumed by both the C and the Go tests. Cases: empty/unsorted/repeated
  query, percent-encoding variants, unicode, empty and large body, `//`, `..`, `%2f` (rejected), lower-case method.
- **SHA-256/HMAC**: NIST and RFC 4231 vectors.
- **Replay**: same nonce twice, nonce after the window, concurrent same nonce, failed request consumes no slot, full
  cache gives 429, wall-clock step backwards does not revive a nonce.
- **Clock**: inside window, +-61 s rejected, 60 s accepted, clock unset uses the challenge, challenge reuse, expiry on
  the monotonic clock.
- **Auth**: unknown key and wrong signature identical (one dummy HMAC), constant-time compare, lockout schedule, scope
  matrix, last admin refused, cache invariant `capacity >= rate x retention + burst`.
- **Registry**: each type parser at min, max, min-1, max+1, overflow, trailing text, empty, sign, whitespace, control
  bytes; enum, pattern, cross-field `check`, all-or-nothing PATCH, apply-mode reporting, model filtering,
  persistence round trip, torn/invalid files, rate-limit buckets with a fake clock.
- **MQTT generation**: golden discovery documents of existing entities (byte-identical), alias ids, payloads checked by
  the same parser.
- **Components** (fake HAL): on/off, double toggle, stop timeout keeps the old value, lock-order assertions.
- **Fuzz**: HTTP request parser and JSON parser under ASan/UBSan (oversize, partial reads, slowloris).

Integration (host): the API linked with fake components and driven by a **fake HTTP client** signing with the reference
vectors: schema, PATCH ok/partial/invalid, action, key add/rotate/revoke, API working with MQTT off, clock-unset
challenge flow, port closed with no key.

Camera checks (to add to `docs/hardware-test-checklists.md`): measured RSS and binary delta, API reachable with MQTT
off, stream toggles with a client attached, audio toggle, power cut during a settings write, fps counters unaffected
under a request flood.

### 12.1 Assistant-side client sketch (Go)

```go
// internal/camapi/sign.go
func Sign(keyID string, secret []byte, method, path, rawQuery string, body []byte, ts, nonce string) http.Header {
    bh := sha256.Sum256(body)
    s := strings.Join([]string{"AKAM1-HMAC-SHA256", strings.ToUpper(method), path, canonicalQuery(rawQuery),
        hex.EncodeToString(bh[:]), ts, nonce, keyID}, "\n")
    m := hmac.New(sha256.New, secret)
    m.Write([]byte(s))
    h := http.Header{}
    h.Set("X-Akam-Key", keyID)
    h.Set("X-Akam-Timestamp", ts)
    h.Set("X-Akam-Nonce", nonce)
    h.Set("X-Akam-Signature", hex.EncodeToString(m.Sum(nil)))
    return h
}
// canonicalQuery: percent-decode, re-encode with the unreserved set, sort by name then value, join with "&".
// Client.Do: on 401 clock_skew retry once with the challenge flow (GET /hello, sign with "challenge:<hex>");
// never retry other 401s; show per-key PATCH results; 5 s timeout.
```

The assistant stores `{address, key_id, secret ref, scope}` per camera next to the pinned SSH host key, probes with
unsigned `GET /akam/v1/hello`, and falls back to SSH when the API is closed or the key is stale. The Go signer is
tested against the same vector file as the C code.

## 13. Phased implementation plan (small, reviewable; 2K first)

1. **Registry core and parsers** (pure C, host tests): types, strict parsers, table API, model filter. Wrap the
   existing video keys as the first entries (no behaviour change; conf and `/config/akam/video` still work).
2. **Persistence and settings worker**: `/config/akam/settings`, precedence, coalesced rate-limited writes, request
   queue. Migrate `akam.conf` key groups one at a time, each proving identical effective values.
3. **Crypto and signing library**: SHA-256, HMAC, canonicalizer, nonce cache, clock/challenge, lockout; host tests with
   the shared vector file in `tests/fixtures/control_api_vectors.txt`.
4. **HTTP server and read endpoints** (`hello`, `info`, `schema`, `settings`, `state`), keys read from the file;
   fake-client integration tests; **measure the budget on the camera**. Port opens only with a key.
5. **Write path**: PATCH, actions, audit, scopes, key endpoints, `akam-key` SSH bootstrap, assistant Go client
   and CLI commands.
6. **Live component controllers**, one per step: MQTT, RTSP stream on/off, audio, detect, track; each with its
   stop-timeout test and a camera check with a client attached.
7. **Generated MQTT entities**: golden-file parity, then replace the hand-written tables group by group (aliases kept).
8. **Docs and hardening**: schema-derived docs, threat-model review, fuzzing in CI, TLS decision record.
9. **1080p adoption** (own spec step, behind the no-regression proof): registry table, control-port adapter, then the
   HTTP API.

Every phase ends with the host suite green and `git diff dev -- src/` empty for phases 1 to 8 (they only add files);
phases 4 and 6 also validate on the 2K and record the measured budget here.

## Deferred: payload encryption (decided 2026-11-15)

v1 is signed (HMAC-SHA256: authenticity, integrity, anti-replay) but NOT encrypted: bodies travel in clear on the LAN, like RTSP. Until payload encryption lands (planned: ChaCha20-Poly1305 with a key derived from the API key by HKDF, per-message nonce, responses encrypted too, reference client libs), SECRET settings (MQTT password, RTSP credentials, any key material) are write-only AND refused by the HTTP API; they are set through the assistant over SSH or `akam-key`/config files on the camera. No secret is ever returned by GET.

## Phase 1 building blocks (implemented 2026-11-15, host-tested, not wired)

Pure portable C99 under `platforms/anyka-ak3918av130/` with generic names (they move to `src`
unchanged in the phase-B step); no I/O, no allocation, no global state, no `src/` change, not in the Makefile yet.

| File | Content |
|---|---|
| `sha256.[ch]` | SHA-256, HMAC-SHA256, `ct_equal`, hex helpers, `akam_wipe` |
| `api_sig.[ch]` | path/method rules, canonical query + canonical string builder, `apisig_verify` (spec 5.3 order, clock window, `/hello` challenge table, nonce cache that fails closed, scope check, dummy HMAC for unknown ids), all time/random injected |
| `api_keys.[ch]` | `/config/akam/api_keys` parse/serialise (8 keys, scopes, labels), add/remove with last-admin guard, atomic save through an injected writer, `GET /keys` JSON |
| `settings_reg.[ch]` | registry core (section 2): strict parsers, bounds/options, patterns (`led`, `pan_tilt`, `zone4`), apply modes, persist and secret flags, per-model and capability availability, get/set/check hooks, all-or-nothing PATCH, `key=value` persistence text, JSON for values, one setting, schema, PATCH reply |
| `json_min.[ch]` | bounded JSON writer and a flat-object parser (PATCH and action bodies) |
| `tests/fixtures/control_api_vectors.txt` | shared signing vectors (produced with openssl) for C, `tools/akam-api-sign.sh` and the future Go client |
| `tools/akam-api-sign.sh` | reference signer (POSIX sh + openssl + awk), validated against the vectors |

Test commands (from the repo root; `tests/rot2k_control_api_test.sh` runs all of it, `FUZZ_ITERS` and `SH_UNDER_TEST`
tune it):

```sh
APP=platforms/anyka-ak3918av130
COMMON=src
SRC="$COMMON/net/sha256.c $COMMON/api/api_keys.c $COMMON/api/api_sig.c $COMMON/api/json_min.c $APP/settings_reg.c"
FLAGS="-std=gnu99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -I$APP"
cc $FLAGS -o /tmp/capi_unit $SRC tests/rot2k_control_api_unit.c && /tmp/capi_unit tests/fixtures/control_api_vectors.txt
cc $FLAGS -o /tmp/capi_fuzz $SRC tests/rot2k_control_api_fuzz.c && /tmp/capi_fuzz 200000
bash tests/rot2k_control_api_test.sh        # all of the above plus the signer against the vectors
```

Decisions the spec left open (pinned by the vectors):
- Canonical query: a parameter without `=` is written `name=`; sorting is bytewise on the re-encoded name, then value;
  an empty segment (`a&&b`, `&a`, `a&`) or `%00` is malformed (400).
- Path: only `[A-Za-z0-9._~-]` segments, so `%` (hence `%2f`), `..` anywhere, `.` segments, `//` and a trailing `/` are all
  refused; the signer refuses them too (exit 3).
- Method is exact upper case at the verifier (`get` is 400); the signer upper-cases.
- `check` hooks also receive the registry (`streg_peek` reads the pending PATCH value of another key).
- Secrets: a `secret` row must be write-only, not exposed on HTTP, not persistent, without a getter (checked at
  `streg_init`); over HTTP it is unknown (no schema line), anywhere else its value is never returned or saved.
- Labels may not start or end with a space; ids and labels are unique.
- **Spec inconsistency resolved:** 5.5 said 0.4 req/s and burst 16 with 64 nonce slots, but 0.4 x 125 s + 16 = 66 > 64.
  Decision: burst 14, 64 slots (`APISIG_BURST_DEFAULT`, `APISIG_RATE_MILLI_DEFAULT`, checked by `apisig_cache_invariant_ok`).

Phase 2 wires (not done here): the HTTP server and the per-peer lockout and accepted-rate token bucket (7.1, 5.5), the
`/hello` rate limit; reading `api_keys` with the 0600 check and `a2k_store_write` as the writer (`akeys_*_save`), the
CSPRNG (`/dev/urandom`, `entropy` refusal) feeding `akey_make` and `apisig_hello`, wall and monotonic clocks, `sys.time_set`;
the settings worker queue with the 3 s wait, the `/config/akam/settings` file with its flash-wear limiter, the registry
table of the 2K with real hooks (video keys first, `a2k_vid_parse` as the `ST_INT`/`ST_ENUM` source of truth), the
`akam.conf` precedence, the action table and the actions in `GET /schema`, audit; the generated MQTT entities and aliases;
`akam-key` and the Go client consuming the vectors; adding the new files to the Makefile, the anti-brick scan and
`tools/ci/test_groups.txt`.


## Phase 2: the HTTP server on the 2K (implemented 2026-11-16, host-tested; camera checks pending)

Reference for users and integrators: [docs/porting/lsc-rotatable-2k/control-api.md](../docs/porting/lsc-rotatable-2k/control-api.md). This section records what
was built and where it departs from the sections above.

**Why it exists first:** there is no MQTT broker in the target setup, so this API is the path to a native Home Assistant integration. It therefore exposes
everything HA needs: settings (LED, IR, picture, tracking, video, network), actions (PTZ), state/metrics, and a **push channel** (`GET /events`, a long poll).

Files (written under `platforms/anyka-ak3918av130/`; the generic ones moved to `src` with the 1080p port, Phase 3 below; the a2k_ symbol names are unchanged):

| File | Content |
|---|---|
| `http_req.[ch]` (now `src/net/`) | strict request-head parser (pure, fuzzed): CRLF only, no folding, no control or non-ASCII byte, duplicate X-Akam-*/Content-Length refused, raw `+` kept |
| `api_limit.[ch]` (`src/api/`) | per-peer lockout (5 free failures, 1 s doubling to 5 min, LRU 16), global 20 failures/min with an exemption for a recently proven peer, accepted-request token bucket (0.4/s, burst 14), `/hello` limits (pure) |
| `akam2k_apievt.[ch]` (now `src/api/api_events.[ch]`) | event ring (32) + wake pipe; producers: button and person (in `mqtt_bind.c`'s existing hooks, which run with MQTT off too), day/night and PTZ (the notify adapters in `main.c`), settings changes |
| `akam2k_settings.[ch]` | the 2K registry table over the real modules, the settings worker (32 KiB stack, one job slot, 3 s wait, 504 `busy`), persistence (`/config/akam/settings`, burst 4 then 1 per 30 s), actions, the net.* backend |
| `akam2k_api.[ch]` (now `src/api/api_core.[ch]`, bound by `akam2k_apibind.[ch]`) | routing, verification order of 5.3, endpoints, key store (0600, reload by stat), audit ring, `--api-key` tool |
| `akam2k_httpd.[ch]` (now `src/api/api_httpd.[ch]`) | the socket side: one thread, `poll`, 4 connections (3 per peer), 5 s idle / 10 s per request, 2 s write bound, held long polls; the only API file that may create a socket |
| `firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/akam_api_key.sh` | SSH tool: runs the app in key-tool mode (`akamd --api-key new\|list\|revoke`) |
| `own_app_run.sh` `net_request` | hands one validated request line of `/tmp/akam_net.req` to `akam_net.sh` |

Decisions and departures from the earlier text:
- **One HTTP thread, not "one request at a time":** the thread multiplexes up to 4 connections with `poll` so a slow client cannot starve the others (the spec's
  sequential model would let a slowloris hold the port for 5 s per attempt). Handlers still run to completion one at a time. Only `GET /events` is held.
- **Events (new):** `GET /events?since=N&timeout=S` (S up to 25), types `button`, `person`, `night`, `ptz`, `setting`, `detections`; at most 2 held connections.
- **Person boxes out (2026-11-16):** event type `detections` (data: `ts_ms`, frame `w`/`h`, `pan`/`tilt` when known, `boxes[{x,y,w,h per-mille,score,id,target,ignored}]`), the read-only state key
  `detect.boxes` (the latest list as JSON text; `STREG_VAL_MAX` is 832 for it) and the setting `detect.publish_hz` (0..10, default 2, persisted; 0 = off; at most 5 a second). Pure policy in
  `detpub.c` (rate limit, change detection, one empty list when the last person leaves, nothing while privacy or parked, ids by nearest centre), fan-out from the detection thread to
  the event ring, the state key and `a2k_mqtt_detections` (MQTT `BASE/detections`, QoS 0, not retained, no discovery entity). The events have a **separate ring of 4** entries of 800 bytes
  (`api_events.c` merges the two rings by sequence number; `missed` is true when any number after the cursor is gone), so detections never evict `button`/`ptz`/`setting`. Boxes in an ignore zone
  are published with `ignored:true` (the tracker and the presence still skip them). RAM only. Caveat: the signed-request bucket (0.4/s) limits a long-poll client to about one reply per 2.5 s.
- **Bootstrap:** the SSH tool is `akam_api_key.sh` calling the app (`akamd --api-key ...`) instead of a separate `akam-key` binary: the app already has the
  CSPRNG, the key code and the atomic store, so nothing new ships. The running app picks the file up by `stat` (mtime, ctime, mode, size, inode) about once a second, so
  no loopback reload socket exists. A file with any group/other permission bit, or an invalid one, closes the API (`keys_perm`, `keys_invalid`) and is kept.
- **Randomness:** `rtsp_auth_rng` (getrandom without blocking, falling back to the non-blocking urandom): the spec's refusal when the pool is not seeded
  is as strong as that helper; the tool exits 4 when it reports no randomness.
- **Global failure cap exemption:** a peer with a valid signature in the last 10 minutes is exempt from the global 20/min cap (its own lockout still applies),
  otherwise 20 spoofed-free failures from anywhere lock the owner out of the API for a minute.
- **net.\* rows:** the app cannot spawn, so a change is a request line in `/tmp/akam_net.req` (one outstanding), taken by the supervisor within about 5 s; the value read back is
  the persisted one. Reply `applied` means queued, not done: the state `net.apply` (`queued|pending|ok|refused|failed|reverted|idle`, from `/tmp/akam_net.res`
  written by `akam_net.sh`) tells the outcome. `net.wifi_band` is `2.4|5` (default 2.4), `net.wifi_country` is `none` (default) or two capitals; band 5 without a
  country (and removing the country on band 5) is refused by the registry check, as `akam_net.sh` does.
- **Audit:** ring of 64 plus the last 20 writes in `/config/akam/audit` (0600, same limiter idea: burst 2 then 1 per 30 s) and one `a2k_log` line per request (the app log on
  the card); the separate `/tmp/akam_api.log` of 7.5 is not written.
- **Registry subset:** only keys with an existing live setter are rows (see the user doc); `mqtt.*`, `rtsp.streamN.enabled`, `audio.enabled`, `detect.enabled`, `night.mode`,
  `ptz.park|privacy|cal`, `zones.*` wait for their component controllers (phase 6). `image.*` is RAM only; `video*.*` persist through `/config/akam/video` (the video module).
- **Video cross-field rules** (`min_qp <= max_qp`, peak >= target) are registry `check` hooks, so a bad PATCH is refused as a whole.
- **Actions:** `ptz.home|park|recalibrate|step|move`, `settings.reset`, `settings.save`. Not done: `audio.beep` (no audio output on the 2K), `snapshot.refresh`, `mqtt.test`.
- **Limits actually enforced:** head 4096, body 8192, path 128, query 512, 40 headers, value 160; 413/414/431/411/417/408 as in the user doc.

Measured on the host (ASan/UBSan) by `tests/rot2k_control_api_http_test.sh` (134 checks) and `tests/rot2k_control_api_test.sh` (unit, fuzz); budget measured with the
pinned ARM toolchain: stripped `akamd` **+53,312 bytes** (211,712 -> 265,024), above the +40 KB phase gate of section 9 (the estimate there said +25 KB: the registry table and the
JSON/schema code are larger than guessed). bss +57 KB after the review (the 4 connection buffers of 12 KiB are malloc'd at accept and freed at close, so an idle slot holds none; what remains is the shared reply buffer 12 KiB, audit ring 16 KiB and write ring 5 KiB, key/nonce/peer tables). The persisted audit file comes from a separate ring of the last 20 WRITE events, so a flood of reads or failed logins cannot push them out; `GET /audit` returns the newest entries that fit one reply. **RSS on the camera is not
measured yet**; the gate (+64 KB) is to be checked with `a2k_mem_report` before and after a request burst, see the checklist.

Left for later phases: the Go client and `akam api-key` commands, `GET /schema`-driven docs, the live component controllers, generated MQTT entities, TLS/payload encryption.


## Phase 3: the signed API on the 1080p (convergence P7 and P8, implemented 2026-11-17, host-tested; camera checks pending)

Section 11 said the 1080p adopts the registry and turns port 8091 into an adapter. What was built, and where it departs from that text:
reference for users and integrators: [docs/porting/lsc-1080p/control-api.md](../docs/porting/lsc-1080p/control-api.md).

- **One shared API, two bindings.** `api_core.c`, `api_httpd.c`, `http_req.c` and the job/action types moved to `src` (`api/api_core`,
  `api/api_httpd`, `net/http_req`, `api/api_backend.h`) and take their model from `a2k_api_env_t`: model name and version, a log function, the file
  store (read, atomic write), the settings backend (registry, executor, action list) and extra routes (the 2K: PTZ presets and patrol, the button and
  its modes; the 1080p: none). The 2K binds in `api_bind.c` (behaviour unchanged: `tests/rot2k_control_api_http_test.sh` passes with its 357
  checks), the 1080p in `firmware/lsc-indoor-1080p-ak3918/api_bind.c`. `src/build/features.mk`: the 1080p builds with `API SYS LIGHT` on and `PTZ BUTTON SCENE PATROL
  TRACK` off.
- **Registry on the 1080p** (`firmware/lsc-indoor-1080p-ak3918/settings.c`, 41 rows, no registry file: each module keeps its own file, persisted only on a user change). The
  rows call the code of the 8091 port (`control_apply_setting`), the LED engine, `video_ctl`, `ctl_sys`, the ONVIF and MQTT services, so there is one
  writer per value whichever door is used. `control.c` offers any key it does not know to the registry (`GET/SET led.mode`, `image.contrast`,
  `video0.kbps`...); the keys it knows keep their names, replies and write rule byte for byte (`tests/control_test.sh`).
- **Departures from the plan text:** (a) `control.c` was not rewritten onto the table (the adapter is a fallback, the old code stays the writer behind
  the rows): rewriting would have changed the error texts the assistant reads for no gain. (b) No settings worker thread: a PATCH runs inline on the one
  HTTP thread (RAM and a 32 KiB stack saved); the 504 `busy` path of the 2K therefore cannot happen. (c) The flash limiter is per request (burst 6,
  then one per 20 s, `429 flash_rate`), not the deferred save of the 2K (the modules save at once). (d) Video rows apply at the next start
  (`restart_component`), no live setter (P10). (e) `net.hostname` and `net.ntp` are read-only (their files belong to `akam_hostname.sh` and
  `akam_ntp.sh`, which also set the kernel host name and start the time helper; the app never spawns).
- **Events:** `night` (the night thread calls `night_set_notify` after the ISP lock is released) and `setting`. No `person`, `ptz` or `button`.
- **Budget** (pinned zig, stripped `akamd`, against `dev` 639d44a, details per step in `tests/golden/lsc-indoor-1080p-ak3918.sha256`): P4 +2.4 KB, P6 +4.9 KB, P7 +17.9 KB,
  P8 +61.6 KB; **+86.8 KB in total (text +86.5 KB, bss +62.4 KB)**. The registry-plus-API part (P7+P8, +79.5 KB) is OVER the section 9 gate (+56 KB),
  like the 2K (+53 KB for the API alone, whose gate was +40 KB): the schema and JSON code, the HMAC and key code and the 41-row table are larger than
  estimated, and the 1080p links them as static musl. bss is mostly untouched pages (the 12 KiB reply buffer, the 16 KiB audit ring, the 5 KiB write ring).
  RSS on the camera is not measured.
