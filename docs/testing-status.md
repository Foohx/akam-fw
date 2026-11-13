# Testing status: what was tested, and where

This page separates three levels of evidence, so you know what to trust:

- **Host tests**: the `tests/*.sh` scripts, run on a PC with fixtures, fake binaries and stubs. They check logic,
  parsing, limits and file formats. They say nothing about the real hardware, the real kernel or real clients.
- **Validated in RAM**: a development build copied to `/tmp` on one camera and run from there, before the final image.
- **Hardware validated**: the flashed image, running on the cameras listed in [tested hardware](tested-hardware.md).

Anything marked "not yet" has **not** been run on a camera. Older, more detailed checklists are in the
[test plan](akam_v1_test_plan.md).

## Hardware results of 2026-11-12 (maintainer records)

- **Image "v6"** (klog filter, RTSP/TCP backpressure `tx_pace`, ONVIF, watchdog, NTP, video config) flashed on two cameras (LSC Smart
  Indoor 1080p 3215672.3) and run with a home server pulling RTSP: 14.9 to 15 fps, TCP send queue 0 to 24 KB, 0 encoder errors. The klog
  filter works on hardware (syslog about 460 KB a day, about 1.5 days kept in RAM).
- **Image "v7"** (adds 20 and 25 fps) flashed on one camera: 25 fps configured; night mode correctly drops to 10 fps; 0 encoder errors.
  A RAM test delivered exactly 15, 20, 25 and 30 fps. On 2026-11-13 in daylight the flashed camera delivered exactly 25 fps (751 frames in 30 s), about 1.4 Mbit/s, 0 encoder errors and 0 skipped frames, with a picture as bright as at 15 fps.
- **MQTT**: host-tested only. **Native image builder** (`akam image --builder native`): experimental, opt-in, never flashed.
- **Assistant**: Linux amd64 partly tested on real cameras (SSH, 8091 options, `video show`, `led`, `ntp`, `talk`); update, restore, telnet
  backup and flash, the SD writer and first install were never run on hardware from the Go assistant. Windows amd64: an earlier build was
  tested on a real camera (discovery, options over SSH, talk with the PC microphone, clock offer); the new full-screen interface build is
  pending its Windows test; the SD writer and flash are not tested. macOS and arm64: built, not hardware-tested.
  Microphone capture is built in on Linux (PulseAudio or PipeWire) and on Windows (WASAPI); only macOS needs `ffmpeg`.

## Hardware results in short

**akam-01 was flashed with the image built 2026-11-11** (ONVIF, encoder-stall watchdog, Wi-Fi supervisor, opt-in NTP,
video config, quieter logs, review fixes). akam-02 is still on the 2026-11-10 image. Seen on akam-01:

- a clean boot after a power cycle; ports 22, 554 and 8091 only by default, no UDP; settings kept across the flash;
- ONVIF enabled, then validated with `tools/hw_tests/onvif_check.py`: 34 of 35 checks OK (the failure was a tool bug,
  since fixed); WS-Discovery; WSSE authentication with the camera clock unset (year 2000); imaging Set/Get with restore;
  no flash write. Answers take 25 to 75 ms, with outliers of about 1 s on weak Wi-Fi (RSSI -70 dBm);
- a second RTSP client gets 503 while one client streams (100 attempts), and the stream is unaffected.

ONVIF Device Manager v2.2.250 on Windows: camera found by WS-Discovery, login with the stream user, live video plays, imaging changes are visible live (2026-11-11).

**Hardware validated on two cameras** (firmware image built 2026-11-10, "v3 ctl"; akam-01 has since moved to the 2026-11-11 image):

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
| Only one RTSP client, 503 for a second | `rtsp_busy_test.sh` | Yes, akam-01 (image of 2026-11-11): 100 attempts while one client streamed, each extra client got 503, the stream was unaffected | Use go2rtc in front for several viewers |
| ONVIF backchannel talk (speaker) | `rtsp_backchannel_test.sh`, `talk_test.sh`, `audio_out_test.sh`, `audio_vad_test.sh`, `g711_test.sh`, `akam_talk_test.sh`, `wizard_talk_test.sh` | Yes, two cameras | Needs the video password |
| Microphone off (`audio.mic_enable=0`) with the speaker | `audio_sup_test.sh`, `wizard_audio_test.sh` | Not yet | Capture supervisor with backoff is host-tested only |
| Front LED, IR LED | `leds_test.sh`, `wizard_led_test.sh` | Yes: status on, off, auto; IR auto | |
| Automatic day/night switching | `night_logic_test.sh`, `isp_effects_test.sh`, `isp_blocks_test.sh`, `isp_block_diff_test.sh` | Yes, including the night probe and the IR back-on logic | Long runs across dusk and dawn are in the [test plan](akam_v1_test_plan.md) |
| Picture settings (brightness, contrast, saturation, sharpness) | `isp_tune_test.sh`, `imgq_test.sh`, `wizard_picture_test.sh` | In RAM, one camera: persistence across a daemon restart and a forced night to day switch | Not yet checked on the final image |
| Video config (bitrate, fps, gop) | `video_cfg_test.sh`, `video_apply_test.sh`, `wizard_video_test.sh` | In the flashed image of akam-01; not exercised on the camera yet | Applied at the next restart of the video service |
| Port 8091 metrics, read-only from the network | `control_test.sh`, `ctl_sys_test.sh`, `http_srv_test.sh`, `wizard_core_test.sh` | Yes: reads work, `SET` refused remotely | |
| Strict `SET` ranges (`ae.*`, `night.*`, `isp.*`) | `control_test.sh` | Not yet | |
| Debug mode (`./akam debug on`) | `akam_debug_test.sh`, `wizard_debug_test.sh` | Not yet on the final image | Loopback `SET` and timed network `SET` |
| ONVIF Profile S (device, media, imaging, WS-Discovery, SystemReboot) | `onvif_test.sh`, `onvif_svc_test.sh`, `onvif_media_test.sh`, `soap_test.sh`, `wsd_test.sh`, `sha1_test.sh`, `akam_onvif_test.sh`, `wizard_onvif_test.sh` | Yes, akam-01: enabled, then `tools/hw_tests/onvif_check.py` 34/35 checks OK (the one failure was a bug of the tool, since fixed); on the 2026-11-11 image b36: `onvif_check.py --rtsp` 34/34 OK including RTSP Digest DESCRIBE (200, H264 SDP) and the camera clock set (offset -1 s); WS-Discovery answers; WSSE auth works with the camera clock unset (year 2000); imaging Set/Get with restore; no flash write | ONVIF Device Manager v2.2.250 on Windows (2026-11-11, akam-01, image b36): found by WS-Discovery, login with the stream user, pages browsed, live video plays, imaging changes (brightness/contrast) visible live; ak_rtsp never restarted. Off by default |
| ONVIF check tool for a real camera (`tools/hw_tests/onvif_check.py`: Device, Media, Imaging, WS-Discovery, optional RTSP Digest DESCRIBE) | `onvif_check_test.sh` (fake ONVIF and RTSP server on 127.0.0.1) | Yes, once: most calls answer in 25-75 ms; the occasional 1 s outliers match Wi-Fi round-trip jitter (ping average 57 ms, maximum 224 ms, RSSI -70 dBm), so the slowness is the radio link, not the code; found the missing `fw.version` key; second run on b36: 34/34 OK, 18-50 ms; it also found its own bug (a second TCP connection for the Digest answer: the camera binds the nonce to the connection) | Run `python3 -I tools/hw_tests/onvif_check.py <camera-ip> --rtsp` (see [troubleshooting](troubleshooting.md#an-onvif-app-does-not-find-the-camera)) |
| Encoder-stall self-exit | `swdog_test.sh` | In the flashed image of akam-01; no real stall seen yet | The 15-minute RAM soak saw no spurious exit |
| Wrapper respawn backoff | `wrapper_backoff_test.sh` | Not yet | |
| Wi-Fi supervisor | `akam_net_test.sh` | In the flashed image of akam-01 (clean boot, no fault injected) | |
| Optional MQTT client with Home Assistant discovery (`mqtt.[ch]`, `mqtt_ha.[ch]`, `mqtt_svc.[ch]`, `akam_mqtt.sh`, `./akam mqtt`) | `mqtt_codec_test.sh` (golden bytes, remaining-length edges, malformed input, fuzz), `mqtt_ha_test.sh` (discovery documents, states, the command allow-list against hostile payloads), `mqtt_svc_test.sh` (a fake broker: dormant guarantee, CONNECT/will/discovery/state/commands, keepalive, drops, refusals with backoff, clean stop, one-shot test; ASan/UBSan), `control_test.sh`, `akam_mqtt_test.sh`, `wizard_mqtt_test.sh`, `hwcheck_test.sh`, `assistant_test.sh` | **Not yet.** Host only; no camera, no real broker | Off by default. Budget on the host build: +39 KB stripped (+23 KB without name resolution), +3 KB bss; RSS on the camera still to measure ([test plan](akam_v1_test_plan.md#mqtt-optional-needs-a-camera-and-a-broker)) |
| Opt-in NTP | `akam_ntp_test.sh`, `wizard_ntp_test.sh` | In the flashed image of akam-01; not enabled or exercised yet | Camera clock starts in 1970 without it |
| SSH key admin, Telnet and FTP off | `ssh_init_test.sh`, `lockdown_test.sh`, `hwcheck_test.sh` | Yes, two cameras | |
| Flash over SSH with read-back, clean boot | `reflash_ssh_test.sh`, `flash_guard_test.sh`, `build_firmware_test.sh`, `preflight_test.sh` | Yes, two cameras, power cycle | |
| Stack-protector build | none dedicated (compile flags; `build_firmware_test.sh` covers the image build) | Boot not yet tested | Canaries on the network parsers only |
| Log caps and `drop_caches` every 60 s | `wrapper_backoff_test.sh`, `akam_net_test.sh`, `akam_ntp_test.sh` (log caps), `build_firmware_test.sh` and `kit_test.sh` (shipped files) | In the flashed image of akam-01 ("quieter logs"); caps not measured yet | |
| Kernel log filter (`ak_rtsp klog`: LED trace dropped, repeats collapsed, priorities kept) | `klog_test.sh` (filter core with a fake clock, file-to-datagram-socket run), `leds_test.sh` (no rewrite of an unchanged LED), `hwcheck_test.sh`, `build_firmware_test.sh` and the assistant `image` tests (the `main.sh` patch) | Yes, two cameras (image v6, 2026-11-12): the filter works, syslog about 460 KB per day, about 1.5 days kept in RAM | Measure `/var/log/messages` growth per hour before and after |
| UDP listener and process audit | `hwcheck_test.sh` | Yes, akam-01 by default: ports 22, 554 and 8091 only, no UDP listener | |
| Go assistant, daily commands (experimental) | `assistant_test.sh` (fake cameras) | Partly, Linux amd64 on real cameras: SSH, 8091 options, `video show`, `led`, `ntp`, `talk`. Windows amd64: an earlier build ran on a real camera on 2026-11-12 (discovery, options over SSH, talk with the PC microphone, clock offer); the current full-screen interface build is pending its Windows test | The bash wizard is the reference |
| Go assistant phases P2 and P3 (update, restore, verify, telnet backup and flash, SD writer) | `assistant_test.sh` (fake cameras, files as cards) | **Not yet.** Update, restore, telnet backup and flash, the SD writer and first install were never run on hardware from the Go assistant | Never run against a real camera or card |
| Go assistant, cross-platform layers (phase 11): Windows SD disk layer and UAC pipe, macOS SD disk layer and `osascript` elevation, Linux SD layer, backup location and private folders, options engine, discovery, backup over SSH, squashfs reader, home screen and guided flows, licences, six-target build | `assistant_test.sh` and `go test ./...` on Linux: each OS-specific layer is tested on fake system facts, parsers and pipes (Windows and macOS code is cross-compiled and vetted, not run on those systems) | Linux amd64: partly (see the first row). Windows amd64: an earlier build tested on one camera (2026-11-12); the **SD writer, UAC flow and flash are not tested**, nor the new interface. **macOS and arm64: built, not hardware-tested** | Built for six targets (Linux, Windows, macOS, amd64 and arm64). See the [assistant hardware test plan](assistant_hardware_test_plan.md) |
| Squashfs gate for the native image builder (spec 11, 4.8) | reader and writer fixtures, round trips | Step 1 PASSED locally: the reader equals `unsquashfs` on a real stock APP dump and on an Akam image. Writer round trip on an Akam image PASSED (rewritten image about 13% larger). A stock image needs its xattrs dropped | Step 6 PASSED locally: the native builder (`akam image --builder native`) and the bash builder give the same tree (300 entries, 0 listing differences) from a real stock backup and a fresh release kit; the native squashfs is about 13% larger (2.92 MB vs 2.59 MB, Go xz encoder), well inside the 5.24 MB partition. Step 7 PASSED: the native image was loop-mounted read-only on a real camera (kernel 4.4.302) and all 117 files read back identical, no kernel error, no reboot. Not done: step 5 (QEMU) and step 8 (a flash on a spare camera with UART), so the native builder stays opt-in and experimental |

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
