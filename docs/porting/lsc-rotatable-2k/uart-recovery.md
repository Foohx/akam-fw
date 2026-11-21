# UART recovery — LSC Smart Rotatable Camera 2K (AK3918AV130)

Serial-console recovery for the rotatable 2K. Written from the flash backup
(`~/akam-backup-lsc-rotatable-2k-20261114_170301`, U-Boot `mtd0.bin`), **not yet confirmed on real
hardware** — the console being open and un-passworded can only be proven by connecting the UART (step 0).

## 0. TL;DR — what UART can and cannot save

U-Boot on this unit has an **interactive console** (`=>`), an **interruptible autoboot**
(`Hit any key to stop autoboot`, `nbootdelay=1`, ~1 s), serial download (`loady`/`loadx`, ymodem/kermit)
and **SPI-flash write** commands (`sf probe/read/erase/write/update`). So, over three wires and a USB-TTL
adapter, you can re-flash any partition from a backup.

| What a bad write corrupted | Recoverable over UART? |
|---|---|
| appfs (mtd3), rootfs (mtd2), kernel (mtd1), config (mtd4) | **Yes** — interrupt autoboot, `sf write` the backup back |
| **U-Boot (mtd0)** | **No** — no console left → needs a CH341A + SOIC8 clip on the chip |

**Rule that keeps UART as a sufficient net: never write mtd0.** Any experiment writes only mtd3 (appfs).
If U-Boot stays intact, UART alone recovers everything. The CH341A is the deeper backstop, only needed if
mtd0 is ever damaged (e.g. a stray write, or a power cut mid-erase of mtd0).

## 1. Hardware

- **USB-TTL adapter at 3.3 V logic** (CP2102, FT232, CH340, …). **Do not use 5 V logic** — the SoC is 3.3 V
  and 5 V can kill it.
- Three wires: adapter **TX → camera RX**, adapter **RX → camera TX**, **GND → GND**. **Do not connect VCC** —
  the camera is powered by its own USB as usual.
- UART port: `ttySAK0`, **115200 8N1**, no flow control.
- The UART pads are on the camera board (open the case). They are **not yet physically located/labelled** on
  this model — identify TX/RX/GND with a multimeter/scope before connecting (TX idles high at 3.3 V and
  bursts on power-on; GND against the board ground). Record the pad positions here once found.

## 2. Terminal software (Linux PC)

Any of `picocom`, `minicom`, `screen` at `115200`. For the ymodem upload you need `lrzsz` (`sb`/`sz`):

```sh
# picocom with ymodem send bound to C-a C-s:
picocom -b 115200 --send-cmd "sb -vv" /dev/ttyUSB0
# minicom: configure 115200 8N1, hardware/software flow control OFF, then "Send files > ymodem".
```

## 3. Step 0 — prove the console is open (zero risk, do this first)

1. Wire TX/RX/GND, open the terminal, then power the camera.
2. Watch the boot log scroll. As soon as it appears, **press a key repeatedly** (the window is ~1 s;
   if `bootdelay` was set to 0 at the factory, spam keys from the instant power is applied).
3. You should land on the `=>` prompt. If instead it asks for a password, **stop** and plan for the CH341A;
   this unit has a locked console (not expected, but only hardware tells).
4. Sanity-check without writing anything:
   ```
   => sf probe 0:0 20000000 0        # expect "SF: Detected <chip> with page size ..."
   => sf read 0x81000000 0x310000 0x1000   # read first 4 KiB of appfs into RAM
   => md.b 0x81000000 0x10           # expect 68 73 71 73  ("hsqs" = squashfs magic)
   ```
   If `sf probe` detects the chip and `md` shows `hsqs`, UART recovery is fully available.

## 4. Flash map (authoritative recovery offsets)

8 MiB SPI NOR, 4 KiB erase. Offsets are cumulative and match the live `/proc/mtd` and the backup file sizes.

| Partition | Backup file | Offset | Length | Write in recovery? |
|---|---|---|---|---|
| uboot | `mtd0.bin` | `0x000000` | `0x040000` | **NEVER** |
| kernel | `mtd1.bin` | `0x040000` | `0x180000` | only if corrupted |
| rootfs | `mtd2.bin` | `0x1c0000` | `0x150000` | only if corrupted |
| **appfs** | `mtd3.bin` | `0x310000` | `0x490000` | **this is the one we ever touch** |
| config | `mtd4.bin` | `0x7a0000` | `0x040000` | holds settings/keys — write back only to undo a config change |
| factory | `mtd5.bin` | `0x7e0000` | `0x020000` | holds the MAC/calibration — almost never |

Re-confirm against your own backup before writing: `sha256sum mtd*.bin` vs `SHA256SUMS` in the backup dir,
and the offsets above vs the live order in `README.txt`.

## 5. Recovery recipe — restore appfs (mtd3) over UART

The common case: an appfs experiment left the camera not booting. Put `mtd3.bin` (the known-good backup)
back.

```
=> sf probe 0:0 20000000 0
=> loady 0x81000000
        (now, in the terminal, send the file mtd3.bin by YMODEM — picocom: C-a C-s, type the path)
        (U-Boot prints the received byte count; it must equal 0x490000 = 4784128)
=> sf erase 0x310000 0x490000
=> sf write 0x81000000 0x310000 0x490000
=> reset
```

- `0x81000000` is a safe RAM load address (`loadaddr`); appfs is 4.56 MiB and fits in the 64 MiB DRAM there.
- `sf update 0x81000000 0x310000 0x490000` does erase+write in one step (writes only changed blocks) and is
  an acceptable alternative to the explicit `sf erase` + `sf write`.
- **Read-back verify before rebooting** (optional but recommended):
  ```
  => sf read 0x82000000 0x310000 0x490000
  => cmp.b 0x81000000 0x82000000 0x490000     # expect "Total of ... byte(s) were the same"
  ```

To restore any other partition, use its row from §4 (same four commands, swap the file, offset and length).
**Do not run these with mtd0's offset (`0x000000`).**

## 6. Getting the backup file into the upload

`mtd3.bin` lives on the PC in the backup directory. The YMODEM send in step 5 streams it straight from the
PC over the serial line into the camera's RAM; no SD card or network is involved. If `loady` is slow (115200
is ~11 KiB/s, so 4.56 MiB ≈ 7 min), that is normal; raise the console baud only if both ends agree
(`setenv baudrate 921600; saveenv` is **not** worth the risk here — keep 115200).

## 7. If the console never opens or mtd0 is dead

Then UART cannot help and you need a **CH341A programmer with a SOIC8 clip** on the SPI-NOR chip: power the
chip from the clip (camera off), read the whole 8 MiB, write back `mtd0.bin`..`mtd5.bin` (or just the broken
one), re-seat. This is the hardware backstop referenced by PLAN §7bis and should be on hand before any flash
write is attempted, precisely for the mtd0-damage case this document cannot cover.

---
Status: procedure derived from the backup (U-Boot env + command set). **Step 0 must be run on the real camera
to confirm the console is open before relying on this.** No UART hardware has been connected yet.
