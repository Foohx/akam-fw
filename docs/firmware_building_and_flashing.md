# Firmware Building & Flashing Guide

> ⚠️ **CAUTION: FLASHING IS STRICTLY AT YOUR OWN RISK**  
> This firmware and its tooling were synthesized and assembled with AI assistance from an internal research and exploration project. It has not been exhaustively tested by hand in all edge cases and scenarios.  
> Modifying embedded firmware or writing to the SPI NOR flash chip carries an inherent risk of bricking your device. In the worst case, an unrecoverable hard-brick may occur requiring physical chip desoldering and an external SPI programmer (such as CH341A).  
> **The creators and contributors of this project assume zero responsibility for damaged hardware, lost warranties, or bricked devices.** Proceed only if you understand these risks.

---

## 1. MANDATORY: Backup Your Stock Firmware First!

Dump all 8 partitions, check sizes and hashes, and keep copies off the SD card. Camyn's procedure is
in [camyn_v1_backup.md](camyn_v1_backup.md); the upstream commands are the `dd if=/dev/mtdblockN
of=... bs=64k` loop used there.

---

## 2. Restoring Stock Firmware (Emergency Fallback)

See [camyn_v1_flash_and_restore.md](camyn_v1_flash_and_restore.md). In short: `tools/flash_scripts/restore.sh`
restores the APP partition only, `tools/flash_scripts/restore_full_firmware.sh` rewrites all 8
partitions (last resort). Restore is not guaranteed.

---

## 3. Building `ak_rtsp` & `flash_tool`

Native Linux build with a pinned Zig: see [camyn_v1_build.md](camyn_v1_build.md).

```bash
bash tools/build/bootstrap.sh
. tools/build/env.sh
make -C src/ak_rtsp ak_rtsp flash_tool
```

Alternative (untested by Camyn): the Anyka GCC toolchain, `make CC=arm-anykav200-linux-uclibcgnueabi-gcc`.

---

## 4. Packaging the APP SquashFS

Needs your own stock APP dump. Full walkthrough: [camyn_v1_build.md](camyn_v1_build.md).

```bash
APP_EXTRACTED=/path/to/app_extracted FW_DIR=/path/to/out bash src/ak_rtsp/build_firmware.sh
```

---

## 5. The SPI NOR DMA Contention Gotcha

> ⚠️ **WARNING:** On the Anyka AK3918AV130 SoC, writing directly to `/dev/mtdblock7` using `dd` on a running system causes **silent flash corruption** due to DMA contention between the SPI-NOR controller, active ISP video capture, and WiFi drivers.
>
> Always use `flash_tool` (`src/ak_rtsp/flash_tool.c`) / `install_with_flash_tool.sh` which operates directly on `/dev/mtd7` (raw char MTD) in 4KB erase-blocks with immediate read-back verification and re-erase retries.

---

## 6. Flashing Step-by-Step

Camyn's procedure (UART shell, `install_with_flash_tool.sh`, read-back check, hand power cycle) is in
[camyn_v1_flash_and_restore.md](camyn_v1_flash_and_restore.md). The upstream notes below describe the
same tool; Camyn no longer uses Telnet for it.

### Verify the Flash Logs!
> **Carefully check the final output of `install_with_flash_tool.sh`:**
> ```text
> FAULT SUMMARY: 0/1280 chunks needed retry, 0 total failed verify attempts.
> Flashing and verification completed successfully!
> ```
> **Do not power off if verification failed.** Restore your stock backup first.

### Power Cycle the Camera
> The software `reboot` command **does not work reliably** on this hardware. Unplug and replug the USB-C power cable by hand.
