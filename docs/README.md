# Documentation index

New here? Read the [README](../README.md) and run `./akam`: you do not need anything below to install
Akam. These pages are for when you want details, want to do a step by hand, or something went wrong.

## Start here

1. [README](../README.md): what Akam is, the risks, and the install in 5 steps.
2. [Compatible cameras](compatible-cameras.md): is my camera supported, and what do I get compared with the stock firmware.
3. [Installing the `akam` binary](assistant_install.md): the single-file assistant for Linux, Windows and macOS (experimental).
4. [Troubleshooting](troubleshooting.md): real problems and their fixes (network, card reader, VLC, passwords, SSH).

## Use Akam

- [Using Akam day to day](using_akam.md): every command and setting, open ports, watching the stream, updates, security notes.
- [The setup wizard](wizard_guide.md): the menu, what each step does and risks, the native binary.
- [ONVIF](onvif.md): NVRs and ONVIF apps (optional, off by default).
- [Two-way audio](two_way_audio.md): talk through the camera speaker (`./akam talk`, go2rtc, Frigate, Home Assistant), settings, hardware check list.
- [MQTT and Home Assistant](mqtt.md): the optional MQTT client with Home Assistant discovery (`./akam mqtt`), entities, security, a temporary Mosquitto to try it, hardware checks.

## Tests and status

- [Testing status](testing-status.md): what was tested on the host, in RAM and on hardware, feature by feature; how to run the host tests.
- [Test plan and status](akam_v1_test_plan.md): what was validated on hardware, what is still to check, the post-flash check script.
- [Assistant hardware test plan](assistant_hardware_test_plan.md): tickable checklists for Windows, macOS and Linux with the native `akam` binary.
- [Assistant screen manual test](assistant_tui_manual_test.md): what to look at and which keys to press on each terminal (Windows, macOS, Linux, tmux, 16 colours, `NO_COLOR`, small windows).
- [Release notes draft](release_notes_draft.md): the template of the release notes (camera kit and assistant): supported hardware, what was validated, known limits.

## Reference

- [Overview, services and network hardening](akam_v1_install.md): what changes on the camera, ports, the 8091 control port, licences.
- [SSH keys, Telnet/FTP lockdown and the maintenance flag](akam_v1_ssh_and_lockdown.md): how remote access works on the camera.
- [The release kit](akam_v1_kit.md): what the prebuilt kit holds, how it is verified, supported hardware.
- [Repository overview](repository_overview.md): components and folder layout.
- [Kernel time64 note](akam_v1_kernel_time64.md): why the binaries carry a clock shim.

## Manual procedures (advanced, without the wizard)

The wizard automates all of these. Use them to understand what it does, to audit it, or when you
cannot run it. Do them in this order.

1. [Back up the camera, by UART](akam_v1_backup.md) or [by microSD root shell (no UART)](akam_v1_backup_sd.md).
2. [Build the image from your dump](akam_v1_build.md) (host tools, source build; the kit route needs no compiler).
3. [Flash, reflash over SSH, and restore](akam_v1_flash_and_restore.md).

## Hardware and upstream background

Written by the upstream author, with Akam notes where behaviour differs.

- [Upstream background](upstream_background.md): features, image quality, hardware table, methods.
- [Hardware and UART](hardware_and_uart.md): disassembly, UART pads, U-Boot shell.
- [Flash partition map and rootfs](flash_partition_map_and_rootfs.md): layout, boot traps, de-Tuyafication.
- [Firmware packaging and flash DMA gotchas](firmware_building_and_flashing.md): why `flash_tool` exists.
- [Camera, ISP and audio pipeline](camera_and_audio_pipeline.md): ISP registers, audio capture.
- [Serial console and tuning app](serial_console_and_camera_tuning.md): Windows .NET tool (upstream).
- [Building your own firmware](building_your_own_firmware.md): short pointer to the build guide.
- [Stock speaker/ISP capture](stock_capture.md): developer procedure to record how the stock `anyka_ipc` drives the speaker, without writing to the camera.

Project history: [CHANGELOG](../CHANGELOG.md). For contributors: design notes are in `specs/` (French,
internal).
