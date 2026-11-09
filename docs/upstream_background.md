# Upstream background: features, image quality, hardware, methods

Context carried over from the upstream project
([FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware)),
for readers who want to know how the firmware works and how it was found. You do not need any of it to
install Akam: start with the [README](../README.md).

## Key features

- **Cloud-free (de-Tuyafied):** the Tuya cloud daemons, MQTT beacons and telemetry binaries are gone.
- **Native SmolRTSP streaming engine:** a small standalone C server (`ak_rtsp`, about 126 KB) built on
  [OpenIPC's SmolRTSP](https://github.com/OpenIPC/smolrtsp), talking directly to the Anyka ISP and H.264 encoder.
- **1080p video:** 1920x1080 at 15 to 25 FPS with calibrated ISP parameters (colour correction matrix, white
  balance gains, noise reduction, VBR+ rate control).
- **Synchronised audio:** the on-board microphone (`/dev/pcmC0D0c`) is streamed over RTP (L16, 8000 Hz) with
  RTCP Sender Reports.
- **IR-cut and night mode:** dual GPIO solenoid pulse control for the IR-cut filter and night vision.
- **Hardened MTD flasher (`flash_tool`):** a chunked 4 KB write/verify/retry flasher that avoids the SPI-NOR
  DMA corruption seen when writing flash while the network and camera drivers are active.
- **Hardware toolkit:** an ESP32 U-Boot interceptor bridge and a Windows .NET 9 serial console (upstream
  tools, see [serial console guide](serial_console_and_camera_tuning.md)).

## Image quality

The firmware is stable for continuous streaming, but the dynamic image tuning (contrast, wide-dynamic-range
curves, auto exposure) is still behind the stock Tuya firmware's closed-source tuning. The static white
balance gains (`R=460, G=256, B=504`) fix the raw sensor's blue tint and VBR+ rate control limits
macroblocking. Akam's own tests show more grain than stock; an improvement is planned. Pull requests and ISP
tuning experiments are welcome.

| | Stock Tuya (`anyka_ipc`) | This firmware (`ak_rtsp`) |
|---|:---:|:---:|
| **Static scene** | ![Stock stable](../images/video_out/anyka_stable.png) | ![Custom stable](../images/video_out/ours_stable.png) |
| **Motion** | ![Stock movement](../images/video_out/anyka_movement.png) | ![Custom movement](../images/video_out/ours_movement.png) |

### Known limitations

- **No dynamic auto white balance (AWB):** fixed, hand-calibrated gains. The stock firmware does not seem to
  do dynamic AWB in the scenes tested either, but it matters if your lighting varies a lot.
- **No two-way audio:** microphone to viewer works; playback through the camera's speaker (NVR "talk") is not
  implemented.

## Hardware overview

| Component | Specification |
|---|---|
| **Retail model** | LSC Smart Connect Smart Indoor IP Camera 1080p HD (Action) |
| **Model / SKU** | `3215672.2` / `SI B26101` (upstream's unit); Akam is validated on `3215672.3` / `SI D26228` |
| **PCB revision** | `IPC280KG2-GNA-MAIN-V1.0` (circular PCB, 2025-08-28) |
| **SoC** | Anyka AK3918AV130 (ARM926EJ-S at 400 MHz) |
| **Image sensor** | GalaxyCore GC20C3 (2 MP 1/2.9" CMOS, I2C) |
| **Flash** | 8 MiB SPI NOR (`XM25QH64D` / Winbond SOIC-8) |
| **Wireless** | Altobeam ATBM6012BX / ATBM6x3x USB Wi-Fi |
| **Power** | USB-C 5 V / 1 A |

This repository targets **revision 2** (circular PCB, AK3918AV130). Older rectangular boards used other SoCs
(AK3918EV200 or GK7102) with different register maps and flash layouts. Check your own unit (label,
`/proc/mtd`) before flashing: the wizard does it for you. Disassembly and UART:
[hardware and UART](hardware_and_uart.md).

## RTSP stream URLs

`ak_rtsp` is path-agnostic: `rtsp://<camera-ip>:554/`, `/live`, `/main_ch` (the stock firmware's path) and
any other sub-path give the same 1920x1080 H.264 + L16 audio stream. With the optional stream password, use
`rtsp://<user>:<password>@<camera-ip>:554/`.

## Methods and approach

The firmware comes from a fairly standard embedded reverse-engineering toolchain, applied by hand over many
sessions before this repository was compiled:

1. **UART access first.** Solder to the 4 UART pads, capture the boot log, interrupt U-Boot's autoboot with a
   scripted Ctrl+C flood, and get a root shell with `init=/bin/sh` boot arguments.
2. **strace the stock behaviour.** With the vendor's `anyka_ipc` running its factory RTSP demo mode, `strace`
   every ioctl it makes against the ISP, VI, VENC and audio devices. This is the ground truth for every
   hardware init sequence the project reimplements.
3. **Decompile in Ghidra** where the trace alone does not explain a struct layout or an ioctl.
4. **Reimplement from scratch, verify against the trace.** `ak_rtsp` calls no vendor `.so`; it drives the same
   character devices directly, and its output was checked byte for byte against captures of the stock session
   (see [camera and audio pipeline](camera_and_audio_pipeline.md)).
5. **De-Tuyafy at the application layer.** Only the APP partition's process list is patched to start `ak_rtsp`
   instead of the Tuya client; see [flash map and rootfs](flash_partition_map_and_rootfs.md).
