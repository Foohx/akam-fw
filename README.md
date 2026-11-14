# Akam: local firmware for the LSC Smart Indoor 1080p camera

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20L16%20Audio-red.svg)]()

## What is Akam?

Akam is an open-source replacement firmware for the **LSC Smart Indoor 1080p** IP camera (sold at Action, Anyka
AK3918AV130 chip). It removes the Tuya cloud application and puts a small program in its place that works **only on
your own network**: no cloud, no account, no phone app, no telemetry.

It is for people who want to plug a cheap camera into their own setup: a video player, an NVR (network video
recorder), Home Assistant or any tool that speaks standard protocols (RTSP, ONVIF, MQTT, SSH). The goals are a light
firmware, standard protocols, easy home-automation integration, and features that are off until you turn them on.

A guided installer (`./akam`) does the whole job from your PC: back up the camera, build the image from your own
backup, flash, check. Only the camera's application area (the APP partition) is rewritten. The boot loader, the
kernel and the system are never touched.

> **Flashing can brick your camera, and restoring a backup is not guaranteed.** Read the
> [risks](#important-disclaimer-and-risks). The installer makes you take a full backup first.

## Supported camera

| Camera | Article | SI code | Status |
|---|---|---|---|
| LSC Smart Indoor 1080p | `3215672.3` | SI `D26228` | validated on 2 units |
| LSC Smart Indoor 1080p | `3215672.2` | SI `B26101` / `C26101` | untested |
| any other camera | | | not supported |

The codes are on the back of the camera body, under the LSC logo. Details, how to report a new model:
[compatible cameras](docs/compatible-cameras.md).

## Features in short

- **Video and sound:** RTSP H.264 1080p with microphone, 10/15/20/25 fps (default 15), optional password. One viewer
  at a time: put [go2rtc](https://github.com/AlexxIT/go2rtc) or MediaMTX in front for several.
- **Talk through the camera speaker** (ONVIF audio backchannel, `./akam talk`, go2rtc, Frigate, Home Assistant).
- **ONVIF-compatible services** (off by default, not certified) and WS-Discovery for NVRs and apps.
- **MQTT with Home Assistant discovery** (off by default, host-tested only).
- **Camera controls:** day/night with infrared LEDs, status LED, picture settings, frame rate, bitrate.
- **Safe by default:** SSH with a key only, Telnet and FTP closed, optional clock sync (NTP).
- **Not provided:** the Tuya/Smart Life app, cloud and remote viewing, push notifications, motion detection,
  recording to microSD, vendor updates. Use a VPN into your home network, an NVR or Home Assistant instead.

The complete list with ports, defaults and test status: [features](docs/features.md).

## How it works, in 5 lines

1. The installer reads your camera's own firmware backup, made by you with a microSD card.
2. Akam's **kit** (its own programs and scripts, signed, no vendor files) is downloaded.
3. Your PC builds a new APP image from your backup plus the kit.
4. The image is flashed to the APP partition, with read-back verification.
5. Vendor files stay on your camera and in your private backup. They are never distributed.

More: [how it works](docs/how-it-works.md).

## What you need

- the camera, and a 2.4 GHz Wi-Fi network it can join;
- a **microSD card** and a **USB card reader** ([card reader problems](docs/troubleshooting.md#no-medium-found-with-a-usb-card-reader));
- a **Linux PC** (Arch Linux, Debian 12 and Ubuntu 24.04 tested) for the bash wizard. A single-file `akam` program
  also exists for Windows, macOS and Linux, but it is less tested: see the
  [tested platforms](docs/assistant.md#tested-platforms);
- optional but wise: a 3.3 V USB-UART adapter, the safety net if a flash goes wrong
  ([hacking guide](docs/hacking.md#uart-access)).

## Install quick start

**Linux (reference):**

```bash
git clone https://github.com/Foohx/akam-fw && cd akam-fw
./akam
```

**Windows, macOS, Linux (single binary):** download `akam` for your system from the releases page, check it
([steps](docs/assistant.md)), then run `akam`.

The program checks your PC, remembers where you stopped, and walks you through: check the PC, get the kit, prepare
the backup card, find the camera, **back it up (all 8 partitions)**, build the image, prepare the flash card, flash,
verify. Arrow keys and Enter run a step, `?` explains it, `q` quits. Every risky step shows a red box with a yes/no
whose **default is No**. You unplug and replug the camera's power yourself: the program never reboots it.

`./akam help` lists every command; add `--dry-run` to rehearse. Step by step: [wizard guide](docs/wizard_guide.md).
Watch the stream: `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/`.

## Daily use

| I want to... | Go to |
|---|---|
| change a setting, update, set a password, name the camera | [using Akam](docs/using_akam.md) |
| talk through the speaker | [two-way audio](docs/two_way_audio.md) |
| connect an NVR or ONVIF app | [ONVIF](docs/onvif.md) |
| connect Home Assistant or MQTT | [MQTT](docs/mqtt.md) |
| fix a problem | [troubleshooting](docs/troubleshooting.md) |
| hack, build, debug | [hacking guide](docs/hacking.md) |
| everything else | [documentation index](docs/README.md) |

## Important disclaimer and risks

> **FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.**
>
> - **Experimental:** Akam is built on a reverse-engineering project and was written with AI assistance. It was
>   validated on two cameras so far ([testing status](docs/testing-status.md)).
> - **Risk of permanent bricking:** writing to the SPI NOR flash can brick the camera. In the worst case only an
>   external SPI programmer (for example a CH341A) recovers it. **Restoring your backup is not guaranteed.**
> - **No liability:** the authors and contributors accept no liability, to the extent the law allows, for damaged
>   hardware, bricked cameras, data loss or voided warranties.
> - **Always make and verify a complete 8-partition backup first**, and never publish it.
> - **Never cut the power while a flash runs.** If a flash reports errors, do not power cycle: flash your stock
>   backup immediately.

Legal notes (your device, privacy, radio, interoperability): [legal](docs/legal.md).

## Documentation

[Documentation index](docs/README.md): [features](docs/features.md), [how it works](docs/how-it-works.md),
[hacking guide](docs/hacking.md), [assistant](docs/assistant.md), [testing status](docs/testing-status.md),
[changelog](CHANGELOG.md). Contributors: [CONTRIBUTING](CONTRIBUTING.md). Vulnerabilities: [SECURITY](SECURITY.md).
Technical specifications of the code (as built): [specs/](specs/README.md).

## Release signing key

Releases are signed with this key (ssh-keygen, namespace and principal `akam-release`); the assistant refuses a kit
it cannot verify against it. Fingerprint `SHA256:Az0SyH/Aoi3ZY/mtaIdM5qnfvJhmOite9qvZDIN3dCM`:

```
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC2qtKAhTkbHvkklqSiZAdGqq98kQiMmRzoOIBlmjPG1 akam-release
```

How to check a download: [docs/assistant.md](docs/assistant.md).

## License

MIT, see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The licence covers Akam's own code and documentation only. The camera's original software (boot loader, Linux kernel
and modules, system, Anyka and Tuya programs and libraries) belongs to its owners. Akam never distributes it: it stays
on your camera and in your private backup, and the image is assembled on your own computer. Do not publish or share a
backup, a dump or an image built from one.

Akam is an independent, non-commercial community project. It is not affiliated with, endorsed by or supported by
Action, LSC Smart Connect, Tuya, Anyka, ONVIF, Home Assistant or any other company or project named here. Their names
and trademarks belong to their owners and are used only to say what Akam works with.

## Credits

Akam is a fork of
[FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware):
the reverse-engineered `ak_rtsp` and `flash_tool` code, the hardware documentation and the photos are the upstream
author's.

- **Anyka community:** [VGerris](https://github.com/VGerris/Anyka_ak3918_hacking_journey) and
  [Lawliet95](https://github.com/Lawliet95/ANYKA-Tuya-Hacking-Journey-AK3918v200EN080-v200): earlier SoCs, useful for
  ioctl naming and orientation.
- **SmolRTSP (OpenIPC):** the RTSP library, [OpenIPC/smolrtsp](https://github.com/OpenIPC/smolrtsp).
- **Action-Smart-Connect-Flasher (4ddict):** four `ak_rtsp` fixes ported into this tree
  ([MIT](THIRD_PARTY_NOTICES.md)), from [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher).
- **lsc-tuya-toolkit (tasarren):** the microSD toolkit that opens the maintenance shell for the backup,
  [tasarren/lsc-tuya-toolkit](https://github.com/tasarren/lsc-tuya-toolkit).
- **Dropbear (Matt Johnston):** the SSH server, [mkj/dropbear](https://github.com/mkj/dropbear).
- **Zig:** the cross-compiler, downloaded by the bootstrap and not redistributed.

## Support

Akam is a personal, non-commercial project, provided with no warranty. If it is useful to you, you can
[buy me a coffee](https://buymeacoffee.com/foohx). A donation does not buy support, features or priority.
