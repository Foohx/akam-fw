# Building Your Own Firmware From Your Own Dump

Short pointer (advanced). Most people use `./akam` with the prebuilt kit: see the
[README](../README.md#install-in-5-steps). To build from source, follow the guides below.

This repository ships the `ak_rtsp` source code and the tooling, but deliberately does **not** ship a
stock APP-partition dump or a pre-built image: that partition is the vendor's proprietary firmware.
You dump your own camera, patch it locally, and rebuild it.

The Windows/WSL/PowerShell workflow that used to be described here has been replaced by a native
Linux build (reference host: Arch Linux). The upstream project was developed on Windows 11 + WSL.

The current guides:

1. [Back up all 8 partitions first](akam_v1_backup.md)
2. [Build the image from your own dump](akam_v1_build.md) (host tools, `bootstrap.sh`, `ak_rtsp`,
   Dropbear, `unsquashfs`, `build_firmware.sh`, size guard, `build_report.txt`)
3. [Flash and restore](akam_v1_flash_and_restore.md)

What `build_firmware.sh` does to the tree (patch templates are in `tools/firmware_patch_templates/`):
removes `bin/anyka_ipc`, installs `ak_rtsp`, `ak_rtsp_wrapper.sh` and a single-entry
`_ht_process.ini`, adds Dropbear and the Akam boot scripts, patches `sbin/service.sh`, repacks with
`mksquashfs` (xz by default, like the stock APP; gzip with `AKAM_SQUASHFS_COMP=gzip`), round-trip verifies, and pads to exactly 5 242 880 bytes.

> **Why keep `cmdserver`/`msgserver`?** They are pure local IPC (UNIX socket / SysV message queue),
> not Tuya cloud clients; removing them stalls boot. See
> [flash_partition_map_and_rootfs.md](flash_partition_map_and_rootfs.md).

> **Why a timestamp in the image file name?** The camera has no working RTC and file mtimes get reset
> by copies. The flashing script picks the lexicographically last `ak_rtsp_firmware_*.squashfs`.

## Iterating quickly without reflashing

While tuning `ak_rtsp`, keep a card in the camera and use `tools/flash_scripts/start_rtsp.sh` to
stop the running `ak_rtsp`/`anyka_ipc`, reload the venc modules and start an uploaded binary, without a
reflash. With Akam running you can upload via SSH:
`make -C src/ak_rtsp upload CAMERA_IP=<camera-ip>` (files go to `/tmp`; see
[the SSH guide](akam_v1_ssh_and_lockdown.md#5-file-transfer-without-scp)). This workflow is
upstream's and has not been tried with Akam's SSH on a camera.
