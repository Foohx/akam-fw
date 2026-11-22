# Changelog

All notable changes to Akam are listed here, in the style of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has two kinds of releases with their own versions and tags, both starting at 0.1.0:

- **Firmware** (`firmware-vX.Y.Z`): the camera firmware kits, one per camera model, and the source.
- **Assistant** (`assistant-vX.Y.Z`): the `akam` program for Linux, macOS and Windows. The bash wizard `./akam` ships
  with the source and follows the assistant version.

The text under each heading is what `tools/release/make_release.sh` puts in the release notes. The development
history before the first release is in the git log.

## [firmware 0.1.0] - unreleased

First public release of the Akam camera firmware, with a kit for each of two models, both Anyka AK3918AV130:

- the **LSC Smart Indoor 1080p** (article `3215672.3`, `lsc-indoor-1080p-ak3918`), validated on two units;
- the **LSC Smart Rotatable Camera 2K (Dual Band)** (article `3225722`, `lsc-rotatable-2k-ak3918av130`), validated on one unit,
  running card-less from its flash since 2026-11-15.

**Which binaries were validated:** the current 2K build was flashed on the validated unit on 2026-11-22 (flip and mirror
checked on the camera). The current 1080p build (code shared with the 2K refactored, golden re-recorded on 2026-11-22) has not
been flashed yet; the 1080p validations were made with an earlier build: see
[testing status](docs/testing-status.md#which-build-the-hardware-results-come-from).

Experimental: keep a verified backup of your stock camera before you flash anything. Everything under "Added" is the 1080p
unless a line says 2K; the 2K has its own sections below.

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
- The noise-reduction experiment keys `img.nr2d` and `img.nr2d_bias` (RAM-only, off by default) decode the first noise-reduction
  block of both cameras (it reports mode 192).
- Signed local control API on port 8092 (HMAC key, scopes, audit, rate limits), the same as the Rotatable 2K: it listens only
  while an API key exists (`ak_rtsp --api-key new --scope admin --name NAME` over SSH), settings registry behind it and behind the
  8091 text port (LED mode and patterns, night mode, picture, video, ONVIF and MQTT switches, system and stream state), events and
  a "find the camera" LED action. Host-tested only. [Reference](docs/porting/lsc-1080p/control-api.md)
- The status LED has rules and patterns (`led.stream led.boot led.nowifi led.night led.idle`); the defaults behave exactly as before.
- Day/night switching, front and IR LED settings, picture settings, NTP, an encoder-stall watchdog, a Wi-Fi supervisor
  and a kernel log filter.
- Secure defaults: SSH keys only, Telnet and FTP off, only ports 22, 554 and 8091 listen.
- One kit per camera model (`akam-kit-<model>-X.Y.Z.tar.xz`), signed, with the fingerprints of the stock firmware it was
  validated against. The kit holds Akam's own files only, never a camera image.

### Added, LSC Rotatable 2K

- **Image flip, mirror and ceiling mount** (host-tested only, not tried on the camera): settings `image.flip`, `image.mirror` (vertical / horizontal, default off,
  live, saved only when you change them) and `image.ceiling` (both at once, for a camera mounted upside down), with Home Assistant switches. The SDK call is
  `ak_vpss_switch_flip_mirror(0, flip, mirror)`. The detection zones are stored in the sensor frame and shown in the displayed one, person tracking and the PTZ
  directions (ONVIF, `ptz.vel`, the step buttons) follow the picture, and the scene / drift references are compared in the sensor frame so they stay valid.
  [Details](docs/porting/lsc-rotatable-2k/isp-controls.md#32-flip-and-mirror-ak_vpss_switch_flip_mirror-high-on-the-call-and-its-arguments-unverified-on-the-camera)

- Card-less operation: Akam runs from the flash (the appfs partition is rewritten, U-Boot and the kernel never), no SD card
  needed. An Akam SD card inserted at power-up overrides the flash and is the rescue path.
- Two RTSP streams (`/stream0` 2304x1296, `/stream1` 640x360), optional Digest login, optional microphone track.
- Pan/tilt, presets, patrol, home, soft limits and drift correction; NPU person detection (on by default) and person
  tracking (**off by default**, `track=1`); front button actions and image zones; day/night and LEDs.
- The same signed API (port 8092), ONVIF with pan/tilt and WS-Discovery (port 8000) and MQTT with Home Assistant discovery,
  opt-in. ONVIF, MQTT and the full API suite are host-tested only; PTZ API actions were used on the camera.
- Wi-Fi country and band settings (off by default; 5 GHz untested).
- Security note: while an Akam SD card is inserted, a card without the file `akam/no_telnet` opens a passwordless root
  telnet shell on port 23 (the rescue design). Cards from `akam prepare-2k-sd` carry that file by default;
  `--rescue-telnet` keeps the telnet. The video stream has no password until you set one.

All features, with their default and how well each one is tested: [features](docs/features.md).

### Known limits

- Validated on two 1080p cameras and one 2K camera. Anything else may not work or may brick the camera; the installer
  warns and a different flash layout is refused ([compatible cameras](docs/compatible-cameras.md)).
- 2K: no speaker output, no recording to microSD, no generic motion detection (person presence events only); no
  `akam restore-2k` command, back to stock is a manual procedure
  ([back to stock](docs/porting/lsc-rotatable-2k/README.md#back-to-stock)); open ports of the card-less image are not
  confirmed by a port scan.
- MQTT, the watchdog and several settings are host-tested only, and the image builder built into the assistant was
  never flashed. The evidence, test by test: [testing status](docs/testing-status.md).
- 1080p: one RTSP client at a time; no motion detection, no recording to microSD.

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
- LSC Rotatable 2K commands, **host-tested only** (fake camera, fake SD card; never run on a real camera): `backup-2k` (six
  partitions, read only, double read), `prepare-2k-sd` (the first-install card from the signed kit; disables the rescue telnet by
  default, `--rescue-telnet` keeps it), `seed-2k` (copies the card's Wi-Fi and SSH key into `/config/akam` before the flash),
  `key-2k` (the API key of the assistant), `appfs-build` and `appfs-flash` (build and write the appfs image, rehearsal by default,
  typed confirmation), `update` and `update --rollback` (one flow with a rehearsal, read-back check and the previous image kept).
  These commands exist only in the Go program, not in the bash wizard `./akam`.

Details of what it does: [assistant guide](docs/assistant.md).

### Known limits

- The binaries carry no Windows or Apple certificate: SmartScreen, Gatekeeper and the UAC prompt warn. Check the SHA256
  first.
- Only Linux amd64 was partly tested on real cameras; update, restore, telnet backup, flash and the SD writer were never
  run on hardware from this program, and neither was any 2K command. The other targets are built, not hardware-tested
  ([testing status](docs/testing-status.md)).
- Building an image on Windows and macOS is not part of the default path: import an image built on Linux.
