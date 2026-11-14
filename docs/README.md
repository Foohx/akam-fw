# Documentation index

New here? Read the [README](../README.md) and run `./akam`: you do not need anything below to install Akam. These
pages are for details, hacking, or when something goes wrong.

## Understand Akam

- [Features and protocols](features.md): everything Akam implements, with ports, defaults and test status.
- [How it works](how-it-works.md): hardware, flash map, boot, `ak_rtsp`, configuration, security.
- [Compatible cameras](compatible-cameras.md): is my camera supported, and how to report a new one.
- [Legal notes](legal.md): your device, privacy, radio, interoperability.

## Install and use

- [The setup wizard](wizard_guide.md): the menu, what each step does and risks.
- [The `akam` program and tested platforms](assistant.md): Windows, macOS and Linux binary, checks, warnings.
- [Using Akam day to day](using_akam.md): every command and setting, ports, updates, security notes.
- [Troubleshooting](troubleshooting.md): real problems and their fixes.
- [Two-way audio](two_way_audio.md): talk through the speaker.
- [ONVIF](onvif.md): NVRs and ONVIF apps.
- [MQTT and Home Assistant](mqtt.md).

## Hack

- [Hacking guide](hacking.md): repository layout, build from source, RAM test, tests, UART and photos.
- Manual procedures, in this order: [back up by UART](manual/backup-uart.md) or
  [by microSD](manual/backup-sd.md), [build the image](manual/build.md),
  [flash and restore](manual/flash-and-restore.md).
- [SSH keys, lockdown, maintenance flag](manual/ssh-and-lockdown.md), [the release kit](manual/kit.md),
  [stock capture](manual/stock-capture.md).

## Tests and status

- [Testing status](testing-status.md): what was tested on the host, in RAM and on hardware.
- [Hardware test checklists](hardware-test-checklists.md): tick-lists for your own camera and for the `akam` program.

Project history: [CHANGELOG](../CHANGELOG.md). Contributors: [CONTRIBUTING](../CONTRIBUTING.md). Exact behaviour of the
code: [specs/](../specs/README.md).
