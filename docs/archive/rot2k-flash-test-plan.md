# Card-less flash: staged test plan (rot2k)

> Archived document (2026-11): historical, kept for its findings. Paths and status may predate the repository reorganisation (old to new table: `tools/dev/path-map.txt`); current docs are in [docs/README.md](../README.md).

Goal: eventually boot Akam from internal flash (appfs/mtd3) with no SD card. This file is the **staged,
reversible** path to get there, ordered from zero-risk to brick-risk. **Nothing here has been run.** Each
step is gated on an explicit user "go".

## The one risk that governs everything

Our SD recovery hook (`/appfs/script/init.sh`, which launches our overlay) lives **inside appfs**. So a
**corrupt appfs removes the SD recovery path too**: a half-written appfs → no `init.sh` → no SSH → soft-brick
that only **UART** (console `sf write` of the backup) or a **CH341A** clip can fix. Everything below is
designed around that fact.

Facts established statically (2026-11-15, all read-only):
- Tools on camera: `flashcp` (erase+write+verify), `flash_eraseall`, `dd`. No single-sector `flash_erase`.
- Watchdog is a **separate** busybox feeder (pid of `busybox watchdog -F -t 5 -T 20`), 20 s timeout, independent
  of the app. Stopping the app does **not** disarm it; keep that feeder alive during any write.
- The running app mmaps `/appfs/lib/*.so` → **stop the app before writing mtd3**.
- Current mtd3 == backup `mtd3.bin`, byte-identical (known-good image on hand).
- U-Boot normal boot does **not** verify a signature (CRC-only `bootm`); the signature is forgeable and set by
  the yy_upgrade path. See [flash-signature-findings.md](../porting/lsc-rotatable-2k/flash-signature-findings.md).
- appfs squashfs `bytes_used = 0x44d21a`; partition `0x490000`; the last eraseblock `0x48f000` (index 1167) is
  in the **unused padding tail** (~264 KiB free above `bytes_used`).

## Recovery matrix

| Broken by a test | Recoverable without UART/CH341A? | How |
|---|---|---|
| nothing (MT-0) | n/a | no flash write |
| padding tail of appfs (MT-micro) | **Yes** | squashfs `[0, bytes_used)` intact → still boots → rewrite over SSH |
| whole appfs, write completed but bad | **Yes** | read-back verify before reboot; rewrite from backup while still up |
| whole appfs, write interrupted (power/watchdog) | **No** | UART `sf write mtd3.bin`, else CH341A |
| uboot (mtd0) | **No** | CH341A only |

## MT-0 — dry run, no flash write (zero risk) — DONE 2026-11-15, PASSED

Rehearse the choreography only: stop `akam2k_rtsp` (keep the watchdog feeder), confirm nothing maps
`/appfs/lib`, re-dump mtd3 and check it still equals the backup, restart the app via the supervisor. Proves the
stop/restart dance without touching flash. Result: watchdog feeder stayed alive, stopping supervisor+app freed `/appfs/lib` (safe write window), mtd3 still == backup, clean reboot restored the stack.

## MT-micro — one identical padding block (near-zero risk) — DONE 2026-11-15, PASSED

Rewrite eraseblock 1167 (`0x48f000`, 4 KiB) of appfs **with its own current bytes**, via the mtdblock layer,
then read it back and compare. It sits in unused padding, so even a power cut mid-write leaves the mountable
squashfs intact. Proves the NOR erase+write path works and verifies. It does **not** prove a modified appfs
boots. Result: eraseblock 1167 (0x48f000) erase+write+verify via mtdblock succeeded; whole-appfs md5 unchanged (38c52e0a…), so booting unaffected. NOTE: the on-device preflight uses a whole-partition md5 match as the padding gate (busybox `od` lacks `-A/-t`). Residual risk: negligible (≈30 ms window, unused
region, SD-recoverable).

## MT-1 — identical full appfs rewrite (real write; wants UART)

`flashcp` the byte-identical appfs image back onto mtd3, read-back verify, reboot, confirm the overlay still
comes up. Proves the full write+reboot pipeline on the real partition with zero content risk. **Residual risk:
power loss during the ~1–2 min erase+write → unmountable appfs → UART needed.** Recommendation: do MT-1 only
with a UART cable connected (≈5 €), after MT-micro. Then UART makes it fully safe.

## MT-2 — boot our own appfs, card-less (the goal; needs the net)

Build an Akam appfs image (our app + scripts baked into the squashfs, replacing the SD overlay role), compute
its sign with `tools/calc_partition_sign.py`, write mtd3, set `appfs_sign`, reboot without the card. This is the
real brick-risk step (new content + the power-loss window). **Prerequisites, non-negotiable:** UART confirmed
open (its step 0) AND a CH341A+SOIC8 on hand, full backup, read-back verify, explicit go. Keep the SD overlay
as the fallback until MT-2 is proven over several reboots.

**Prepared (not run):** the image, the boot logic, the `/config/akam` seeding, the write+verify+rollback script and the
pre-flight checklist are in [cardless-appfs.md](../porting/lsc-rotatable-2k/cardless-appfs.md) (`appfs/build_appfs.sh`, `tools/flash_appfs.sh`,
dry run by default). Unlike what the first risk paragraph assumes, the image keeps the SD override: `init.sh` checks the
card first, so the Akam card still wins on a *mountable* appfs; only a half-written appfs needs UART/CH341A.

## Recommendation

MT-0 and MT-micro can be run now at essentially no risk and confirm the mechanics. **MT-1 and MT-2 should wait
for a UART cable** — it is the cheapest possible removal of the only irreducible risk (a power cut mid-write),
and it turns "possible brick" into "always recoverable".
