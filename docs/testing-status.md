# Testing status: what was tested, and where

This page separates three levels of evidence, so you know what to trust:

- **Host tests:** the `tests/*.sh` scripts, run on a PC with fixtures, fake binaries and stubs. They check logic,
  parsing, limits and file formats. They say nothing about the real hardware, the real kernel or real clients.
- **Validated in RAM:** a development build copied to `/tmp` on one camera and run from there, before the final image.
- **Hardware validated:** the flashed image, running on the cameras of [compatible cameras](compatible-cameras.md).

## Which build the hardware results come from

**LSC Rotatable 2K:** the camera runs image `76a3aa1f`, flashed on 2026-11-23 (a hot update, read back byte for byte, card-less
reboot; before it, a build flashed on 2026-11-22). It has the speaker, talk and picture settings, the 8091 API work, the video
latency bound and the night re-home fix, but **not the `akamd` rename** (the program on the camera is still named after the old
convention). After the 2026-11-22 reboot: motors calibrated to home, tracking on, both RTSP streams at 2304x1296 and 640x360, signed
API, settings reload, and the orientation checks O2K-2 to O2K-4. The results of the other 2K rows were recorded with earlier
binaries and apply to the current tree through its tests. The build of the current tree (with the `akamd` rename) has not been
flashed on either camera.

**LSC Indoor 1080p:** a build from 2026-11-24 (with the `akamd` rename) runs on one camera, installed over SSH with `akam update`. Checked on it: the update and the read-back, `akamd` and the `ak_rtsp` link, the read-only hwcheck (`step verify` green, with and without a power cycle), port 8091 on 127.0.0.1 only and 8092 open once a key exists, the API key made by `akam api-key --yes`, the mic off by default with the speaker setting kept, and flip and mirror live from the assistant. The second 1080p camera still runs an older build; rows below that were not re-checked on the 2026-11-24 build keep their earlier date.

"Not yet" means **not run on a camera**. Checklists to tick on your own camera:
[hardware test checklists](hardware-test-checklists.md).

## Summary of the hardware results

Two LSC Smart Indoor 1080p cameras (`3215672.3`) were flashed with an earlier image (see above) and ran continuously with a home
server pulling RTSP.

- **Video:** 15 fps measured on both cameras (14.9 to 15 fps, 0 encoder errors); 25 fps measured in daylight on one
  (751 frames in 30 s, about 1.4 Mbit/s, 0 skipped frames); night mode correctly drops to 10 fps. A RAM test delivered
  exactly 15, 20, 25 and 30 fps (30 is not offered).
- **Boot and ports:** clean boot after a power cycle; only ports 22 and 554 listen by default (8091 only after the opt-in; older images also had 8091 open), no UDP listener;
  settings survive a flash.
- **ONVIF-compatible services:** `tools/hw_tests/onvif_check.py` 34 of 34 checks (including RTSP Digest `DESCRIBE`),
  WS-Discovery, WS-Security with the camera clock unset, imaging set and get with restore; ONVIF Device Manager on
  Windows found the camera, logged in, played live video and changed the image. Answers take 18 to 75 ms, with
  outliers of about 1 s on weak Wi-Fi.
- **RTSP:** a second client gets 503 while one streams (100 attempts) and the stream is unaffected.
- **Logs:** the kernel log filter works: syslog about 460 KB a day, about 1.5 days kept in RAM.
- **RAM soak** (one camera, 15 minutes): same process throughout, no encoder-stall exit, load about 0.1.

## LSC Indoor 1080p

| Feature | Host tests | Hardware | Notes |
|---|---|---|---|
| RTSP H.264 1080p video and microphone (15 fps) | RTSP, RTP, SDP, jitter, TCP pacing tests | Yes, two cameras (VLC, ffmpeg, go2rtc over TCP) | VLC over UDP can freeze behind a client firewall: use TCP |
| RTSP Digest authentication | auth and credential tests | Yes, two cameras | Off by default |
| One RTSP client, 503 for a second | `rtsp_busy_test.sh` | Yes | Use go2rtc in front for several viewers |
| Talk through the speaker (ONVIF backchannel) | backchannel, talk, G.711, VAD tests | Yes, two cameras | Needs the video password |
| Microphone off with the speaker on | `audio_sup_test.sh`, `wizard_audio_test.sh` | Not yet | |
| Status LED, IR LEDs | `leds_test.sh`, `wizard_led_test.sh` | Yes: status on, off, auto; IR auto | |
| Automatic day/night switching | night logic and ISP tests | Yes, including the night probe | Long runs across dusk and dawn: not yet |
| Picture settings (brightness, contrast, saturation, sharpness) | `isp_tune_test.sh`, `imgq_test.sh` | In RAM on one camera | Not yet on the final image |
| Frame rate 10, 15, 20, 25 | `video_cfg_test.sh`, `video_apply_test.sh` | 15 and 25 yes; 20 in RAM only | |
| Bitrate and key-frame interval | same | Not yet exercised | Applied at the next video restart |
| Control port 8091: loopback only by default, opt-in to the LAN (read-only from the network) | `control_test.sh`, `ctl_sys_test.sh` | Reads and `SET` refused remotely were validated before it was closed; the closed default and the opt-in are host-tested only (checklist C) | |
| Strict `SET` ranges (`ae.*`, `night.*`, `isp.*`) | `control_test.sh` | Not yet | |
| On-camera `akam_debug.sh` (needs `control.lan`; the Go program has no `debug`) | `akam_debug_test.sh` | Not yet on the final image | |
| ONVIF-compatible services and WS-Discovery | SOAP, WS-Discovery, SHA-1, ONVIF tests | Yes, one camera | Off by default. Not certified, not tested with the ONVIF test tool |
| MQTT client with Home Assistant discovery | codec (with fuzzing), discovery, fake broker tests | **Not yet.** No camera, no real broker | Off by default |
| Opt-in NTP | `akam_ntp_test.sh` | Yes, both cameras | The clock starts in 1970 without it |
| First install from a stock camera (bash wizard, to be retired) | `wizard_camera_test.sh`, `build_from_kit_test.sh` | Yes, two cameras: backup, flash through the maintenance telnet, boots without the card | Linux bash wizard only |
| Update Akam to Akam over SSH | `reflash_ssh_test.sh`, `flash_guard_test.sh` | Yes, two cameras: md5 read-back, then a power cycle | |
| SSH key admin, Telnet and FTP off | `ssh_init_test.sh`, `lockdown_test.sh` | Yes, two cameras | Telnet staying off after a Wi-Fi reconnect: not yet |
| Restore to stock (`akam restore`) | `reflash_ssh_test.sh` | **Never rehearsed** | No guarantee |
| Encoder-stall self-exit, Wi-Fi supervisor | `swdog_test.sh`, `akam_net_test.sh` | In the image; never triggered | |
| Wrapper respawn backoff | `wrapper_backoff_test.sh` | Not yet | |
| Kernel log filter | `klog_test.sh` | Yes | |
| Configs, SSH key per camera (Go assistant) | `assistant_test.sh` | Not yet | Never run on a real camera |
| Go assistant, daily commands | `assistant_test.sh` (fake cameras) | Partly: Linux amd64 and an earlier Windows build | See [tested platforms](assistant.md#tested-platforms) |
| Go assistant, update, restore, telnet backup and flash, SD writer, firmware chooser | `assistant_test.sh` | **Not yet** | Never run on a real camera or card |
| Native image builder (`akam image --builder native`) | reader and writer round trips | Partly: same file tree as the bash builder from a real backup; the image was mounted read-only on a real camera and 117 files read back identical. **Never flashed** | Experimental, opt-in |
| No internet at all (isolated network) | | Not yet | |
| 1080p picture orientation: `image.flip`, `image.mirror`, `image.ceiling` (ISP call `0x40044979`, sensor readout), the assistant words and config keys | settings and API tests with a fake ISP call, assistant tests with a fake camera | **Partly** (2026-11-24, current dev build) | Flip is vertical, mirror horizontal, back to normal, applied live from the assistant (`akam picture flip|mirror on|off`), colours fine in daylight. Not yet: O1080-1, 4, 6 to 11. Checks O1080-1 to O1080-11 in the [1080p orientation checklist](hardware-test-checklists.md#1080p-picture-orientation-flip-mirror-ceiling-mount) |

## LSC Rotatable 2K

One unit (article `3225722`), card-less since 2026-11-15; the build on it is named above. The table above is the 1080p. Evidence for the 2K, by page:
[features](features.md#lsc-rotatable-2k), the findings pages of its [entry page](porting/lsc-rotatable-2k/README.md).

| Feature | Host tests | Hardware | Notes |
|---|---|---|---|
| Card-less boot from the flashed appfs, SD rescue | `tests/rot2k_*_test.sh` (scripts) | Yes, one unit | [card-less appfs](porting/lsc-rotatable-2k/cardless-appfs.md) |
| Two RTSP streams (`/stream0`, `/stream1`), key-only SSH | host tests | Yes | |
| Pan/tilt (`ptz.move`, `ptz.home`), person detection, tracking | policy and closed-loop simulations | Yes: runs of 2026-11-16 and 2026-11-17 | Tracking is off by default; see [tracking](porting/lsc-rotatable-2k/tracking.md) |
| Front button, image zones, night, LEDs | fake hardware | Partly | [button and zones](porting/lsc-rotatable-2k/button-and-zones.md) |
| Signed control API (port 8092) | full suite | signed requests used on the camera: settings changes (speaker, latency, flip and mirror), `ptz.recalibrate`, state reads; image `76a3aa1f` on 2026-11-23, with a curl-based helper | The rest of the endpoints: host tests only |
| `akamd` rename on a camera (checks R1, R2, R4, B7 partly) | host tests | **Partly** (2026-11-24, current dev build): 1080p: `/usr/bin/ak_rtsp` link to `akamd`, `akamd` running, update over SSH from the previous image OK, 8091 on 127.0.0.1 only, 8092 open once a key exists (key made by `akam api-key --yes`), microphone off by default after the update, speaker setting kept. 2K: `akam2k.conf` migrated to `akam.conf`, identical, old file kept | Not yet: R3, R5 to R8; `akam step verify` after an update (see the post-update check) |
| ONVIF with pan/tilt | yes | **Not yet** | [ONVIF](onvif.md) |
| MQTT, Home Assistant discovery | yes | **Not yet** | [MQTT](mqtt.md) |
| Speaker beep and talk (checks SPK-1, TALK-2) | shared engine, talk session and loopback RTSP tests | **Yes, partly** (2026-11-23, image `76a3aa1f`): beep and melodies audible and clean at volume 70; talk with `tools/akam-talk` over TCP, six sessions up to 40 s played clean | Not yet: SPK-2 to SPK-6, TALK-1, TALK-3 to TALK-6 (volume and gain, restart, go2rtc, Frigate); the assistant's `audio beep` and `say` on a 2K never ran on a camera |
| Picture: saturation, sharpness, denoise, white balance (PIC-1 to PIC-10) | fake ISP tests | **Partly** (2026-11-24, current dev build, 2K, daylight): saturation validated (-30 dulls, +30 over-saturates, +15 natural, live); sharpness 70 crisper than 50 without halo, 90 darkens edges; denoise applied without errors, 2 smooths slightly on a bright static scene; bitrate 3000 against 4500: no visible difference. **White balance FAILED**: `img.wb manual` gives a strong blue cast right away, `conf` restores it | Not yet: denoise in a dim room, PIC-2, PIC-4, PIC-6 to PIC-8; `img.wb` manual needs a fix and is not saved on the 2K |
| Microphone setting through the API (`audio.mic_enable`) | registry and API tests | **Not yet** | The track itself is validated |
| Video latency bound (L2K-1) | stream queue and send-path tests | **Yes, camera side** (2026-11-23): frame age at send avg about 5 ms, max about 20 ms | Glass to glass: improved, not yet measured with a stopwatch |
| Go assistant: `backup-2k`, `prepare-2k-sd`, `key-2k`, `seed-2k`, `appfs-build`, `appfs-flash`, `update`, `update --rollback` | `assistant_test.sh` (fake cameras, fake card) | **Partly** (2026-11-24, current dev build): `audio show`, `audio beep`, `audio volume`, `picture show`, `settings get` and `say` (after the `/stream0` fix) work on the camera; `key-2k` was NOT run (it needs a terminal) | Never run on a real camera or card; the scripts they port (`flash_appfs.sh`, `build_appfs.sh`) did run |
| Back to stock (restore of the six-partition backup) | none: no `akam restore-2k` command | **Never run** | Manual procedure: [back to stock](porting/lsc-rotatable-2k/README.md#back-to-stock) |
| Picture orientation: `image.flip` (vertical), `image.mirror` (horizontal), `image.ceiling`; also applied to zones, tracker, PTZ directions and scene reference | orientation, zones and tracker unit tests, API and MQTT tests | **Partly** (2026-11-22) | Flip is vertical, mirror horizontal, both off restores the picture, applied live without a stream restart or a motor recalibration. Not yet checked: colour (the test ran in night mode), day/night switches, ceiling-mount tracking direction, Home Assistant. Checklist: [hardware checklist](hardware-test-checklists.md#2k-picture-orientation-flip-mirror-ceiling-mount) |
| Open ports of the card-less image | | **Not yet** (no full port scan) | |

## How to run the host tests

```bash
bash tools/build/bootstrap.sh     # once: pinned toolchain (Zig, BusyBox, Go modules)
tools/ci/local_check.sh           # shellcheck, every tests/*_test.sh, Go tests, tracked-file hygiene
bash tests/readme_links_test.sh   # documentation links and `akam` commands only
```

A single test runs on its own, for example `bash tests/swdog_test.sh`. Without the bootstrap, tests that need the
pinned BusyBox or compiler print `skip:` and are counted as skipped, not passed.

Known flaky under load: `rtsp_backchannel_test.sh` and the thread-count check of `wsd_test.sh` can fail on a busy
machine (rerun alone). `control_test.sh` and `http_srv_test.sh` use fixed local ports: **do not run two
`local_check.sh` at once**.
