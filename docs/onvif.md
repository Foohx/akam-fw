# ONVIF: NVRs and ONVIF apps

This page is for people who want an NVR (network video recorder) or an ONVIF app to find the camera. ONVIF is optional and
off by default; the plain RTSP stream needs none of this.

ONVIF is the standard that NVRs, ONVIF Device Manager, Synology Surveillance Station and Home Assistant use to
find a camera on the network and pull its video. Akam implements the discovery (WS-Discovery), the device
service, the media service and the imaging service of Profile S, so the camera shows up in those apps and they get its RTSP stream.
An image built before the `onvif` command has no ONVIF at all (run `./akam update` first); `./akam onvif status`
then says it does not know. The camera's video program (`ak_rtsp`) starts and stops the ONVIF server and the
discovery by itself from a flag file, with no reboot. The imaging service lets apps read and change brightness,
contrast and similar settings. It is **validated on a real camera** (akam-01, 2026-11-11): ONVIF Device Manager on Windows finds it, logs in, plays the live
video and changes the image; `tools/hw_tests/onvif_check.py` passes 34/34. Other NVRs are not tried yet (steps in
the [install notes](akam_v1_install.md#onvif-with-onvif-device-manager)).

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
