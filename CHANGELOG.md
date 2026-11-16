# Changelog

All notable changes to Akam are listed here, in the style of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has two kinds of releases with their own versions and tags, both starting at 0.1.0:

- **Firmware** (`firmware-vX.Y.Z`): the camera firmware kits, one per camera model, and the source.
- **Assistant** (`assistant-vX.Y.Z`): the `akam` program for Linux, macOS and Windows. The bash wizard `./akam` ships
  with the source and follows the assistant version.

The text under each heading is what `tools/release/make_release.sh` puts in the release notes. The development
history before the first release is in the git log.

## [firmware 0.1.0] - unreleased

First public release of the Akam camera firmware: open add-ons for the stock firmware of the **LSC Smart Indoor 1080p**
(article `3215672.3`, Anyka AK3918AV130), the one model validated so far (`lsc-indoor-1080p-ak3918`). Experimental:
keep a verified backup of your stock camera before you flash anything.

### Added

- Video and audio over RTSP (H.264 1080p with microphone, 10, 15, 20 or 25 fps), optional Digest login, two-way audio
  through the ONVIF audio backchannel.
- ONVIF-compatible services and WS-Discovery, and MQTT with Home Assistant discovery, both opt-in and off by default.
- The 1080p picture runs with sharpness -15 on every ISO level of the ISP sharpening by default (the stock sharpening amplifies
  sensor noise: P frames 7.9 KB to 1.8 KB on a still scene). `./akam picture sharpness 0` gives the stock picture back.
- The 1080p picture also runs with 3D noise-reduction bias 2 by default (`isp.denoise`, `./akam picture denoise N`, 0 to 4): each light
  level uses the 3D noise-reduction tables of the level 2 steps higher, applied at start-up and after every day/night switch. Measured on
  two cameras: less pulsing at each key frame, 3 to 7 % less static detail. `./akam picture denoise 0` gives the stock tables back; a camera
  whose live tables are not as expected keeps the stock tables and logs it.
- Decoding fix for the noise-reduction keys on the 1080p: the first noise-reduction block of both cameras reports mode 192, which the
  decoder used to refuse, so `img.nr2d` and `img.nr2d_bias` (RAM-only experiment keys, off by default) are now accepted.
- Day/night switching, front and IR LED settings, picture settings, NTP, an encoder-stall watchdog, a Wi-Fi supervisor
  and a kernel log filter.
- Secure defaults: SSH keys only, Telnet and FTP off, only ports 22, 554 and 8091 listen.
- One kit per camera model (`akam-kit-<model>-X.Y.Z.tar.xz`), signed, with the fingerprints of the stock firmware it was
  validated against. The kit holds Akam's own files only, never a camera image.

All features, with their default and how well each one is tested: [features](docs/features.md).

### Known limits

- Validated on two cameras of one model only. Anything else may not work or may brick the camera; the installer warns
  and a different flash layout is refused ([compatible cameras](docs/compatible-cameras.md)).
- MQTT, the watchdog and several settings are host-tested only, and the image builder built into the assistant was
  never flashed. The evidence, test by test: [testing status](docs/testing-status.md).
- One RTSP client at a time; no motion detection, no recording to microSD.

## [assistant 0.1.0] - unreleased

First public release of `akam`, a single static program that guides you through putting Akam on a supported camera and
managing it afterwards. It downloads the firmware kit of your camera model itself and checks its checksum and signature.

### Added

- A full-screen home screen, a camera chooser (remembered cameras, name lookup, ONVIF discovery, network scan), option
  screens (LED and night, picture, video, audio, network, security), `talk` to the camera speaker.
- Backup and restore (SSH for an Akam camera; telnet and the toolkit card for a stock camera, with a verified stock
  backup required before any flash), first install and update, `akam doctor`, `akam licenses`.
- Camera model detection from the stock firmware of the backup (shown by `akam status`), and a "Which firmware?" chooser
  that lists only the firmware releases that have a kit for your model.
- Six targets (Linux, macOS, Windows; amd64 and arm64), reproducible builds, no runtime dependency.

Details of what it does: [assistant guide](docs/assistant.md).

### Known limits

- The binaries carry no Windows or Apple certificate: SmartScreen, Gatekeeper and the UAC prompt warn. Check the SHA256
  first.
- Only Linux amd64 was partly tested on real cameras; update, restore, telnet backup, flash and the SD writer were never
  run on hardware from this program. The other targets are built, not hardware-tested
  ([testing status](docs/testing-status.md)).
- Building an image on Windows and macOS is not part of the default path: import an image built on Linux.
