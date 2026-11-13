# Akam: local firmware for the LSC Smart Indoor 1080p camera

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20L16%20Audio-red.svg)]()

**Akam replaces the Tuya cloud software of the LSC Smart Indoor 1080p camera (sold at Action) with a small local
firmware.** The camera then streams video and audio on your own network (RTSP, port 554): no cloud, no account, no
app. A guided program, `./akam`, does the whole installation from a Linux PC: back up, build the image from your own
backup, flash, check. No compiling. Only the camera's application area (the APP partition) is rewritten; the boot
loader and the system are never touched.

> **Flashing can brick your camera, and restoring a backup is not guaranteed.** Read the
> [risks](#important-disclaimer-and-risks). The wizard makes you take a full backup first.

## Is my camera compatible?

Look at the back of the camera body, under the LSC logo: two codes such as `3215672.3  SI D26228`.

| Camera | Article | SI code | Status |
|---|---|---|---|
| LSC Smart Indoor 1080p | `3215672.3` | SI `D26228` | ✅ validated (2 units) |
| LSC Smart Indoor 1080p | `3215672.2` | SI `B26101` / `C26101` | ⚠️ untested |
| any other camera | | | ❌ not supported: not expected to work; the wizard warns, and refuses a different flash layout |

Full table (SoC, sensor, flash, stock firmware fingerprint) and the checks the wizard does:
**[compatible cameras](docs/compatible-cameras.md)**.

## What you get

On the validated camera, compared with the stock firmware ([feature-by-feature table](docs/compatible-cameras.md#table-2-what-you-get-stock-firmware-versus-akam)):

- **Local video and sound:** RTSP H.264 1080p with microphone at 10, 15, 20 or 25 fps (default 15), optional password.
  One viewer at a time: put [go2rtc](https://github.com/AlexxIT/go2rtc) or MediaMTX in front for several.
- **Talk through the camera speaker** (`./akam talk`, go2rtc, Frigate, Home Assistant) and **ONVIF** (NVRs, apps;
  optional, off by default, not certified).
- **Camera settings:** day/night and infrared LEDs, status LED, frame rate; picture, bitrate and key frames
  (available, not yet checked on the flashed image).
- **Home automation:** MQTT with Home Assistant discovery (optional, off by default, **host-tested only**) and an
  opt-in clock (NTP, checked on both cameras).
- **Safe by default:** SSH with a key only; Telnet and FTP closed; read-only metrics on port 8091.
- **Not available:** the Tuya/Smart Life phone app, the cloud and remote viewing away from home, push
  notifications, motion detection, recording to microSD, vendor OTA updates. Instead: VLC or ffplay on your network, Home
  Assistant, an NVR (network video recorder) with ONVIF or RTSP, and a VPN into your home network for remote access.

What was tested on a real camera and what only on a PC: [testing status](docs/testing-status.md). Every command,
port and setting: [using Akam day to day](docs/using_akam.md).

## What you need

- the camera, and a 2.4 GHz Wi-Fi network it can join;
- a **microSD card** and a **USB card reader** (cheap readers sometimes fail:
  [troubleshooting](docs/troubleshooting.md#no-medium-found-with-a-usb-card-reader));
- a **Linux PC**: Arch, Debian 12 or Ubuntu 24.04 (all tested in containers) for the bash wizard. An experimental
  single-file `akam` binary also exists for Windows and macOS, but a first install from Windows or macOS has never
  been done on a real camera: use Linux ([install](docs/assistant_install.md),
  [what it does](docs/wizard_guide.md#native-binary-experimental));
- optional but wise: a UART adapter (a serial cable to the camera's board), the safety net if a flash goes wrong
  ([UART guide](docs/hardware_and_uart.md)).

## Install in 5 steps

1. **Get Akam** (or unpack the release archive instead):
   ```bash
   git clone https://github.com/Foohx/akam-fw && cd akam-fw
   ```
2. **Run the wizard:** `./akam`. Its first step checks your PC and prints the exact command to paste if a tool is
   missing. It asks a short name for your camera (for example `salon`) and remembers where you stopped. The wizard
   downloads the **kit** (Akam's own files, checked by checksum and signature; it never contains a camera image).
   Until the first release is published, use `./akam build` or `--kit FILE`.
3. **Follow the menu**, one step after the other: check this PC, get the kit, prepare the backup card, find the
   camera, **back it up (all 8 partitions)**, build the image, prepare the flash card, flash, verify. Arrow keys and
   Enter run a step, `?` says what it does and what it risks, `q` quits (progress is kept).
4. **Confirm the risky steps.** Anything irreversible (erasing a card, writing the camera's flash) shows a red box
   that says exactly what will happen and asks a yes/no whose **default is No**.
5. **Unplug and replug the camera's power yourself** when told (the wizard never reboots it), then let `verify` check
   SSH, ports and video. Watch the stream with `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/`.

`./akam help` lists every command; add `--dry-run` to rehearse without changing anything. The menu, the steps and
the risk of each one: [wizard guide](docs/wizard_guide.md). The wizard never keeps your Wi-Fi password in its own state or log files and never puts it on a command line;
it is written only to the cards the wizard prepares (in clear text: erase them with `./akam wipe-sd`) and to the
camera. Keep your backup private, on two media.

## Daily use and details

| I want to... | Go to |
|---|---|
| change a setting, update, set a password, name the camera, use several cameras | [using Akam day to day](docs/using_akam.md) |
| watch the stream, restream to several viewers | [watching the stream](docs/using_akam.md#watching-the-stream) |
| talk through the speaker | [two-way audio](docs/two_way_audio.md) |
| connect an NVR or ONVIF app | [ONVIF](docs/onvif.md) |
| connect Home Assistant or an MQTT broker | [MQTT](docs/mqtt.md) |
| see which ports are open and the security advice | [open ports](docs/using_akam.md#open-ports), [security notes](docs/using_akam.md#security-notes) |
| fix a problem | [troubleshooting](docs/troubleshooting.md) |
| do it by hand, or understand each step | [documentation index](docs/README.md) |

## If something goes wrong

- [Troubleshooting](docs/troubleshooting.md): no network after removing the card, card reader problems, VLC, 401
  errors, router names, SSH warnings. Start with `./akam status` and the log in `~/.local/state/akam/`.
- **Camera does not come back after a flash:** [recovery](docs/akam_v1_flash_and_restore.md#5-restore)
  (maintenance card, UART, your backup). Never unplug anything while a flash runs.

## Status and limits

Full matrix: [testing status](docs/testing-status.md). In short, on the two validated cameras:

- **Validated on hardware** (two cameras, images of 2026-11-10 and 2026-11-11; both reflashed with "v6" on 2026-11-12 and
  running continuously since at 14.9 to 15 fps with a home server pulling RTSP; one unit then moved to "v7": 25 fps
  measured in daylight, 20 fps only in a RAM test): RTSP with microphone, Digest login, ONVIF with a check tool and
  ONVIF Device Manager (one camera), talk, LEDs, day/night, port 8091, NTP, SSH key admin with Telnet and FTP off,
  flash over SSH with read-back, clean boot.
- **Not validated on a camera** (host tests, RAM tests or experimental): MQTT and Home Assistant discovery, picture
  settings on the final image, bitrate and key frame settings, microphone off, `./akam restore` (never rehearsed),
  the Go assistant's update, restore, telnet and card phases. The encoder-stall
  self-exit and the Wi-Fi supervisor are in the image but were never triggered on hardware. The native image builder
  (`akam image --builder native`) is experimental and was never flashed.
- **Known limits:** the image is grainier than the stock firmware's in dim light; one RTSP client at a time; talk needs
  the video password. Please report what you see on your camera.

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

The [documentation index](docs/README.md) lists everything else: manual procedures, build from source, hardware and
UART, upstream background, [changelog](CHANGELOG.md). For contributors: design notes live in `specs/` (French,
internal) and `tools/ci/local_check.sh` runs the checks.

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
