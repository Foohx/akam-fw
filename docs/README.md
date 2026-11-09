# Documentation index

New here? Read the [README](../README.md) and run `./akam`: you do not need anything below to install
Akam. These pages are for when you want details, want to do a step by hand, or something went wrong.

## Help

- [Troubleshooting](troubleshooting.md): real problems and their fixes (network, card reader, VLC, passwords, SSH).
- [Test plan and status](akam_v1_test_plan.md): what was validated on hardware, what is still to check, the post-flash check script.

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

Project history: [CHANGELOG](../CHANGELOG.md). For contributors: design notes are in `specs/` (French,
internal).
