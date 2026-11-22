# Documentation index

New here? Read the [README](../README.md), then follow the install of your camera: the bash wizard `./akam` for the 1080p,
the Go `akam` program for the 2K ([2K install](porting/lsc-rotatable-2k/README.md#install)). The pages below are for details, integration,
development, or when something goes wrong.

## User docs

- [Compatible cameras](compatible-cameras.md): is my camera supported, how to report a new one.
- [The setup wizard](wizard_guide.md) (1080p) and [the `akam` program](assistant.md) (tested platforms, 2K install and update).
- [Using Akam day to day](using_akam.md) (1080p): commands, settings, ports, updates. The 2K: [using the 2K](porting/lsc-rotatable-2k/using-the-2k.md).
- [Features and protocols](features.md): everything implemented, with defaults, ports and test status.
- [Troubleshooting](troubleshooting.md) (1080p). The 2K has no troubleshooting page yet: start with its [recovery section](porting/lsc-rotatable-2k/README.md#recovery) and [UART recovery](porting/lsc-rotatable-2k/uart-recovery.md).
- [Legal notes](legal.md).

## Integration

- [RTSP and Frigate](../README.md#integrations), [ONVIF](onvif.md), [MQTT and Home Assistant](mqtt.md) (both cameras),
  [Two-way audio](two_way_audio.md).
- Signed HTTP API: [1080p](porting/lsc-1080p/control-api.md), [2K](porting/lsc-rotatable-2k/control-api.md),
  [spec](../specs/control-api.md).

## Per camera

Same sections for both: hardware, stock firmware findings, what runs, install, update, recovery, known limits.

| | Entry page | What runs on the camera | Control API |
|---|---|---|---|
| LSC Indoor 1080p | [1080p](porting/lsc-1080p/README.md) | [on-camera](porting/lsc-1080p/on-camera.md) | [API](porting/lsc-1080p/control-api.md) |
| LSC Rotatable 2K | [2K](porting/lsc-rotatable-2k/README.md) | [on-camera](porting/lsc-rotatable-2k/on-camera.md) | [API](porting/lsc-rotatable-2k/control-api.md) |

The 2K findings (stock firmware, PTZ, NPU, audio, tracking, recovery) are listed on its entry page.
Also: [models](models.md), [SoCs](socs.md), [how it works](how-it-works.md).

## Developer

- [Hacking guide](hacking.md): repository layout, build from source, RAM test, tests, UART.
- [HAL architecture](hal-architecture.md): portable modules and the per-model layer.
- Manual procedures: [back up by UART](manual/backup-uart.md) or [by microSD](manual/backup-sd.md),
  [build the image](manual/build.md), [flash and restore](manual/flash-and-restore.md),
  [SSH keys and lockdown](manual/ssh-and-lockdown.md), [release kit](manual/kit.md), [stock capture](manual/stock-capture.md).
- Tests: [testing status](testing-status.md) (1080p and 2K sections; which builds the hardware results come from), [hardware checklists](hardware-test-checklists.md); CI: `tools/ci/local_check.sh`.
- Code behaviour: [specs/](../specs/README.md). Contributing: [CONTRIBUTING](../CONTRIBUTING.md). Assistant design:
  [2K flash design (partly implemented)](assistant-rot2k-flash-design.md), [install and update journey](assistant/install-update-journey.md).

## Porting a new camera

[Template and checklist](porting/TEMPLATE-new-camera.md), then [compatible cameras](compatible-cameras.md#adding-a-new-model).

## Archive

Historical plans and studies: [docs/archive](archive/README.md). Project history: [CHANGELOG](../CHANGELOG.md).
