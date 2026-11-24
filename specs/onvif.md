# ONVIF

`akamd` can answer a subset of ONVIF Profile S (Device, Media and Imaging services plus WS-Discovery), so NVRs and apps
find the camera and play its stream. It is **off by default**. While off it costs no thread, no socket and no timer.

Status: **hardware-validated** on one camera: `tools/hw_tests/onvif_check.py` 34 of 35 then 34 of 34 checks (including RTSP
Digest `DESCRIBE`), WS-Discovery, WS-Security with the camera clock unset, imaging set and get with restore, no flash
write, ONVIF Device Manager (discovery, login, live video, live imaging changes). The fuzzing of the SOAP and HTTP
parsers has not been done.

## 1. Switching it on and off

- Flag file `/etc/config/akam/onvif`, exactly two lines in any order: `enabled=1|0` and `port=N` (N is 1 to 65535 except
  21 to 24, 554, 8091 and 8092). LF; a trailing CR is tolerated. Absent file means off. Anything else (unknown or repeated key,
  missing key, bad number, a third line, over 64 bytes) means off, with one log line.
- `akam_onvif.sh` (run over SSH): `enable [PORT]` (default port 80), `disable`, `status`. `enable` refuses when the RTSP
  stream password is not set, then writes the file atomically and runs `akamd ctl ONVIF reload`.
- `ONVIF reload` (control port, loopback only) re-reads the file and starts, stops or restarts the services (also on a
  port change). Video is untouched. WS-Discovery is restarted on every reload so the name scope is rebuilt.
- Cost when on: two threads (HTTP and WS-Discovery), both asleep until traffic arrives (the HTTP poll has no timeout
  with no open connection; the WS-Discovery loop wakes every 30 s, 5 s while its multicast join fails).
- State (`GET onvif.state`): `off`, `on:<port>`, `no_password`, `error:<reason>`.
  - `no_password`: enabled, the RTSP password is ON (or its file broken) but there are no valid clear-text credentials
    for the WS-Security login. Nothing is started.
  - `error:port_in_use`, `error:permission_denied`, `error:http_failed`, `error:discovery_failed`.
  - When the RTSP password is OFF (no file) and ONVIF is enabled, ONVIF does start: read operations are open and write
    operations are refused. The helper script, however, will not turn ONVIF on without the password.
- Open ports when on: TCP `<port>` (default 80) and UDP 3702 (multicast `239.255.255.250`).

## 2. HTTP server

`http_srv.c`: single poll thread, at most 4 connections, request head at most 4096 bytes, body at most 16384 bytes
(413 above), output buffer 1024 bytes, request timeout 5 s, idle timeout 30 s, at most 8 routes. POST for SOAP, GET and
POST allowed per route (405 otherwise). Answers may be deferred (up to 5 s) so a login-failure delay never blocks the
poll loop.

| Service | Path |
|---|---|
| Device | `/onvif/device_service` |
| Media | `/onvif/media_service` |
| Imaging | `/onvif/imaging_service` |

Addresses in answers (XAddr) are built per request from the local address that routes to the client; the camera IP is
never captured at start.

## 3. Authentication

Each operation has an access class:

| Class | Rule |
|---|---|
| `PRE` | Always open: `GetSystemDateAndTime`, `GetCapabilities`, `GetServices`, `GetServiceCapabilities`, `GetScopes`, `GetHostname` |
| `READ` | Needs a login when the RTSP password is ON; open when it is OFF |
| `WRITE` | Needs a login when ON; refused (`ter:NotAuthorized`) when OFF |

A login is a valid WS-Security UsernameToken with `PasswordDigest` in the SOAP header, or a valid HTTP Digest
`Authorization` header. The credentials are the same user and password as the RTSP stream (the clear-text copy in
`/etc/config/akam/credentials`).

- **WS-Security.** `Base64(SHA1(nonce + created + password))`. Failure answers a Sender fault `ter:NotAuthorized` with
  HTTP 400. The nonce is checked against a cache of 1024 entries (8 bytes of SHA-1 each). `Created` must be within
  +/- 5 minutes, but only when the wall clock is 2024 or later (the camera has no RTC). A clock step is tolerated for
  600 s after it happens.
- **HTTP Digest.** Realm `akam`, MD5, `qop=auth` required. The `uri` must equal the request target or be it as an
  absolute `http://` URI. The nonce is stateless (issue time plus keyed hash, valid 120 s); an unknown or old nonce gets
  a new challenge with `stale=true` and no penalty. `nc` must increase per nonce (32 logged-in nonces are tracked); an
  out-of-order `nc` also gets `stale=true` without penalty. Neither token and no or bad Digest header: HTTP 401 with a
  fresh challenge.
- **Failure limiter** (`cred.c`, shared with RTSP). After 3 failed logins an address (8 are tracked, forgotten after
  5 minutes) is locked out for a growing time; its credentials are not evaluated meanwhile. Answers to failures are
  deferred by the limiter's delay.
- A broken `rtsp_auth` file refuses every protected request (fail closed). `cred.c` refuses a credentials file that
  other users can read, and opens it with `O_NOFOLLOW`.
- The pre-auth list above is the ONVIF-required set; it reveals the scopes, services, host name and time only.

## 4. Operations

Unknown operations answer a Receiver fault `ter:Action` / `ter:ActionNotSupported` (HTTP 500).

### Device service

| Operation | Class | Behaviour |
|---|---|---|
| `GetSystemDateAndTime` | PRE | UTC and local time; type `NTP` when NTP servers are configured, else `Manual`; time zone text |
| `GetCapabilities`, `GetServices`, `GetServiceCapabilities` | PRE | Device, Media and Imaging only. `DiscoveryResolve` and `DiscoveryBye` true, `RemoteDiscovery`, `SystemBackup` false |
| `GetScopes` | PRE | Fixed: `Profile/Streaming`, `Profile/S`, `hardware/AK3918AV130`; configurable: `name/<hostname>`, `location/` |
| `GetHostname` | PRE | The network name, `FromDHCP=false` |
| `GetDeviceInformation` | READ | Manufacturer `Akam`, model `LSC Smart Indoor (AK3918AV130)`, firmware version, serial number = MAC without colons, hardware id `AK3918AV130` |
| `GetNetworkInterfaces`, `GetNetworkProtocols`, `GetNetworkDefaultGateway`, `GetDNS`, `GetNTP`, `GetUsers`, `GetDiscoveryMode`, `GetZeroConfiguration` | READ | Read-only views |
| `SetSystemDateAndTime` | WRITE | Only `DateTimeType=Manual`; UTC date and time validated; year 2024 to 2037; sets the clock |
| `SetHostname` | WRITE | RFC 1123 label, 1 to 32 characters, stored lowercase in `/etc/config/akam/hostname` (no write when unchanged) |
| `SetNTP` | WRITE | Refused: "NTP servers are set with akam_ntp.sh over SSH" |
| `SystemReboot` | WRITE | Replies "Rebooting in 1 second", flushes pending picture writes, reboots after 1 s (once) |

### Media service

One profile only (`MaximumNumberOfProfiles=1`): one H.264 video source and encoder, one audio source and encoder (L16),
one audio output and decoder (the speaker). Streaming capabilities: `RTP_RTSP_TCP=true`, `RTPMulticast=false`,
`RTP_TCP=false`, `NonAggregateControl=false`. `SnapshotUri`, `Rotation`, `VideoSourceMode`, `OSD` false.

| Operation group | Class | Behaviour |
|---|---|---|
| `GetProfiles`, `GetProfile`, `GetVideoSources`, `GetVideoSourceConfiguration(s)`, `GetVideoSourceConfigurationOptions`, `GetVideoEncoderConfiguration(s)`, `GetVideoEncoderConfigurationOptions`, `GetGuaranteedNumberOfVideoEncoderInstances`, `GetAudioSources`, `GetAudioSourceConfigurations`, `GetAudioEncoderConfigurations`, `GetAudioOutputs`, `GetAudioOutputConfiguration(s)`, `GetAudioOutputConfigurationOptions`, `GetAudioDecoderConfiguration(s)`, `GetAudioDecoderConfigurationOptions`, `GetCompatible*Configurations` | READ | Describe the fixed profile with the configured resolution, frame rate, bitrate and GOP |
| `GetAudioSourceConfiguration`, `GetAudioSourceConfigurationOptions`, `GetAudioEncoderConfiguration`, `GetAudioEncoderConfigurationOptions` | READ | A no-such-configuration fault |
| `GetStreamUri` | READ | `rtsp://<ip>:554/`, `Timeout PT0S`. The user name and password are not put in the URI |
| `GetSnapshotUri` | READ | Fault `ActionNotSupported` (no JPEG path) |
| `Set*Configuration`, `Add*`, `Remove*`, `CreateProfile`, `DeleteProfile` | WRITE | Refused (the profile is fixed; encoder settings are set with `akam video`) |

### Imaging service

| Operation | Class | Behaviour |
|---|---|---|
| `GetServiceCapabilities` | PRE | No stabilisation, presets or adaptable presets |
| `GetImagingSettings`, `GetOptions`, `GetStatus`, `GetMoveOptions` | READ | `Brightness`, `ColorSaturation`, `Contrast`, `Sharpness` (each -50 to 50), `IrCutFilter` `ON`, `OFF` or `AUTO` |
| `SetImagingSettings` | WRITE | Validated, applied through the same setter as the control port |
| `Move`, `Stop` | WRITE | Fault: no motor |

Mapping of `IrCutFilter` to `night.mode`: `ON` = `day`, `OFF` = `night`, `AUTO` = `auto`. The four sliders map to
`isp.brightness`, `isp.saturation`, `isp.contrast` and `isp.sharpness`. Slider changes are held in RAM and written to
flash after 5 s of quiet (`ONVIF_IMAGING_PERSIST_MS`). A request with `ForcePersistence=true` writes at once, but at most
once per 2 s. A discrete change of `IrCutFilter` is saved at once (only when the mode changed). A pending write is
flushed before `SystemReboot`.

## 5. WS-Discovery (`wsd.c`)

- UDP port 3702, multicast group `239.255.255.250`. Dormant until started: no socket, no thread, no timer.
- Joins the group, and retries every 5 s while the join fails (this covers Wi-Fi reconnects; a new join triggers a Hello).
- Sends a multicast `Hello` (after 0 to 500 ms) at start and a `Bye` at stop.
- Answers `Probe` with `ProbeMatches` and `Resolve` with `ResolveMatches` (unicast to the sender). Types:
  `dn:NetworkVideoTransmitter tds:Device`. Scopes as in `GetScopes`. Anything that is not a matching Probe or Resolve,
  including the camera's own messages, is ignored silently.
- Endpoint reference: a stable version-5 `urn:uuid:` derived from the MAC address with a fixed namespace. The `AppSequence InstanceId` grows at
  every start, using the boot counter in `/etc/config/akam/wsd_instance`, so clients do not discard messages after a
  reboot.

## 6. Not implemented

Events and motion detection, PTZ, Analytics, Recording, Replay, Search, Snapshot (JPEG), a second profile or sub-stream,
multicast streaming, RTP over HTTP tunnelling, user management, `SetNetworkInterfaces`, `SetNTP`, `SetDNS`,
`SetNetworkProtocols`, firmware upgrade and backup, TLS.

## 7. Tests

`tests/onvif_test.sh`, `onvif_media_test.sh`, `onvif_svc_test.sh`, `soap_test.sh`, `http_srv_test.sh`, `wsd_test.sh`,
`sha1_test.sh`, `cred_test.sh`, `akam_onvif_test.sh`, `wizard_onvif_test.sh`, `onvif_check_test.sh`. On a camera:
`python3 -I tools/hw_tests/onvif_check.py <camera-ip> --rtsp`.

## The Rotatable 2K (as built)

The 2K runs the same shared ONVIF core with its own model binding (`platforms/anyka-ak3918av130/onvif_services.c`, `onvif_ptz.c`, `onvif_model.c`). The user part is in [docs/onvif.md](../docs/onvif.md#rotatable-2k-onvif-with-ptz-opt-in). Status: host-tested only, not run on a camera.

- **PTZ service** (`/onvif/ptz_service`): GetServiceCapabilities, GetNodes, GetNode, GetConfigurations, GetConfiguration, GetConfigurationOptions,
  GetStatus (MoveStatus only), GetPresets, ContinuousMove, Stop, GotoHomePosition, GotoPreset. Only the generic velocity space
  (`.../PanTiltSpaces/VelocityGenericSpace`, x and y in -1..1), no zoom, no absolute / relative move (those answer `ter:ActionNotSupported`).
  - **ContinuousMove** maps each axis to the motor's speed divisor: `|v| < 0.05` is no motion; above it the step rate grows linearly from 50 steps/s
    (divisor 20) to 500 steps/s (divisor 2) at 1.0, `divisor = round(1000 / rate)`; a **diagonal is capped at divisor 3** per axis (the verified limit).
    Examples: 1.0 -> 2, 0.8 -> 2, 0.6 -> 3, 0.5 -> 4, 0.3 -> 6, 0.2 -> 8, 0.1 -> 14, 0.05 -> 20. Pan + is right, tilt + is up.
    The motion goes through the same continuous session as person tracking (the motor's safe layer), so it also gets its protections: a speed **ladder** (start at divisor 10,
    one rung per 50 ms up to the target: 10, 6, 4, 3, 2), a **reversal settle** (a direction change on an axis stops the motor and waits 100 ms) and the **tilt cap**
    (`tilt_min_div`, default 3). The **effective** divisor per axis is therefore: pan alone = the mapped one (2 at |v| >= 0.8), **tilt alone = at least 3** (the mapped 2 becomes 3, about 330 steps/s),
    **diagonal = at least 3 on both axes**. Steps count in the wear budget weighted like every other motion. A manual hold counts as motion activity for the drift check and the drift
    recalibration (neither starts during a hold; their idle times restart after it); while a drift recalibration runs, or the head is re-declared after a measured shift, ContinuousMove is refused like during any calibration. Manual moves stay allowed while the position is **untrusted** (`ptz.untrusted`): the soft range still applies.
  - **Timeout** (`PT0.5S`...) is the dead-man **lease**: clamped to 150 ms..2 s, **default 450 ms**. The head stops by itself that long after the last command;
    a **repeat** (identical or not) restarts the lease and sends nothing to the driver when the command is unchanged. A client that repeats every T seconds
    should send `Timeout` a little above T (Akam's 1 s watchdog: `PT1.5S`).
  - **Stop** (`PanTilt` true or absent) stops at once (about 50 ms; a park or a calibration is never interrupted). **GotoHomePosition** = `ptz.home`.
    **GetPresets / GotoPreset**: the stored patrol presets, token = name.
  - **Refused with a SOAP fault** (Receiver, HTTP 500, `ter:Action` / `ter:PTZUnavailable`) while the camera is **parked** (privacy), **calibrating**, its position
    is unknown (`ptz_cal=off`) or the motor is down. Stop is always accepted. Soft limits apply per axis exactly as for tracking.
  - **Priority:** a manual ONVIF move preempts what runs (a step, goto, patrol move, a tracking session) like a manual nudge and stops a patrol; while its
    lease runs, tracking commands (nudge, velocity, home return) are refused; tracking resumes by itself after the Stop or the expiry.
  - Other faults: unknown `ProfileToken` -> `ter:NoProfile`; unknown `PresetToken` -> `ter:NoToken`; another velocity space or a non-zero Zoom ->
    `ter:SpaceNotSupported`; a velocity outside -1..1 or not a number -> `ter:InvalidVelocity`; a bad Timeout -> `ter:InvalidTimeout`.
- **Cost** (stripped ARM binary, measured with `make poc3-dist`): see the release notes of the change; RAM is one 2 KB response block per HTTP request while it runs and nothing when off
  (the 4-connection HTTP thread and the discovery thread only exist while ONVIF is on).
- Tests: `tests/rot2k_onvif_test.py` (python standard library client on loopback: Digest, golden XML in `tests/fixtures/rot2k_onvif/`, mapping table, leases,
  refusals, WS-Discovery) over `tests/rot2k_onvif_server.c`; `tests/ptz_vel_test.sh` (mapping), `tests/ptz_lease_test.sh` (fake-clock leases), the manual cases of
  `tests/rot2k_ptz_cont_unit.c` (controller on a fake driver).
