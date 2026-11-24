# Akam: take back full control of your cameras

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264-red.svg)]()

**Akam is an open-source firmware that lets you take back full control of your IP cameras. No mandatory cloud.**

It replaces the vendor's cloud application with one small program that works **only on your own network**: no
cloud, no account, no phone app, no telemetry. The camera then speaks standard protocols (RTSP, ONVIF, MQTT, SSH)
that any player, NVR or Home Assistant understands. The goals: light, standard, easy to integrate, every extra off
until you turn it on.

> **Flashing can brick a camera, and restoring a backup is not guaranteed.** Akam is experimental. Read
> [Safety and recovery](#safety-and-recovery) first.

## Who is it for

People who own one of the cameras below and want it to work with their own setup (Home Assistant, Frigate, an NVR, a
VPN into the home network) instead of a vendor cloud. Being comfortable with a command line and a microSD card helps.

## Supported cameras

Two cameras, one project: same Akam core, each with its own hardware features.

| | LSC Indoor 1080p | LSC Rotatable 2K (Dual Band) |
|---|---|---|
| Identify it | article `3215672.3`, label `SI D26228` | article `3225722`, label `SZ G26312` |
| SoC | Anyka AK3918AV130 | Anyka AK3918AV130 |
| Model id | `lsc-indoor-1080p-ak3918` (same chip as the 2K; the id suffix is historical, see [SoCs](docs/socs.md)) | `lsc-rotatable-2k-ak3918av130` |
| Works on | 2 units | 1 unit (runs with no SD card since 2026-11-15) |
| Akam is written to | the APP flash partition (`mtd7`) | the appfs flash partition (`mtd3`); runs with **no SD card** |
| Installed with | the `akam` program | the `akam` program |
| Docs | [1080p](docs/porting/lsc-1080p/README.md) | [2K](docs/porting/lsc-rotatable-2k/README.md) |

Which build was tested on which camera, and what is checked on hardware or only by host tests: [testing status](docs/testing-status.md). The
2K runs a build from 2026-11-23 (before the `akamd` rename); the 1080p build of this tree has not been flashed yet.
Install: [install guide](docs/install.md).

The codes are on the back of the camera body, under the LSC logo (photos: [hacking guide](docs/hacking.md#photos-of-the-board)).
5 GHz on the 2K is untested. Other revisions (for example `3215672.2`) are untested; any other
camera is not supported. See [compatible cameras](docs/compatible-cameras.md) and the
[porting template](docs/porting/TEMPLATE-new-camera.md).

## Before and after

**LSC Indoor 1080p**

| | Stock firmware | With Akam |
|---|---|---|
| Application | `anyka_ipc` (cloud app) | `akamd` (local only) |
| Cloud account, phone app | required | not needed |
| Local video | none by default (an RTSP server is in the image, switched off) | RTSP H.264 1080p, port 554 (microphone opt-in) |
| ONVIF, MQTT, Home Assistant | ONVIF service present but switched off; no MQTT | opt-in |
| Talk through the speaker | through the vendor app | ONVIF backchannel (`akam talk`, go2rtc, Frigate) |
| Telnet and FTP | launched by the stock start script | launch lines disabled, killed if started (checked every 60 s) |
| SSH | no | key only |
| Vendor updates | yes | gone |
| Picture orientation | not offered | flip, mirror, ceiling mount (`image.flip`, `image.mirror`, `image.ceiling`; not yet tried on this camera) |
| Not provided | | pan/tilt (no motor), motion detection, recording to microSD, a second stream (`/stream1` is the same picture as `/stream0`, on purpose) |

**LSC Rotatable 2K**

| | Stock firmware | With Akam |
|---|---|---|
| Application | `yy_cam` (cloud app) | `akamd` (local only) |
| Cloud account, phone app | required | not needed |
| Local video | RTSP on port 8554 with no password, only if enabled in its config | two RTSP streams on port 554: `/stream0` 2304x1296, `/stream1` 640x360 |
| ONVIF, MQTT, Home Assistant | ONVIF only if enabled | opt-in (ONVIF with pan/tilt) |
| Pan/tilt, person detection | cloud app only | API, ONVIF, MQTT and button; NPU person detection and tracking (tracking off by default) |
| Picture orientation | not offered | flip, mirror, ceiling mount (flip and mirror checked on the camera on 2026-11-22; ceiling tracking not yet) |
| Speaker and two-way audio | through the vendor app | ONVIF backchannel on the stream, speaker volume and gain (beep and talk over TCP checked on the camera on 2026-11-23; volume, go2rtc and Frigate not yet; off until you turn the speaker on) |
| Saturation, denoise, white balance | not offered | settings like the 1080p's (host-tested only, not yet tried on the camera) |
| SSH | no | key only |
| Vendor updates | yes | the stock update tool is kept but inert |
| Boot | internal flash | internal flash, no SD card needed |
| Not provided | | generic motion detection (person presence events only), recording to microSD |

What each row's evidence is (validated on a camera, or host-tested only): [features](docs/features.md),
[testing status](docs/testing-status.md).

## What Akam adds

- **Standard video:** RTSP H.264 on port 554, the same URLs on both cameras: `rtsp://<camera-ip>:554/stream0` (main)
  and, on the 2K, `/stream1` (sub). Optional Digest password.
- **Integrations:** ONVIF with WS-Discovery, MQTT with Home Assistant discovery, a signed JSON HTTP API with a
  settings registry and an event feed. All opt-in.
- **Local control:** LEDs and patterns, day/night with IR, picture settings, frame rate and bitrate. On the 2K also
  pan/tilt, presets, patrol, a programmable button, image zones, person detection and tracking.
- **Safe by default:** SSH with an ed25519 key only, no passwords, no telnet or FTP in normal operation, opt-in clock
  sync. Open by default, on both cameras: SSH (22) and RTSP (554). Everything else is opt-in (the signed API on 8092 opens with its first key). **The video stream
  has no password until you set one** (both cameras: `akam rtsp-auth set`). Keep the camera on your LAN; never forward its ports to
  the Internet, and consider blocking its Internet access at your router as well.
- **Light:** code a camera does not use is not compiled in; no recurring flash writes in normal operation.

## What Akam replaces

| | 1080p | 2K |
|---|---|---|
| Vendor application | `anyka_ipc` replaced by `akamd` | `yy_cam` removed, replaced by `akamd` |
| Cloud connection, vendor OTA | gone | gone (the stock OTA tool stays, inert: nothing downloads) |
| Telnet, FTP, SD-card hook | launch lines disabled, watched by a helper | the stock SD hook is kept as the recovery path |
| Flash partition rewritten | APP (`mtd7`) | appfs (`mtd3`) |

## What Akam keeps

Everything below the application stays the stock one and is never redistributed by Akam:

- **Both cameras:** U-Boot, kernel and drivers, Wi-Fi stack, root file system, hardware watchdog.
- **1080p:** the stock boot scripts (patched at build time), the `cmdserver` and `msgserver` helpers.
- **2K:** the vendor libraries and drivers (ISP, NPU, motor) that the Akam app uses at run time, and the rest of the
  stock appfs; the Tuya data in `/config` is never read.

Vendor files stay on your camera and in your private backup; Akam images are assembled on your computer.

## Quick start

You need the camera, a 2.4 GHz Wi-Fi network (5 GHz on the 2K is untested), a microSD card with a USB reader and a PC. Wise
but optional: a 3.3 V USB-UART adapter as a safety net. Full walk-through for both cameras: **[install guide](docs/install.md)**.

1. **Download and check `akam`**, one program for both cameras, from the
   [assistant releases](https://github.com/Foohx/akam-fw/releases?q=assistant-v) (SHA256 and signature:
   [check the download](docs/assistant.md#check-the-download)). It is experimental: host-tested, with only the parts listed in
   [testing status](docs/testing-status.md) run on real cameras. Install from Linux; a first install from macOS or Windows has
   never been done.
2. **1080p:** run `akam` and follow the menu (backup, image, flash, verify; every risky step defaults to No).
3. **2K:** follow the [2K install page](docs/porting/lsc-rotatable-2k/README.md#install) (card first, then backup, seed, build,
   flash). **Keep a UART or a CH341A at hand.**
4. **Then:** `akam rtsp-auth set` (the stream has no password until you set one) and
   `ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/stream0`.

Update: `akam update`. Rollback: `akam restore` (1080p, your stock APP backup; never rehearsed on hardware) or
`akam update --rollback` (2K, previous Akam image); back to stock on the 2K: [back to stock](docs/porting/lsc-rotatable-2k/README.md#back-to-stock).
Releases: [assistant](https://github.com/Foohx/akam-fw/releases?q=assistant-v) and
[firmware kits](https://github.com/Foohx/akam-fw/releases?q=firmware-v) (signed, one per camera model; the program downloads
and verifies the right one). `akam help` lists every command. More: [assistant](docs/assistant.md),
[using Akam](docs/using_akam.md), [troubleshooting](docs/troubleshooting.md).

The old bash wizard (`./akam` in a source checkout, 1080p, Linux) will be retired once the Go program's install has run on a
camera; new features go to the Go program only.

## Integrations

| Protocol | Port | Default | Notes |
|---|---|---|---|
| RTSP | 554 | on | `/stream0` (both), `/stream1` (2K). The 1080p serves one client at a time: put go2rtc or MediaMTX in front |
| ONVIF and WS-Discovery | 80 (1080p), 8000 (2K), 3702/udp | off | subset of Profile S (pan/tilt on the 2K), not certified: [ONVIF](docs/onvif.md) |
| MQTT, Home Assistant discovery | outbound to your broker | off | no broker on the camera, no TLS yet: [MQTT](docs/mqtt.md) |
| Signed HTTP API | 8092 | closed until an API key exists (the assistant makes it, once, over SSH) | HMAC-signed JSON, scopes, events: [1080p](docs/porting/lsc-1080p/control-api.md), [2K](docs/porting/lsc-rotatable-2k/control-api.md), [spec](specs/control-api.md) |
| SSH | 22 | on | ed25519 key only |
| Text control port | 8091 | 1080p only, closed to the network (the camera itself reaches it on 127.0.0.1); `akam control-port open` opens it for monitoring tools | metrics and local settings; the assistant does not use it |

Frigate on the 2K: detect on `stream1`, record on `stream0`. On the 1080p use a single input (`stream0`) for both
roles, or put go2rtc in front: the 1080p serves one client at a time.

```yaml
# 2K
cameras:
  cam:
    ffmpeg:
      inputs:
        - {path: "rtsp://<camera-ip>:554/stream1", input_args: preset-rtsp-restream, roles: [detect]}
        - {path: "rtsp://<camera-ip>:554/stream0", input_args: preset-rtsp-restream, roles: [record]}
```

## Safety and recovery

> **FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.** Akam is experimental, built on reverse
> engineering and written with AI assistance. Writing to the SPI NOR flash can brick the camera; in the worst case
> only an external SPI programmer (for example a CH341A) recovers it. Restoring your backup is not guaranteed. The
> authors accept no liability, to the extent the law allows.

- The installer makes you take a **full verified backup** first. Never publish it.
- **Never cut the power during a flash.** If a flash reports errors, do not power cycle: flash your stock backup.
- One partition is rewritten (APP on the 1080p, appfs on the 2K). U-Boot and the kernel are never written.
- **Rollback:** `akam restore` (1080p; never rehearsed on hardware), `akam update --rollback` (2K; host-tested only).
  Back to stock on the 2K: [back to stock](docs/porting/lsc-rotatable-2k/README.md#back-to-stock), or a UART.
- **SD rescue (2K):** an Akam SD card is read before any Akam code and overrides the flash, so a broken payload does
  not lock you out as long as the appfs still mounts. A half-written `mtd3` is the last resort: a 3.3 V UART or a CH341A
  programmer ([2K recovery](docs/porting/lsc-rotatable-2k/uart-recovery.md)).
- **2K, rescue telnet:** `prepare-2k-sd` makes a card with no telnet (`akam/no_telnet`); the SSH key on the card is enough.
  Any other Akam card (made by the scripts or an older assistant, or by `prepare-2k-sd --rescue-telnet`) opens a root telnet
  on port 23 with no password while it is in, open to your whole LAN: remove it when you are done, or create that empty file.
- **Closed parts:** U-Boot, the kernel, the drivers, the Wi-Fi stack and, on the 2K, the vendor media libraries stay
  closed stock code that Akam cannot audit or update. Blocking the camera's Internet access at your router is good
  defence in depth.
- Legal notes (your device, privacy, radio, interoperability): [legal](docs/legal.md).

## What you can do next

- Put the camera in **Home Assistant** (MQTT discovery) or **Frigate** (RTSP) with no vendor account
  ([MQTT, both cameras](docs/mqtt.md); 2K setup: [using the 2K](docs/porting/lsc-rotatable-2k/using-the-2k.md)).
- Use a **VPN into your home network** for remote viewing instead of a cloud relay.
- On the 2K: pan/tilt from an ONVIF app, **track a person**, patrol presets, bind the button to actions.
- Script anything with the **signed API** and its event feed.

Planned, not implemented: TLS for MQTT, more camera models, motion events and recording. Open gaps: [specs/roadmap.md](specs/roadmap.md); history:
[CHANGELOG](CHANGELOG.md).

## Documentation

[Documentation index](docs/README.md) (with a glossary): user, integration, developer, per camera, porting. Exact behaviour of the
code: [specs/](specs/README.md). Contributors: [CONTRIBUTING](CONTRIBUTING.md). Vulnerabilities: [SECURITY](SECURITY.md).

## Contributing and porting

Bug reports, camera reports and ports are welcome: [open an issue](https://github.com/Foohx/akam-fw/issues) (the bug
template asks for what is needed; how to [report your camera](docs/compatible-cameras.md#report-your-camera)). To add a camera, start with the
[porting template](docs/porting/TEMPLATE-new-camera.md) and [CONTRIBUTING](CONTRIBUTING.md). Never attach a backup,
dump or image to an issue.

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
