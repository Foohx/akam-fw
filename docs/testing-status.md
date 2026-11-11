# Testing status: what was tested, and where

This page separates three levels of evidence, so you know what to trust:

- **Host tests**: the `tests/*.sh` scripts, run on a PC with fixtures, fake binaries and stubs. They check logic,
  parsing, limits and file formats. They say nothing about the real hardware, the real kernel or real clients.
- **Validated in RAM**: a development build copied to `/tmp` on one camera and run from there, before the final image.
- **Hardware validated**: the flashed image, running on the cameras listed in [tested hardware](tested-hardware.md).

Anything marked "not yet" has **not** been run on a camera. Older, more detailed checklists are in the
[test plan](akam_v1_test_plan.md).

## Hardware results in short

**Hardware validated on two cameras** (firmware image built 2026-11-10, "v3 ctl"):

- RTSP H.264 1080p 15 fps video plus microphone, with VLC, ffmpeg and go2rtc over TCP;
- RTSP Digest authentication;
- ONVIF RTSP backchannel talk (speaker);
- status LED on, off and auto, and the IR LED auto mode;
- automatic day/night switching, including the night probe with the IR back-on logic;
- port 8091 read-only from the network with its metrics (`sys.*`, `net.*`, `video.*`, `rtsp.*`); `SET` refused
  remotely;
- SSH key administration, with Telnet and FTP disabled;
- flash over SSH with read-back verification, and a clean boot after a power cycle on both cameras.

**Validated in RAM on one camera** (development build run from `/tmp`, before the final image):

- a 15-minute soak: same process id throughout, no encoder-stall self-exit, load about 0.1;
- picture settings persistence: a saturation value survived a daemon restart and a forced night to day switch.

## Matrix

| Feature | Host tests | Hardware validated | Notes |
|---|---|---|---|
| RTSP H.264 1080p 15 fps video and microphone | `rtsp_sdp_test.sh`, `rtsp_in_test.sh`, `rtp_in_test.sh`, `rtp_clock_test.sh`, `h264_params_test.sh`, `jitter_test.sh`, `rtsp_idle_test.sh`, `net_tx_test.sh` | Yes, two cameras (VLC, ffmpeg, go2rtc over TCP) | VLC over UDP can freeze behind a client firewall: use TCP |
| RTSP Digest authentication | `rtsp_auth_test.sh`, `auth_thread_test.sh`, `rtsp_auth_cmd_test.sh`, `cred_test.sh`, `wizard_rtsp_auth_test.sh` | Yes, two cameras | Off by default |
| Only one RTSP client, 503 for a second | `rtsp_busy_test.sh` | Not yet | Use go2rtc in front for several viewers |
| ONVIF backchannel talk (speaker) | `rtsp_backchannel_test.sh`, `talk_test.sh`, `audio_out_test.sh`, `audio_vad_test.sh`, `g711_test.sh`, `akam_talk_test.sh`, `wizard_talk_test.sh` | Yes, two cameras | Needs the video password |
| Microphone off (`audio.mic_enable=0`) with the speaker | `audio_sup_test.sh`, `wizard_audio_test.sh` | Not yet | Capture supervisor with backoff is host-tested only |
| Front LED, IR LED | `leds_test.sh`, `wizard_led_test.sh` | Yes: status on, off, auto; IR auto | |
| Automatic day/night switching | `night_logic_test.sh`, `isp_effects_test.sh`, `isp_blocks_test.sh`, `isp_block_diff_test.sh` | Yes, including the night probe and the IR back-on logic | Long runs across dusk and dawn are in the [test plan](akam_v1_test_plan.md) |
| Picture settings (brightness, contrast, saturation, sharpness) | `isp_tune_test.sh`, `imgq_test.sh`, `wizard_picture_test.sh` | In RAM, one camera: persistence across a daemon restart and a forced night to day switch | Not yet checked on the final image |
| Video config (bitrate, fps, gop) | `video_cfg_test.sh`, `video_apply_test.sh`, `wizard_video_test.sh` | Not yet | Applied at the next restart of the video service |
| Port 8091 metrics, read-only from the network | `control_test.sh`, `ctl_sys_test.sh`, `http_srv_test.sh`, `wizard_core_test.sh` | Yes: reads work, `SET` refused remotely | |
| Strict `SET` ranges (`ae.*`, `night.*`, `isp.*`) | `control_test.sh` | Not yet | |
| Debug mode (`./akam debug on`) | `akam_debug_test.sh`, `wizard_debug_test.sh` | Not yet on the final image | Loopback `SET` and timed network `SET` |
| ONVIF Profile S (device, media, imaging, WS-Discovery, SystemReboot) | `onvif_test.sh`, `onvif_svc_test.sh`, `onvif_media_test.sh`, `soap_test.sh`, `wsd_test.sh`, `sha1_test.sh`, `akam_onvif_test.sh`, `wizard_onvif_test.sh` | Not yet | First target client: ONVIF Device Manager. Off by default |
| ONVIF check tool for a real camera (`tools/hw_tests/onvif_check.py`: Device, Media, Imaging, WS-Discovery, optional RTSP Digest DESCRIBE) | `onvif_check_test.sh` (fake ONVIF and RTSP server on 127.0.0.1) | Yes, once: most calls answer in 25-75 ms; the occasional 1 s outliers match Wi-Fi round-trip jitter (ping average 57 ms, maximum 224 ms, RSSI -70 dBm), so the slowness is the radio link, not the code; found the missing `fw.version` key | Run `python3 -I tools/hw_tests/onvif_check.py <camera-ip> --rtsp` (see [troubleshooting](troubleshooting.md#an-onvif-app-does-not-find-the-camera)) |
| Encoder-stall self-exit | `swdog_test.sh` | Not yet on a real stall | The 15-minute RAM soak saw no spurious exit |
| Wrapper respawn backoff | `wrapper_backoff_test.sh` | Not yet | |
| Wi-Fi supervisor | `akam_net_test.sh` | Not yet | |
| Opt-in NTP | `akam_ntp_test.sh`, `wizard_ntp_test.sh` | Not yet | Camera clock starts in 1970 without it |
| SSH key admin, Telnet and FTP off | `ssh_init_test.sh`, `lockdown_test.sh`, `hwcheck_test.sh` | Yes, two cameras | |
| Flash over SSH with read-back, clean boot | `reflash_ssh_test.sh`, `flash_guard_test.sh`, `build_firmware_test.sh`, `preflight_test.sh` | Yes, two cameras, power cycle | |
| Stack-protector build | none dedicated (compile flags; `build_firmware_test.sh` covers the image build) | Boot not yet tested | Canaries on the network parsers only |
| Log caps and `drop_caches` every 60 s | `wrapper_backoff_test.sh`, `akam_net_test.sh`, `akam_ntp_test.sh` (log caps), `build_firmware_test.sh` and `kit_test.sh` (shipped files) | Not yet | |
| UDP listener and process audit | `hwcheck_test.sh` | Not yet | |
| Go assistant, daily commands (experimental) | `assistant_test.sh` (fake cameras) | Not yet | The bash wizard is the reference |
| Go assistant phases P2 and P3 (update, restore, verify, telnet backup and flash, SD writer) | `assistant_test.sh` (fake cameras, files as cards) | Not yet | Never run against a real camera or card |

## How to run the host tests

```bash
bash tools/build/bootstrap.sh     # once: pinned toolchain (Zig, BusyBox, Go modules)
tools/ci/local_check.sh           # shellcheck, every tests/*_test.sh, tracked-file hygiene
bash tests/readme_links_test.sh   # documentation links and `./akam` commands only
```

A single test runs on its own, for example `bash tests/swdog_test.sh`. Without the bootstrap, tests that need the
pinned BusyBox or compiler print `skip:` and are counted as skipped, not passed.

## Known flaky tests under parallel load

- `rtsp_backchannel_test.sh` and the thread-count check of `wsd_test.sh` can fail when the machine is busy; rerun
  them alone before suspecting the code.
- `control_test.sh` and `http_srv_test.sh` use fixed local ports: **do not run two `local_check.sh` at once** (nor
  these tests in parallel), or they will fight over the ports.
