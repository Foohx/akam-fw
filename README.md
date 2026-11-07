# Camyn: cloud-free firmware for the LSC Smart Connect 1080p camera (Anyka AK3918AV130)

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Sensor](https://img.shields.io/badge/Sensor-GalaxyCore%20GC20C3%201080p-green.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20PCM%20Audio-red.svg)]()

> **Camyn is a fork** of
> [FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware).
> All the reverse-engineering work behind `ak_rtsp` (ISP, H.264 encoder, audio, IR-cut, RTSP) is the
> upstream author's. Camyn keeps that code, its MIT license and its credits (see
> [Credits](#credits--acknowledgements) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), and adds
> a Linux-native build, a permanent SSH server, Telnet/FTP lockdown, and install/backup/restore
> documentation. Upstream was developed on Windows 11 + WSL; Camyn's build is native Linux (Arch).

Camyn targets the **LSC Smart Connect Smart Indoor IP Camera 1080p HD, model `3215672.3`** (Action; the
SI code is expected to be `D26228`, to be confirmed on your label). It removes the Tuya cloud agent
(`anyka_ipc`), serves RTSP H.264 + audio from the standalone `ak_rtsp`, and runs a key-only SSH server
(Dropbear) so that the stock Telnet/FTP daemons can stay off.

---

## Status: nothing is validated on hardware yet

> **This is Camyn V1 work in progress. No Camyn image has been flashed on any camera.**
>
> - **Compiled / tested on the build host only:** the native Linux build of `ak_rtsp`, `flash_tool` and
>   Dropbear (ARM EABI5, soft-float, static), and the host test suites in `tests/` (they run the
>   install scripts against a synthetic fixture and fake binaries, not against a real firmware dump).
> - **Not tested on hardware:** the Dropbear boot hook, Telnet/FTP lockdown, key provisioning from the
>   microSD, the boot-time exposure
>   window, memory use, and the restore procedures.
> - **Measured on a real APP dump, not on a camera:** the image size (about 2.5 MB of the 5.24 MB
>   partition with xz, like the stock APP) and a byte-identical rebuild from the same inputs.
> - Where this documentation says "expected" or "assumed", it is a hypothesis listed in the
>   [hardware test plan](docs/camyn_v1_test_plan.md) to be checked on the camera.
>
> The V1 acceptance checklist lives in [docs/camyn_v1_test_plan.md](docs/camyn_v1_test_plan.md).
> Tick a box only after you have seen the result on your own camera.

---

## What Camyn V1 contains

| Component | What it does | Where |
|---|---|---|
| `ak_rtsp` | RTSP/RTP H.264 + audio server, 8091 tuning port (functionally upstream) | `src/ak_rtsp/` |
| Native Linux build | Pinned Zig 0.14.1 + pinned SmolRTSP, `make` and `bash` only, no Windows | `tools/build/` |
| Dropbear 2026.94 | Static multi-call binary, ed25519 public-key login only | `tools/dropbear/` |
| SSH boot script | Starts Dropbear in the background, host key and client keys persist in CONFIG | `tools/firmware_patch_templates/camyn_ssh.sh` |
| Telnet/FTP lockdown | Build-time patch of `service.sh` + runtime safety-net script | `tools/firmware_patch_templates/` |
| Image builder | Patches **your own** APP dump, repacks, checks, pads to 5 242 880 bytes | `src/ak_rtsp/build_firmware.sh` |
| Post-flash check | Read-only checklist script, run over SSH | `tools/hw_tests/camyn_hwcheck.sh` |

Only the **APP partition (`mtd7`)** is ever rebuilt. U-Boot, kernel, DTB and ROOTFS are never written, so
U-Boot stays reachable over UART if the APP image turns out to be bad.

---

## Important disclaimer and risks

> **FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.**
>
> - **Experimental codebase:** upstream is the public release of a larger private reverse-engineering
>   project, organised with AI assistance. Camyn's additions were written with AI assistance and have
>   **not been run on a camera**.
> - **Risk of permanent bricking:** writing to the SPI NOR flash can brick the camera. In the worst case
>   only desoldering the flash chip and using an external SPI programmer (for example a CH341A) will
>   recover it. **Restoring your backup is not guaranteed to work** (see
>   [flash and restore](docs/camyn_v1_flash_and_restore.md)).
> - **No liability:** the authors and contributors accept zero liability for damaged hardware, bricked
>   cameras, data loss or voided warranties.
> - **Always make and verify a complete 8-partition backup first** ([backup guide](docs/camyn_v1_backup.md)).
>   Never publish it: it contains your Wi-Fi credentials and Tuya identifiers.
> - **Always read the flash verification output before disconnecting power.** If flashing reports
>   errors, do not power cycle; flash your stock backup immediately.

---

## Quick start (Arch Linux, x86-64)

Nothing below touches the camera. Read the [backup guide](docs/camyn_v1_backup.md) and make your dump
**before** you flash anything.

```bash
# 1. Host tools (the repo never calls sudo)
sudo pacman -S --needed squashfs-tools shellcheck file binutils make git curl tar xz bzip2
bash tools/build/check_host.sh          # lists anything still missing

# 2. Download and verify the pinned toolchain and sources (Zig, SmolRTSP + deps, Dropbear archive)
bash tools/build/bootstrap.sh

# 3. Build ak_rtsp and flash_tool
. tools/build/env.sh
make -C src/ak_rtsp ak_rtsp flash_tool

# 4. Build Dropbear -> dist/dropbear/dropbearmulti
bash tools/dropbear/build_dropbear.sh

# 5. Build the APP image from YOUR OWN dump (the unsquashed mtdblock7.bin)
unsquashfs -d ~/camyn-work/app_extracted ~/camyn-backup/mtdblock7.bin
APP_EXTRACTED=~/camyn-work/app_extracted FW_DIR=~/camyn-work/out \
    bash src/ak_rtsp/build_firmware.sh
```

Step 5 produces `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` (exactly 5 242 880 bytes), its `.sha256`, and
`build_report.txt` in `FW_DIR`. Then follow, in this order:

1. [Backup first](docs/camyn_v1_backup.md): dump and verify the 8 partitions, confirm the model.
2. [Build the image](docs/camyn_v1_build.md): tool details, size guard, compression, report.
3. [SSH and Telnet/FTP lockdown](docs/camyn_v1_ssh_and_lockdown.md): generate your key, provision it.
4. [Flash and restore](docs/camyn_v1_flash_and_restore.md): Camyn's own procedure and recovery.
5. [Hardware test plan](docs/camyn_v1_test_plan.md): what to check on the camera, resource report.

`cmake` is **not** required (the bootstrap populates SmolRTSP's four header-only dependencies itself).
The upstream Windows/PowerShell/WSL build path is gone: the Makefile no longer calls `pwsh` or `wsl`.

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
| **Model / SKU** | `3215672.2` / `SI B26101` (upstream, tested by the upstream author). Camyn target: `3215672.3`, SI `D26228` expected, unconfirmed |
| **PCB Revision** | `IPC280KG2-GNA-MAIN-V1.0` (Circular PCB, 2025-08-28) |
| **SoC** | Anyka AK3918AV130 (ARM926EJ-S @ 400 MHz) |
| **Image Sensor** | GalaxyCore GC20C3 (2MP 1/2.9" CMOS, I2C bus) |
| **Flash Memory** | 8 MiB SPI NOR (`XM25QH64D` / Winbond SOIC-8) |
| **Wireless** | Altobeam ATBM6012BX / ATBM6x3x USB WiFi |
| **Power** | USB-C 5V / 1A |

> ℹ️ **NOTE:** The table above describes the upstream test unit. Camyn's target model `3215672.3` has not been opened or dumped yet; whether it has the same PCB, flash and partition table is to be confirmed (see the [backup guide](docs/camyn_v1_backup.md)). This repository is tailored for **Revision 2** (circular PCB, Anyka AK3918AV130). Older revisions with rectangular boards used different SoCs (AK3918EV200 or GK7102) which have different register maps and flash partitioning.

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
│   ├── camyn_v1_install.md              # Camyn V1 overview, services, network hardening, licenses
│   ├── camyn_v1_backup.md               # 8-partition backup and hardware identification (do this first)
│   ├── camyn_v1_build.md                # Host tools, builds, image from your own dump
│   ├── camyn_v1_ssh_and_lockdown.md     # SSH key provisioning, Telnet/FTP lockdown, maintenance flag
│   ├── camyn_v1_flash_and_restore.md    # Flashing procedure, verification, restore, UART recovery
│   ├── camyn_v1_test_plan.md            # Hardware test plan, hypotheses, resource report
│   ├── hardware_and_uart.md             # Disassembly, UART pads, U-Boot shell & photos (upstream)
│   ├── flash_partition_map_and_rootfs.md# Flash layout, stock boot traps, de-Tuyafication (upstream)
│   ├── firmware_building_and_flashing.md# SPI-NOR DMA gotchas, flash_tool background (upstream)
│   ├── building_your_own_firmware.md    # Short pointer to the Camyn build guide
│   ├── camera_and_audio_pipeline.md     # ISP registers, div0 kernel fix, audio capture (upstream)
│   └── serial_console_and_camera_tuning.md
├── images/                              # Hardware photographs and diagrams
├── specs/                               # Camyn V1 brief and spec (French, working documents)
├── src/ak_rtsp/                         # ak_rtsp, flash_tool, Makefile, build_firmware.sh
├── tests/                               # Host tests (fake binaries, synthetic fixture)
└── tools/
    ├── build/                           # check_host.sh, bootstrap.sh, env.sh, deps.lock (pinned versions)
    ├── dropbear/                        # build_dropbear.sh, localoptions.h
    ├── firmware_patch_templates/        # ini, wrapper, camyn_ssh.sh, camyn_lockdown.sh, patch_service_sh.sh
    ├── flash_scripts/                   # install_with_flash_tool.sh, restore.sh, restore_full_firmware.sh, ...
    ├── hw_tests/                        # camyn_hwcheck.sh (post-flash check, run over SSH)
    └── hardware_debug/                  # Upstream ESP32 U-Boot bridge and .NET serial console (Windows)
```

---

## Detailed Documentation

Camyn V1:
- [Camyn V1 overview, services and network hardening](docs/camyn_v1_install.md)
- [Backup first (8 partitions) and hardware identification](docs/camyn_v1_backup.md)
- [Build the image from your own dump](docs/camyn_v1_build.md)
- [SSH provisioning and Telnet/FTP lockdown](docs/camyn_v1_ssh_and_lockdown.md)
- [Flash and restore](docs/camyn_v1_flash_and_restore.md)
- [Hardware test plan and resource report](docs/camyn_v1_test_plan.md)

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
- **Action-Smart-Connect-Flasher (4ddict):** the four `ak_rtsp` fixes ported into this tree (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) come from [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) (MIT). Camyn does not use that flasher for the first camera; see [flash and restore](docs/camyn_v1_flash_and_restore.md).
- **Dropbear (Matt Johnston):** the SSH server, [mkj/dropbear](https://github.com/mkj/dropbear) (MIT-style license, with public-domain libtomcrypt/libtommath).
- **Zig:** used as the cross-compiler (downloaded by the bootstrap, not redistributed).
- **Upstream author:** [FluxAlchemist](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware) for the whole reverse-engineered `ak_rtsp` and `flash_tool` code base, the hardware documentation and the images.
