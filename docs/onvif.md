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
- **Ports:** TCP 80 for the ONVIF service (or `./akam onvif enable 8080`, any port from 1 to 65535 except 21 to 24, 554 and 8091, which the camera already uses) and UDP 3702
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
