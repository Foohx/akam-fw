# Camyn V1: Back Up Your Camera First

> **Do this before anything else.** Do not build, flash or even boot a modified image until you
> hold a verified copy of all 8 flash partitions. The backup is also the source for building your
> image: the APP partition (`mtdblock7`) is the input of `build_firmware.sh`.
>
> Status: this procedure reuses the upstream dump commands (`dd` per partition) plus extra size and
> hash checks written for Camyn. The extra checks have **not been run on a camera**.

> **WARNING: never boot the camera with `.bin` files on the card.** The stock `service.sh` runs
> `tf_update.sh` at every boot, which mounts the card on `/mnt` and calls `system_upgrade` on every
> file matching `/mnt/*.bin` (top level of the card only; subdirectories such as `dump1/` and
> `dump2/` and hidden files are not matched). `system_upgrade` writes the MTD partitions named in
> the file, so a stray OTA-format `.bin` on any card could reflash the camera. The checks in
> `system_upgrade` (bundle name, higher version) are the only protection. Keep dumps in
> subdirectories, and **remove the card before powering the camera again** once the dump is done.
> Camyn images built from this repository guard that call behind `/mnt/CAMYN_MAINTENANCE`, but the
> stock firmware you are dumping is not protected.

---

## 1. What you need

- A root shell on the stock firmware. The most dependable way on this camera is the UART console
  with the `init=/bin/sh` boot (see [hardware_and_uart.md](hardware_and_uart.md), sections 4 to 6).
  In that state Wi-Fi, ISP and video drivers are not running, which is also the quietest state for
  reading flash.
- A FAT32 microSD card with enough free space (the 8 files total 8 MiB).
- A PC to hold an independent copy.

Without opening the camera, a root shell over Wi-Fi can be obtained with a prepared microSD card
(no UART needed): see [camyn_v1_backup_sd.md](camyn_v1_backup_sd.md). That route is untested by
Camyn on a camera. The stock firmware's own Telnet credentials are not known to this project.

---

## 2. Confirm the partition table first

In the root shell, mount the proc filesystem (if it is not mounted yet) and read the table:

```sh
mount -t proc proc /proc 2>/dev/null
cat /proc/mtd
```

The 8 partitions of the reference unit and their exact sizes in bytes (hex in `/proc/mtd`):

| Device | Name | Size (bytes) | Size in `/proc/mtd` (hex) |
|---|---|---|---|
| `mtd0` / `mtdblock0` | UBOOT | 204800 | `00032000` |
| `mtd1` / `mtdblock1` | ENV | 4096 | `00001000` |
| `mtd2` / `mtdblock2` | ENVBK | 4096 | `00001000` |
| `mtd3` / `mtdblock3` | DTB | 65536 | `00010000` |
| `mtd4` / `mtdblock4` | KERNEL | 1572864 | `00180000` |
| `mtd5` / `mtdblock5` | ROOTFS | 1032192 | `000fc000` |
| `mtd6` / `mtdblock6` | CONFIG | 262144 | `00040000` |
| `mtd7` / `mtdblock7` | APP | 5242880 | `00500000` |

These sizes are the ones hard-coded in `tools/flash_scripts/restore_full_firmware.sh` and
`tools/flash_scripts/install_with_flash_tool.sh`. **If your `/proc/mtd` differs in count or sizes,
stop.** Your unit is not the layout Camyn was written for; do not flash anything and ask before going on.

The partition names printed by your kernel are expected to match the table but are not relied on by
any script; the sizes are what the scripts check.

---

## 3. Confirm the hardware before trusting the layout

Camyn targets model **`3215672.3`**, SI code **`D26228` (expected, unconfirmed)**. The upstream
documentation and tests are for `3215672.2` / `SI B26101`. Check and write down:

1. The label on the camera and the retail box: model number and the `SI` code. Compare with the
   expected `D26228`. Whether `.3` / `D26228` uses the same flash layout and the same APP contents
   as upstream's unit is exactly what the partition table above and the dump itself will tell you.
2. If you open the camera ([disassembly guide](hardware_and_uart.md)): the PCB marking (upstream:
   `IPC280KG2-GNA-MAIN-V1.0`), the SoC (AK3918AV130) and the 8 MiB SPI NOR chip.
3. The U-Boot / kernel boot log over UART (SoC name, `mtdparts`).

If the label does not say `3215672.3` / `D26228`, or `/proc/mtd` differs, treat the unit as
unsupported until you understand why.

---

## 4. Dump all 8 partitions

Mount the SD card and dump. `bs=64k` is much faster than the BusyBox default.

```sh
mount -t vfat /dev/mmcblk0p1 /mnt
mkdir -p /mnt/original_firmware

dd if=/dev/mtdblock0 of=/mnt/original_firmware/mtdblock0.bin bs=64k
dd if=/dev/mtdblock1 of=/mnt/original_firmware/mtdblock1.bin bs=64k
dd if=/dev/mtdblock2 of=/mnt/original_firmware/mtdblock2.bin bs=64k
dd if=/dev/mtdblock3 of=/mnt/original_firmware/mtdblock3.bin bs=64k
dd if=/dev/mtdblock4 of=/mnt/original_firmware/mtdblock4.bin bs=64k
dd if=/dev/mtdblock5 of=/mnt/original_firmware/mtdblock5.bin bs=64k
dd if=/dev/mtdblock6 of=/mnt/original_firmware/mtdblock6.bin bs=64k
dd if=/dev/mtdblock7 of=/mnt/original_firmware/mtdblock7.bin bs=64k
sync
```

(The SPI-NOR DMA corruption described in
[firmware_building_and_flashing.md](firmware_building_and_flashing.md) is reported by upstream for
**writing** with `dd`; no read problem is documented, but see the double-read check below.)

### Check the sizes on the camera

The file sizes must equal the table above exactly. `stat` exists in the stock root filesystem's
BusyBox (the restore scripts rely on it); `wc` may not:

```sh
for i in 0 1 2 3 4 5 6 7; do
    echo "mtdblock$i: $(stat -c%s /mnt/original_firmware/mtdblock$i.bin)"
done
```

### Hash on the camera

If the BusyBox has `sha256sum`, use it; otherwise use `md5sum` (the flashing script uses `md5sum`):

```sh
cd /mnt/original_firmware
sha256sum mtdblock*.bin > SHA256SUMS 2>/dev/null || md5sum mtdblock*.bin > MD5SUMS
cat SHA256SUMS MD5SUMS 2>/dev/null
```

A cheap extra precaution (not part of the upstream procedure): dump each partition a second time to
a different file name and compare the hashes of both reads. Two different results mean an unreliable
read; do not use that dump.

---

## 5. Copy to the PC and verify there

Power the camera down (do not flash anything yet), move the SD card to the PC, and copy the whole
`original_firmware/` directory to a local directory:

```bash
mkdir -p ~/camyn-backup
cp -a /path/to/sd/original_firmware/. ~/camyn-backup/
cd ~/camyn-backup
sha256sum mtdblock*.bin | tee SHA256SUMS.pc
stat -c '%n %s' mtdblock*.bin
```

Verify:

- The sizes match the table in section 2 exactly.
- If the camera computed `md5sum` values, recompute `md5sum mtdblock*.bin` on the PC and compare.
  If the camera computed `sha256sum`, compare it with `SHA256SUMS.pc`.
- `file mtdblock7.bin` reports a SquashFS filesystem; `unsquashfs -s mtdblock7.bin` prints its
  compression and block size (write both down, they are used in the
  [build guide](camyn_v1_build.md#6-compression-gzip-or-xz)).

### Keep an independent copy

- Keep **at least two copies on different media** (for example the PC and an external drive), plus the
  one left on the SD card. The SD copy alone is not a backup.
- Record `SHA256SUMS.pc` somewhere separate from the dump, so silent corruption of a copy can be detected later.

### Never publish the dump

`mtdblock6.bin` (CONFIG) holds your Wi-Fi credentials, and the other partitions hold the vendor's
proprietary firmware and device identifiers. **Never commit dumps to Git, attach them to issues, or
share them.** The repository ignores `*.bin` and `*.squashfs`, but do not rely on that: keep dumps
outside the working tree (the examples use `~/camyn-backup`). Images built by Camyn embed your
stock APP tree; do not publish those either.

---

## 6. The upstream flasher also takes a backup

The Action-Smart-Connect-Flasher saves the 8 partitions in its first stage and checks sizes and MD5.
Camyn's first-camera procedure does not use that flasher (see
[flash and restore](camyn_v1_flash_and_restore.md#1-why-camyns-own-procedure-and-not-the-flasher)), and you
should validate your backup yourself in any case, as above.

---

## 7. Checklist before you continue

- [ ] Label reads `3215672.3`; SI code noted (expected `D26228`).
- [ ] `/proc/mtd` shows 8 partitions with exactly the sizes above.
- [ ] 8 files dumped, each with the exact size.
- [ ] Hashes computed on the camera and re-computed on the PC, and they match.
- [ ] At least two independent copies, outside Git.
- [ ] `mtdblock7.bin` opens with `unsquashfs -s` on the PC.

Next: [build the image from your dump](camyn_v1_build.md).
