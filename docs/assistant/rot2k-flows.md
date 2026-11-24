# The 2K flows of the assistant: reference

How the assistant's 2K commands work inside (checks, gates, files, state). A user who only wants to install a camera reads the
[install guide](../install.md) and the [2K install page](../porting/lsc-rotatable-2k/README.md#install); this page is for
people who want to know exactly what a command does before they run it. Part of [the assistant](../assistant.md).
Hardware status of each command: [testing status](../testing-status.md).

## The card-less 2K installer (`appfs-build`, `appfs-flash`)

Two commands, only for a model of kind `cardless-appfs` (pass `--model lsc-rotatable-2k-ak3918av130`; a 1080p is refused):

* `akam appfs-build --backup-dir DIR [--kit FILE|DIR] [--version X.Y.Z] [--out DIR]` builds the appfs image **in this program** (Linux, macOS and
  Windows, no `mksquashfs`, no shell) from **your own `mtd3.bin`** and the 2K kit. The kit is verified exactly like the one of
  `prepare-2k-sd` and `update`: `--kit FILE` is the signed `.tar.xz` of the release (checksum, signature, archive layout, model),
  with no `--kit` the newest release (or `--version`) is downloaded over HTTPS and its signature is mandatory, and `--kit DIR`
  (an unpacked folder, no signature to check) is for a kit you made yourself (`--allow-unsigned` for a development kit). It is the port of `firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh`:
  the stock appfs with one `init.sh` branch changed (no override card: start Akam from flash; the SD override modes,
  the recovery, stay stock code), the Akam payload under `/appfs/akam`, `bin/yy_cam` replaced by a launcher of the stub.
  Its proofs are those of the script (binaries match their BUILDINFO, ARM, uClibc, pinned busybox, symcheck, dropbear
  programs, squashfs superblock equal in kind to the stock one, image decoded and compared with its tree, 0x490000 bytes).
  Output: `appfs-akam.squashfs`, `.sha256`, `appfs_sign.txt`, `MANIFEST.txt`, the same names as the script.
* `akam appfs-flash --image-dir DIR --backup-dir DIR --ip ADDRESS [...]` is the port of `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh`. **Without
  `--execute` it is a dry run**: local checks (image, sign, backup against its `SHA256SUMS`) and the printed plan, no
  connection. `--execute` needs a terminal, then two answers in order: the recovery acknowledgement (a UART or a CH341A
  programmer is at hand and the backup is verified: only they bring back a half-written `mtd3`) and the typed text
  `<first 12 digits of the image sha256> mtd3`. Steps: (a) fresh dump of `mtd0..mtd5` byte-compared with the backup
  (`mtd4`, the jffs2 config, may differ: warning), (b) camera preflight (real SD card mounted, feeder alive and not
  running from `/appfs`, app stopped, nothing maps `/appfs/lib`), (c) both images staged and md5-checked, then a detached
  on-camera worker that flashes, verifies, and rolls back by itself, (d) read-back to the PC and byte compare (never
  writes), (e) the optional `appfs_sign` nvram write (`--nvram`, off by default). It never reboots: the staged reboot is
  printed. `--current-image FILE` updates an appfs that is already flashed (the rollback source becomes that image);
  `--hot` does it from a card-less boot (one detached on-camera runner, `--hot-preflight` is its reversible rehearsal).
  A failed write is never retried by the program; the message carries the rollback worker commands.
* **Recovery.** The Akam SD card inserted at power-up overrides the flashed boot (stock `debug` mode of `init.sh`). Only a
  half-written `mtd3` needs UART (`sf write`) or a CH341A.

**The 2K kit** (a directory; the signed tarball is opened by `internal/kit` first). It holds only Akam code, never a vendor
file: `MANIFEST.txt` (`model`, `kind=cardless-appfs`, `akam_version`, `source_commit`, `kit_grade`, exactly one
`time64_gate=pass`), `SHA256SUMS` (every other file, checked: any mismatch, missing or extra file refuses the kit),
`appfs/overlay/` (`own_app_stub.sh own_app_run.sh common.sh decloud.sh start_ssh.sh akam_net.sh akam_api_key.sh
akam_flash_boot.sh akam_ntp.sh akam_hostname.sh`) and `appfs/bin/` (`akamd`, `akam-sntp`, `akamd.BUILDINFO`,
`dropbearmulti`, `dropbearmulti.BUILDINFO`, the pinned static `busybox`). The vendor part of the image comes from your backup.
Optional, all hashed in `SHA256SUMS`: `card/` (`yy_debug.ini akam_boot.sh akam/enable_local.sh akam/restore_param.sh`, all or none:
the files of the first-install SD card that are not in `appfs/`), `supported_hardware.txt` and `licenses/*`. The kit is built by
`tools/release/make_kit_2k.sh` (reproducible tarball, `tests/kit2k_test.sh`), signed like the 1080p one, and verified by the same
`internal/kit` code with the 2K layout (`appfs.KitLayout`) before `appfs.LoadKit` applies the 2K checks.

## Install and update of the 2K from the assistant (`prepare-2k-sd`, `update`)

Same on Linux, macOS and Windows, no bash. The journey and the status of each step are in
[assistant/install-update-journey.md](install-update-journey.md).

* `akam prepare-2k-sd [--kit FILE] [--device DEV]` writes the first-install card **from the signed kit** (newest release when no
  `--kit`): the card-less Akam boots from it with **no flash write** and removing it undoes everything. It asks your SSH public key
  and your Wi-Fi (stored on the card as a hex SSID and the 256-bit PSK, never the password), shows the red-box confirmation naming
  the device, formats MBR + FAT32 and **verifies every file by sha256 after the write**. **The card disables the rescue
  telnet by default** (it creates the empty file `akam/no_telnet`; the SSH key on the card is enough). `--rescue-telnet`
  keeps the passwordless root telnet of the card's boot script (port 23, no password prompt, the root password is `root` in RAM, open to your whole LAN while the
  card is in) for a rescue card, behind a red-box confirmation: a card without `akam/no_telnet` is a security risk, remove it
  when you are done. Like `build_card.sh` it never puts a
  `*.bin`, `fac_av130.zip` or `yy_upgrade*` file on the card and checks that `yy_debug.ini` is `mode=debug`.
* `akam backup-2k [--out DIR]` backs the 2K up: `/proc/mtd` must be the 2K layout, each of the six partitions is read twice over SSH
  (read only, nothing is written on the camera) and compared, sizes are checked, `mtd0..mtd5.bin` and `SHA256SUMS` are written
  in a new folder, verified from the files, and recorded for `update` (state key `rot2k_backup`). The folder follows the same
  rules as the 1080p backup ([Backup location](../../tools/assistant/README.md#backup-location)): `<backup folder>/Akam backups/<camera>/<date>[-N]`,
  where the backup folder is the one you chose (`akam backup-location DIR`, or the menu), else Documents, else your home
  folder when Documents is synced; the folders and files are private to your user; `README.txt` and `camera_info.txt`
  (`model=`, `kind=cardless-appfs`, the six partition sizes) are written next to `SHA256SUMS`. `--out DIR` names another new
  (or empty) folder and wins. A folder inside a git tree is refused. In the menu, "Back up / restore" of a 2K offers the
  backup, the folder choice and a check of a backup folder.
* `akam backup-location [show | DIR | default]` (both models) shows or changes the folder that holds `Akam backups`, without a
  menu. `DIR` must exist and must not be inside a git tree; a cloud-synced folder (OneDrive, iCloud Drive, Dropbox...) needs
  a yes, or `--yes`; `default` goes back to Documents (home folder when Documents is synced). It is a setting of the camera in
  use: `--camera NAME` sets it for another camera. `--dry-run` says what would be set.
* `akam import-backup --from DIR [--camera NAME] [--model 1080p|2k] [--date YYYYMMDD] [--record] [--dry-run]` brings a
  backup you already have into the layout above. It recognises the 1080p (`mtdblock0..7.bin`) and the 2K (`mtd0..5.bin`),
  optionally with `SHA256SUMS`, `MD5SUMS` (2K) and `camera_info.txt`. It refuses a partial backup (a partition missing), a
  file of the wrong size for the model, a mix of both models, a link instead of a file, and any `SHA256SUMS` or `MD5SUMS`
  that does not match (a `SHA256SUMS` must list every partition). The source is only read: it is **copied** (never moved or
  deleted) with its other files (`hack_log.txt`, `dump*`, `MD5SUMS`...) into
  `<backup folder>/Akam backups/<camera>/<date>[-N]`; the date is the one in the folder name (`20260314`, `2026-03-14`, Windows
  or Unix paths alike), else the newest date of the partition files, or `--date`. A missing `SHA256SUMS` (computed from the
  partitions), `README.txt` and `camera_info.txt` are written; an existing `camera_info.txt` is kept and gets `imported=1`,
  `imported_from=` and `imported_at=` appended. The copy is read back and verified (each file, the sums, and for the 2K the
  same check `update` uses); a failure removes the copy. An existing destination is never overwritten (the `-N` suffix). The
  same partitions already under the camera (same checksums) are reported and not copied twice. `--record` makes the copy the
  camera's backup: `rot2k_backup` for a 2K (what `update` and `appfs-build` use), `backup_dir` for a 1080p (the setup step is not
  marked done: accept it with `backup --backup-dir DIR`, which compares it with the original firmware). `--dry-run` prints
  every file it would create or copy, and writes nothing.
* `akam seed-2k` (once, after booting from the card, **before** `appfs-flash`): copies `akam/wpa.conf` and
  `akam/authorized_keys` of the card into `/config/akam` of the camera (jffs2, kept across the flash), over SSH. The copy is
  done by the camera: no secret reaches this PC or the screen. It refuses a camera that does not run from the card, or a
  card without both files; a file already in `/config/akam` is left alone (identical: "already seeded"; different: kept
  and reported, never overwritten), so it can be run again. Without it the card-less camera has no Wi-Fi and no SSH
  ([card-less appfs, section 4](../porting/lsc-rotatable-2k/cardless-appfs.md)). Host-tested only.
* `akam key-2k` creates the control-API key for this assistant: the camera makes it (`akam_api_key.sh new --scope admin --name
  assistant`), the secret comes back once over SSH, is stored in the camera's state folder (`api_key`, 0600) and is never printed;
  `update` then reads the model and version. It asks before it writes `/config/akam/api_keys` on the camera. The `prepare-2k-sd` setup questions also cover the speaker, two-way audio and the picture (saturation, denoise): the answers are the file `settings` of the card, copied by `seed-2k` (an existing settings file is never overwritten; the same settings are in the options screen later, groups Picture and Audio). All of it is host-tested only.
* **Camera options of the 2K** (menu item 3, and the commands `led`, `picture`, `video`, `hostname`, `track`, `wifi`,
  `settings list|get|set`) go through the same signed API: `GET /settings` to read, one `PATCH /settings` to change, with the
  key of `key-2k` (no key: the assistant says so and points to `key-2k`, it never makes one). The catalogue is the real registry of
  the camera (host name, Wi-Fi country and band, LED rules, picture, flip/mirror/ceiling, video, tracking, patrol); values are
  checked like the firmware does before anything is sent and the camera's own refusal is shown as it is. Each change says
  applied now, queued (Wi-Fi, name), needs a restart, or not saved; Wi-Fi and name changes ask first (default No, `--yes`).
  `--dry-run` shows the PATCH body. The same registry also holds the service rows (`mqtt.*`, `onvif.*`, `rtsp.user` and the
  write-only `rtsp.password`, `ntp.servers`, `audio.mic_enable`): `mqtt`, `onvif`, `rtsp-auth`, `ntp` and `audio` use them, and the
  secrets are never read back. The commands are host-tested only (a fake signed API); the API rows they use were checked on the camera
  on 2026-11-23 (settings written with a curl helper). See [using the 2K](../porting/lsc-rotatable-2k/using-the-2k.md#5-the-signed-api-port-8092).
* `akam update` on a 2K is one flow (the 1080p flow stays what it was):
  1. detect: model and Akam version from the signed control API `GET /info` (`--api-key-file FILE`, or `api_key` in the camera's
     state folder, or `AKAM_KEY_ID` + `AKAM_KEY_SECRET`); no key or a closed API only means "not read";
  2. artifact: the kit of the newest release (or `--version`, `--kit`), verified; the image is built from **your** backup
     (`--backup-dir`, kept in the state for next time); the image on the camera is the one recorded at the last update (or
     `--current-image FILE` the first time on a camera flashed by the scripts);
  3. what changes: versions, both image hashes, the list of files that differ (`/akam/common.sh changed ...`);
  4. preflight: the hot rehearsal (H1 to H3, stops nothing for good, writes no flash) after its own yes/no;
  5. confirmation: the recovery question (UART or CH341A at hand, verified backup), then the typed `<12 hex of the sha256> mtd3`;
  6. the write by `appfs.Flash` (all the gates of `flash_appfs.sh`: fresh dump equal to the current image byte for byte, card-less
     proof, process census, memory budget, detached worker with its own rollback, read-back compared before any reboot);
  7. you power-cycle the camera (this program never reboots it); it waits, reads `mtd3` back, compares it to the image and
     compares the version the API reports; only then are the images recorded: **current** = new, **previous** = the one before.
     The images are kept in the camera's state folder, `appfs-images/`: `<sha256>.squashfs` and `<sha256>.json` (never changed),
     and ONE `store.json` {current, previous, pending} replaced by a single atomic rename (temporary file, fsync, rename).
     If the run stopped after the write and before this record (Ctrl-C, closed terminal, another IP after the boot), the next
     `akam update` (or `--rollback`) first reads `mtd3` READ ONLY: equal to the pending image, it is recorded and the update goes
     on; equal to the current one, the pending record is dropped; neither, it refuses and shows both hashes. `--current-image
     FILE` is recorded only after a live step (a) compared it with a fresh dump of `mtd3`, never before. After a verified update
     the work folder keeps no dump (the Wi-Fi configuration dump, `mtd4`, is never kept).
  A kit that is **downloaded** (newest release or `--version`) must be signed: a release without `SHA256SUMS.sig` is refused, and
  `--allow-unsigned` only covers a local `--kit FILE`. A development kit (`kit_grade=test`) is accepted only as a local folder
  with `--kit DIR --allow-unsigned`, with a warning. `appfs-flash --execute` records the image it wrote (after its verified
  read-back) so that the first `update` has a current image.
  `--rollback` flashes the previous image with exactly the same steps. `--card` updates from an SD-card boot instead of card-less
  (a dry run replaces the rehearsal), `--force` allows a downgrade, `--yes` answers the rehearsal question and the typed question
  (never the recovery question: that needs `--recovery-at-hand` as well), and a terminal is always required: **no flag writes
  flash on its own**. A failed write is never retried; the message carries the rollback commands, the Akam SD card recovers an
  appfs that mounts, a half-written `mtd3` needs UART or a CH341A. All of this is host-tested only (fake camera, fake card).
  Back to stock on a 2K: there is no `akam restore-2k`; see [back to stock](../porting/lsc-rotatable-2k/README.md#back-to-stock).

**Byte for byte.** The compressed streams of this program's squashfs writer and of `mksquashfs` differ (two xz encoders),
so the images are not the same byte string; what is tested equal against `build_appfs.sh` on the same fixture is the whole
tree (names, types, modes, owners, links, contents), the squashfs parameters, the superblock in kind, the size, the padding,
the creation time and the `appfs_sign` recipe (a test runs the script when `mksquashfs` is installed). The squashfs writer
also had to learn the flags of the stock image (`mksquashfs -xattrs` without xattrs: `NO_XATTRS` clear).
