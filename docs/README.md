# Documentation index

New here? Read the [README](../README.md), then the [install guide](install.md): one page for both cameras (1080p and 2K).
The pages below are for details, integration, development, or when something goes wrong. Unknown words: the
[glossary](#glossary) at the end.

## User docs

- [Compatible cameras](compatible-cameras.md): is my camera supported, how to report a new one.
- [Install guide](install.md), both cameras; [the `akam` program](assistant.md) (download, check, tested platforms); the old
  bash wizard, to be retired: [wizard guide](wizard_guide.md).
- [Using Akam day to day](using_akam.md), both cameras: commands, settings, ports, updates. 2K specifics (pan/tilt, files):
  [using the 2K](porting/lsc-rotatable-2k/using-the-2k.md).
- [Features and protocols](features.md): everything implemented, with defaults, ports and test status.
- [Troubleshooting](troubleshooting.md), both cameras (the 2K has its own [section](troubleshooting.md#rotatable-2k)); recovery:
  [2K recovery](porting/lsc-rotatable-2k/README.md#recovery), [UART recovery](porting/lsc-rotatable-2k/uart-recovery.md).
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
  [the 2K flows](assistant/rot2k-flows.md), [install and update journey](assistant/install-update-journey.md); the original design is archived.
- Code layout per platform and camera: [`platforms/anyka-ak3918`](../platforms/anyka-ak3918/README.md),
  [`platforms/anyka-ak3918av130`](../platforms/anyka-ak3918av130/README.md) (see [hacking](hacking.md#repository-layout)).

## Porting a new camera

[Template and checklist](porting/TEMPLATE-new-camera.md), then [compatible cameras](compatible-cameras.md#adding-a-new-model).

## Archive

Historical plans and studies: [docs/archive](archive/README.md). Project history: [CHANGELOG](../CHANGELOG.md).

## Glossary

- **Host-tested only**: checked by automated tests on a PC (a fake camera, no hardware); not yet run on a real camera. Every
  status says which: [testing status](testing-status.md).
- **Validated on a camera**: run on a real camera, with the build named in the testing status.
- **Golden**: a recorded expected output that a test compares against; it is regenerated only when the output is meant to change.
- **Kit**: the signed archive of one camera model (`akam-kit-<model>-X.Y.Z.tar.xz`) that holds only Akam code; the program
  downloads and verifies it. Your image is assembled on your computer from the kit and your own backup.
- **Image**: the file written to the camera's flash (APP on the 1080p, appfs on the 2K).
- **APP, appfs**: the flash partition Akam replaces (`mtd7` on the 1080p, `mtd3` on the 2K). **Card-less**: the 2K boots Akam
  from its flash with no SD card.
- **Rehearsal**: a run that shows every step and writes nothing (`--dry-run`, or the default of `appfs-flash`).
- **Hot update**: the 2K updates its flash from itself, with no card.
- **jffs2**: the small writable flash file system that keeps the settings (`/config` on the 2K).
- **HA**: Home Assistant. **NPU**: the camera's neural-network unit (person detection on the 2K). **Signed API**: the camera's
  HMAC-signed JSON API on port 8092 that the assistant uses for settings.
