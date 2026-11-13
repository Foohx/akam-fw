# Using Akam day to day

This page is for when Akam already runs on your camera: every setting and command, the ports that are open,
how to watch the stream, how to update, and the security notes. Not installed yet? Start with the
[README](../README.md) and the [compatible cameras](compatible-cameras.md) page.

## Features

- **Video and audio:** RTSP, H.264 1080p at 15 fps by default (10, 15, 20 or 25 selectable) plus the microphone, on port 554. Optional Digest login
  (VLC, ffmpeg, go2rtc, Frigate). The camera serves **one RTSP client at a time**: a second one is answered
  at once with `503 Service Unavailable`. To serve several viewers or record and watch together, put
  [go2rtc](https://github.com/AlexxIT/go2rtc) (or MediaMTX) in front of the camera.
- **Two-way audio:** the ONVIF audio backchannel on port 554 (go2rtc, Frigate, Home Assistant, `./akam talk`).
- **LEDs and night:** front LED on, off or status light; infrared LEDs auto or off; automatic day/night
  switching.
- **Port 8091:** read-only metrics (`sys.*`, `net.*`, `video.*`, `rtsp.*`) for the network. Changes are only
  accepted from the camera itself, or from the network after `./akam debug on` (loopback and timed).
- **ONVIF Profile S (optional, off by default; implemented and checked against real clients, not conformance-certified):** WS-Discovery, device, media and imaging services for NVRs and
  ONVIF Device Manager. Needs the video password (`./akam rtsp-auth set`, re-run it once after an update so the
  ONVIF login copy exists). Checked on one real camera with a check tool and ONVIF Device Manager, see [testing status](testing-status.md).
- **Settings you can change:** video bitrate, frame rate and key frame interval; brightness, contrast,
  saturation, sharpness, white balance and day/night mode; speaker volume, talk and microphone on/off.
- **Time:** opt-in NTP (`./akam ntp set SERVER`); with it off the camera sends no time traffic and its clock
  starts in 1970.
- **Robustness:** the video service exits by itself if the encoder stalls, the camera's wrapper restarts it with a
  growing delay, and a supervisor brings the Wi-Fi back when it drops.
- **Admin:** SSH with a key only; Telnet and FTP are off.
- **Install:** a guided wizard (`./akam`) from a Linux PC; only the APP partition is rewritten.

What was tested on a real camera and what was only tested on a PC is listed in
[docs/testing-status.md](testing-status.md); the units used are in
[compatible cameras](compatible-cameras.md).

## Open ports

| Port | Service | When |
|---|---|---|
| 22/tcp | SSH (Dropbear, public key only) | always |
| 554/tcp | RTSP video, audio and the talk backchannel | always |
| 8091/tcp | metrics and control, read-only from the network | always |
| 80/tcp (or the port you chose) | ONVIF device, media and imaging services | only after `./akam onvif enable` |
| 3702/udp | ONVIF WS-Discovery (multicast) | only after `./akam onvif enable` |

Telnet (23, 24) and FTP (21) are closed. UDP 123 is used outgoing only, and only if you set an NTP server.
Never expose any of these to the Internet.

## Several cameras

Each camera has its own name, progress, log and folders. Switch with `./akam --camera NAME`, or press
`c` in the menu (list, "New camera", "Register an existing Akam camera"). `./akam cameras` lists them,
`./akam rename-camera NEW` renames the current one. A camera that already runs Akam (flashed by
hand, or by an earlier version) is recorded without redoing anything:

```bash
./akam --camera salon --ip 192.0.2.50 [--backup-dir DIR] register
```

## Command reference

`./akam help` is the full text. Run `./akam` with no command for the menu.

| Command | What it does |
|---|---|
| `doctor`, `get-kit`, `build` | check this PC, download and verify the prebuilt kit, or build from source |
| `prepare-backup-sd`, `find-camera`, `backup`, `image`, `prepare-flash-sd`, `flash`, `verify` | the installation steps, in order |
| `update`, `restore` | flash a newer image over SSH, or put your original APP back |
| `wipe-sd` | erase a card that holds your Wi-Fi password |
| `status`, `step NAME` | show every step; run one step again |
| `cameras`, `rename-camera NEW`, `register` | work with several cameras |
| `hostname [NAME]` | the camera's network name |
| `rtsp-auth [set\|disable\|status]` | the Digest password on the stream (also needed by ONVIF and talk) |
| `led [show \| status on\|off\|auto] [ir auto\|off]` | front LED and infrared LEDs |
| `audio [show \| volume N \| beep \| on \| off \| talk on\|off \| mic on\|off]` | speaker and microphone |
| `picture [show \| brightness\|contrast\|saturation\|sharpness N \| wb \| night \| reset]` | the image |
| `video [show \| bitrate N \| fps N \| gop N]` | encoder settings |
| `onvif [enable [PORT] \| disable \| status]` | ONVIF, off by default |
| `ntp [status \| set SERVER... \| off]` | time synchronisation, off by default |
| `debug [on [MINUTES] \| off \| status]` | let the network change settings on port 8091 for a while |
| `talk [SECONDS \| test]`, `say FILE` | speak or play a sound through the camera speaker |

## Update, change the name, add a video password

All of this works over SSH, with no card, on a camera set up by the wizard (or registered):

| I want to... | Command |
|---|---|
| flash a newer image | `./akam update` (`--image FILE` for a specific one) |
| change the camera's network name (what your router lists) | `./akam hostname [NAME]` (default: the camera's name), applied at the next power cycle |
| ask for a user and password on the stream (off by default) | `./akam rtsp-auth set` (also `status`, `disable`) |
| turn the front LED off or make it a status light, or keep the infrared LEDs dark at night | `./akam led` shows both; `./akam led status on\|off\|auto`, `./akam led ir auto\|off` |
| talk through the camera speaker from this computer, or play a sound file on it | `./akam talk` (microphone, Ctrl-C stops), `./akam talk test`, `./akam say FILE`; needs the video password ([two-way audio](two_way_audio.md)) |
| check or change the speaker volume, play a beep, switch the speaker, talk or the microphone on or off | `./akam audio` shows the settings; `./akam audio volume 0-100`, `beep`, `on`, `off`, `talk on\|off`, `mic on\|off` (saved on the camera; `mic off` removes the audio track from the stream and needs a reboot) |
| publish the camera to Home Assistant or another MQTT broker, and take a few commands from it (off by default; the camera connects out, no new port) | `./akam mqtt set` (asks the broker, the password is not shown), then `mqtt test`; also `status`, `disable`, `forget` ([MQTT and Home Assistant](mqtt.md)) |
| give the camera the right time (it has no clock of its own); off by default, no traffic unless you ask | `./akam ntp set pool.ntp.org` (1 to 3 servers; also `status`, `off`) |
| change brightness, contrast, saturation, sharpness, white balance or day/night mode | `./akam picture` shows them; `./akam picture brightness N` (also `contrast`, `saturation`, `sharpness`, N from -50 to 50), `wb conf\|manual`, `night auto\|day\|night`, `reset` (all of them are saved on the camera) |
| lower the bitrate or frame rate, or change the key frame interval of the video | `./akam video` shows them; `./akam video bitrate N` (500 to 3000 kbit/s, default 1536), `fps 10\|15\|20\|25` (default 15; higher rates make dim light darker and grainier and start night mode earlier, see [troubleshooting](troubleshooting.md)), `gop N` (key frame every N seconds, 1 to 4, default 2). Saved on the camera and applied when the video service restarts (it offers to restart it for you; the stream stops for a few seconds). Resolution and flip/mirror are not available yet |
| make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager); off by default | `./akam onvif enable [PORT]` (asks first; needs the video password), `./akam onvif status`, `./akam onvif disable` ([ONVIF](onvif.md)) |
| go back to the original firmware | `./akam restore` (from your backup; no guarantee, see [risks](../README.md#important-disclaimer-and-risks)) |

The LED settings are saved on the camera. `status auto` makes the LED blink while the camera starts or
has no Wi-Fi, blink slowly while someone watches the video, and stay solid otherwise; the default `on`
leaves the LED as it always was. `ir off` still switches the image to night mode but never lights the
infrared LEDs ([details](akam_v1_install.md#led-settings)).

`update` and `restore` write the camera's flash and ask the same red yes/no. The video password is a
Digest login (works with VLC, ffmpeg, go2rtc and Frigate); it does not encrypt the video. `./akam
rtsp-auth set` stores the password on the camera twice: as a hash for the Digest login, and in clear text in
a file only root can read (`/etc/config/akam/credentials`), because the ONVIF login (a later step) cannot work
from a hash. `./akam rtsp-auth status` says `ONVIF-ready` when that copy is there; `disable` removes both. Put it in the address: `rtsp://<user>:<password>@<camera-ip>:554/`, with
special characters URL-encoded ([401 errors](troubleshooting.md#401-unauthorized-or-a-password-prompt-on-the-stream)).

## Watching the stream

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

Any path works. The camera serves **one RTSP client at a time** (port 554; port 8091 is separate and takes up to 4, see below): to serve several viewers or to record and
watch together, put [go2rtc](https://github.com/AlexxIT/go2rtc) or
[MediaMTX](https://github.com/bluenviron/mediamtx) in front of it. In VLC, if hardware decoding fails,
use `vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/`; a VLC window on a hidden Wayland
workspace pauses the stream ([why and what to do](troubleshooting.md#vlc-goes-black-or-stops-when-its-window-is-on-another-workspace)).

**Two-way audio:** the RTSP port also accepts the standard ONVIF audio backchannel, so go2rtc, Frigate
and Home Assistant's WebRTC card can talk through the camera speaker, as can `./akam talk`. It is offered
only when the video password is on. See [two-way audio](two_way_audio.md) (including configuration
examples and the one-client limit).

Port 8091 (`LIST`, `GET`: exposure, day/night, image, LEDs, state, system and stream metrics) is a plain text
protocol with no password. It accepts up to **4 clients at once**, one of them reserved for the camera itself (so
the SSH helpers always get in; a LAN peer can use 3 and is dropped after 2 minutes of silence), and is
**read-only from the network**: anyone on your LAN can read the state, nobody can change it. Changes
(`SET`) are accepted from the camera itself (over SSH, as `./akam led` does) or, for a limited time, after
`./akam debug on`, which the PC tuning tool needs. Raw changes are lost at the next boot; the settings made with `./akam led`, `audio`, `picture` (its four numbers) and `video` are saved on
the camera ([details](akam_v1_install.md#4-the-8091-control-port)).

## Security notes

- **Never expose ports 554, 8091 or 22 to the Internet:** no port forwarding, UPnP or DMZ. Use a VPN into
  your LAN ([network hardening](akam_v1_install.md#5-network-hardening)).
- The stream is open unless you set a password, and anyone on your network can occupy its single RTSP client
  slot (a second client gets `503 Service Unavailable` at once; several viewers: restream with go2rtc, see
  [troubleshooting](troubleshooting.md#only-one-viewer-works-at-a-time)): keep untrusted devices (guest
  Wi-Fi, IoT) off the camera's network.
- Port 8091 is read-only for the network. Its log stream (`LOG` lines: client addresses, auth failures) is sent
  only to the camera itself or while `akam_debug.sh` debug mode is on, and the Wi-Fi name and MAC address read
  as `hidden` for other machines (the IP address stays readable).
- A card with a file named `AKAM_MAINTENANCE` is root access to the camera
  ([details](akam_v1_ssh_and_lockdown.md#7-maintenance-flag)). Never publish your backup, images or keys.
- **Firmware updates from a card are not signed.** With the `AKAM_MAINTENANCE` flag on the card, the stock
  `tf_update.sh` flashes any `*.bin` it finds, with no signature or hash check; without the flag it never runs
  (checked by `tests/lockdown_test.sh`). There is no network update. Treat the flag as physical access.
- **A backup holds secrets:** the CONFIG partition has your Wi-Fi password and the ONVIF password in clear text.
  The wizard creates the backup folder private to your user (0700, files 0600): keep copies private too and
  never upload them ([details](akam_v1_backup.md#never-publish-the-dump)).
