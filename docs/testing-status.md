# Testing status: what was tested, and where

This page separates three levels of evidence, so you know what to trust:

- **Host tests:** the `tests/*.sh` scripts, run on a PC with fixtures, fake binaries and stubs. They check logic,
  parsing, limits and file formats. They say nothing about the real hardware, the real kernel or real clients.
- **Validated in RAM:** a development build copied to `/tmp` on one camera and run from there, before the final image.
- **Hardware validated:** the flashed image, running on the cameras of [compatible cameras](compatible-cameras.md).

"Not yet" means **not run on a camera**. Checklists to tick on your own camera:
[hardware test checklists](hardware-test-checklists.md).

## Summary of the hardware results

Two cameras (LSC Smart Indoor 1080p `3215672.3`) were flashed with a recent image and ran continuously with a home
server pulling RTSP.

- **Video:** 15 fps measured on both cameras (14.9 to 15 fps, 0 encoder errors); 25 fps measured in daylight on one
  (751 frames in 30 s, about 1.4 Mbit/s, 0 skipped frames); night mode correctly drops to 10 fps. A RAM test delivered
  exactly 15, 20, 25 and 30 fps (30 is not offered).
- **Boot and ports:** clean boot after a power cycle; only ports 22, 554 and 8091 listen by default, no UDP listener;
  settings survive a flash.
- **ONVIF-compatible services:** `tools/hw_tests/onvif_check.py` 34 of 34 checks (including RTSP Digest `DESCRIBE`),
  WS-Discovery, WS-Security with the camera clock unset, imaging set and get with restore; ONVIF Device Manager on
  Windows found the camera, logged in, played live video and changed the image. Answers take 18 to 75 ms, with
  outliers of about 1 s on weak Wi-Fi.
- **RTSP:** a second client gets 503 while one streams (100 attempts) and the stream is unaffected.
- **Logs:** the kernel log filter works: syslog about 460 KB a day, about 1.5 days kept in RAM.
- **RAM soak** (one camera, 15 minutes): same process throughout, no encoder-stall exit, load about 0.1.

## Matrix

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
| Control port 8091, read-only from the network | `control_test.sh`, `ctl_sys_test.sh` | Yes: reads work, `SET` refused remotely | |
| Strict `SET` ranges (`ae.*`, `night.*`, `isp.*`) | `control_test.sh` | Not yet | |
| Debug mode (`./akam debug on`) | `akam_debug_test.sh` | Not yet on the final image | |
| ONVIF-compatible services and WS-Discovery | SOAP, WS-Discovery, SHA-1, ONVIF tests | Yes, one camera | Off by default. Not certified, not tested with the ONVIF test tool |
| MQTT client with Home Assistant discovery | codec (with fuzzing), discovery, fake broker tests | **Not yet.** No camera, no real broker | Off by default |
| Opt-in NTP | `akam_ntp_test.sh` | Yes, both cameras | The clock starts in 1970 without it |
| First install from a stock camera (bash wizard) | `wizard_camera_test.sh`, `build_from_kit_test.sh` | Yes, two cameras: backup, flash through the maintenance telnet, boots without the card | Linux bash wizard only |
| Update Akam to Akam over SSH | `reflash_ssh_test.sh`, `flash_guard_test.sh` | Yes, two cameras: md5 read-back, then a power cycle | |
| SSH key admin, Telnet and FTP off | `ssh_init_test.sh`, `lockdown_test.sh` | Yes, two cameras | Telnet staying off after a Wi-Fi reconnect: not yet |
| Restore to stock (`./akam restore`) | `reflash_ssh_test.sh` | **Never rehearsed** | No guarantee |
| Encoder-stall self-exit, Wi-Fi supervisor | `swdog_test.sh`, `akam_net_test.sh` | In the image; never triggered | |
| Wrapper respawn backoff | `wrapper_backoff_test.sh` | Not yet | |
| Kernel log filter | `klog_test.sh` | Yes | |
| Configs, SSH key per camera (Go assistant) | `assistant_test.sh` | Not yet | Never run on a real camera |
| Go assistant, daily commands | `assistant_test.sh` (fake cameras) | Partly: Linux amd64 and an earlier Windows build | See [tested platforms](assistant.md#tested-platforms) |
| Go assistant, update, restore, telnet backup and flash, SD writer, firmware chooser | `assistant_test.sh` | **Not yet** | Never run on a real camera or card |
| Native image builder (`akam image --builder native`) | reader and writer round trips | Partly: same file tree as the bash builder from a real backup; the image was mounted read-only on a real camera and 117 files read back identical. **Never flashed** | Experimental, opt-in |
| No internet at all (isolated network) | | Not yet | |

## How to run the host tests

```bash
bash tools/build/bootstrap.sh     # once: pinned toolchain (Zig, BusyBox, Go modules)
tools/ci/local_check.sh           # shellcheck, every tests/*_test.sh, Go tests, tracked-file hygiene
bash tests/readme_links_test.sh   # documentation links and `./akam` commands only
```

A single test runs on its own, for example `bash tests/swdog_test.sh`. Without the bootstrap, tests that need the
pinned BusyBox or compiler print `skip:` and are counted as skipped, not passed.

Known flaky under load: `rtsp_backchannel_test.sh` and the thread-count check of `wsd_test.sh` can fail on a busy
machine (rerun alone). `control_test.sh` and `http_srv_test.sh` use fixed local ports: **do not run two
`local_check.sh` at once**.
