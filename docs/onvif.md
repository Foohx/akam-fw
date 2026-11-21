# ONVIF: NVRs and ONVIF apps

This page is for people who want an NVR (network video recorder) or an ONVIF app to find the camera. ONVIF is optional and
off by default; the plain RTSP stream needs none of this.

ONVIF is the standard that NVRs, ONVIF Device Manager, Synology Surveillance Station and Home Assistant use to
find a camera on the network and pull its video. Akam offers **ONVIF-compatible services (a subset of the ONVIF
Profile S feature set: WS-Discovery, device, media, imaging)**, so the camera shows up in those apps and they get its
RTSP stream. The imaging service lets apps read and change brightness, contrast and similar settings.

> **Akam is not an ONVIF conformant product, has not been tested with the ONVIF test tool and is not listed by ONVIF.
> ONVIF is a trademark of ONVIF, Inc.**

It was checked on a real camera with a check tool (`tools/hw_tests/onvif_check.py`, 34 of 34 checks) and with ONVIF
Device Manager on Windows (discovery, login, live video, image changes). Other NVRs are not tried yet. An image built
before the `onvif` command has no ONVIF (run `./akam update` first). The camera's video program starts and stops the
ONVIF server by itself from a flag file, with no reboot.

- **Off by default.** Nothing listens on the ONVIF ports until you ask: `./akam onvif enable` (it asks a
  yes/no, default No; `verify` also offers it at the end). `./akam onvif disable` closes them again.
- **A password is required.** ONVIF logins are checked against the video password, so run
  `./akam rtsp-auth set` first; `enable` refuses without it and says so. The wizard's `verify` offers to set it
  when you say yes to ONVIF. While the password is off, state-changing ONVIF calls (reboot, set the time,
  set the name) are refused anyway.
- **Ports:** TCP 80 for the ONVIF service (or `./akam onvif enable 8080`, any port from 1 to 65535 except 21 to 24, 554, 8091 and 8092, which the camera already uses) and UDP 3702
  (multicast discovery), on your LAN only: never forward them to the Internet. Without ONVIF, only 22, 554
  and 8091 are open.
- **In your app:** let it search, or add the camera by hand with its address (`<camera-ip>`, port 80) and the
  video user name and password; the service address is `http://<camera-ip>/onvif/device_service`. If it is not found, see [troubleshooting](troubleshooting.md#an-onvif-app-does-not-find-the-camera).
- The setting is a small file on the camera that survives reboots; an older image says to reboot the camera to apply it.
  `./akam onvif status` shows what the camera really did (`onvif.state`: `off`, `on:<port>`, `no_password`, or
  `error:port_in_use`). When ONVIF is off the camera runs no ONVIF thread and no socket; when it is on, two idle
  threads that wake up at most every 30 s. A new camera name (`./akam hostname`) shows in discovery at the next
  `./akam onvif enable` or reboot.

## Try it with ONVIF Device Manager (Windows)

1. Update the camera (`./akam update`), then set the video password, and **run it again even if a password already
   exists**, so the clear-text copy that ONVIF needs is stored: `./akam rtsp-auth set`. `./akam rtsp-auth status` must
   say `ONVIF-ready`.
2. Turn ONVIF on: `./akam onvif enable` (default port 80, or `./akam onvif enable 8080`). `./akam onvif status` should
   show `on:80`.
3. In ODM the camera appears by itself through WS-Discovery (UDP multicast 239.255.255.250:3702, same subnet, the PC
   firewall open for UDP 3702). Otherwise add it with `http://<camera-ip>/onvif/device_service`. The other services are
   `/onvif/media_service` and `/onvif/imaging_service`.
4. Log in with the video user name and password. The live view uses the camera's RTSP stream, so the one-client limit
   applies (a second viewer gets 503: put go2rtc in front).

With the video password off, ONVIF still starts but only answers read calls. Technical details: [`specs/onvif.md`](../specs/onvif.md).

## Rotatable 2K: ONVIF with PTZ (opt-in)

The LSC Rotatable 2K runs the same shared ONVIF core (`src/net`: `http_srv`, `soap`, `onvif`, `wsd`, `cred`) with its own model binding
(`platforms/anyka-ak3918av130/akam2k_onvif*.c`). Its point is **smooth pan/tilt from ONVIF clients**: a held ContinuousMove drives the
motor's continuous mode (`a2k_ptz_vel`) instead of one 200-step jump at a time. Not tested on the real camera yet.

- **Off by default.** `akam2k.conf`: `onvif=1` (and optionally `onvif_port=N`), then restart the app. `GET /akam/v1/state` shows `onvif.state`
  (`off`, `on:<port>`, `no_password`, `error:bad_port|port_in_use|permission_denied|http_failed|discovery_failed|auth_broken`);
  `onvif.enabled` / `onvif.port` show what was asked (read only: the setting lives in `akam2k.conf`, the API writes no flash for it).
- **Port:** TCP **8000** by default, any port 1024..65535 except 22, 554, 8091, 8092, the RTSP port in use and the API port; WS-Discovery on UDP 3702
  (LAN only). The stock firmware's 5000, 8554 and 6668 are not used. RTSP stays on 554.
- **Login = the stream credentials** (the RTSP `rtsp.conf` file, user and password you already set for RTSP) with **HTTP Digest** (realm `akam`, MD5,
  qop auth), the same failure limiter and per-address lockout as RTSP. Open without a login (ONVIF's pre-authentication list): GetSystemDateAndTime,
  GetCapabilities, GetServices, GetServiceCapabilities, GetScopes, GetHostname, and the PTZ GetServiceCapabilities. WS-Security `PasswordDigest` is
  **refused** (it needs the clear-text password, which the 2K does not store: only the Digest hash); clients fall back to HTTP Digest.
- **No stream password, no ONVIF.** With `onvif=1` and no stream password (or an invalid credentials file) the service is **not started**
  (`no_password` / `error:auth_broken`, logged): an open PTZ on the LAN is never offered. If the password is removed while ONVIF runs, reads stay open
  like RTSP and every PTZ command is refused (write class needs a login).
- **Profiles:** `stream0` (main, `rtsp://<ip>:<rtsp port>/stream0`, H.264 Main 2304x1296) and `stream1` (sub, `/stream1`, H.264 Baseline 640x360), each with
  the PTZ configuration `ptzcfg` of node `ptznode`. Frame rate, GOP and bit rate are the live video settings. Read only: every Set/Add/Remove/Create is
  refused. No audio (the L16 microphone cannot be described by ONVIF Media 1; it is in the RTSP SDP), no snapshots, no imaging service.
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
