# Features and protocols

Everything Akam implements, with its default, its port and how well it is tested. The first part is the LSC Smart Indoor 1080p
(article `3215672.3`), the section [LSC Rotatable 2K](#lsc-rotatable-2k) is the 2K (article `3225722`); features both cameras share
say so in their notes. Technical details of each item: [`specs/`](../specs/README.md).

Two kinds of status matter. **Hardware-validated** means it ran on a real camera; **host-tested only** means it passed automated
tests on a PC and has never run on a camera. The status words refer to the units and builds named in the
[testing status](testing-status.md#which-build-the-hardware-results-come-from), not necessarily to the current code. Unknown words:
[glossary](README.md#glossary).

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
| RTSP 1.0 server, H.264 Main profile, 1920x1080 | on | TCP 554 | hardware-validated | Any URL path gives the same stream; the canonical form, shared with the Rotatable 2K, is `rtsp://<ip>:554/stream0` (main). The 1080p has no sub stream on purpose, so `/stream1` is the same picture as `/stream0` (the 2K has a real 640x360 `/stream1`). TCP interleaved and UDP transports. **One client at a time**; a second one gets `503` |
| Frame rate | 15 fps | | hardware-validated (15, 25) | Only 10, 15, 20 or 25 fps. 20 fps was only seen in a RAM test. Night mode always runs at 10 fps. 30 fps is refused (picture too dark) |
| Bitrate and key-frame interval | 1536 kbit/s, 2 s | | host-tested only | Bitrate 500 to 3000 kbit/s, key frames every 1 to 4 s. Applied at the next restart of the video service |
| Microphone track | off (opt-in) | in the RTSP stream | hardware-validated | **L16** (raw 16-bit PCM, 8 kHz, mono), not G.711. Turn it on with `akam audio mic on` (needs a reboot); an existing explicit setting is kept |
| RTSP Digest authentication | off | TCP 554 | hardware-validated | One user and password, set with `akam rtsp-auth set` |
| Speaker output and two-way audio (ONVIF audio backchannel, G.711 PCMU/PCMA) | on, needs the video password | in the RTSP session | hardware-validated (talk, two 1080p); host-tested only (`say`, speaker volume and beep on a 1080p) | Used by `akam talk`, go2rtc, Frigate, Home Assistant. Speaker volume default 100 |
| Picture settings (brightness, contrast, saturation, sharpness) | 0; sharpness -15 | via the signed API (assistant) | checked in RAM on one camera | Not yet checked on the final image. These four, `img.wb` and `night.mode` are saved on the camera and applied at every start (a change made from the network in debug mode stays in RAM) |
| Default sharpness -15 on every ISO level of the ISP sharpening (`isp.sharpness`, `img.sharp_all`) | on | `SET isp.sharpness 0` and `SET img.sharp_all 0` for the stock picture | measured on one camera (static scene, P frames 7.9 KB to 1.8 KB) | Long-run picture quality in all lights not yet judged |
| Default 3D noise-reduction bias 2 (`isp.denoise`, the 3D_NR tables of each light level come from the level 2 steps higher) | on | `akam picture denoise 0` for the camera's own tables; 1 to 4 for other strengths | A/B on two cameras (RAM, lamp light, ISO level 9): less pulsing at each key frame, 3 to 7 % less static detail | Not yet judged in all lights; refused (stock tables kept, logged) when the live table is not as decoded |
| Day/night switching with IR-cut filter and IR LEDs | automatic | | hardware-validated | Includes a night probe so the IR LEDs do not fool the exposure reading |
| Status LED (front, blue) | on | | hardware-validated | `on`, `off`, or `auto` (blinks while starting or streaming) |

| Image flip, mirror and ceiling mount | off (`image.flip`, `image.mirror`, `image.ceiling`) | via the signed API, assistant (`akam picture flip on`, options screens, saved config) | host-tested only, **not yet hardware-validated on the 1080p** | Same names as the 2K. Live, done by the sensor readout through the kernel ISP call (no CPU cost, sent again after every day/night switch), Home Assistant switches over MQTT, saved in `/etc/config/akam/orient` only when you change it. [How it works and what is unknown](../specs/video-and-streaming.md#57-flip-and-mirror-reverse-engineering-of-2026-11-23-not-yet-hardware-validated-on-the-1080p) |

Not offered: other resolutions, a second (sub) stream (on purpose: `/stream1` is `/stream0`), snapshots, motion detection, recording to microSD.

### Parity with the Rotatable 2K (settings and streams)

| Feature | 1080p | 2K |
|---|---|---|
| Image flip / mirror / ceiling mount (`image.flip`, `image.mirror`, `image.ceiling`) | yes, same rows, API and assistant options; **not yet hardware-validated** | yes, hardware-validated (2026-11-22) |
| Settings in the Go assistant (options screens, `config` save and apply, command line) | all rows, flip and mirror included (over the signed API) | all rows (over the signed API) |
| Sub stream `/stream1` | the same picture as `/stream0`, on purpose | real 640x360 stream |
| Home Assistant switches for flip and mirror | yes (`Image flip`, `Image mirror`; host-tested only) | yes |

## Home automation and discovery

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| ONVIF-compatible services: Device, Media, Imaging | off | TCP 80 (configurable) | hardware-validated on one camera | See the ONVIF note below. Needs the video password to be turned on |
| WS-Discovery (ONVIF) | off (with ONVIF) | UDP 3702, multicast `239.255.255.250` | hardware-validated on one camera | Found by ONVIF Device Manager on Windows |
| MQTT 3.1.1 client | off | outbound TCP to your broker (usually 1883) | host-tested only | One connection, no TLS (the password travels in clear on the LAN). The camera never runs a broker |
| Home Assistant MQTT discovery | on, when MQTT is on | | host-tested only | One device with sensors, switches, numbers and buttons ([MQTT](mqtt.md)) |
| Control port (text) | closed to the network: it listens on 127.0.0.1 only (opt-in to the LAN: `akam control-port open`, setting `control.lan`) | TCP 8091 | host-tested only since it was closed; reads and local writes were hardware-validated | Text commands (`LIST`, `GET`, `SET`): metrics for monitoring tools once opened, settings from the camera itself (SSH scripts). The assistant uses the signed API instead. Writes from the network stay refused; the on-camera `DEBUG` command (for the PC tuning tool) lets them in for a while, only with the opt-in on |
| Signed control API | off (opens only while an API key exists) | TCP 8092 | host-tested only | The same signed JSON API as the Rotatable 2K, and the one way the assistant reads and changes the 1080p: settings (LED mode and patterns, night mode, picture, white balance, video, speaker and microphone, ONVIF and MQTT switches, the text-port opt-in), state, events, a "find the camera" LED action and a speaker beep. The assistant creates its key over SSH once (`akam api-key`, asked at the end of an install and at the first settings command of an existing install); by hand: `akamd --api-key new --scope admin --name NAME` ([reference](porting/lsc-1080p/control-api.md)) |

**ONVIF note.** ONVIF-compatible services (a subset of the ONVIF Profile S feature set: WS-Discovery, device, media,
imaging). Akam is not an ONVIF conformant product, has not been tested with the ONVIF test tool and is not listed by
ONVIF. ONVIF is a trademark of ONVIF, Inc.

## System and security

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| SSH (Dropbear 2026.94) | on | TCP 22 | hardware-validated | Public-key login only, **ed25519 keys only**, no passwords, no SFTP, root only |
| Telnet and FTP lockdown | closed | | hardware-validated | Patched at build time and double-checked by a helper script every minute |
| NTP clock sync | off | outbound UDP 123 | hardware-validated | The camera has no clock chip and boots in the past. `akam ntp set ntp.example.net` (no default server: you choose it, for example your router) |
| DHCP host name | `akam-XXXX` | UDP 68 (client) | hardware-validated | `XXXX` = last 4 hex digits of the Wi-Fi MAC. Change with `akam hostname` |
| Wi-Fi supervisor | on | | in the image, never triggered on hardware | Restarts the Wi-Fi client or DHCP when the camera loses its address |
| Encoder watchdog | on | | in the image, never triggered on hardware | The program exits and restarts if the encoder stalls for 30 s |
| Hardware watchdog | on (stock) | | stock | Unchanged |
| Kernel log filter | on | | hardware-validated | Drops noisy lines so logs stay small in RAM |
| Update over SSH (Akam to Akam) | | | hardware-validated | `akam update`, read-back verified |
| Maintenance flag (microSD) | off | | hardware-validated (install) | A card marked `AKAM_MAINTENANCE` opens the maintenance shell: treat the card as physical access |

Open ports after a normal boot: **22 and 554** (TCP) and 68 (UDP client), on both cameras (this page owns the ports list; other pages link here). Nothing else, unless you enable ONVIF (80 and 3702), create an API key (8092), open the text port to the network (8091, `akam control-port open`) or enable MQTT (outbound only). The RTSP stream (and the microphone, once you switch it on) has no password until you set one with `akam rtsp-auth set`: keep the camera on your LAN.

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
| `akam` (Go program, six targets) | Install, update, restore, settings, talk, configs, camera options screen, both cameras | partly tested ([platforms](assistant.md#tested-platforms)) |
| `./akam` (bash wizard, Linux, 1080p; to be retired) | Install, update, restore, settings | used for the real 1080p installs |
| `akam-talk` | Send a voice or a sound file to the camera speaker | hardware-validated |
| `tools/hw_tests/akam_hwcheck.sh` | Read-only post-install check over SSH (`akam verify`) | hardware-validated |

## LSC Rotatable 2K

Same Akam core, other hardware (details and evidence per feature: [2K page](porting/lsc-rotatable-2k/README.md)). Model
features are set in `firmware/lsc-rotatable-2k-ak3918av130/model.mk`.

| Feature | Default | Port | Status | Notes |
|---|---|---|---|---|
| RTSP H.264, two streams | on | TCP 554 | hardware-validated | `/stream0` 2304x1296, `/stream1` 640x360; optional Digest login |
| Microphone track (L16, 8 kHz) | off (opt-in: `audio.mic_enable`, `akam audio mic on`, or `audio=1`) | in the RTSP session | hardware-validated (track), setting through the API host-tested only | Both streams carry it when on; set at install time (`prepare-2k-sd`) or later through the signed API |
| Speaker output and two-way audio (ONVIF backchannel, G.711) | speaker off (`audio.out_enable` 0), talk on once it is on; needs the stream password | in the RTSP session (TCP only) | hardware-validated in part (2026-11-23): beep and talk; the rest host-tested only | See note 1 below |
| Picture: saturation (`isp.saturation`, -50 to 50) | 0 (the camera's own) | via API, options screen | **not validated on a camera**: host-tested only (fake ISP) | Same name, range and meaning as the 1080p; through `ak_vpss_effect_set` type 2; saved; put back after every day/night switch |
| Picture: denoise (`isp.denoise`, 0 to 4) | 0 (the camera's own tables; the 1080p defaults to 2) | via API, options screen | **not validated on a camera**: host-tested only (fake ISP) | Same rule as the 1080p (each light level takes the 3D noise-reduction record N levels higher); refused, nothing sent, when the live table is not as decoded; saved; replayed after a day/night switch |
| Picture: white balance (`img.wb`, `conf` or `manual`) | `conf` (the camera's automatic white balance) | via API, options screen | **not validated on a camera**: host-tested only (fake ISP) | Same names as the 1080p; `manual` freezes the white balance (day only); **not saved on the 2K** (back to `conf` at a restart; the 1080p saves it) |
| Pan/tilt, presets, patrol, home | on | via API, ONVIF, MQTT | see [PTZ findings](porting/lsc-rotatable-2k/ptz-findings.md) | Soft limits, drift correction |
| Person detection (NPU) and tracking | detection on (`detect=1`), tracking off (`track.enabled`, `track=1`) | | see [tracking](porting/lsc-rotatable-2k/tracking.md) | Hardware tests of 2026-11-16 and 2026-11-17 are recorded there |
| Image flip, mirror and ceiling mount | off (`image.flip`, `image.mirror`, `image.ceiling`; Home Assistant switches) | via API, MQTT | hardware-validated: flip vertical, mirror horizontal, round trip (2026-11-22); colour, day/night and ceiling tracking not yet checked | Live, saved only when you change it. The zones, tracking, PTZ directions and the scene check follow the orientation ([how](porting/lsc-rotatable-2k/isp-controls.md#33-what-follows-the-orientation-one-convention-for-the-whole-app)) |
| Front button, image zones, night, LEDs | on | | see [button and zones](porting/lsc-rotatable-2k/button-and-zones.md) | Host-tested with fake hardware |
| ONVIF with pan/tilt, WS-Discovery | off (`onvif.enabled`, `akam onvif enable`, or `onvif=1` in `akam.conf`) | TCP 8000, UDP 3702 | host-tested only | [ONVIF](onvif.md); needs the stream password |
| MQTT, Home Assistant discovery | off (`mqtt.*`, `akam mqtt set`, or the file `mqtt`) | outbound | host-tested only | [MQTT](mqtt.md) |
| Stream password (Digest) | off (`rtsp.user` + write-only `rtsp.password`, `akam rtsp-auth set`, or `rtsp.conf`) | TCP 554 | host-tested only | Only the hash is stored; same as the 1080p `rtsp-auth` |
| Time sync (NTP) | off (`ntp.servers`, `akam ntp set`, or the file `ntp`) | UDP 123 out | host-tested only | Same as the 1080p `ntp` |
| Signed control API | closed until an API key exists | TCP 8092 | hardware-used: settings, actions and state reads (2026-11-23, current build family); full suite host-tested | See note 2 below. [2K API](porting/lsc-rotatable-2k/control-api.md) |
| SSH (Dropbear) | on | TCP 22 | hardware-validated | ed25519 key only; needs `authorized_keys` on the card or in `/config/akam` |
| Card-less boot, SD rescue | on | | hardware-validated | [card-less appfs](porting/lsc-rotatable-2k/cardless-appfs.md) |
| Video latency bound (the encoder never queues old frames) | on | | camera side validated (2026-11-23: frame age at send avg about 5 ms, max about 20 ms); glass to glass not yet measured | The log prints `frame age at send avg/max` every 10 s: [video latency](porting/lsc-rotatable-2k/video-latency.md), [troubleshooting](troubleshooting.md#the-picture-is-late) |

**Notes on the 2K table.**

1. Speaker and two-way audio: the same engine and settings as the 1080p (`audio.out_enable`, `audio.out_volume` 0..100 default 100,
   `audio.out_gain` 0..5, `audio.out_amp`, `audio.talk_enable`, `audio.talk_duplex`, action `audio.beep`), works with `akam talk`,
   `akam say`, `akam audio beep`, go2rtc and Frigate over TCP; the ak_pcm driver switches the amplifier. Seen on the camera on
   2026-11-23: the beep and talk with `tools/akam-talk` over TCP (checks SPK-1 and TALK-2 of the
   [checklists](hardware-test-checklists.md#2k-speaker-two-way-audio-and-picture-settings)); not yet: volume and gain, go2rtc and Frigate.
   The assistant's own `audio beep` and `say` on a 2K were written after that session and have not run on a camera. The speaker
   stays off by default until the rest is checked. **No MQTT entities for the speaker on the 2K yet** (no volume, talk or beep in Home
   Assistant); the 1080p has them. [Findings](porting/lsc-rotatable-2k/audio-findings.md#10-speaker-and-two-way-audio).
2. Signed API on the 2K: the speaker and latency settings were written on the camera with a curl-based signed helper on
   2026-11-23, flip and mirror on 2026-11-22; the assistant's 2K commands (`akam settings`, `led`, `picture`...) are host-tested only.
