# Akam — firmware for LSC 1080p cameras

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20L16%20Audio-red.svg)]()

Akam replaces the Tuya cloud software of the **LSC Smart Indoor 1080p camera** (Action) with a small
local firmware:

- a **local-only RTSP stream** (H.264 1080p + audio) on port 554: no cloud, no account, no app;
- **SSH** (key only) for maintenance; Telnet and FTP stay off;
- only the APP partition is rewritten, from **your own** backup. U-Boot, kernel and ROOTFS are never
  written.

> **Flashing can brick your camera, and restoring a backup is not guaranteed.** Read the
> [disclaimer](#important-disclaimer-and-risks). The wizard asks for a full backup first.

Akam is a fork of
[FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware);
the reverse-engineering behind `ak_rtsp` is the upstream author's (see [Credits](#credits--acknowledgements)).

## Status

**Validated on a real camera on 2026-11-08:**

- boots without a microSD card; Wi-Fi connects by itself;
- H.264 1080p with a fast start, plus an audio track in the RTSP stream;
- the IR night image is delivered;
- port 8091 `GET` / `SET`;
- SSH as root with an ed25519 key, also after a power cut;
- only ports 22, 554 and 8091 listen; Telnet and FTP are off; the Tuya client is gone;
- about 1% CPU with a viewer connected.

**Still to check:** microphone audibility, day/night stability over a long run, Telnet staying off
after a Wi-Fi reconnect. **Known:** the image is grainier than the stock firmware's (improvement
planned for V1.1). Only one unit was tested: please report what you see on yours. Details:
[test plan](docs/akam_v1_test_plan.md).

## Supported hardware

| Camera | Label | Status |
|---|---|---|
| LSC Smart Indoor 1080p `3215672.3` | SI `D26228`, kernel `4.4.302-cip94`, APP compressed with xz | **validated** |
| LSC Smart Indoor 1080p `3215672.2` | SI `B26101` / `C26101` | untested (works with upstream, not tried with Akam) |
| anything else (other SoC such as AK3918EV200 or GK7102, other kernel or APP) | | not supported |

**Check your camera:** read the label under the camera (model `3215672.x` and the `SI ...` code). The
wizard also compares the camera's kernel, flash layout and APP with the list in
`tools/release/supported_hardware.txt`: a match continues, anything else shows a warning and needs a
typed confirmation, and a different flash layout is refused.

## What you need

- the camera, and a 2.4 GHz Wi-Fi network it can join;
- a **microSD card** and a **USB card reader** (cheap readers sometimes fail, see
  [troubleshooting](docs/troubleshooting.md#no-medium-found-with-a-usb-card-reader));
- a **Linux PC**: Arch, Debian 12 or Ubuntu 24.04 (all three tested in containers). macOS is not
  supported yet;
- a UART adapter is optional but recommended: it is the safety net if something goes wrong
  ([UART guide](docs/hardware_and_uart.md)).

No compiling is needed: the default path uses a **prebuilt kit**.

## Quick start with the wizard

1. Get Akam: clone the repository (`git clone https://github.com/Foohx/akam-fw.git`) and enter it, or
   unpack the release archive.
2. Check your computer (changes nothing; prints the install command for anything missing):
   ```bash
   ./akam doctor
   ```
3. Run the guided menu. It shows every step as done / todo / blocked and resumes where you stopped:
   ```bash
   ./akam
   ```
4. Follow the prompts, one step after the other. `./akam help` lists every command, and `--dry-run`
   rehearses without changing anything.

## What the wizard does

| Step | What happens | Risk |
|---|---|---|
| `doctor` | checks the tools on your PC | none |
| `get-kit` | downloads the prebuilt kit, verifies checksum and signature (`./akam build` builds from source instead) | none |
| `prepare-backup-sd` | writes the backup toolkit to a microSD card | erases that card |
| `find-camera` | locates the camera on your network | none |
| `backup` | copies the 8 flash partitions twice, compares them, stores them on your PC | none (read only) |
| `image` | builds the Akam image from your backup and shows the hardware verdict | none |
| `prepare-flash-sd` | writes the image, your SSH key and the Wi-Fi settings to a card | erases that card |
| `flash` | writes the APP partition; you type `YES` yourself | **can brick the camera** |
| `verify` | checks over SSH that the camera behaves as expected | none |
| `wipe-sd` | erases a card that holds your Wi-Fi password | erases that card |

The wizard never runs `sudo` itself, shows every command before running it, and never stores your
Wi-Fi password. Erasing a card needs a typed `ERASE <device>`. Keep your backup (two copies, on two
media) and never publish it: it contains your Wi-Fi credentials.

Prefer to do it by hand, or want to understand each step? See the
[manual procedure](docs/akam_v1_install.md). To compile everything yourself, see the
[build guide](docs/akam_v1_build.md). Something went wrong? See [troubleshooting](docs/troubleshooting.md).

## Watching the stream

The stream is at `rtsp://<camera-ip>:554/` (any path works).

```bash
ffplay -rtsp_transport tcp rtsp://<camera-ip>:554/
```

- **One client at a time.** To serve several viewers, put [go2rtc](https://github.com/AlexxIT/go2rtc)
  or [MediaMTX](https://github.com/bluenviron/mediamtx) in front of the camera.
- **VLC:** on Arch install `vlc-plugin-live555` and `vlc-plugin-ffmpeg`. If hardware decoding fails,
  run `vlc --rtsp-tcp --avcodec-hw=none rtsp://<camera-ip>:554/`.
- Other problems: [troubleshooting](docs/troubleshooting.md).

## Control port 8091

A text protocol for exposure, day/night and image settings (`LIST`, `GET`, `SET`):

```bash
printf 'LIST\n' | nc -w 2 <camera-ip> 8091
printf 'GET <name>\n' | nc -w 2 <camera-ip> 8091
printf 'SET <name> <value>\n' | nc -w 2 <camera-ip> 8091
```

Settings are **volatile**: they are lost at the next boot. There is no password, so keep the port on
your LAN (see [Security notes](#security-notes)). More in the
[overview](docs/akam_v1_install.md#4-the-8091-control-port).

## Updating

The wizard has no separate update step: flash again with the same steps, or reflash over SSH without
UART with `tools/flash_scripts/reflash_over_ssh.sh`. Both are described in the
[flash guide](docs/akam_v1_flash_and_restore.md#reflashing-over-ssh-without-uart-ram-only-residual-risk).
The over-SSH route runs from RAM and keeps a small residual risk.

## Recovery

- **Maintenance card:** put an empty file named `AKAM_MAINTENANCE` at the root of a microSD card and
  reboot; the card's scripts are allowed to run again
  ([details](docs/akam_v1_ssh_and_lockdown.md#7-maintenance-flag)).
- **UART:** U-Boot stays reachable, so a bad APP can be reflashed from the serial console
  ([restore](docs/akam_v1_flash_and_restore.md#5-restore)).
- **Backup:** restore your stock APP from your own backup. Restore is not guaranteed to work.

## Security notes

- **Never expose ports 554, 8091 or 22 to the Internet:** no port forwarding, UPnP or DMZ. Use a VPN
  into your LAN for remote access. See [network hardening](docs/akam_v1_install.md#5-network-hardening).
- The flash card holds your **Wi-Fi password in clear text**: erase it when done with
  `./akam wipe-sd`.
- Treat a card with `AKAM_MAINTENANCE` as root access to the camera.
- Never publish your backup, your built images or your keys.

## Credits and license

MIT, see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Upstream author
FluxAlchemist, SmolRTSP, Dropbear, 4ddict and others: see [Credits](#credits--acknowledgements).

---

## What Akam V1 contains

| Component | What it does | Where |
|---|---|---|
| `ak_rtsp` | RTSP/RTP H.264 + audio server, 8091 tuning port (functionally upstream) | `src/ak_rtsp/` |
| Native Linux build | Pinned Zig 0.14.1 + pinned SmolRTSP, `make` and `bash` only, no Windows | `tools/build/` |
| Dropbear 2026.94 | Static multi-call binary, ed25519 public-key login only | `tools/dropbear/` |
| SSH boot script | Starts Dropbear in the background, host key and client keys persist in CONFIG | `tools/firmware_patch_templates/akam_ssh.sh` |
| Telnet/FTP lockdown | Build-time patch of `service.sh` + runtime safety-net script | `tools/firmware_patch_templates/` |
| Image builder | Patches **your own** APP dump, repacks, checks, pads to 5 242 880 bytes | `src/ak_rtsp/build_firmware.sh` |
| Post-flash check | Read-only checklist script, run over SSH | `tools/hw_tests/akam_hwcheck.sh` |

---

## Important disclaimer and risks

> **FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.**
>
> - **Experimental codebase:** upstream is the public release of a larger private reverse-engineering
>   project, organised with AI assistance. Akam's additions were written with AI assistance and have
>   been validated on a single camera so far.
> - **Risk of permanent bricking:** writing to the SPI NOR flash can brick the camera. In the worst case
>   only desoldering the flash chip and using an external SPI programmer (for example a CH341A) will
>   recover it. **Restoring your backup is not guaranteed to work** (see
>   [flash and restore](docs/akam_v1_flash_and_restore.md)).
> - **No liability:** the authors and contributors accept zero liability for damaged hardware, bricked
>   cameras, data loss or voided warranties.
> - **Always make and verify a complete 8-partition backup first** ([backup guide](docs/akam_v1_backup.md)).
>   Never publish it: it contains your Wi-Fi credentials and Tuya identifiers.
> - **Always read the flash verification output before disconnecting power.** If flashing reports
>   errors, do not power cycle; flash your stock backup immediately.

---

## Key Features

- **100% Cloud-Free (De-Tuyafied):** Tuya cloud daemons, MQTT beacons, and telemetry binaries are completely eliminated.
- **Native SmolRTSP Streaming Engine:** Lightweight standalone C server (`ak_rtsp`, ~126 KB) built on [OpenIPC's SmolRTSP](https://github.com/OpenIPC/smolrtsp) library, interfacing directly with the Anyka hardware ISP and H.264 encoder.
- **1080p High-Quality Video:** Full 1920x1080 resolution at 15–25 FPS with calibrated ISP parameters (color correction matrix, white balance gains, noise reduction, and VBR+ rate control).
- **Synchronized Audio:** Captures from on-board microphone via `/dev/pcmC0D0c` streamed over RTP (L16 / 8000 Hz) with RTCP Sender Reports for jitter-free playback in VLC and NVRs.
- **IR-Cut & Night Mode:** Dual GPIO solenoid pulse control for daytime IR-cut filtration and night vision illumination.
- **Hardened MTD Flasher (`flash_tool`):** Chunked 4KB write/verify/retry flasher that avoids the SPI-NOR DMA corruption bugs common when writing flash over active network/camera drivers.
- **Hardware Toolkit:** Includes an automated ESP32 U-Boot interceptor bridge and a high-performance Windows .NET 9 WinUI serial console.

---

## Image Quality (upstream status)

> ℹ️ **Image Quality Notice:**  
> The custom firmware is fully functional and stable for continuous 24/7 streaming. However, **the dynamic image tuning (contrast, wide-dynamic range curves, and auto-exposure adjustments) is still somewhat behind the stock Tuya firmware's proprietary closed-source tuning**.  
> We have calibrated the static white balance gains (`R=460, G=256, B=504`) to fix the raw sensor blue tint and tuned VBR+ rate control to minimize macroblocking, but there is ample room for future community improvement and tuning of the 24-module Anyka ISP pipeline! Pull requests and ISP tuning experiments are very welcome.

| | Stock Tuya (`anyka_ipc`) | This Firmware (`ak_rtsp`) |
|---|:---:|:---:|
| **Static scene** | ![Stock stable](images/video_out/anyka_stable.png) | ![Custom stable](images/video_out/ours_stable.png) |
| **Motion** | ![Stock movement](images/video_out/anyka_movement.png) | ![Custom movement](images/video_out/ours_movement.png) |

### Known Limitations

- **No dynamic auto white balance (AWB).** White balance uses fixed, hand-calibrated gains
  (see above) rather than tracking scene color temperature dynamically. In practice, the stock
  Tuya firmware doesn't appear to do dynamic AWB either in the scenes tested so far, so this may
  not be a real functional gap — but it's worth knowing if your lighting varies a lot.
- **Two-way audio (speaker output) is not supported.** Microphone capture → RTP streaming
  (camera-to-viewer audio) is fully working, but the reverse direction (playback through the
  camera's on-board speaker, e.g. for NVR "talk" features) is not implemented. Out of scope for
  now — may be revisited in the future.

---

## Hardware Overview

| Component | Specification |
|---|---|
| **Retail Model** | LSC Smart Connect Smart Indoor IP Camera 1080p HD (Action) |
| **Model / SKU** | `3215672.2` / `SI B26101` (upstream, tested by the upstream author). Akam target: `3215672.3`, SI `D26228` expected on the label |
| **PCB Revision** | `IPC280KG2-GNA-MAIN-V1.0` (Circular PCB, 2025-08-28) |
| **SoC** | Anyka AK3918AV130 (ARM926EJ-S @ 400 MHz) |
| **Image Sensor** | GalaxyCore GC20C3 (2MP 1/2.9" CMOS, I2C bus) |
| **Flash Memory** | 8 MiB SPI NOR (`XM25QH64D` / Winbond SOIC-8) |
| **Wireless** | Altobeam ATBM6012BX / ATBM6x3x USB WiFi |
| **Power** | USB-C 5V / 1A |

> ℹ️ **NOTE:** The table above describes the upstream test unit. Akam runs on model `3215672.3`; check that your own unit matches (label, `/proc/mtd`, see the [backup guide](docs/akam_v1_backup.md)) before flashing. This repository is tailored for **Revision 2** (circular PCB, Anyka AK3918AV130). Older revisions with rectangular boards used different SoCs (AK3918EV200 or GK7102) which have different register maps and flash partitioning.

---

## RTSP Stream URLs

Once booted into the custom firmware, the camera serves RTSP on standard port 554.

The custom `ak_rtsp` server is **path-agnostic** (it serves the 1080p video + audio stream on any requested path or root `/`):

| Format | RTSP URL Example | Notes |
|---|---|---|
| **Root (Simplest)** | `rtsp://<camera-ip>:554/` | 1920x1080 H.264 + Synchronized L16 Audio |
| **Named Path (Optional)** | `rtsp://<camera-ip>:554/live` or `/main_ch` | Any sub-path works interchangeably |

*(Note: `rtsp://<camera-ip>:554/main_ch` was the legacy path used by the original Tuya firmware, which continues to work seamlessly).*

---
## Repository Structure

```
.
├── docs/
│   ├── troubleshooting.md               # Real problems we hit and their fixes
│   ├── akam_v1_install.md               # Akam V1 overview, services, network hardening, licenses
│   ├── akam_v1_backup.md                # 8-partition backup and hardware identification (do this first)
│   ├── akam_v1_build.md                 # Host tools, builds, image from your own dump
│   ├── akam_v1_ssh_and_lockdown.md      # SSH key provisioning, Telnet/FTP lockdown, maintenance flag
│   ├── akam_v1_flash_and_restore.md     # Flashing procedure, verification, restore, UART recovery
│   ├── akam_v1_test_plan.md             # Hardware test plan, hypotheses, resource report
│   ├── hardware_and_uart.md             # Disassembly, UART pads, U-Boot shell & photos (upstream)
│   ├── flash_partition_map_and_rootfs.md# Flash layout, stock boot traps, de-Tuyafication (upstream)
│   ├── firmware_building_and_flashing.md# SPI-NOR DMA gotchas, flash_tool background (upstream)
│   ├── building_your_own_firmware.md    # Short pointer to the Akam build guide
│   ├── camera_and_audio_pipeline.md     # ISP registers, div0 kernel fix, audio capture (upstream)
│   └── serial_console_and_camera_tuning.md
├── images/                              # Hardware photographs and diagrams
├── specs/                               # Akam V1 brief and spec (French, working documents)
├── src/ak_rtsp/                         # ak_rtsp, flash_tool, Makefile, build_firmware.sh
├── tests/                               # Host tests (fake binaries, synthetic fixture)
└── tools/
    ├── build/                           # check_host.sh, bootstrap.sh, env.sh, deps.lock (pinned versions)
    ├── dropbear/                        # build_dropbear.sh, localoptions.h
    ├── firmware_patch_templates/        # ini, wrapper, akam_ssh.sh, akam_lockdown.sh, patch_service_sh.sh
    ├── flash_scripts/                   # install_with_flash_tool.sh, restore.sh, restore_full_firmware.sh, ...
    ├── hw_tests/                        # akam_hwcheck.sh (post-flash check, run over SSH)
    └── hardware_debug/                  # Upstream ESP32 U-Boot bridge and .NET serial console (Windows)
```

---

## Detailed Documentation

Akam V1:
- [Troubleshooting](docs/troubleshooting.md)
- [Akam V1 overview, services and network hardening](docs/akam_v1_install.md)
- [Backup first (8 partitions) and hardware identification](docs/akam_v1_backup.md)
- [Build the image from your own dump](docs/akam_v1_build.md)
- [SSH provisioning and Telnet/FTP lockdown](docs/akam_v1_ssh_and_lockdown.md)
- [Flash and restore](docs/akam_v1_flash_and_restore.md)
- [Hardware test plan and resource report](docs/akam_v1_test_plan.md)
- [Changelog](CHANGELOG.md)

Upstream background (hardware and firmware internals):
- [Hardware & UART Connection Guide](docs/hardware_and_uart.md)
- [Flash Partition Map & De-Tuyafication Details](docs/flash_partition_map_and_rootfs.md)
- [Firmware Packaging & Flash DMA Gotchas](docs/firmware_building_and_flashing.md)
- [Camera ISP, Audio & Div0 Fix Architecture](docs/camera_and_audio_pipeline.md)
- [Serial Console & Live Camera Tuning App Guide](docs/serial_console_and_camera_tuning.md) (Windows .NET tool, upstream)

---

## Methods & Approach

This firmware exists because of a fairly standard embedded-RE toolchain, applied by hand over
many sessions before this repo was compiled:

1. **UART access first.** Solder to the 4 UART pads, capture the boot log, intercept U-Boot's
   autoboot with a scripted Ctrl+C flood, and get a root shell via `init=/bin/sh` bootargs.
2. **strace the stock behavior.** With the vendor's `anyka_ipc` running its factory RTSP demo
   mode, `strace` every ioctl it makes against the ISP/VI/VENC/audio character devices. This is
   the ground truth for every hardware init sequence this project reimplements — reversing from
   a decompiler alone, without a real reference trace of a known-working session, would have
   been far slower and less reliable.
3. **Decompile in Ghidra.** Where the strace log alone doesn't explain a struct layout or ioctl
   semantics (e.g. why a particular field has to be non-zero), decompile the relevant function
   in `anyka_ipc` or a kernel module to confirm it directly.
4. **Reimplement from scratch, verify against the trace.** `ak_rtsp` doesn't call into any
   vendor `.so` — it drives the same character devices directly, with ioctl sequences derived
   from steps 2–3, and its output is repeatedly checked byte-for-byte against captures of the
   stock session (see `docs/camera_and_audio_pipeline.md`) whenever behavior diverged.
5. **De-Tuyafy at the application layer.** Rather than replacing the kernel or rootfs, only the
   APP partition's process list is patched to launch `ak_rtsp` instead of the vendor's Tuya
   cloud client — see `docs/flash_partition_map_and_rootfs.md`.

## Credits & Acknowledgements

- **Anyka Community & Researchers:** Inspiration and reference implementations from [VGerris's Anyka Hacking Journey](https://github.com/VGerris/Anyka_ak3918_hacking_journey) and [Lawliet95's ANYKA Tuya Journey](https://github.com/Lawliet95/ANYKA-Tuya-Hacking-Journey-AK3918v200EN080-v200). Both cover earlier-generation Anyka SoCs (`AK3918EV200`/`GK7102`) with different register maps and flash layouts than this Revision 2 (`AK3918AV130`) hardware, but were valuable for general ioctl naming conventions and orientation.
- **SmolRTSP (OpenIPC):** High-performance, lightweight RTSP library for embedded systems by [OpenIPC/smolrtsp](https://github.com/OpenIPC/smolrtsp).
- **Action-Smart-Connect-Flasher (4ddict):** the four `ak_rtsp` fixes ported into this tree (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) come from [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) (MIT). Akam does not use that flasher for the first camera; see [flash and restore](docs/akam_v1_flash_and_restore.md).
- **Dropbear (Matt Johnston):** the SSH server, [mkj/dropbear](https://github.com/mkj/dropbear) (MIT-style license, with public-domain libtomcrypt/libtommath).
- **Zig:** used as the cross-compiler (downloaded by the bootstrap, not redistributed).
- **Upstream author:** [FluxAlchemist](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware) for the whole reverse-engineered `ak_rtsp` and `flash_tool` code base, the hardware documentation and the images.
