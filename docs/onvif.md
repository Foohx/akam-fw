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
Device Manager on Windows (discovery, login, live video, image changes). Other NVRs are not tried yet. The camera's video program starts and stops the
ONVIF server by itself from a flag file, with no reboot.

- **Off by default.** Nothing listens on the ONVIF ports until you ask: `akam onvif enable` (it asks a
  yes/no, default No; `verify` also offers it at the end). `akam onvif disable` closes them again.
- **A password is required.** ONVIF logins are checked against the video password, so run
  `akam rtsp-auth set` first; `enable` refuses without it and says so. The wizard's `verify` offers to set it
  when you say yes to ONVIF. While the password is off, state-changing ONVIF calls (reboot, set the time,
  set the name) are refused anyway.
- **Ports:** TCP 80 for the ONVIF service (or `akam onvif enable 8080`, any port from 1 to 65535 except 21 to 24, 554, 8091 and 8092, which the camera already uses) and UDP 3702
  (multicast discovery), on your LAN only: never forward them to the Internet. Without ONVIF, only 22 and 554
  are open (8092 once an API key exists).
- **In your app:** let it search, or add the camera by hand with its address (`<camera-ip>`, port 80) and the
  video user name and password; the service address is `http://<camera-ip>/onvif/device_service`. If it is not found, see [troubleshooting](troubleshooting.md#an-onvif-app-does-not-find-the-camera).
- The setting is a small file on the camera that survives reboots.
  `akam onvif status` shows what the camera really did (`onvif.state`: `off`, `on:<port>`, `no_password`, or
  `error:port_in_use`). When ONVIF is off the camera runs no ONVIF thread and no socket; when it is on, two idle
  threads that wake up at most every 30 s. A new camera name (`akam hostname`) shows in discovery at the next
  `akam onvif enable` or reboot.

## Try it with ONVIF Device Manager (Windows)

1. Set the video password, and **run it again even if a password already
   exists**, so the clear-text copy that ONVIF needs is stored: `akam rtsp-auth set`. `akam rtsp-auth status` must
   say `ONVIF-ready`.
2. Turn ONVIF on: `akam onvif enable` (default port 80, or `akam onvif enable 8080`). `akam onvif status` should
   show `on:80`.
3. In ODM the camera appears by itself through WS-Discovery (UDP multicast 239.255.255.250:3702, same subnet, the PC
   firewall open for UDP 3702). Otherwise add it with `http://<camera-ip>/onvif/device_service`. The other services are
   `/onvif/media_service` and `/onvif/imaging_service`.
4. Log in with the video user name and password. The live view uses the camera's RTSP stream, so the one-client limit
   applies (a second viewer gets 503: put go2rtc in front).

With the video password off, ONVIF still starts but only answers read calls. Technical details: [`specs/onvif.md`](../specs/onvif.md).

## Rotatable 2K: ONVIF with PTZ (opt-in)

The LSC Rotatable 2K runs the same shared ONVIF core (`src/net`: `http_srv`, `soap`, `onvif`, `wsd`, `cred`) with its own model binding
(`platforms/anyka-ak3918av130/onvif_services.c`, `onvif_ptz.c`, `onvif_model.c`). Its point is **smooth pan/tilt from ONVIF clients**: a held ContinuousMove drives the
motor's continuous mode instead of one 200-step jump at a time. Not tested on the real camera yet.

Directions are the viewer's: pan right / tilt up in ContinuousMove (and in `ptz.vel`, MQTT `ptz_vel` and the step buttons) mean right / up in the picture, also when
the picture is mirrored, flipped or ceiling-mounted (`image.mirror`, `image.flip`, `image.ceiling`: the mirror reverses the motor's pan sign, the flip its tilt sign; presets
and patrol positions are motor positions and do not change). Not verified on the camera.

- **Off by default.** `akam onvif enable [PORT]` (needs the stream password), `akam onvif disable`, `akam onvif status` (through the camera's signed API;
  the admin key of `key-2k` is needed). By hand: `onvif=1` (and optionally `onvif_port=N`) in `/config/akam/akam.conf`, then restart the app.
  `GET /akam/v1/state` shows `onvif.state` (`off`, `on:<port>`, `no_password`, `error:bad_port|port_in_use|permission_denied|http_failed|discovery_failed|auth_broken`);
  `onvif.enabled` and `onvif.port` are settings of the same registry, writable with the API and the assistant (`cfgreg.c`).
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
- **PTZ in an app:** continuous pan/tilt (hold to move, release to stop), go home, go to a stored preset. No zoom, no absolute or relative move, no audio
  in ONVIF (the microphone is in the RTSP stream). Every Set/Add/Remove/Create call is refused. The speed mapping, the leases, the faults and the
  tests are in [`specs/onvif.md`](../specs/onvif.md#the-rotatable-2k-as-built).
