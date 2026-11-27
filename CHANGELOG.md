# Changelog

All notable changes to Akam are listed here, in the style of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has two kinds of releases with their own versions and tags, both starting at 0.1.0:

- **Firmware** (`firmware-vX.Y.Z`): the camera firmware kits, one per camera model, and the source.
- **Assistant** (`assistant-vX.Y.Z`): the `akam` program for Linux, macOS and Windows. The bash wizard `./akam` ships
  with the source and follows the assistant version; it is kept for the 1080p on Linux until the Go program's install has run on a camera, then it will be removed (new features go to the Go program only; the [wizard guide](docs/wizard_guide.md) lists its differences).

The text under each heading is what `tools/release/make_release.sh` puts in the release notes. The development
history before the first release is in the git log.

## [firmware 0.1.0] - unreleased

First public release of the Akam camera firmware, with a kit for each of two models, both Anyka AK3918AV130:

- the **LSC Smart Indoor 1080p** (article `3215672.3`, `lsc-indoor-1080p-ak3918`), validated on two units;
- the **LSC Smart Rotatable Camera 2K (Dual Band)** (article `3225722`, `lsc-rotatable-2k-ak3918av130`), validated on one unit,
  running card-less from its flash since 2026-11-15.

**One program name:** the camera program is `akamd` on both cameras (1080p `ak_rtsp`, 2K `akam2k_rtsp` before); the 2K helpers are
`akam-sntp`, `akam-mic`, `akam-led`, its config `akam.conf` (an old `akam2k.conf` is copied once). The 1080p image keeps a link
`ak_rtsp` for one release; kits are `kit_format=2` (the assistant reads 1 and 2; older assistants refuse 2).

**Which binaries were validated:** the 2K runs a build from 2026-11-23 on the validated unit (flip and mirror checked on the camera on 2026-11-22; the speaker
beep, talk with `akam-talk` and the latency bound checked on 2026-11-23); that build predates the `akamd` rename. The current 1080p build (code shared with the 2K refactored, golden re-recorded on 2026-11-22) has not
been flashed yet; the 1080p validations were made with an earlier build: see
[testing status](docs/testing-status.md#which-build-the-hardware-results-come-from).

Experimental: keep a verified backup of your stock camera before you flash anything. Everything under "Added" is the 1080p
unless a line says 2K; the 2K has its own sections below.

### Added

- Video over RTSP (H.264 1080p, 10, 15, 20 or 25 fps), with an opt-in microphone track (off by default on both cameras; a saved setting is kept), optional Digest login, two-way audio
  through the ONVIF audio backchannel.
- ONVIF-compatible services and WS-Discovery, and MQTT with Home Assistant discovery, both opt-in and off by default.
- The 1080p picture runs with sharpness -15 on every ISO level of the ISP sharpening by default (the stock sharpening amplifies
  sensor noise: P frames 7.9 KB to 1.8 KB on a still scene). `akam picture sharpness 0` gives the stock picture back.
- The 1080p picture also runs with 3D noise-reduction bias 2 by default (`isp.denoise`, `akam picture denoise N`, 0 to 4): each light
  level uses the 3D noise-reduction tables of the level 2 steps higher, applied at start-up and after every day/night switch. Measured on
  two cameras: less pulsing at each key frame, 3 to 7 % less static detail. `akam picture denoise 0` gives the stock tables back; a camera
  whose live tables are not as expected keeps the stock tables and logs it.
- Two more 1080p picture settings, same names and meaning as on the 2K (host-tested only, **not yet tried on a camera**): `isp.wdr` (0 to 100, 90 = the
  camera's own backlight curves, nothing sent; lower takes out the shadow lift that the stock day picture applies against a bright window, 0 = none; `akam picture wdr N`)
  and `isp.nr2` (0 to 4, 0 = the camera's own spatial luma noise reduction, N steps of 50 % stronger; `akam picture nr2 N`). Saved only when you change them, put back after
  every day/night switch, refused (nothing sent) when the live ISP table is not as decoded; API rows `isp.wdr` and `isp.nr2` (the 2K's names), Home Assistant numbers. The ISP has
  no defog block, so there is no `isp.defog`; what the ISP offers against haze is in `specs/video-and-streaming.md` section 5.8.
- The 1080p `isp.wdr` below 90 now also flattens the day Y gamma toe that lifts the dark part of the picture (90 = the camera's own, nothing sent; 0 = identity toe, linear between; above 90 nothing more is lifted). A first test showed the WDR curves alone did not clear the haze in a backlit room in daylight. Host-tested only, not yet tried on a camera. The start now leaves its `[isp.wdr] ev=load` diagnostic line like every day/night load, and `WDRDUMP` carries the gamma fields.
- The noise-reduction experiment keys `img.nr2d` and `img.nr2d_bias` (RAM-only, off by default) decode the first noise-reduction
  block of both cameras (it reports mode 192).
- Signed local control API on port 8092 (HMAC key, scopes, audit, rate limits), the same as the Rotatable 2K: it listens only
  while an API key exists (`akamd --api-key new --scope admin --name NAME` over SSH), settings registry behind it and behind the
  8091 text port (LED mode and patterns, night mode, picture, video, ONVIF and MQTT switches, system and stream state), events and
  a "find the camera" LED action. Host-tested only. [Reference](docs/porting/lsc-1080p/control-api.md)
- The same API now has the rows the assistant needs now that 8091 is closed: white balance `img.wb`, the speaker and microphone
  (`audio.out_volume`, `audio.out_enable`, `audio.talk_enable`, `audio.mic_enable`, state `audio.out_state`, `audio.out_underruns`,
  `audio.out_drops`), the Wi-Fi name `net.ssid`, `net.wifi_rate`, `led.state`, the LAN opt-in `control.lan`, and the action `audio.beep`.
- The status LED has rules and patterns (`led.stream led.boot led.nowifi led.night led.idle`); the defaults behave exactly as before.
- Day/night switching, front and IR LED settings, picture settings, NTP, an encoder-stall watchdog, a Wi-Fi supervisor
  and a kernel log filter.
- Secure defaults: SSH keys only, Telnet and FTP off, only ports 22 and 554 listen. The text control port 8091 is closed to the
  network: it listens on the camera's own loopback (the SSH helper scripts use it there). An opt-in (`control.lan`, assistant:
  `control-port open`) opens it to the LAN for monitoring tools; it stays read-only from the network. Host-tested only: the
  closed default has not been checked on a camera yet ([checklist C](docs/hardware-test-checklists.md)).
- One kit per camera model (`akam-kit-<model>-X.Y.Z.tar.xz`), signed, with the fingerprints of the stock firmware it was
  validated against. The kit holds Akam's own files only, never a camera image.

- **Image flip, mirror and ceiling mount** (host-tested only, **not yet tried on a 1080p**): settings `image.flip`, `image.mirror`, `image.ceiling`, the same names as the 2K,
  live and saved in `/etc/config/akam/orient` only when you change them. Done by the sensor readout through the kernel ISP call (no CPU cost), found by reading the
  stock `ak_isp` and `sensor_gc20c3` modules. The call is sent again after each day/night switch. Home Assistant gets the switches `Image flip` and `Image mirror`. In the assistant: `akam picture flip on`, the options screens and the config save/apply carry them.
  `/stream1` stays the same picture as `/stream0`, on purpose. [Details](specs/video-and-streaming.md#57-flip-and-mirror-reverse-engineering-of-2026-11-23-not-yet-hardware-validated-on-the-1080p)

### Added, LSC Rotatable 2K

- **Motion detection and a motion-aware search for a lost person** (host-tested only, **not yet tried on a camera**): a coarse motion grid on the picture (frame differencing of the detection plane, no extra
  channel, the vendor statistics optional) gives the rows `motion.active`, `motion.dir` (left / right / up / down in the picture you see, also with flip, mirror or ceiling), `motion.x`, `motion.y`,
  `motion.source`, the settings `motion.enabled` (on) and `motion.sensitivity`, an event, and the Home Assistant entities **Motion** and **Motion direction**. The head's own moves, orientation changes, day / night
  switches and stream restarts never count. New `track.search motion` (the default; `legacy` stays selectable through the API, `akam.conf` and the assistant): when the person is lost, one short look toward where they went or toward new motion, a passer-by gets one
  look then goes home after 3 s, a 30 s cooldown, no scans and no sweeps, never near a soft limit; in the bench the motor travel of a pass-by drops by 77 to 91 % at the same tracking score, at the cost of not
  searching for someone who stands outside the view. The 1080p has no motion detection yet (parity item).
- **2K boot scene check: threshold 0.85 and a new setting** (host-tested only, **not yet tried on a camera**): the default `ptz_scene_min` goes from 0.90 to 0.85 (a real boot with an unmoved head scored 0.899; the recorded negatives score under 0.5 and a head shifted by 3 px is refused whatever the score). The new API setting `ptz.scene_min` (0.80 to 0.99, an option of the assistant, saved only when you change it, used at the next boot check) overrides the `ptz_scene_min` key of `akam.conf` once set. The assistant also lists `track.search`, `motion.enabled` and `motion.sensitivity`. [Details](docs/porting/lsc-rotatable-2k/tracking.md#motion-detection-and-the-motion-aware-search)
- **Lower video latency on `/stream0`** (camera side validated on 2026-11-23: frame age at send avg about 5 ms, max about 20 ms; glass to glass not yet measured with a stopwatch; measured before: about 2 s glass to glass at 2304x1296, 10 fps, 3000 kbit/s on a weak Wi-Fi): a joining client starts only
  on a keyframe younger than 300 ms, a client that falls behind skips to the newest keyframe instead of playing the backlog, the TCP send buffer is 64 KiB (was 256 KiB; also on the 1080p), and the
  pacing drops to the next keyframe when a backlog stands for more than 300 ms plus the keyframe's own time (also on the 1080p). The app log shows every 10 s per client the frame age at send
  (average and maximum) and the largest unsent backlog (the 1080p prints the same line). [Analysis](docs/porting/lsc-rotatable-2k/video-latency.md)

- **Speaker and two-way audio** (validated in part on the camera on 2026-11-23: the beep and talk with `tools/akam-talk` over TCP; volume and gain, go2rtc and Frigate not yet): the 1080p's speaker engine and talk session now run on the 2K (shared code in `src/features/audio`), with the same
  settings and names: `audio.out_enable` (off by default on the 2K), `audio.out_volume`, `audio.out_gain`, `audio.out_amp`, `audio.talk_enable`, `audio.talk_duplex`, the action `audio.beep`, and the
  read-only `audio.out_state` and `audio.talk_state`. The ONVIF audio backchannel (G.711 PCMU/PCMA, TCP interleaved, needs the stream password) is offered by the 2K RTSP server.
  The playback ioctl numbers were read from the vendor libraries; the amplifier is switched by the `ak_pcm` driver itself.
- **Backlight strength and spatial noise reduction** (host-tested only, not tried on a camera): `isp.wdr` (0 to 100, default 90 = the stock WDR, nothing changes by default; `akam picture wdr N`) and
  `isp.nr2` (0 to 4, default 0 = the camera's own table, nothing sent; `akam picture nr2 N`) for the grain in the dark areas of a backlit scene. Both live, saved, put back after every day/night
  switch, in the options screen and the install-time questions. `isp.wdr` is the ISP's own WDR effect: the vendor blends the ISP file's WDR curves with a straight line, 0 = off, 100 = in full (the stock runs 90).
  `isp.nr2` raises the luma 2D filter of every gain record from the original table. The `--isp-test 1` hook gains `get wdr` and logs the nr2 enables and intensities; the API reply buffer of the 2K
  grows by 2 KB (the settings list no longer fitted).
- **Saturation, denoise and white balance** (host-tested only, not tried on a camera): `isp.saturation` (-50 to 50), `isp.denoise` (0 to 4) and `img.wb` (`conf` or `manual`), the 1080p's names, ranges and
  meaning, through the 2K ISP; put back after every day/night switch. `isp.denoise` defaults to 0 on the 2K (the 1080p defaults to 2) until it has been judged on the camera.
- The assistant sets them up at install time (the speaker, two-way audio and picture questions are part of the same `prepare-2k-sd` setup questions as the services: the answers travel as the file `settings` of the first-install card and `seed-2k` copies it to `/config/akam` once) and later in the
  options screen and `akam settings` (signed API).
- MQTT, ONVIF, the stream password, NTP and the microphone are settings of the signed API (`mqtt.*`, `onvif.*`, `rtsp.user`, write-only
  `rtsp.password`, `ntp.servers`, `audio.mic_enable`; admin key), like on the 1080p, and the assistant sets them on a 2K (`audio`, `mqtt`,
  `onvif`, `ntp`, `rtsp-auth`, the options screen) and at install time (`prepare-2k-sd` questions, files on the first-install card, copied by
  `seed-2k`). Host-tested only.
- The drift check no longer re-homes the tilt for a flat night view (IR, plain wall) against a day reference: that is inconclusive until
  a reference of the same light mode exists.

- **Image flip, mirror and ceiling mount** (flip and mirror checked on the camera on 2026-11-22; colour, day/night and ceiling tracking not yet): settings `image.flip`, `image.mirror` (vertical / horizontal, default off,
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

- 2K fix (2026-11-24): `image.brightness`, `image.contrast`, `image.sharpness` and `image.mains_hz` are now saved on the camera like the `isp.*` rows (written only when
  you change one, never at start or while idle), applied at start and put back after every day/night switch; before they went back to 50 at each restart (sharpness 70 was
  lost). Not yet validated on a camera.
- 2K fix (2026-11-24): `img.wb manual` is refused on the 2K (the setting accepts `conf` only): it switched the white balance to the camera's own manual gains and gave a strong blue
  picture, and there is no safe way to freeze the current automatic gains ([evidence](docs/porting/lsc-rotatable-2k/isp-controls.md)). The 1080p keeps `conf` and `manual`.

- **2K long press now really closes the camera, and the motors are spared** (host-tested only, **not yet tried on the camera**): the park pose is tilt 80, 80 steps above the bottom stop (was 300, the soft minimum: the lens still looked out), below the soft range for the park only (the driver frame reaches down to it, every other motion keeps the soft range); a park never presses against the stop. The old `ptz_park 1275:300` still works. The boot does not calibrate when the camera was last left closed (a record written only when you close or open it); the automatic tilt re-home runs at most every 6 hours (`ptz_tilt_rehome_gap_min` default 360, was 30); `/state` rows `ptz.stall_steps_est`, `ptz.cal_full`, `ptz.cal_short` count what the motor skipped. [Details](docs/porting/lsc-rotatable-2k/ptz-findings.md)

### Known limits

- Validated on two 1080p cameras and one 2K camera. Anything else may not work or may brick the camera; the installer
  warns and a different flash layout is refused ([compatible cameras](docs/compatible-cameras.md)).
- 2K: the speaker and two-way audio are validated in part (beep and talk), the new picture settings (saturation, denoise, white balance) are not validated on a camera; no recording to microSD, no generic motion detection (person presence events only); no
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
- **One client for both cameras:** the 1080p is read and changed through its signed API (port 8092), like the 2K; the text port 8091 is
  closed to the network and the assistant no longer uses it. The API key of this PC is made once, over SSH (`akamd --api-key new`, the
  secret never printed), at the end of an install or at the first settings command of an existing install (asked, default No), or with
  `akam api-key` (`replace` makes a new one). An update keeps the keys. `akam control-port [open|close]` sets the LAN opt-in of 8091 (also in the
  options screens). `akam debug` is removed: it let the network write through a port nobody can reach any more. Host-tested only.
- On a 2K, `audio [show | volume N | beep | on | off | talk on|off | mic on|off]`, `say FILE` and `talk` work like on the 1080p: settings through the signed API, the beep through `POST /actions/audio.beep`, speech through the same RTSP backchannel client (no SSH needed, so it fits the card-less layout).
  Host-tested only: the 2K beep and talk were checked on 2026-11-23 with a curl helper and `akam-talk`, not with these commands.
  The stream password comes from `AKAM_RTSP_PASSWORD`, `--password-file`, the secret kept in this camera's config (only when saved for the same user) or is typed.
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
