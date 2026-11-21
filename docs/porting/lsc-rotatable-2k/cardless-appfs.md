# Card-less appfs (MT-2): design, build, flash, rollback

Status: **image built and statically checked on the host; nothing flashed, nothing run on the camera.** The write
script exists and is a dry run by default. Staged plan: [flash-test-plan.md](flash-test-plan.md) (MT-2).

## 1. What it is

A replacement for `mtd3` (appfs, squashfs, 0x490000): the **stock appfs unchanged** (drivers, libs, conf,
`wpa_supplicant`, `yy_upgrade`, `start.sh`) plus the Akam payload in `/appfs/akam/`, plus two edits:

| File in the image | Change |
|---|---|
| `/appfs/script/init.sh` | the stock file with **one branch** changed: mode `normal` (no override card) starts `/appfs/akam/akam_flash_boot.sh`. Modes `debug` and `factory` are untouched stock code. |
| `/appfs/bin/yy_cam` | the stock 3.6 MB app is **removed** (1.2 MB compressed: the only way the payload fits) and replaced by a 3-line launcher: `exec /bin/sh /appfs/akam/own_app_stub.sh`. The file must exist because the SD overlay bind-mounts its stub over it. |
| `/appfs/akam/` | `akam_flash_boot.sh`, `own_app_stub.sh`, `own_app_run.sh`, `common.sh`, `decloud.sh`, `akam_ntp.sh`, `start_ssh.sh`, `dropbearmulti`, `busybox` (the card's pinned static one: the camera's own busybox has no `sha256sum`, the stub needs it), `bin/akam2k_rtsp`, `bin/akam2k_sntp` (+ BUILDINFO each). No vendor libs: `/appfs/lib` already has them. |

Consequence of removing `yy_cam`: no stock app, **and no cloud OTA client** (nothing downloads `yy_upgrade.bin`, so
nothing can overwrite the flashed appfs behind our back). Restoring stock = flash the backup `mtd3.bin` back.
Drivers, wifi driver, wpa_supplicant, the ISP/sensor files and every other stock file are byte-identical to the
backup (checked: a full `unsquashfs` of the image differs from the stock tree only in `init.sh`, `yy_cam` and `akam/`).

## 2. How the SD overlay works today (investigated)

1. `rcS` mounts appfs on `/appfs`, config on `/config`, runs `/appfs/script/init.sh &`.
2. `init.sh` reads `/mnt/sdcard/yy_debug.ini` (`mode`, `start_sh`). The Akam card has `mode=debug`,
   `start_sh=/mnt/sdcard/akam_boot.sh`: init runs that **instead of** `start.sh`.
3. `akam_boot.sh` (card): shadow/telnet rescue, `start_ssh.sh`, then, with the card flag `akam/own_app`, copies the
   card's `own_app_stub.sh` to `/tmp` and **bind-mounts it over `/appfs/bin/yy_cam`** (RAM only; the squashfs file
   is untouched), then runs the stock `/appfs/script/start.sh &`.
4. `start.sh` loads **all** drivers, sets the MAC, VM tunables, and runs `/appfs/bin/yy_cam &` when nvram
   `STATUS=active`. That is now our stub: feeder (`busybox watchdog -F -t 5 -T 20`, the card's busybox), Wi-Fi GPIO
   and `atbm6132c.ko`, `wpa_supplicant` with `akam/wpa.conf`, `udhcpc`, health check (else the feeder is killed =
   hardware reset; after 3 unfinished boots the card falls back to the stock app), then `own_app_run.sh`
   (supervisor, verifies sha256 against BUILDINFO, restarts `akam2k_rtsp`).

## 3. Boot logic of the image (recovery-aware)

`init.sh` decides **first thing**, before any Akam code runs (the stock ini read, nothing else):

| `/mnt/sdcard/yy_debug.ini` | What runs | Role |
|---|---|---|
| absent, or `mode=normal` | `akam_flash_boot.sh` (Akam from flash) | normal card-less boot |
| `mode=debug` + `start_sh=...` | the card's `start_sh`, exactly like today | **recovery / override**: the current Akam SD card always wins |
| `mode=factory` | stock code path (unzip of the card's `fac_av130.zip`) | stock behaviour, kept untouched |
| launcher file missing (broken image) | plain `/appfs/script/start.sh` | never worse than stock |

Host-tested with a path-substituted copy (all four rows). Because the check is the first statement and
`akam_flash_boot.sh` runs in the background, **a broken Akam payload cannot stop the override**: whatever is wrong
with `akam_flash_boot.sh`, the stub, the app, Wi-Fi or `/config`, inserting the Akam card (power cycle, card in)
runs the card's code. Failure modes still needing UART/CH341A: the kernel/rootfs, an **unmountable or
half-written appfs** (see section 7), nothing else.

`akam_flash_boot.sh` (flash mode):

1. builds `/tmp/akamsd` (tmpfs), shaped like the overlay card: `akam/<scripts, dropbearmulti, bin>` as symlinks into
   `/appfs/akam`, `busybox` -> `/appfs/akam/busybox`, and symlinks to the user files in `/config/akam` (fixed
   whitelist, so a file in `/config` can never replace an image script);
2. generates the Dropbear host key **once** into `/config/akam` (the only `/config` write it ever does; needs
   `authorized_keys`);
3. **bind-mounts `/tmp/akamsd` over `/mnt/sdcard`**: the shared overlay scripts and the app (it hard-codes
   `/mnt/sdcard/akam/{zones,mqtt,rtsp.conf}`) run **unchanged**, no source change. Side effects: logs
   (`akam_boot.log`, `akam2k.log`, size-capped by the scripts) live in RAM and are lost at reboot; a card
   inserted in normal mode is hidden from the app (no recording anyway);
4. starts `start_ssh.sh` (background), hands off to the stock `start.sh &` (drivers; `yy_cam` = the stub launcher),
   starts `decloud.sh on` in the background (no-op without the flag).

Not done in flash mode, on purpose: passwordless telnet/root shadow rescue (only the SD card offers it),
`enable_local.sh apply` (never writes `/config/param.conf`), nvram writes.
The boot-counter fallback to a stock `yy_cam` does not exist (no stock app in the image): a Wi-Fi that never comes
up resets the camera every ~2-3 minutes until you insert the Akam card. If you use a card **without** the
`own_app` flag with this image, `yy_cam` now runs the image's stub (card paths) instead of the stock app: use the
Akam card with `own_app` and a `wpa.conf` (the normal one).

## 4. Config and secrets: `/config/akam` (jffs2, persistent), never the image

| File | Content | Without it |
|---|---|---|
| `wpa.conf` (0600) | wpa_supplicant config (SSID/PSK) | no Wi-Fi: reset loop (recover with the card) |
| `authorized_keys` | `ssh-ed25519` lines | no SSH |
| `dropbear_ed25519_host_key` | generated on first boot | generated |
| `akam2k.conf`, `zones`, `mqtt`, `rtsp.conf`, `ntp` | same formats as on the card | app defaults / feature off |
| `no_app`, `decloud` | empty flag files | app runs / no WAN block |
| `hostname`, `net` | one-line hostname; `wifi_country=XX\|none` / `wifi_band=2.4\|5` (see [net-settings.md](net-settings.md)) | `akam-rot2k`, none, 2.4 |

**One-time seeding over SSH, BEFORE flashing** (the camera is on the SD overlay now, `/config` is jffs2 and
survives the appfs flash; `/config/akam` is a new directory, nothing existing is touched):

```sh
ssh root@CAM 'mkdir -p /config/akam && chmod 700 /config/akam'
ssh root@CAM 'umask 077; cat > /config/akam/wpa.conf'       < wpa.conf          # never printed, never committed
ssh root@CAM 'umask 077; cat > /config/akam/authorized_keys' < ~/.ssh/id_ed25519.pub
ssh root@CAM 'cat > /config/akam/akam2k.conf' < akam2k.conf                     # optional, also zones/mqtt/ntp...
ssh root@CAM 'ls -l /config/akam; df /config'                                    # 256 KiB partition: keep it small
```

These are small jffs2 writes (config partition, not firmware); jffs2 is journaled, so a power cut cannot corrupt
older files. The card keeps working in parallel: its own `akam/` files are used in override mode.

## 5. Build (reproducible)

Needs `squashfs-tools` (mksquashfs 4.7.5 / unsquashfs, **available on this host**) and the dist directories of the
card build (`make -C platforms/anyka-ak3918av130 poc3-dist ...`, `tools/dropbear/build_dropbear.sh`).

```sh
firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh --backup ~/akam-backup-.../mtd3.bin \
    [--app-dist dist/rot2k-app] [--dropbear-dist dist/dropbear] [--busybox tools/.cache/rot2k/busybox-...-static] \
    [--out dist/rot2k-appfs]
```

It refuses unless the binaries match their BUILDINFO and the **current** sources (same proofs as `build_card.sh`),
unpacks the backup, patches `init.sh` (fails if the stock branch is not found exactly once), adds the payload,
runs `mksquashfs -comp xz -b 131072 -noappend -xattrs -all-root -mkfs-time <stock time> -all-time <stock time>`,
pads with `0x00` to 0x490000, checks that the parameters equal the stock image's (`unsquashfs -s`: xz, 128 KiB,
exportable, compressed inodes/data/ids/fragments/xattr table, tailends not packed, duplicates removed; flags identical to
stock), compares the superblock bytes (magic, block size, compressor, flags, version, xattr start) with the backup,
re-extracts the whole image and diffs it against its source tree, runs `calc_partition_sign.py appfs`, and writes under `dist/rot2k-appfs/`
(git-ignored): `appfs-akam.squashfs`, `.sha256`, `appfs_sign.txt`, `MANIFEST.txt` (inputs, hashes, tool version),
`unsquashfs-s.txt`. Two builds gave the same sha256.

Result of the build in this session (`MANIFEST.txt` has the rest):

| | |
|---|---|
| squashfs `bytes_used` | 4 369 898 (0x42adea); stock 4 510 234; **414 230 bytes free** in the partition |
| file | 4 784 128 bytes = 0x490000 (padded with 0x00, like the stock dump) |
| sha256 | `18cf63aa60b56a38ec05c79674ad6d25f563295e9dd154b284439f12fa59e2f8` |
| `appfs_sign` | `3bc62fb42490a4da8142fd33af676c10` (covers the first 0x42c000 bytes) |
| kernel check | stock mounts the same squashfs 4.0 / xz / 128 KiB on Linux 4.4.302 (rcS), so the decoder is there; the image itself could not be mounted on the host (needs root), it was fully extracted with `unsquashfs` and compared |

The sign is not checked by the boot path (CRC-only `bootm`, [flash-signature-findings](flash-signature-findings.md));
it is set for consistency only.

## 6. Flash, verify, rollback

Script: `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh`. **Default is a dry run** (prints every step).
Live needs `AKAM_I_HAVE_RECOVERY=yes` **and** `--execute`; `--execute` alone is refused. It never reboots.

```sh
CAM=root@CAM_IP BACKUP_DIR=~/akam-backup-... IMAGE_DIR=dist/rot2k-appfs \
    firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh               # dry run
CAM=... BACKUP_DIR=... AKAM_I_HAVE_RECOVERY=yes [AKAM_NVRAM_WRITE=yes] \
    firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh --execute      # live
```

(a) fresh dump of mtd0..5, byte-compare to the backup (mtd4 may differ: warning; anything else = abort);
(b) check feeder alive, RAM, stop supervisor+app (feeder untouched), `/appfs/lib` unmapped;
(c) upload the new image **and the stock backup** to `/tmp` (md5 checked), detached on-camera worker:
`flashcp -v` the image to `/dev/mtd3`, md5 of the partition == md5 of the file, otherwise a rollback of the backup
(3 tries, each `flashcp` then a `dd` to `/dev/mtdblock3` fallback, md5 verified; survives a lost Wi-Fi link). The
worker refuses to flash if an md5 comes out empty (`WORKER_BAD_INPUT`);
(d) read `mtd3` back to the PC (up to 3 tries, full 0x490000 bytes required) and `cmp` it with the image **before any
reboot**. This step is **non-destructive: it never flashes** (a foreground rollback over a dropped ssh link would
half-write mtd3). A complete but different dump, or 3 failed/short reads, abort with instructions; the worker's md5
check on the camera is the authoritative verify;
(e) only after (d), and only with `AKAM_NVRAM_WRITE=yes`: `nvram set appfs_sign <sign>; nvram commit` (old value saved
to the work dir). **This would be the first nvram write ever made on this camera.** It is optional (nothing checks
the sign at boot) and gated separately for that reason.

(f) staged reboot, manual: **first reboot with the Akam SD card inserted** (proves the new appfs mounts and the
override works), **then** power off, remove the card, boot card-less. Keep the card at hand afterwards.

Rollback: if the new image fails its on-camera verify, the detached worker re-flashes the stock `mtd3.bin` itself
(result `ROLLED_BACK` / `ROLLBACK_FAILED` in `/tmp/akam_flash.result`). If the script's step (d) aborts, check over ssh
first (`dd if=/dev/mtd3 bs=65536 | md5sum` against the image md5), and only on a genuine difference launch the
**detached** rollback worker the script uploaded (never a foreground `flashcp`: a dropped link mid-write bricks):

```sh
ssh root@CAM_IP 'nohup sh /tmp/akam_rollback_worker.sh >/tmp/akam_rollback.log 2>&1 </dev/null &'
ssh root@CAM_IP 'cat /tmp/akam_rollback.result /tmp/akam_rollback.log'   # ROLLED_BACK | ROLLBACK_FAILED
```

It md5s `/mnt/sdcard/akam_flash/appfs-backup.bin`, then up to 3 times: `flashcp`, else `dd` to `/dev/mtdblock3`, verifying md5 each
time. If the launching ssh of the main worker returns non-zero, check `/tmp/akam_flash.result` and
`/tmp/akam_flash.log` over ssh; a re-run of the script is safe (step (a) aborts when mtd3 != the stock backup).
After a good boot, or if the camera runs but is unwanted: insert the Akam card (override), SSH in, upload the backup
to `/tmp` and use the same worker; `nvram set appfs_sign <old>` if (e) was done. Seed `/config/akam` BEFORE flashing
(section 4), not after.

### 6b. Update an already flashed appfs (`--update`)

```sh
CAM=... BACKUP_DIR=... IMAGE_DIR=dist/rot2k-appfs \
    firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh --update PREVIOUS/appfs-akam.squashfs   # dry run
CAM=... BACKUP_DIR=... AKAM_I_HAVE_RECOVERY=yes \
    firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh --update PREVIOUS/appfs-akam.squashfs --execute
```

`CURRENT_IMAGE` is the image believed to be on mtd3 now (the previous dist build or a saved copy). Same gates as above
(`AKAM_I_HAVE_RECOVERY=yes` + `--execute`, dry run by default), same detached worker, same non-destructive step (d).
Differences:

- (a) full re-dump of mtd0..5; mtd0/1/2/5 must still equal the **stock** backup (mtd4: warning) and **mtd3 must equal
  `CURRENT_IMAGE` byte for byte**, otherwise abort with nothing written. `CURRENT_IMAGE` must be a full 0x490000
  squashfs and different from the new image.
- Rollback source = the **fresh mtd3 dump** (`$WORK_DIR/mtd3.bin`, == `CURRENT_IMAGE`), staged on the card as
  `appfs-backup.bin`. The stock backup is never flashed back in this mode (a failed update returns to the previous
  Akam image; the stock backup stays the last resort via UART/CH341A).
- **Boot with the Akam SD card inserted first (recovery mode), then run it.** In a card-less boot `/mnt/sdcard` is a
  tmpfs bind, not the card: the real-mmcblk gate refuses ("insert the Akam card and reboot first"), nothing written.
  The card's stub starts the watchdog feeder and the app from the card even over the flashed appfs; preflight (b)
  still aborts if the feeder maps `/appfs/` or anything maps `/appfs/lib` after the supervisor is stopped.
- (e) and (f) as before; after (d) reboot once with the card, then card-less.

### 6c. Hot update from a card-less boot (`--hot`, `--hot-preflight`)

Status: **host-tested only (dry run, helper and runner against a fake `/proc` with a fake PC answering the checkpoints); never run on the camera.** Run `--hot-preflight`
on hardware first.

```sh
CAM=... BACKUP_DIR=... IMAGE_DIR=dist/rot2k-appfs \
    flash_appfs.sh --update PREVIOUS/appfs-akam.squashfs --hot-preflight     # dry run; live = + AKAM_I_HAVE_RECOVERY=yes --execute
CAM=... BACKUP_DIR=... AKAM_I_HAVE_RECOVERY=yes flash_appfs.sh --update PREVIOUS/appfs-akam.squashfs --hot --execute
```

`--hot` only exists together with `--update CURRENT_IMAGE` (same (a) check: mtd3 == CURRENT_IMAGE, rollback source =
the fresh dump). It replaces the real-SD gate by proofs, and writes **only when all of them hold**; each failure
before the write restarts what was stopped and exits with nothing written. `--hot-preflight` runs H1-H3 and then
restores (it skips (a) and never reaches the write). Both keep the gates: dry run by default, live needs
`--execute` and `AKAM_I_HAVE_RECOVERY=yes` (also for the rehearsal, deliberately).

**One detached on-camera job.** The whole camera-side sequence (H1, H2, H3, then for `--hot` the write and verify) is
a single runner, `akam_hot.sh run hot|rehearsal`, started with `nohup` (plus `setsid` if present), fds redirected and
`oom_score_adj` -1000 (only when `/proc/sys/vm/panic_on_oom` is 0; the feeder, `flashcp` and the workers inherit it).
The PC only (1) uploads the helper (md5-checked), (2) starts the runner, (3) polls `/tmp/akam_hot/status`
(`PHASE|last message`) and `run.log`, (4) answers the checkpoints by touching `/tmp/akam_hot/confirm.<name>` over a
fresh ssh: `wpa` (after the wpa_supplicant swap), `dbalt` (through the port-2222 dropbear), `db22` (after the dropbear
swap), `upload` (the PC stages the two images and workers in `/tmp/akam_flash`, md5-checked, after the memory gate) and
`go` (last, before the write). **Dead-man:** a checkpoint not answered within 120 s (`AKAM_CONFIRM_S`) makes the runner
restore by itself and exit, nothing written; so does any failure before the write, a SIGINT/SIGTERM/SIGHUP/exit of
the runner (EXIT trap), and the death of the PC script. Terminal states: `DONE_RESTORED` (rehearsal),
`ABORTED_UNCHANGED`, `ABORTED_RESTORED`, `ABORTED_RESTORE_FAILED`, `WRITTEN|<worker result>`. After the write starts the
runner never waits for the PC (worker and rollback as before). A SIGKILL of the runner is the one case that does not
restore; run `restore` by hand. Single instance: `/tmp/akam_hot/run.lock` holds the runner's pid (a dead pid's lock is
taken over; the PC refuses to start, or to overwrite the helper, while a runner is alive). From the write on the
runner ignores TERM/INT/HUP (inherited by the worker and `flashcp`), the PC polls every 15 s (each poll forks a
dropbear session) and also reads `/tmp/akam_flash.result` and the runner's liveness. The final gate also requires the
flash and rollback worker scripts. The restore stops the port-2222 listener and reports when no `wpa_supplicant`
is alive (`ABORTED_RESTORED_NO_WIFI` / `DONE_RESTORED_NO_WIFI`: power-cycle). The runner gets the ssh port from
`AKAM_HOT_SSH_PORT`. The restore also deletes `/tmp/akam_flash` (frees ~9.3 MB).

| Step | What | Proof / abort rule |
|---|---|---|
| H1 | card-less: no `mmcblk` mounted, `/mnt/sdcard` is `tmpfs`, its `busybox` links to `/appfs/akam/busybox`; flash tools (`flashcp md5sum nohup dd sh cat awk sed ip cut sync mkdir rmdir sleep`) do not resolve into `/appfs` | else abort, nothing touched |
| H1 | scan `/proc`: every process whose exe, cwd, open file or mapping is under `/appfs`; each must be stub, init (the resident stock `/bin/sh /appfs/script/init.sh` loop), supervisor, app, ntp helper, decloud keeper, feeder, dropbear or wpa_supplicant | any other (e.g. `udhcpc`) = abort, nothing stopped |
| H1 | capture command lines (+ `AKAM_*` env) for a restore | an argument or variable with characters outside `A-Za-z0-9_./:=@%+,-` = abort |
| H2 | copy `busybox`, `dropbearmulti`, `wpa_supplicant` to `/tmp/akam_hot`, md5-compared (a copy already in `/tmp` from a rehearsal is md5-checked and replaced when different) | a binary that maps another `/appfs` file (a library) = abort |
| H2 | new feeder from `/tmp` (`oom_score_adj` -1000); alive after 3 s and 7 s while the old one runs. If the driver is exclusive (new one exits) the old one is SIGKILLed and the new one started at once. Then the old one is SIGKILLed and the new one **soaks alone for 25 s** (longer than the watchdog timeout of 20 s) | SIGKILL, never SIGTERM: no magic close, the timer is never disarmed. New feeder dead = old command restarted and **verified alive** (else a CRITICAL message), abort |
| H2 | `wpa_supplicant` restarted from `/tmp` with the same arguments (the link drops for seconds) | checkpoint `wpa`: a fresh SSH round trip from the PC confirms; **dead-man**: without confirmation in 120 s the runner restarts the old binary from `/appfs` itself. Already on `/tmp` (after a rehearsal, nothing runs from `/appfs`): skipped cleanly, no checkpoint |
| H2 | second dropbear from `/tmp` on port 2222 (same host key; ssh uses `HostKeyAlias`), reconnect through it, stop every `/appfs` dropbear (listener and sessions), start a `/tmp` dropbear on 22, check 22, stop the 2222 listener (checkpoints `dbalt` and `db22`) | the stop refuses to run from a session that itself runs from `/appfs` |
| H2 | stop supervisor (it stops the app), app, ntp helper, keeper; SIGKILL the stub (its TERM trap would signal the feeder) and `init.sh` (it only polls `yy_debug.ini` and an OTA trigger) | the feeder from `/tmp` must be alive first |
| H2 | rescan | **zero** processes may touch `/appfs`, else abort + restore |
| H3 | `echo 3 > /proc/sys/vm/drop_caches`, then `MemFree` > 2 x image + 3 MiB = 12416 kB (new image + rollback dump staged in RAM, there is no card) and `/tmp` free > 2 x image + 1 MiB. `MemFree` and `MemAvailable` are logged at every gate (H1, after the feeder, memory gate, final gate) so the rehearsal decides whether the threshold is right | else abort + restore |
| write | checkpoint `upload` (images to `/tmp/akam_flash`), checkpoint `go`, then a **final gate** (rescan = zero, feeder alive, caches dropped, MemFree after staging > 2 MiB), then the worker (inside the runner), then the PC's non-destructive (d). From the worker launch on nothing is restarted and nothing waits for the PC. The rollback worker's lock holds the pid: a lock whose pid is dead (SIGKILL) is taken over | |

After success the script prints the reboot instruction; it never reboots. Do not start anything from `/appfs`
in between (the old squashfs is still mounted, its page cache is stale).

Restore (every pre-write failure, and the end of the rehearsal): supervisor (which restarts the app), ntp helper and
keeper are replayed from the captured command lines. **Not** restored to `/appfs`: the feeder, `wpa_supplicant` and
dropbear stay on their `/tmp` copies (equivalent, and independent of the squashfs); the **stub is not restarted**
(its feeder/Wi-Fi/health job is done), and the stock `init.sh` loop is not restarted either (so no `yy_debug.ini`/OTA polling until a reboot, which restores both). So after a rehearsal the camera works but is not byte-identical to a fresh
boot; a reboot returns to the normal state. Manual restore: `ssh CAM 'sh /tmp/akam_hot/akam_hot.sh restore'` (it detects a live port-22 listener by `/tmp/akam_hot/db22.pid`).

Risks, precisely (what this does NOT remove):

- **The flash window itself** (section 7) is unchanged: power loss or a crash in the `flashcp` write = brick, UART or
  CH341A needed. Hot mode adds no recovery card: a failed new image is rolled back by the worker from the RAM copy; if
  the camera resets mid-write the RAM copy is gone.
- **RAM staging**: both images (9.1 MiB) live in `/tmp` while a camera with little RAM keeps running; an OOM kill of the
  worker or `flashcp` mid-write is the new failure mode (hence the 3 MiB margin and the final gate; still a heuristic).
- **Watchdog handover** (feeder): if the driver is exclusive there is a gap of about one second (timer 20 s), and a
  failed handover can reset the camera before any write (harmless). Whether the Anyka driver allows two opens is
  unknown: the rehearsal reports which case it is.
- **Link drop**: the Wi-Fi is down for a few seconds during the `wpa_supplicant` swap; if it never returns (wrong
  assumptions about `-B`, the driver state) the dead-man restores the old binary, and if even that fails the camera
  has no network until a power cycle (nothing written).
- **Processes the scan cannot name** abort the run by design; a hardware scan showed only the known roles (init.sh included), but `start.sh` may still be
  resident (unverified): the rehearsal tells.
- **Stale squashfs under the running system**: processes that start after the write read the old page cache mixed with
  the new flash; reboot at once. The ssh session itself runs from `/tmp` copies and `/bin`.
- Unverified on hardware: all of H1-H3 (busybox `ls -l /proc/N/{exe,fd}`, `tr '\0'`, `nohup`, the dropbear `-p`/`-P`
  replacement, `ip`/`df` output formats).

## 7. Residual risk

- **Power loss or crash during the `flashcp` window (~1-2 min)**: mtd3 half written, `init.sh` gone with it, no SD
  recovery. The only brick case. Needs UART (U-Boot `sf write` of the backup) or CH341A + SOIC8. The script cannot
  remove it. On-camera worker + RAM copy of the backup shorten the exposure to the write itself.
- Watchdog: the feeder is a separate process; a 1-2 min flash is a long window for a 20 s timeout if the system
  starves it (not seen in MT-micro, which was 4 KiB). Untested for the full partition.
- Running processes read the squashfs while it is rewritten (e.g. `wpa_supplicant` from `/appfs/bin`): mixed
  cache until the reboot. Reboot right after (d). Do not start anything from `/appfs` in between.
- Unverified on hardware: the bind mount over `/mnt/sdcard` on the camera's busybox `mount`, the `ln -sfn` forms,
  that the app runs with `/tmp` logs, `STATUS=active` gating `yy_cam` (as for the card today), boot time.
- Recovery card caveats (review): the ini parse is the stock one, so a card whose `yy_debug.ini` has spaces around
  `=`, a UTF-8 BOM, or `Mode=Debug`-style value case is NOT recognised (stock behaviour: normal/undefined), and with
  this image "normal" means Akam from flash. Use only the ini written by `build_card.sh` (`mode=debug`, LF). The
  stock app is gone, so any card whose `akam_boot.sh` does not bind-mount its own stub (no `own_app` flag, or the
  3-unfinished-boots guard tripped) now runs the image's stub: without a valid `akam/wpa.conf` on that card the
  camera resets every ~2-3 min (feeder killed) instead of falling back to the stock app. Fix the card's `wpa.conf`
  on the PC. A rescue-only card (telnet) gives no network on this image.
- `flash_appfs.sh` without `--update` requires mtd3 == the stock backup (first flash only). To update an already
  flashed appfs use `--update` (section 6b). From a card-less boot only with `--hot` (section 6c); plain `--update` refuses: the feeder there runs from
  `/appfs/akam/busybox` and `/mnt/sdcard` is a tmpfs bind.
- Changed assumptions: no stock app in the image (restore = backup), RAM logs only.

## 8. Pre-flight checklist

- [ ] UART confirmed open **or** CH341A + SOIC8 + a clip on hand, tested to read the chip ([uart-recovery](uart-recovery.md))
- [ ] full backup mtd0..5 verified against `SHA256SUMS`; copy kept off the PC too
- [ ] `/config/akam` seeded (`wpa.conf`, `authorized_keys`) and an SSH login with the key tested **from the SD overlay**
- [ ] Akam SD card at hand, tested, with `own_app`, `wpa.conf`, keys
- [ ] image rebuilt from clean inputs; sha256 equals `MANIFEST.txt`; dry run read end to end
- [ ] camera on stable power (UPS or at least no planned cut), wired to a good Wi-Fi spot, nobody unplugs it
- [ ] `AKAM_I_HAVE_RECOVERY=yes` set consciously; decision on `AKAM_NVRAM_WRITE` made
- [ ] after (d): reboot with the card first, then without; keep the card for the following days

## 9. Why there is no on-camera mount test before the reboot (review decision)

The camera kernel has no loop device, no ramdisk and no spare partition, so the new squashfs cannot be mounted
anywhere but over the flashed mtd3. A "mount /dev/mtdblock3 on /tmp/x" step was considered and rejected:

- It is vacuous. `mount_bdev` reuses the superblock of a block device that is already mounted (same fs type, same
  ro flag): mounting `/dev/mtdblock3` again while `/appfs` is mounted returns the OLD superblock, not a fresh parse
  of the new bytes. It would print "success" for an image that never mounted.
- Making it real needs the old `/appfs` unmounted (kills `wpa_supplicant`, i.e. Wi-Fi and the SSH path) or a
  `drop_caches` that lets running `/appfs` users fault in the NEW bytes through the OLD inode tables (SIGBUS).

Instead the risk is removed structurally: the image has the same kind as the stock squashfs (same superblock
fields and flags, xz, 128 KiB, xattr table present, checked by `build_appfs.sh` against the backup) and the whole
image is re-extracted and diffed on the host. The on-flash check is the byte-for-byte `cmp` of step (d).

RTSP: the own app serves `rtsp://<ip>:554/stream0` (main) and `/stream1` (sub) only (strict paths), the same URL scheme as the 1080p; the stock yy_cam used :8554 (see stock-firmware-reference.md). Nothing else in the Akam image binds 554 (stock yy_cam does not run; dropbear is on 22; the 2K has no ONVIF).
