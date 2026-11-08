# Akam: cloud-free firmware for the LSC Smart Connect 1080p camera (Anyka AK3918AV130)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Sensor](https://img.shields.io/badge/Sensor-GalaxyCore%20GC20C3%201080p-green.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20L16%20Audio-red.svg)]()

## What is Akam?

Akam replaces the Tuya cloud software of the **LSC Smart Connect Smart Indoor IP Camera 1080p HD, model
`3215672.3`** (Action) with a small local firmware. After flashing, the camera:

- serves **RTSP H.264 1080p video + audio** on port 554 (`ak_rtsp`), with a tuning port on 8091;
- runs a **key-only SSH server** (Dropbear) so that the stock Telnet/FTP daemons can stay off;
- no longer runs the Tuya cloud client (`anyka_ipc` is removed). Running with no Internet route at all has
  not been tested yet.

Only the APP partition (`mtd7`) is rebuilt, from **your own** dump of your camera. U-Boot, kernel, DTB
and ROOTFS are never written, so U-Boot stays reachable over UART if the APP image turns out to be bad.

> **Akam is a fork** of
> [FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware).
> All the reverse-engineering work behind `ak_rtsp` (ISP, H.264 encoder, audio, IR-cut, RTSP) is the
> upstream author's. Akam keeps that code, its MIT license and its credits (see
> [Credits](#credits--acknowledgements) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), and adds
> a Linux-native build, a permanent SSH server, Telnet/FTP lockdown, and install/backup/restore
> documentation. Upstream was developed on Windows 11 + WSL; Akam's build is native Linux (Arch).
> See the [CHANGELOG](CHANGELOG.md) for the project history.

## Status

**Akam V1 is validated on a real camera (2026-11-08).** Seen working:

- boots without a microSD card, Wi-Fi connects by itself;
- H.264 1080p with a fast start, and an L16 audio track in the RTSP stream;
- the IR night image is delivered (day/night stability over time is not checked yet);
- port 8091 `GET` / `SET`;
- Dropbear starts by itself; SSH as root by ed25519 key, also after a power cut;
- only ports 22, 554 and 8091 listen; telnet and FTP are off; `anyka_ipc` is gone;
- `ak_rtsp` uses about 1% CPU with a client connected (about 90% before the fixes), about 29 MB of RAM
  stay free, and the image has 2.7 MB free in the APP partition.

Still to check: microphone audibility, telnet staying off after a Wi-Fi reconnect, and day/night
stability over a long run. Details and the full checklist: [test plan](docs/akam_v1_test_plan.md).
It was validated on one unit: report what you see on yours.

## Supported camera

Only the **LSC Smart Connect 1080p, model `3215672.3`** (SoC Anyka AK3918AV130, Revision 2 circular
PCB). Other revisions or SoCs (AK3918EV200, GK7102) have different register maps and flash layouts and
are **not** supported. Hardware details are in [Hardware Overview](#hardware-overview).

## What you need

- the camera, a microSD card, and a 2.4 GHz Wi-Fi network the camera can join;
- a **Linux x86-64 PC** (Arch is the reference) with `squashfs-tools`, `make`, `git`, `curl`, `tar`, `xz`,
  `bzip2`, `shellcheck`, `file`, `binutils` (`bash tools/build/check_host.sh` lists what is missing);
- an SSH key pair dedicated to the camera;
- a UART adapter is a useful safety net but not required: the documented route works without it.

## Install in 6 steps

Steps 1 to 3 do not change the camera's flash. Read the [overview](docs/akam_v1_install.md) first.

1. **Back up** all 8 flash partitions twice, compare, store them off-line:
   [backup guide](docs/akam_v1_backup.md) (the no-UART microSD route is in
   [akam_v1_backup_sd.md](docs/akam_v1_backup_sd.md)).
2. **Build** the image from your own APP dump (pinned toolchain, native Linux, no `cmake`):
   [build guide](docs/akam_v1_build.md).
   ```bash
   bash tools/build/bootstrap.sh && . tools/build/env.sh
   make -C src/ak_rtsp ak_rtsp flash_tool
   bash tools/dropbear/build_dropbear.sh
   unsquashfs -d ~/akam-work/app_extracted ~/akam-backup/mtdblock7.bin
   APP_EXTRACTED=~/akam-work/app_extracted FW_DIR=~/akam-work/out bash src/ak_rtsp/build_firmware.sh
   ```
3. **Prepare the SD card** (FAT32). For the first flash from the stock firmware, reuse the toolkit card
   of the backup and add `custom_firmware/<image>.squashfs` (exactly one), `flash_tool`,
   `install_with_flash_tool.sh`, `flash_guard.sh`, `preflight_check.sh` and `original_firmware/` (your
   backup, never a `*.bin` at the card root). The layout is in the "Prepare the SD card" section and the
   no-UART section of the [flash guide](docs/akam_v1_flash_and_restore.md). Your SSH public key goes
   on a second, "first-boot" card made with
   `bash tools/sd/prepare_first_boot_sd.sh --device /dev/sdX --pubkey ~/.ssh/akam.pub` (it also holds
   an empty `AKAM_MAINTENANCE`); see [SSH and lockdown](docs/akam_v1_ssh_and_lockdown.md).
4. **Flash** the APP partition (`mtd7`) and verify the read-back before touching the power. Boot the
   camera with the card, then `telnet <camera-ip> 24`. On the stock firmware mount the card with
   `mount -t vfat /dev/mmcblk0p1 /mnt`; in a later Akam maintenance boot the card is on `/tmp/sd`, so
   use `mount --bind /tmp/sd /mnt` instead. Then:
   ```sh
   sh /mnt/install_with_flash_tool.sh      # compare the printed md5sum with your PC, then type YES alone
   dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum    # must equal the image md5sum
   ```
   The `flash_tool` output must end with `Flashing and verification completed successfully!`. If it
   reports errors or the md5 differs, do not power cycle. Then unplug and replug USB-C by hand (`reboot`
   is unreliable). Read the gates (Wi-Fi config) and details in
   [flash and restore](docs/akam_v1_flash_and_restore.md).
5. **First boot** with the first-boot card inserted: Dropbear creates its host key and installs your
   key. Then `ssh -i ~/.ssh/akam root@<camera-ip>` and run the post-flash check from the PC:
   `ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh`
   ([test plan](docs/akam_v1_test_plan.md)).
6. **Remove the card** (this also removes the maintenance flag and its telnet), power cycle, and check
   again: only ports 22, 554 and 8091 must listen.

The stream is then at `rtsp://<camera-ip>:554/` (see [RTSP Stream URLs](#rtsp-stream-urls)).

## Update later

Rebuild the image and reflash the APP partition, through the maintenance route or over SSH from RAM:
see "Reflashing later" in the [flash guide](docs/akam_v1_flash_and_restore.md).

## Recover

Keep your 8-partition backup. Restore the stock APP, or recover through UART/U-Boot if the APP is bad:
see Restore in the [flash guide](docs/akam_v1_flash_and_restore.md). Restore is not guaranteed to work.

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
