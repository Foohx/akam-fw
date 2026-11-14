# ONVIF

`ak_rtsp` can answer a subset of ONVIF Profile S (Device, Media and Imaging services plus WS-Discovery), so NVRs and apps
find the camera and play its stream. It is **off by default**. While off it costs no thread, no socket and no timer.

Status: **hardware-validated** on one camera: `tools/hw_tests/onvif_check.py` 34 of 35 then 34 of 34 checks (including RTSP
Digest `DESCRIBE`), WS-Discovery, WS-Security with the camera clock unset, imaging set and get with restore, no flash
write, ONVIF Device Manager (discovery, login, live video, live imaging changes). The fuzzing of the SOAP and HTTP
parsers has not been done.

## 1. Switching it on and off

- Flag file `/etc/config/akam/onvif`, exactly two lines in any order: `enabled=1|0` and `port=N` (N is 1 to 65535 except
  21 to 24, 554 and 8091). LF; a trailing CR is tolerated. Absent file means off. Anything else (unknown or repeated key,
  missing key, bad number, a third line, over 64 bytes) means off, with one log line.
- `akam_onvif.sh` (run over SSH): `enable [PORT]` (default port 80), `disable`, `status`. `enable` refuses when the RTSP
  stream password is not set, then writes the file atomically and runs `ak_rtsp ctl ONVIF reload`.
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
