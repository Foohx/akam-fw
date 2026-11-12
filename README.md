# Akam: local firmware for the LSC Smart Indoor 1080p camera

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20L16%20Audio-red.svg)]()

Akam replaces the Tuya cloud software of the **LSC Smart Indoor 1080p camera** (sold at Action) with a
small local firmware:

- a **local RTSP stream** (H.264 1080p + audio) on port 554: no cloud, no account, no app;
- **SSH** (key only) for maintenance; Telnet and FTP stay off;
- only the APP partition is rewritten, from **your own** backup. U-Boot, kernel and ROOTFS are never
  written.

A guided program, `./akam`, does the whole installation from a Linux PC: back up, build the image
from your backup, flash, check. No compiling.

> **Flashing can brick your camera, and restoring a backup is not guaranteed.** Read the
> [risks](#important-disclaimer-and-risks). The wizard makes you take a full backup first.

## Features

- **Video and audio:** RTSP, H.264 1080p at 15 fps plus the microphone, on port 554. Optional Digest login
  (VLC, ffmpeg, go2rtc, Frigate). The camera serves **one RTSP client at a time**: a second one is answered
  at once with `503 Service Unavailable`. To serve several viewers or record and watch together, put
  [go2rtc](https://github.com/AlexxIT/go2rtc) (or MediaMTX) in front of the camera.
- **Two-way audio:** the ONVIF audio backchannel on port 554 (go2rtc, Frigate, Home Assistant, `./akam talk`).
- **LEDs and night:** front LED on, off or status light; infrared LEDs auto or off; automatic day/night
  switching.
- **Port 8091:** read-only metrics (`sys.*`, `net.*`, `video.*`, `rtsp.*`) for the network. Changes are only
  accepted from the camera itself, or from the network after `./akam debug on` (loopback and timed).
- **ONVIF Profile S (optional, off by default):** WS-Discovery, device, media and imaging services for NVRs and
  ONVIF Device Manager. Needs the video password (`./akam rtsp-auth set`, re-run it once after an update so the
  ONVIF login copy exists). Host-tested only so far, see [testing status](docs/testing-status.md).
- **Settings you can change:** video bitrate, frame rate and key frame interval; brightness, contrast,
  saturation, sharpness, white balance and day/night mode; speaker volume, talk and microphone on/off.
- **Time:** opt-in NTP (`./akam ntp set SERVER`); with it off the camera sends no time traffic and its clock
  starts in 1970.
- **Robustness:** the video service exits by itself if the encoder stalls, the camera's wrapper restarts it with a
  growing delay, and a supervisor brings the Wi-Fi back when it drops.
- **Admin:** SSH with a key only; Telnet and FTP are off.
- **Install:** a guided wizard (`./akam`) from a Linux PC; only the APP partition is rewritten.

What was tested on a real camera and what was only tested on a PC is listed in
[docs/testing-status.md](docs/testing-status.md); the units used are in
[docs/tested-hardware.md](docs/tested-hardware.md).

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

## Is my camera supported?

Read the label under the camera: model `3215672.x` and the `SI ...` code.

| Camera | Label | Status |
|---|---|---|
| LSC Smart Indoor 1080p `3215672.3` | SI `D26228` | **validated** (two cameras, see [tested hardware](docs/tested-hardware.md)) |
| LSC Smart Indoor 1080p `3215672.2` | SI `B26101` / `C26101` | untested (works with upstream, not tried with Akam) |
| anything else (other SoC such as AK3918EV200 or GK7102, other kernel or APP) | | not supported |

The wizard also compares the camera's kernel, flash layout and APP with
`tools/release/supported_hardware.txt`: a match continues, anything else shows a warning and needs an
explicit yes, and a different flash layout is refused.

## What you need

- the camera, and a 2.4 GHz Wi-Fi network it can join;
- a **microSD card** and a **USB card reader** (cheap readers sometimes fail:
  [troubleshooting](docs/troubleshooting.md#no-medium-found-with-a-usb-card-reader));
- a **Linux PC**: Arch, Debian 12 or Ubuntu 24.04 (all tested in containers). macOS and Windows are
  not supported;
- optional but wise: a UART adapter, the safety net if a flash goes wrong ([UART guide](docs/hardware_and_uart.md)).

## Quick start

1. **Get Akam** (or unpack the release archive instead):
   ```bash
   git clone https://github.com/Foohx/akam-fw && cd akam-fw
   ```
2. **Run the wizard:**
   ```bash
   ./akam
   ```
   Its first step checks your PC and prints the exact command to paste if a tool is missing. Then it
   guides you through everything and remembers where you stopped.
3. **Follow the menu**, one step after the other. Arrow keys (or `j`/`k`) and Enter to run a step, `?`
   to read what it does and what it risks, `q` to quit (your progress is kept).

That is all. The first run asks for a short name for your camera (for example `salon`). `./akam help`
lists every command; add `--dry-run` to rehearse without changing anything. The menu (in colour on a
real terminal; `--plain` or no terminal gives line-by-line prompts):

```text
╭─ Akam · guided setup ───────────────────────────────── v1.0.0-dev  3/9 done ╮
│ Host  Arch Linux x86_64        Camera  salon · no address yet · c to change │
│ Kit   akam 1.0.0               Image   not built yet                        │
╰─────────────────────────────────────────────────────────────────────────────╯
   ✔  1  Check this computer                                          done
   ●  2  Get the Akam kit                                             todo
   ✔  3  Prepare the backup microSD card                              done
   ✔  4  Find the camera on your network                              done
 ▸ ●  5  Back up the camera (all 8 partitions)                        todo
   ✖  6  Build the Akam image from your backup                        blocked
   ✖  7  Prepare the flash microSD card                               blocked
   ✖  8  Flash the image to the camera                                blocked
   ✖  9  Verify the result                                            blocked
 ↑↓ move  ⏎ run  ? help  r pick several  a run all  c camera  q quit
```

Below the steps, a *Maintenance* list holds `update`, `hostname`, `rtsp-auth`, `restore` and `wipe-sd`; the other settings commands (`led`, `audio`, `picture`, `video`, `onvif`, `ntp`, `debug`) are typed.
Before a step starts, the wizard shows what it does and asks **Start this step? [Y/n]**. Anything
irreversible (erasing a card, writing the camera's flash) shows a **red box** that says exactly what
will happen and asks one yes/no whose **default is No** (Enter, Esc and `q` mean No). Nothing has to be
typed, and without a terminal these steps refuse to run.

### Native binary (experimental)

A single static binary version of the wizard is being written in Go ([tools/assistant](tools/assistant/README.md),
[plan](specs/09-spec-assistant-binary.md)). It is **experimental: the bash wizard (`./akam`) remains the
reference**. The binary covers the daily commands on a camera that already runs Akam (`status`, `led`, `audio`,
`picture`, `video`, `debug`, `onvif`, `ntp`, `rtsp-auth`, `hostname`, `talk`, `say`, `find-camera`) with no SSH,
`nc` or `akam-talk` to install, kit download and verification, `update`, `restore` and `verify` over SSH, and a
first install through the maintenance telnet and a built-in microSD writer (`backup`, `flash`, `prepare-*-sd`,
`wipe-sd`). The `image` step stays in the bash wizard. It has been tested on fake cameras and files only: the
update, restore, verify, telnet backup and flash and SD writer phases were never run against a real camera or card.
Phase 11 adds builds for Linux, Windows and macOS (amd64 and arm64), a home screen with a camera chooser and option screens,
and a backup over SSH. The Windows and macOS binaries are **built, not hardware-tested**, and they import an image built on Linux
or in WSL for now ([install and unsigned-binary steps](docs/assistant_install.md), [hardware test plan](docs/assistant_hardware_test_plan.md)).
It shares the progress files with `./akam`. Build it with `tools/assistant/build.sh` and try it with
`AKAM_USE_BIN=1 ./akam ...`.

## What the wizard does

| Step | What happens | Risk |
|---|---|---|
| `doctor` | checks the tools on your PC | none |
| `get-kit` | downloads the prebuilt kit, checks checksum and signature (`./akam build` builds from source instead) | none |
| `prepare-backup-sd` | writes the backup toolkit to a microSD card | erases that card |
| `find-camera` | asks the camera's address, probes its ports | none |
| `backup` | copies the 8 flash partitions twice, compares them, stores them in `<Documents>/Akam backups/<name>/<date>` (private to your user) | none (read only) |
| `image` | builds the Akam image from your backup, shows the hardware verdict | none |
| `prepare-flash-sd` | writes the image, your SSH key and the Wi-Fi settings to a card | erases that card |
| `flash` | writes the APP partition (over SSH when Akam already runs, else through the maintenance telnet and the card) | **can brick the camera** |
| `verify` | checks SSH, ports and video, saves the Wi-Fi on the camera before you remove the card, then checks the boot without the card | none |

Good to know:

- The wizard never runs `sudo` itself, shows every command before running it, never stores your Wi-Fi
  password and never reboots the camera: you unplug and replug the power yourself.
- **SSH key:** `~/.ssh/akam`, or you pick one of your ed25519 keys, or the wizard creates one.
- **Wi-Fi:** `flash` and `verify` save it on the camera and never ask you to remove the card before
  ([if it fails](docs/troubleshooting.md#the-camera-has-no-network-after-removing-the-card)).
- Keep your backup (two copies, on two media) and never publish it: it holds your Wi-Fi credentials.
  The cards hold your Wi-Fi password in clear text: erase them with `./akam wipe-sd` when done.

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
| talk through the camera speaker from this computer, or play a sound file on it | `./akam talk` (microphone, Ctrl-C stops), `./akam talk test`, `./akam say FILE`; needs the video password ([two-way audio](docs/two_way_audio.md)) |
| check or change the speaker volume, play a beep, switch the speaker, talk or the microphone on or off | `./akam audio` shows the settings; `./akam audio volume 0-100`, `beep`, `on`, `off`, `talk on\|off`, `mic on\|off` (saved on the camera; `mic off` removes the audio track from the stream and needs a reboot) |
| publish the camera to Home Assistant or another MQTT broker, and take a few commands from it (off by default; the camera connects out, no new port) | `./akam mqtt set` (asks the broker, the password is not shown), then `mqtt test`; also `status`, `disable`, `forget` ([MQTT and Home Assistant](docs/mqtt.md)) |
| give the camera the right time (it has no clock of its own); off by default, no traffic unless you ask | `./akam ntp set pool.ntp.org` (1 to 3 servers; also `status`, `off`) |
| change brightness, contrast, saturation, sharpness, white balance or day/night mode | `./akam picture` shows them; `./akam picture brightness N` (also `contrast`, `saturation`, `sharpness`, N from -50 to 50), `wb conf\|manual`, `night auto\|day\|night`, `reset` (all of them are saved on the camera) |
| lower the bitrate or frame rate, or change the key frame interval of the video | `./akam video` shows them; `./akam video bitrate N` (500 to 3000 kbit/s, default 1536), `fps 10\|15\|20\|25` (default 15; higher rates make dim light darker and grainier and start night mode earlier, see [troubleshooting](docs/troubleshooting.md)), `gop N` (key frame every N seconds, 1 to 4, default 2). Saved on the camera and applied when the video service restarts (it offers to restart it for you; the stream stops for a few seconds). Resolution and flip/mirror are not available yet |
| make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager); off by default | `./akam onvif enable [PORT]` (asks first; needs the video password), `./akam onvif status`, `./akam onvif disable` ([ONVIF](#onvif-nvrs-and-onvif-apps)) |
| go back to the original firmware | `./akam restore` (from your backup; no guarantee, see [risks](#important-disclaimer-and-risks)) |

The LED settings are saved on the camera. `status auto` makes the LED blink while the camera starts or
has no Wi-Fi, blink slowly while someone watches the video, and stay solid otherwise; the default `on`
leaves the LED as it always was. `ir off` still switches the image to night mode but never lights the
infrared LEDs ([details](docs/akam_v1_install.md#led-settings)).

`update` and `restore` write the camera's flash and ask the same red yes/no. The video password is a
Digest login (works with VLC, ffmpeg, go2rtc and Frigate); it does not encrypt the video. `./akam
rtsp-auth set` stores the password on the camera twice: as a hash for the Digest login, and in clear text in
a file only root can read (`/etc/config/akam/credentials`), because the ONVIF login (a later step) cannot work
from a hash. `./akam rtsp-auth status` says `ONVIF-ready` when that copy is there; `disable` removes both. Put it in the address: `rtsp://<user>:<password>@<camera-ip>:554/`, with
special characters URL-encoded ([401 errors](docs/troubleshooting.md#401-unauthorized-or-a-password-prompt-on-the-stream)).

## ONVIF: NVRs and ONVIF apps

ONVIF is the standard that NVRs, ONVIF Device Manager, Synology Surveillance Station and Home Assistant use to
find a camera on the network and pull its video. Akam implements the discovery (WS-Discovery), the device
service and the media service of Profile S, so the camera shows up in those apps and they get its RTSP stream.
**This is part of the same release as the `onvif` command: an image built before it has no ONVIF at all
(`./akam update` first), and `./akam onvif status` then says it does not know.** The firmware side is now wired
(`ak_rtsp` starts and stops the ONVIF server and the discovery by itself from the flag file, no reboot). It is
**validated on a real camera** (akam-01, 2026-11-11): ONVIF Device Manager on Windows finds it, logs in, plays the live
video and changes the image; `tools/hw_tests/onvif_check.py` passes 34/34. Other NVRs are not tried yet (steps in
the [install notes](docs/akam_v1_install.md#onvif-with-onvif-device-manager)).

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
  video user name and password; the service address is `http://<camera-ip>/onvif/device_service`. If it is not found, see [troubleshooting](docs/troubleshooting.md#an-onvif-app-does-not-find-the-camera).
- The setting is a small file on the camera that survives reboots; an older image says to reboot the camera to apply it.
  `./akam onvif status` shows what the camera really did (`onvif.state`: `off`, `on:<port>`, `no_password`, or
  `error:port_in_use`). When ONVIF is off the camera runs no ONVIF thread and no socket; when it is on, two idle
  threads that wake up at most every 30 s. A new camera name (`./akam hostname`) shows in discovery at the next
  `./akam onvif enable` or reboot.

## Watching the stream

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

Any path works. The camera serves **one RTSP client at a time** (port 554; port 8091 is separate and takes up to 4, see below): to serve several viewers or to record and
watch together, put [go2rtc](https://github.com/AlexxIT/go2rtc) or
[MediaMTX](https://github.com/bluenviron/mediamtx) in front of it. In VLC, if hardware decoding fails,
use `vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/`; a VLC window on a hidden Wayland
workspace pauses the stream ([why and what to do](docs/troubleshooting.md#vlc-goes-black-or-stops-when-its-window-is-on-another-workspace)).

**Two-way audio:** the RTSP port also accepts the standard ONVIF audio backchannel, so go2rtc, Frigate
and Home Assistant's WebRTC card can talk through the camera speaker, as can `./akam talk`. It is offered
only when the video password is on. See [two-way audio](docs/two_way_audio.md) (including configuration
examples and the one-client limit).

Port 8091 (`LIST`, `GET`: exposure, day/night, image, LEDs, state, system and stream metrics) is a plain text
protocol with no password. It accepts up to **4 clients at once**, one of them reserved for the camera itself (so
the SSH helpers always get in; a LAN peer can use 3 and is dropped after 2 minutes of silence), and is
**read-only from the network**: anyone on your LAN can read the state, nobody can change it. Changes
(`SET`) are accepted from the camera itself (over SSH, as `./akam led` does) or, for a limited time, after
`./akam debug on`, which the PC tuning tool needs. Raw changes are lost at the next boot; the settings made with `./akam led`, `audio`, `picture` (its four numbers) and `video` are saved on
the camera ([details](docs/akam_v1_install.md#4-the-8091-control-port)).

## If something goes wrong

- [Troubleshooting](docs/troubleshooting.md): no network after removing the card, card reader problems,
  VLC, 401 errors, router names, SSH warnings. Start with `./akam status` and the log in
  `~/.local/state/akam/`.
- **Camera does not come back after a flash:** [recovery](docs/akam_v1_flash_and_restore.md#5-restore)
  (maintenance card, UART, your backup). Never unplug anything while a flash runs.
- Want to do it by hand, or understand each step? See the
  [manual procedures](docs/README.md#manual-procedures-advanced-without-the-wizard).

## Status and limits

Full matrix: [testing status](docs/testing-status.md). In short:

**Validated on real hardware** (two cameras, firmware image built 2026-11-10): RTSP H.264 1080p 15 fps with the
microphone (VLC, ffmpeg, go2rtc over TCP), Digest login, ONVIF talk (speaker), front LED and infrared LED
settings, automatic day/night switching, port 8091 read-only with its metrics (a remote `SET` is refused), SSH
key admin with Telnet and FTP off, flash over SSH with read-back verification, clean boot after a power cycle.

**Not yet tested on a camera** (host tests only): ONVIF Profile S, the encoder-stall self-exit, the respawn
backoff, the Wi-Fi supervisor, NTP, microphone off, the video settings, the 503 for a second RTSP client, and the
Go assistant's update, restore, telnet and card phases.

**Known limits:** the image is grainier than the stock firmware's (improvement planned); one RTSP client at a
time; talk needs the video password. Please report what you see on your camera.

## Security notes

- **Never expose ports 554, 8091 or 22 to the Internet:** no port forwarding, UPnP or DMZ. Use a VPN into
  your LAN ([network hardening](docs/akam_v1_install.md#5-network-hardening)).
- The stream is open unless you set a password, and anyone on your network can occupy its single RTSP client
  slot (a second client gets `503 Service Unavailable` at once; several viewers: restream with go2rtc, see
  [troubleshooting](docs/troubleshooting.md#only-one-viewer-works-at-a-time)): keep untrusted devices (guest
  Wi-Fi, IoT) off the camera's network.
- Port 8091 is read-only for the network. Its log stream (`LOG` lines: client addresses, auth failures) is sent
  only to the camera itself or while `akam_debug.sh` debug mode is on, and the Wi-Fi name and MAC address read
  as `hidden` for other machines (the IP address stays readable).
- A card with a file named `AKAM_MAINTENANCE` is root access to the camera
  ([details](docs/akam_v1_ssh_and_lockdown.md#7-maintenance-flag)). Never publish your backup, images or keys.
- **Firmware updates from a card are not signed.** With the `AKAM_MAINTENANCE` flag on the card, the stock
  `tf_update.sh` flashes any `*.bin` it finds, with no signature or hash check; without the flag it never runs
  (checked by `tests/lockdown_test.sh`). There is no network update. Treat the flag as physical access.
- **A backup holds secrets:** the CONFIG partition has your Wi-Fi password and the ONVIF password in clear text.
  The wizard creates the backup folder private to your user (0700, files 0600): keep copies private too and
  never upload them ([details](docs/akam_v1_backup.md#never-publish-the-dump)).

## Important disclaimer and risks

> **FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.**
>
> - **Experimental:** upstream comes from a private reverse-engineering project organised with AI
>   assistance; Akam's additions were written with AI assistance and validated on two cameras so far (see [testing status](docs/testing-status.md)).
> - **Risk of permanent bricking:** writing to the SPI NOR flash can brick the camera. In the worst case
>   only an external SPI programmer (for example a CH341A) recovers it. **Restoring your backup is not
>   guaranteed to work.**
> - **No liability:** the authors and contributors accept zero liability for damaged hardware, bricked
>   cameras, data loss or voided warranties.
> - **Always make and verify a complete 8-partition backup first**, and never publish it.
> - **Never cut the power while a flash runs.** If a flash reports errors, do not power cycle: flash your
>   stock backup immediately.

## Learn more

The [documentation index](docs/README.md) lists everything else: manual procedures, build from source,
hardware and UART, upstream background, [changelog](CHANGELOG.md). For contributors: design notes live in
`specs/` (French, internal) and `tools/ci/local_check.sh` runs the checks.

## Credits and license

MIT, see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Akam is a fork of
[FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware):
the reverse-engineered `ak_rtsp` and `flash_tool` code, the hardware documentation and the images are the
upstream author's.

- **Anyka community:** [VGerris](https://github.com/VGerris/Anyka_ak3918_hacking_journey) and
  [Lawliet95](https://github.com/Lawliet95/ANYKA-Tuya-Hacking-Journey-AK3918v200EN080-v200): earlier SoCs, but
  valuable for ioctl naming and orientation.
- **SmolRTSP (OpenIPC):** the RTSP library, [OpenIPC/smolrtsp](https://github.com/OpenIPC/smolrtsp).
- **Action-Smart-Connect-Flasher (4ddict):** the four `ak_rtsp` fixes ported into this tree
  ([MIT](THIRD_PARTY_NOTICES.md)) come from [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher).
- **lsc-tuya-toolkit (tasarren):** the microSD toolkit that opens the maintenance shell for the backup,
  [tasarren/lsc-tuya-toolkit](https://github.com/tasarren/lsc-tuya-toolkit).
- **Dropbear (Matt Johnston):** the SSH server, [mkj/dropbear](https://github.com/mkj/dropbear).
- **Zig:** the cross-compiler, downloaded by the bootstrap and not redistributed.
