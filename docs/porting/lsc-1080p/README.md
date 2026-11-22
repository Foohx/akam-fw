# LSC Indoor 1080p (`lsc-indoor-1080p-ak3918`)

Entry page of this camera. The 2K has the same page with the same sections: [LSC Rotatable 2K](../lsc-rotatable-2k/README.md).
Source of the model definition: [`firmware/lsc-indoor-1080p-ak3918`](../../../firmware/lsc-indoor-1080p-ak3918) and
[`platforms/anyka-ak3918`](../../../platforms/anyka-ak3918).

Status: **validated on 2 units** (article `3215672.3`, label `SI D26228`). The revision `3215672.2` is untested.

## Hardware

| Item | Value |
|---|---|
| SoC, sensor | Anyka AK3918AV130, GalaxyCore GC20C3 (2 MP) |
| Board | `IPC280KG2-GNA-MAIN-V1.0` (circular PCB) |
| Flash | 8 MiB SPI NOR, 8 partitions ([flash map](../../how-it-works.md#flash-map)) |
| Motor, speaker | no motor; microphone and speaker; front status LED, IR LEDs and IR-cut filter |
| Features compiled in | system, light, API, ONVIF; PTZ, button, scene, patrol and tracking are off (`firmware/lsc-indoor-1080p-ak3918/model.mk`) |

## Stock firmware findings

- Stock application `anyka_ipc` (cloud app), telnet and FTP started by the stock scripts, Linux 4.4.302-cip94, squashfs xz
  with 128 KiB blocks, BusyBox 1.30.1: fingerprints in [compatible cameras](../../compatible-cameras.md) and in
  `tools/release/models/lsc-indoor-1080p-ak3918.txt`.
- Boot chain and what the stock scripts do: [how it works](../../how-it-works.md), [what runs on the camera](on-camera.md).
- How to get your own stock dump: [back up by microSD](../../manual/backup-sd.md) or [by UART](../../manual/backup-uart.md),
  and [stock capture](../../manual/stock-capture.md).
- Findings on other models stay in the per-camera docs; no vendor file is stored in this repository.

## What runs on the camera

One application, `ak_rtsp`, started by the stock watchdog in place of `anyka_ipc`: video, audio, RTSP, ONVIF, MQTT, control
ports, signed API, day/night and LEDs. Key-only SSH. Stock boot chain, U-Boot, kernel and drivers kept.
Full table: [on the camera](on-camera.md). Settings and API: [control API](control-api.md), [features](../../features.md).

## Install

1. Read [compatible cameras](../../compatible-cameras.md) and the risks in the [README](../../../README.md#safety-and-recovery).
2. Run `./akam` (or the `akam` binary): backup of all 8 partitions, image built from your backup plus the signed kit, flash
   by SSH or by the maintenance shell, verification. [Wizard guide](../../wizard_guide.md), [assistant](../../assistant.md).
3. Manual route: [back up](../../manual/backup-sd.md), [build the image](../../manual/build.md),
   [flash and restore](../../manual/flash-and-restore.md).

## Update

`akam update` flashes a new image over SSH with no card, read-back verified ([using Akam](../../using_akam.md)).

## Recovery

- `akam restore` writes your stock APP backup back. It was **never rehearsed on hardware**: no guarantee
  ([testing status](../../testing-status.md)).
- Manual restore and the SPI programmer path: [flash and restore](../../manual/flash-and-restore.md).
- Problems: [troubleshooting](../../troubleshooting.md).

## Known limits

- One RTSP client at a time (a second gets 503): put go2rtc or MediaMTX in front.
- One stream only (no sub stream), no snapshots, no motion detection, no recording to microSD.
- MQTT and the signed API are host-tested only; the encoder watchdog and the Wi-Fi supervisor were never triggered on hardware.
- Details per feature: [features](../../features.md), [testing status](../../testing-status.md).
