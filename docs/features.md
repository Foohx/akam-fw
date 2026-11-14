# Features and protocols

Everything Akam implements on the validated camera (LSC Smart Indoor 1080p, article `3215672.3`), with its default,
its port and how well it is tested. Technical details of each item are in [`specs/`](../specs/README.md).

**Status labels**

| Label | Meaning |
|---|---|
| hardware-validated | Ran on a real camera as part of a flashed image or a RAM test, and the result was recorded |
| host-tested only | Covered by tests on a PC (fake cameras, fixtures). Never run on a camera |
| experimental | Opt-in, incomplete or unproven. Do not rely on it |

The evidence, test by test: [testing status](testing-status.md).

## Video and audio

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| RTSP 1.0 server, H.264 Main profile, 1920x1080 | on | TCP 554 | hardware-validated | Any URL path gives the same stream. TCP interleaved and UDP transports. **One client at a time**; a second one gets `503` |
| Frame rate | 15 fps | | hardware-validated (15, 25) | Only 10, 15, 20 or 25 fps. 20 fps was only seen in a RAM test. Night mode always runs at 10 fps. 30 fps is refused (picture too dark) |
| Bitrate and key-frame interval | 1536 kbit/s, 2 s | | host-tested only | Bitrate 500 to 3000 kbit/s, key frames every 1 to 4 s. Applied at the next restart of the video service |
| Microphone track | on | in the RTSP stream | hardware-validated | **L16** (raw 16-bit PCM, 8 kHz, mono), not G.711. Can be turned off (`./akam audio mic off`, host-tested only) |
| RTSP Digest authentication | off | TCP 554 | hardware-validated | One user and password, set with `./akam rtsp-auth set` |
| Speaker output and two-way audio (ONVIF audio backchannel, G.711 PCMU/PCMA) | on, needs the video password | in the RTSP session | hardware-validated (talk), host-tested only (`say`, speaker volume and beep) | Used by `./akam talk`, go2rtc, Frigate, Home Assistant. Speaker volume default 100 |
| Picture settings (brightness, contrast, saturation, sharpness) | 0 | via 8091 | checked in RAM on one camera | Not yet checked on the final image |
| Day/night switching with IR-cut filter and IR LEDs | automatic | | hardware-validated | Includes a night probe so the IR LEDs do not fool the exposure reading |
| Status LED (front, blue) | on | | hardware-validated | `on`, `off`, or `auto` (blinks while starting or streaming) |

Not offered: other resolutions, a second (sub) stream, mirror or flip, snapshots, motion detection, recording to microSD.

## Home automation and discovery

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| ONVIF-compatible services: Device, Media, Imaging | off | TCP 80 (configurable) | hardware-validated on one camera | See the ONVIF note below. Needs the video password to be turned on |
| WS-Discovery (ONVIF) | off (with ONVIF) | UDP 3702, multicast `239.255.255.250` | hardware-validated on one camera | Found by ONVIF Device Manager on Windows |
| MQTT 3.1.1 client | off | outbound TCP to your broker (usually 1883) | host-tested only | One connection, no TLS (the password travels in clear on the LAN). The camera never runs a broker |
| Home Assistant MQTT discovery | on, when MQTT is on | | host-tested only | One device with sensors, switches, numbers and buttons ([MQTT](mqtt.md)) |
| Control port | on, read-only from the network | TCP 8091 | hardware-validated | Text commands (`LIST`, `GET`, `SET`): metrics for monitoring, settings from the camera itself. `./akam debug on` lets the network write for a while |

**ONVIF note.** ONVIF-compatible services (a subset of the ONVIF Profile S feature set: WS-Discovery, device, media,
imaging). Akam is not an ONVIF conformant product, has not been tested with the ONVIF test tool and is not listed by
ONVIF. ONVIF is a trademark of ONVIF, Inc.

## System and security

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| SSH (Dropbear 2026.94) | on | TCP 22 | hardware-validated | Public-key login only, **ed25519 keys only**, no passwords, no SFTP, root only |
| Telnet and FTP lockdown | closed | | hardware-validated | Patched at build time and double-checked by a helper script every minute |
| NTP clock sync | off | outbound UDP 123 | hardware-validated | The camera has no clock chip and boots in the past. `./akam ntp set pool.ntp.org` |
| DHCP host name | `akam-XXXX` | UDP 68 (client) | hardware-validated | `XXXX` = last 4 hex digits of the Wi-Fi MAC. Change with `./akam hostname` |
| Wi-Fi supervisor | on | | in the image, never triggered on hardware | Restarts the Wi-Fi client or DHCP when the camera loses its address |
| Encoder watchdog | on | | in the image, never triggered on hardware | The program exits and restarts if the encoder stalls for 30 s |
| Hardware watchdog | on (stock) | | stock | Unchanged |
| Kernel log filter | on | | hardware-validated | Drops noisy lines so logs stay small in RAM |
| Update over SSH (Akam to Akam) | | | hardware-validated | `./akam update`, read-back verified |
| Maintenance flag (microSD) | off | | hardware-validated (install) | A card marked `AKAM_MAINTENANCE` opens the maintenance shell: treat the card as physical access |

Open ports after a normal boot: **22, 554, 8091** (TCP) and 68 (UDP client). Nothing else, unless you enable ONVIF
(80 and 3702) or MQTT (outbound only).

## What Akam removes or disables

| Item | What happens |
|---|---|
| Tuya cloud application (`anyka_ipc`) | Removed from the APP partition. This is the program that talks to the Tuya cloud |
| Cloud connection, telemetry | None. The camera contacts no server unless you enable NTP or MQTT |
| Telnet (ports 23 and 24) and FTP (port 21) | Launch lines disabled in the patched start script; a helper closes them again if they ever start |
| Password logins | None. SSH accepts keys only |
| Vendor over-the-air updates | Gone with the application |

The stock firmware's own behaviour is not described here beyond these facts, which come from the firmware files
themselves. Kept stock parts: the boot loader, the kernel and its drivers, the root file system, the Wi-Fi and
network scripts, the hardware watchdog and two local helper programs (`cmdserver`, `msgserver`).

## PC-side tools

| Tool | What it does | Status |
|---|---|---|
| `./akam` (bash wizard, Linux) | Install, update, restore, settings | the reference; used for real installs |
| `akam` (Go program, six targets) | Same, plus talk, configs, camera options screen | partly tested ([platforms](assistant.md#tested-platforms)) |
| `akam-talk` | Send a voice or a sound file to the camera speaker | hardware-validated |
| `tools/hw_tests/akam_hwcheck.sh` | Read-only post-install check over SSH (`./akam verify`) | hardware-validated |
