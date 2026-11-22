# Assistant: flashing the rotatable 2K (design, step 2)

Status: **mostly implemented, historical design**. Built: the camera type and the refusal of the 1080p flows on a 2K, the
backup (`backup-2k`), the card first install (`prepare-2k-sd`), the config seeding (`seed-2k`), the build and the flash of
section 3.4 (`appfs-build`, `appfs-flash`: `internal/installer/appfs`, Go ports of `build_appfs.sh` and `flash_appfs.sh`) and
the update flow (`update`, `update --rollback`); see [assistant.md](assistant.md#install-and-update-of-the-2k-from-the-assistant-prepare-2k-sd-update).
All host-tested only. **Left to do:** the staged-reboot screens of section 3.5, and a `restore-2k` command (back to stock is manual:
[back to stock](porting/lsc-rotatable-2k/README.md#back-to-stock)). The text below is the original design and is not
updated line by line.

Sources of truth (the assistant must not re-implement them, it drives them): [card-less appfs](porting/lsc-rotatable-2k/cardless-appfs.md),
[flash test plan](archive/rot2k-flash-test-plan.md), [UART recovery](porting/lsc-rotatable-2k/uart-recovery.md).

## 1. Principles

1. **Dispatch on the camera type.** The model recorded with the camera (`camera_model`, kind `cardless-appfs`) selects a
   separate flow set (`internal/cli/rot2k_*.go`). No 1080p code path is shared with it: the 1080p backup assumes 8
   partitions and an APP in `mtd7`, the flash writes `mtd7`. The refusal gate (`modelGate`) stays in front of every 1080p
   flow and is only lifted per command, when the 2K implementation of that command exists and is tested.
2. **The assistant never writes flash by itself.** It runs the existing, reviewed scripts (`build_card.sh`,
   `firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh`, `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh`) and shows what they print. Their gates (dry run by default,
   `AKAM_I_HAVE_RECOVERY=yes` plus `--execute`, never reboots, md5/cmp verification, detached worker, rollback) are not
   weakened or bypassed; the assistant supplies them only after the user answered the matching question.
3. **The user confirms each physical step** (insert or remove the card, power cycle, plug the UART). The assistant never
   assumes it happened: it waits for "done" and then checks from the camera (SSH).
4. **Reversible first.** Order of risk: backup (read only) < SD card (nothing in flash) < dry run < real flash. Each
   stage is a checkpoint stored in the camera's state; a refused or failed stage leaves the previous ones valid.
5. **Scripts come from the release, not from the network at run time.** The scripts and the pinned payloads are taken from
   the verified firmware kit of the model (a future `akam-kit-lsc-rotatable-2k-ak3918av130-X.Y.Z`, signed like the 1080p
   one), or from a source checkout (`--firmware-dir`). Anything that is not hash-checked is refused.

## 2. Registry and state

| Item | Value |
|---|---|
| model id | `lsc-rotatable-2k-ak3918av130`, `kind=cardless-appfs` (registry: `tools/release/models/`) |
| validated unit | AK3918AV130, 6 partitions, `mtd3` = appfs `0x490000`, squashfs xz 128 KiB, stock appfs sha256 `74575a6fe3707cd5a077bc7fd419cd394ede95f57d7b622b444f0636e691c0ef` |
| new state keys | `rot2k_stage` (see section 3), `rot2k_backup` (folder), `rot2k_image_sha` (sha256 of the appfs flashed) |
| recognition | the stock appfs sha256 of the `mtd3.bin` of the backup, like `ByAppSHA` for the 1080p; an unknown sha256 is `untested` and needs `--i-understand-untested` |

## 3. Stages (one state machine, resumable)

```
type -> backup -> card (first install) -> seed config -> [dry run -> flash -> verify] -> staged reboot -> done
```

A stage is shown with its risk and re-runnable; "back" never undoes anything on the camera.

### 3.1 Backup (read only)

SSH to the camera booted from the Akam SD card (or any root shell): dump `mtd0..mtd5` (6 partitions, **not** the 8 of the
1080p), write `SHA256SUMS`, verify each against a second read, keep it in the backup folder of the camera
(`akam-backup-lsc-rotatable-2k-<date>`) and optionally a second copy elsewhere (same prompts as the 1080p). Recognise
the model from `mtd3.bin`. A full backup is the **precondition of every later stage**: the flash step refuses without it
(the scripts compare a fresh dump to it byte for byte, except `mtd4`, the jffs2 config).

### 3.2 First install: SD card (`build_card.sh`)

This is how a stock 2K first boots Akam, with **no flash write**. Steps:

1. Ask for Wi-Fi (SSID, password) and the SSH public key (the assistant's own key or the user's), as for the 1080p.
2. Build the card tree with `build_card.sh <dir> --authorized-keys FILE --own-app --wpa-conf FILE [--decloud]`
   (the assistant writes the inputs to private temporary files, never prints them, removes them afterwards).
3. Write it to the card with the existing SD writer (`internal/sdcard`: guarded device choice, FAT32, same
   confirmations and refusals as the 1080p toolkit card). `build_card.sh` itself never touches a block device.
4. **Physical step, confirmed by the user:** power off, insert the card, power on. The assistant waits for SSH (up to ~3
   minutes), then checks `uname`, the mtd layout and the appfs sha256 against the registry. A mismatch stops here.

Outcome: the 2K runs Akam from the card. This is a complete, supported end state; the card-less step below is optional.
**Recovery of every later stage is this card**: it overrides the flashed appfs (`yy_debug.ini` `mode=debug`).

### 3.3 Config seeding (before any flash)

For card-less boot, Wi-Fi and the SSH key live in `/config/akam` (jffs2, survives the appfs flash): `wpa.conf` (0600),
`authorized_keys`, optional `akam2k.conf`, `zones`, `mqtt`, `rtsp.conf`, `ntp`. The assistant writes them over SSH with
the commands of cardless-appfs.md section 4 (small jffs2 writes, `umask 077`, nothing printed) and **lists them back**.
It then tests an SSH login with the key. Without `wpa.conf` a card-less camera loops in resets, so this stage is a hard
precondition of flashing, not a suggestion.

### 3.4 Card-less flash (`build_appfs.sh` + `flash_appfs.sh`)

Prerequisite checklist, each item answered by the user (the assistant cannot verify them):

- UART confirmed open **or** a CH341A + SOIC8 clip on hand (the only way back from a half-written `mtd3`);
- the backup of 3.1 verified, and a copy kept off the PC;
- config seeded (3.3) and SSH login tested **from the SD overlay**;
- the Akam SD card at hand, tested;
- the camera on stable power, nobody will unplug it for several minutes.

Then, in order, each shown before it runs:

1. **Build.** `firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh --backup <backup>/mtd3.bin` (output in a private work dir). Its own proofs (binaries
   match their BUILDINFO and sources, squashfs parameters equal to the stock image, full re-extract and diff) must pass;
   the assistant shows the sha256 and `bytes_used`, and refuses an image whose size is not exactly `0x490000`.
2. **Dry run.** `flash_appfs.sh` without `--execute` (its default). The assistant shows the plan, and the user reads it.
3. **Explicit go.** A separate typed confirmation, naming the sha256 of the image and the partition (`mtd3`, appfs).
   Only then `AKAM_I_HAVE_RECOVERY=yes flash_appfs.sh --execute` (the variable is set only for this child process and only
   after the checklist). `AKAM_NVRAM_WRITE` is **not** set by default: the sign is not checked at boot; it is a separate
   opt-in question that explains it would be the first nvram write on this camera.
4. **Watch.** Progress comes from the script; a lost SSH link is expected to be survivable (detached worker), so the
   assistant reconnects and reads `/tmp/akam_flash.result` (`FLASHED_OK` / `ROLLED_BACK` / `ROLLBACK_FAILED`).
   `ROLLBACK_FAILED` or any read-back mismatch: stop, show the rollback instructions of cardless-appfs.md section 6, and
   **never retry on its own**.
5. **Verify.** The script's step (d) compares `mtd3` read back to the PC with the image before any reboot; the assistant
   only reports it. A failed step (d) is a hard stop (no reboot offered).

The assistant never reboots the camera itself during this stage.

### 3.5 Staged reboot (user confirms each step)

The only order accepted (cardless-appfs.md 6.f):

1. "Leave the Akam SD card **in** the camera, power-cycle it" -> user says done -> the assistant waits for SSH and checks
   that the camera runs the new appfs (`/appfs/akam/akam_flash_boot.sh` present, appfs md5 equals the image) and the
   override still works.
2. "Power off, **remove the card**, power on" -> user says done -> the assistant waits for SSH (card-less), checks Wi-Fi,
   the RTSP/ONVIF ports and the app (the same checks as `akam verify` for the 1080p, adapted to the 2K).
3. "Keep the card at hand for the next days." The state stage becomes `done` only after step 2 passed; until then the
   camera is shown as "flashed, not yet verified card-less".

Each step has a timeout and a clear failure text ("no answer after 4 minutes: the camera did not come back").

### 3.6 Recovery

| Situation | What the assistant says and does |
|---|---|
| boot fails card-less, appfs mountable | "Power off, insert the Akam SD card, power on": the card wins; then offer to re-flash the stock `mtd3.bin` of the backup with the same worker (rollback), after confirmation |
| flash interrupted, camera dead | stop; point to [uart-recovery.md](porting/lsc-rotatable-2k/uart-recovery.md) (`sf write` of the backup over UART) or the CH341A; the assistant cannot help over the network |
| user wants the stock firmware | flash the backup `mtd3.bin` back (rollback worker) from an SD-card boot, then remove the card |
| Wi-Fi never comes up card-less | camera resets every ~2-3 min: insert the card, fix `/config/akam/wpa.conf` over SSH |

## 4. User interface

- New camera: the type question already exists (step 1). For a 2K, the "Set up a new camera" menu entry opens the 2K flow of
  3.2 (card), "Update this camera" offers 3.4 (card-less) only when the camera runs from the card and the checklist is done,
  "Back up / restore" runs 3.1 and the rollback of 3.6.
- Every physical step is its own screen: what to do, what the assistant checks next, the way back. No hidden waits.
- CLI mirror: `akam --camera NAME rot2k-install`, `akam --camera NAME rot2k-flash [--dry-run]` (a rehearsal prints the
  commands and runs nothing, like `--dry-run` today). Non-interactive runs never pass the live gate: `--execute` needs a
  tty and the typed confirmation.
- Logs: the scripts' stdout goes to the camera log, with the existing redaction of secrets (Wi-Fi, keys, passwords).

## 5. Gate changes when step 2 lands

- `Model.NativeFlows()` stays; add `Model.Rot2kFlows()` (kind `cardless-appfs`), and per command a table
  `command -> allowed kinds`. A command not in the table for a kind stays refused ("not yet supported for this model").
- Backup, `prepare-*-sd` and the config seeding are enabled first (reversible, read only or card only). The live flash
  command is enabled last, behind the checklist and the typed confirmation, and only when the model has a `validated`
  record whose stock appfs sha256 matches the backup.
- The registry file stays the single source of the facts above; no hash or layout is duplicated in Go.

## 6. Tests (planned)

Fake camera over SSH (existing `testfake`), with the 6-partition layout: backup writes 6 files and refuses 8; wrong stock
sha256 refused; flash refused without backup, without the checklist, with a non-matching image size, and without the
typed confirmation; `AKAM_I_HAVE_RECOVERY` is never set in a dry run; staged reboot waits for the user and times out;
`ROLLBACK_FAILED` never retries; a 1080p camera is unaffected. The scripts themselves are covered by
`tests/rot2k_*_test.sh`; the assistant tests only check how they are invoked.

## 7. Open points

- Windows and macOS: `build_card.sh` and `build_appfs.sh` are bash and need `mksquashfs`; the assistant runs on Linux first
  (as the bash wizard is the reference for the 1080p). The appfs build is now ported to Go
  (`internal/installer/appfs`, in memory, on the writer of `internal/squashfs`), so it runs on Windows and macOS too.
- Card-less boot has been validated on one unit only; "validated" in the registry therefore means one unit, not a family.
- The UART pads of this model are not located yet (see uart-recovery.md): the checklist item stays a user declaration.
