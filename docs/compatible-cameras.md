# Compatible cameras, and what you get compared with the stock firmware

This page answers two questions: **will Akam run on my camera?** and **what can it do compared with the
original (stock) firmware?** Read it before you flash anything.

In short: Akam has been run on real cameras of **one** model, the LSC Smart Indoor 1080p, article `3215672.3`.
Everything else is untested, and a camera with a different flash layout is refused.

## Is my camera compatible?

1. Find the **article number** and the **board label** printed on the camera's product label (on the camera and on
   the retail box; the label is visible on the photos of the [hardware guide](hardware_and_uart.md)). The article
   number looks like `3215672.3` and the label code like `SI D26228`.
2. Look for that article number in Table 1 below.
3. Run `./akam`: its `backup` and `image` steps compare the camera's kernel, flash layout and stock APP partition with
   [`tools/release/supported_hardware.txt`](../tools/release/supported_hardware.txt), and show a verdict. A match
   continues; anything else shows a warning and needs an explicit yes; a different flash layout is refused. That is a
   safety net, not a guarantee.

If you are not sure, do not flash. Make the backup (it changes nothing on the camera) and report what the wizard says
(see [Report your camera](#report-your-camera)).

## Table 1: cameras

Legend: ✅ validated (an Akam image was flashed and ran on real units) | ⚠️ untested (may work, may brick) | ❌ refused.

| Status | Product | Article no. | Board label | SoC | Image sensor | Flash | Stock firmware fingerprint | Units tested | Notes |
|---|---|---|---|---|---|---|---|---|---|
| ✅ validated | LSC Smart Indoor 1080p (sold at Action) | `3215672.3` | SI `D26228` | Anyka AK3918AV130 | GalaxyCore GC20C3 (2 MP) | 8 MiB SPI NOR, 8 partitions; only APP (`mtd7`, 5 242 880 bytes) is rewritten | Linux `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`, APP squashfs xz 128 KiB blocks, BusyBox 1.30.1 ([`supported_hardware.txt`](../tools/release/supported_hardware.txt)) | 2 (`akam-01`, `akam-02`) | Image "v6" flashed on both on 2026-11-12 and running 24/7 with a home server pulling the RTSP stream (14.9 to 15 fps, 0 encoder errors). Image "v7" (adds 20 and 25 fps) on one unit. Details: [testing status](testing-status.md) |
| ⚠️ untested | LSC Smart Indoor 1080p (sold at Action) | `3215672.2` | SI `B26101` / `C26101` | Anyka AK3918AV130 (per the upstream project) | GalaxyCore GC20C3 (per the upstream project) | 8 MiB SPI NOR (per the upstream project) | not recorded | 0 | The upstream project's own unit (`SI B26101`) ran its `ak_rtsp` stream server, but Akam's installer and image were never run on it. Source: [upstream background](upstream_background.md) |
| ⚠️ untested | Other Anyka or Tuya cameras | any | any | any | any | any | unknown | 0 | May use a different kernel, APP or flash layout. Older rectangular boards use other SoCs (AK3918EV200, GK7102) with other register maps: not supported. Back up first |
| ❌ refused | Any camera whose `/proc/mtd` is not 8 partitions with APP = `00500000` | any | any | any | any | different layout | different | 0 | The wizard stops: the scripts hard-code the layout and a wrong write can brick the camera |

The upstream hardware table (PCB `IPC280KG2-GNA-MAIN-V1.0`, Wi-Fi chip Altobeam ATBM6012BX, USB-C 5 V) is in
[upstream background](upstream_background.md#hardware-overview). No other camera model is listed anywhere in this
repository.

## Table 2: what you get, stock firmware versus Akam

Legend: ✅ works, tested on hardware | 🧪 available, host-tested only or experimental | ❌ not available | ❔ unknown.

The **stock** column only repeats what this repository documents ([upstream background](upstream_background.md),
[stock capture](stock_capture.md), [install notes](akam_v1_install.md)); ❔ there means "not documented here", not
"missing". The **3215672.2** column is ❔ wherever it would need a test, because Akam was never run on that model.

| Feature | Stock firmware (Tuya) | Akam on 3215672.3 | Akam on 3215672.2 | More |
|---|---|---|---|---|
| Tuya / Smart Life app and cloud | ✅ (that is what Akam replaces) | ❌ cloud daemons and telemetry removed | ❌ by design | [upstream background](upstream_background.md#key-features) |
| Works with no internet and no account | ❔ | ✅ | ❔ | [README](../README.md) |
| Local RTSP stream, H.264 1080p | ❔ (a factory RTSP demo mode exists, see [upstream background](upstream_background.md#methods-and-approach)) | ✅ port 554, any path, one client at a time | ❔ | [watching the stream](using_akam.md#watching-the-stream) |
| Stream password (Digest) | ❔ | ✅ off by default | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| ONVIF (discovery, Profile S-like; not certified) | ❔ | ✅ off by default. Check tool 34/34 on one unit, and ONVIF Device Manager (see note 1) | ❔ | [ONVIF](onvif.md) |
| Frame rate choice: 10, 15, 20, 25 fps | ❔ | ✅ 15 (default) and 25 fps measured on a camera; 20 fps only in a RAM test; 10 fps is what night mode falls back to | ❔ | [video settings](troubleshooting.md#video-settings-akam-video-bitrate-frame-rate-key-frames) |
| Bitrate and key frame interval (GOP) | ❔ | 🧪 in the flashed image, not exercised on a camera | ❔ | [video settings](troubleshooting.md#video-settings-akam-video-bitrate-frame-rate-key-frames) |
| Picture settings (brightness, contrast, saturation, sharpness, white balance) | ❔ | 🧪 checked in RAM on one camera, not on the final image | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| Automatic day/night switching | ✅ (the stock switch was recorded in [stock capture](stock_capture.md)) | ✅ including the night probe | ❔ | [camera and audio pipeline](camera_and_audio_pipeline.md) |
| Infrared LEDs auto or off | ❔ | ✅ | ❔ | [LED settings](akam_v1_install.md#led-settings) |
| Status LED control (on, off, status light) | ❔ | ✅ | ❔ | [LED settings](akam_v1_install.md#led-settings) |
| Microphone audio in the stream | ❔ | ✅ L16 8 kHz; `mic off` is 🧪 | ❔ | [two-way audio](two_way_audio.md) |
| Two-way audio (talk through the speaker, `talk`, `say`) | ❔ | ✅ `talk` through the ONVIF backchannel; needs the video password | ❔ | [two-way audio](two_way_audio.md) |
| MQTT and Home Assistant discovery | ❔ | 🧪 host-tested only, off by default | ❔ | [MQTT](mqtt.md) |
| Clock by NTP | ❔ | 🧪 opt-in, never enabled on a camera; the clock starts in 1970 without it | ❔ | [troubleshooting](troubleshooting.md#the-camera-clock-is-wrong-1970-or-the-build-date) |
| Metrics on port 8091 (read-only from the network) | ❔ | ✅ | ❔ | [install notes](akam_v1_install.md#4-the-8091-control-port) |
| SSH administration with a key | ❔ | ✅ key only, no password | ❔ | [SSH and lockdown](akam_v1_ssh_and_lockdown.md) |
| Telnet and FTP open | ✅ (the stock scripts open Telnet 23 and FTP 21) | ❌ closed on purpose (only the maintenance card opens telnet) | ❌ by design | [SSH and lockdown](akam_v1_ssh_and_lockdown.md) |
| Motion detection | ❔ | ❌ not implemented | ❌ | n/a |
| Recording to microSD | ❔ | ❌ not implemented (record from an NVR, go2rtc or Frigate) | ❌ | [watching the stream](using_akam.md#watching-the-stream) |
| Vendor OTA (over-the-air) updates | ❔ | ❌ no network update | ❌ | [security notes](using_akam.md#security-notes) |
| Change the Wi-Fi network | ❔ | ✅ through the microSD card route (the wizard saves it on the camera) | ❔ | [troubleshooting](troubleshooting.md#the-camera-has-no-network-after-removing-the-card) |
| First install without opening the camera | n/a | ✅ microSD maintenance card plus the wizard | ❔ | [wizard guide](wizard_guide.md) |
| Updates without opening the camera (Akam to Akam, over SSH) | n/a | ✅ `./akam update`, read-back verified | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |

Notes:

1. ONVIF Device Manager (ODM) on Windows: [testing status](testing-status.md) records a validation on `akam-01`
   (found by discovery, login, live video, imaging changes, 2026-11-11). Other NVRs are not tried.
2. Akam's image is grainier than the stock firmware's in dim light; an improvement is planned
   ([upstream background](upstream_background.md#image-quality)).
3. Akam serves one RTSP client at a time; use go2rtc or MediaMTX in front for several viewers.

## Report your camera

Whether it works or not, a report helps. Please give:

- the article number and the code printed on the label (for example `3215672.3`, `SI D26228`);
- the SoC, from the camera: `cat /proc/cpuinfo` (over SSH on an Akam camera, or on the maintenance shell);
- the partition table: `cat /proc/mtd`, and the kernel line: `uname -a`;
- the wizard's hardware verdict, shown by `./akam backup` and `./akam image`, and the output of `./akam status`;
- on a camera that already runs Akam, the output of the read-only check script, which changes nothing:
  `ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh` (`./akam verify` runs it for you);
- what you did, what you saw, and the end of the log in `~/.local/state/akam/` (redacted).

Never attach your backup, images or keys, and remove Wi-Fi names, passwords and MAC addresses from anything you post.
**Always take a full backup first** and keep it on two media ([backup](akam_v1_backup.md)).
