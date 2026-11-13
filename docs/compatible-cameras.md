# Compatible cameras, and what you get compared with the stock firmware

This page answers two questions: **will Akam run on my camera?** and **what can it do compared with the
original (stock) firmware?** Read it before you flash anything.

In short: Akam has been run on real cameras of **one** model, the LSC Smart Indoor 1080p, article `3215672.3`
(two units, images of 2026-11-10 and 2026-11-11, both reflashed with image "v6" on 2026-11-12 and running
continuously since at 14.9 to 15 fps; one unit then moved to "v7", which adds 20 and 25 fps: 25 fps measured in
daylight, 20 fps only in a RAM test). Everything else is untested, and a camera with a different flash layout is
refused.

## Is my camera compatible?

1. Find the **article number** and the **SI code**: look at the back of the camera body, under the LSC logo, for two
   codes such as `3215672.3  SI D26228`. The [hardware guide](hardware_and_uart.md) has photos; the one in
   [images/8.jpg](../images/8.jpg) shows a `3215672.2` unit.
2. Look for the article number in Table 1 below.
3. Run `./akam`: its `backup` and `image` steps show a verdict from
   [`tools/release/supported_hardware.txt`](../tools/release/supported_hardware.txt). The verdict is decided by the
   **sha256 of your stock APP partition** (a match with a validated line continues; anything else is "untested": a
   warning and an explicit yes). The kernel line is compared and only warns. The flash layout is checked separately:
   the whole 8-line partition table (`/proc/mtd`) must match exactly, otherwise the wizard refuses. This is a
   safety net, not a guarantee.

If you are not sure, do not flash. Make the backup (it changes nothing on the camera) and report what the wizard says
(see [Report your camera](#report-your-camera)).

## Table 1: cameras

Legend: ✅ validated (an Akam image was flashed and ran on real units) | ⚠️ untested (may work, may brick) |
❌ not supported.

| Status | Camera | Article no. and SI code | Notes |
|---|---|---|---|
| ✅ validated | LSC Smart Indoor 1080p (sold at Action) | `3215672.3`, SI `D26228` | 2 units tested (`akam-01`, `akam-02`). Details below |
| ⚠️ untested | LSC Smart Indoor 1080p (sold at Action) | `3215672.2`, SI `B26101` / `C26101` | The upstream project ran its `ak_rtsp` on a unit of this model ([upstream background](upstream_background.md)); Akam's installer and image were never run on it |
| ❌ not supported | Other Anyka or Tuya cameras | any | Not expected to work: `ak_rtsp` is specific to the AK3918AV130 SoC and the GC20C3 sensor. The wizard warns and asks, and refuses a different flash layout. Older rectangular boards (AK3918EV200, GK7102) use other register maps and layouts |

**The validated model, in detail**

- **SoC:** Anyka AK3918AV130. **Image sensor:** GalaxyCore GC20C3 (2 MP).
- **Flash:** 8 MiB SPI NOR (`XM25QH64D` or a Winbond equivalent), 8 MTD partitions (MTD = the numbered flash
  partitions; [layout](flash_partition_map_and_rootfs.md)). Only the APP partition (`mtd7`, 5 242 880 bytes) is
  rewritten.
- **Stock firmware fingerprint:** Linux `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`, APP squashfs xz with
  128 KiB blocks, BusyBox 1.30.1. The exact record, with the sha256 of the stock APP, is in
  [`supported_hardware.txt`](../tools/release/supported_hardware.txt). No retail firmware version number is recorded.
- **Units tested:** 2. Image "v6" (kernel log filter, RTSP/TCP backpressure, ONVIF, watchdog, NTP, video config) on both
  since 2026-11-12; image "v7" on one of them. MQTT is host-tested only; the native image builder is experimental and
  was never flashed. Evidence: [testing status](testing-status.md).

The upstream hardware table (PCB `IPC280KG2-GNA-MAIN-V1.0`, Wi-Fi chip Altobeam ATBM6012BX, USB-C 5 V) is in
[upstream background](upstream_background.md#hardware-overview). No other camera model is listed anywhere in this
repository.

## Table 2: what you get, stock firmware versus Akam

Legend: ✅ works, tested on hardware | 🧪 not validated on the flashed image (host-tested, RAM test or experimental) |
❌ not available | ❔ unknown.

The **stock** column only repeats what this repository documents ([upstream background](upstream_background.md),
[stock capture](stock_capture.md), [install notes](akam_v1_install.md)); ❔ there means "not documented here", not
"missing". The **3215672.2** column is ❔ wherever it would need a test, because Akam was never run on that model.

| Feature | Stock firmware (Tuya) | Akam on 3215672.3 | Akam on 3215672.2 | More |
|---|---|---|---|---|
| Tuya / Smart Life app and cloud | ✅ (that is what Akam replaces) | ❌ cloud daemons and telemetry removed | ❌ by design | [upstream background](upstream_background.md#key-features) |
| Viewing away from home, phone app, push notifications | ✅ through the Tuya cloud | ❌ none; use a VPN into your network, Home Assistant or an NVR | ❌ | [security notes](using_akam.md#security-notes) |
| Works with no internet and no account | ❔ | 🧪 by design (no cloud code); isolated-network test still to do | ❔ | [README](../README.md) |
| Local RTSP stream, H.264 1080p | ❔ (a factory RTSP demo mode exists, see [upstream background](upstream_background.md#methods-and-approach)) | ✅ port 554, any path | ❔ | [watching the stream](using_akam.md#watching-the-stream) |
| Several viewers at once | ❔ | ❌ one RTSP client at a time; use go2rtc, MediaMTX or an NVR as a relay | ❌ | [troubleshooting](troubleshooting.md#only-one-viewer-works-at-a-time) |
| Stream password (Digest) | ❔ | ✅ off by default | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| ONVIF (discovery, Profile S-like; not certified) | ❔ | ✅ off by default. Check tool 34/34 on one unit, and ONVIF Device Manager (see note 1) | ❔ | [ONVIF](onvif.md) |
| Frame rate choice: 10, 15, 20, 25 fps | ❔ | ✅ 15 (default) and 25 fps measured on a camera; 20 fps only in a RAM test; 10 fps is what night mode falls back to | ❔ | [video settings](troubleshooting.md#video-settings-akam-video-bitrate-frame-rate-key-frames) |
| Bitrate and key frame interval (GOP) | ❔ | 🧪 in the flashed image, not exercised on a camera | ❔ | [video settings](troubleshooting.md#video-settings-akam-video-bitrate-frame-rate-key-frames) |
| Resolution, substream, flip/mirror | ❔ | ❌ 1080p, one stream, no flip | ❌ | [video settings](troubleshooting.md#video-settings-akam-video-bitrate-frame-rate-key-frames) |
| Snapshots (still pictures) | ❔ | ❌ not implemented (grab a frame from the stream) | ❌ | n/a |
| Picture settings (brightness, contrast, saturation, sharpness, white balance) | ❔ | 🧪 checked in RAM on one camera, not on the final image | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| Picture quality in dim light | ✅ better (per [upstream background](upstream_background.md#image-quality)) | ✅ works, but grainier than stock; improvement planned | ❔ | [upstream background](upstream_background.md#image-quality) |
| Automatic day/night switching | ✅ (the stock switch was recorded in [stock capture](stock_capture.md)) | ✅ including the night probe | ❔ | [camera and audio pipeline](camera_and_audio_pipeline.md) |
| Infrared LEDs | ❔ | ✅ auto; `ir off` 🧪 | ❔ | [LED settings](akam_v1_install.md#led-settings) |
| Status LED control (on, off, status light) | ❔ | ✅ | ❔ | [LED settings](akam_v1_install.md#led-settings) |
| Microphone audio in the stream | ❔ | ✅ L16 8 kHz; `mic off` 🧪 | ❔ | [two-way audio](two_way_audio.md) |
| Two-way audio through the speaker | ❔ | `talk` ✅, `say` 🧪; needs the video password | ❔ | [two-way audio](two_way_audio.md) |
| Speaker volume and beep | ❔ | 🧪 | ❔ | [two-way audio](two_way_audio.md) |
| MQTT and Home Assistant discovery | ❔ | 🧪 host-tested only, off by default | ❔ | [MQTT](mqtt.md) |
| Clock by NTP | ❔ | ✅ opt-in; enabled on both cameras, synchronised, offset 0 after a resync (2026-11-12). The clock starts in 1970 without it | ❔ | [troubleshooting](troubleshooting.md#the-camera-clock-is-wrong-1970-or-the-build-date) |
| Camera name announced by DHCP (what your router lists) | ❔ | ✅ `./akam hostname` | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| Metrics on port 8091 (read-only from the network) | ❔ | ✅ | ❔ | [install notes](akam_v1_install.md#4-the-8091-control-port) |
| Debug mode (`./akam debug`) | ❔ | 🧪 not yet on the final image | ❔ | [install notes](akam_v1_install.md#4-the-8091-control-port) |
| SSH administration with a key | ❔ | ✅ key only, no password | ❔ | [SSH and lockdown](akam_v1_ssh_and_lockdown.md) |
| Telnet and FTP closed | ❔ (launch lines commented out in the audited dump; not port-scanned) | ✅ closed, checked on two cameras; only the maintenance card opens telnet | ❔ | [SSH and lockdown](akam_v1_ssh_and_lockdown.md) |
| Encoder watchdog and Wi-Fi supervisor | ❔ | 🧪 in the image, never triggered on hardware | ❔ | [testing status](testing-status.md) |
| Motion detection | ❔ | ❌ not implemented | ❌ | n/a |
| Recording to microSD | ❔ | ❌ not implemented (record from an NVR, go2rtc or Frigate) | ❌ | [watching the stream](using_akam.md#watching-the-stream) |
| Vendor OTA (over-the-air) updates | ❔ | ❌ no network update | ❌ | [security notes](using_akam.md#security-notes) |
| Change the Wi-Fi network | ❔ | 🧪 no command yet: edit `/etc/config/wpa_supplicant.conf` over SSH (by hand, untested); the card route only creates Wi-Fi settings on a camera that has none | ❔ | [troubleshooting](troubleshooting.md#the-camera-has-no-network-after-removing-the-card) |
| Full 8-partition backup of the camera | n/a | ✅ `./akam backup` (read only) | ❔ | [wizard guide](wizard_guide.md) |
| First install without opening the camera | n/a | ✅ microSD maintenance card plus the wizard (Linux PC) | ❔ | [wizard guide](wizard_guide.md) |
| First install from Windows or macOS | n/a | 🧪 never done on a real camera; use Linux | ❔ | [installing the binary](assistant_install.md) |
| Updates without opening the camera (Akam to Akam, over SSH) | n/a | ✅ `./akam update`, read-back verified | ❔ | [update, name, password](using_akam.md#update-change-the-name-add-a-video-password) |
| Go back to the stock firmware | n/a | 🧪 `./akam restore` from your backup; never rehearsed on a camera, no guarantee | ❔ | [flash and restore](akam_v1_flash_and_restore.md#5-restore) |

Notes:

1. ONVIF Device Manager (ODM) on Windows: [testing status](testing-status.md) records a validation on `akam-01`
   (found by discovery, login, live video, imaging changes, 2026-11-11). Other NVRs are not tried.
2. Akam serves one RTSP client at a time; use go2rtc or MediaMTX in front for several viewers.

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
