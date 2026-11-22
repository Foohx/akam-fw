# The `akam` program: install, check, tested platforms

`akam` is the single-file version of the setup wizard, written in Go, for Windows, macOS and Linux. It covers both
cameras: the 1080p flows (also in the bash wizard) and the 2K flows (`backup-2k`, `prepare-2k-sd`, `seed-2k`, `key-2k`,
`appfs-build`, `appfs-flash`, `update`), which exist **only** in the Go program. On Linux, the bash wizard (`./akam` in
the repository, see the [wizard guide](wizard_guide.md)) remains the **reference for the 1080p**: it is the one used for
real installs and flashes. The Go program is **experimental**: the 1080p install, update, restore and flash from it, and
every 2K command, have passed host tests only. The Go `build` (build from source) and some single steps are not in it yet
(`akam build` tells you to use the bash wizard). What it does: [tools/assistant/README.md](../tools/assistant/README.md).
To run the Go program from the source tree: `tools/assistant/build.sh`, then `tools/assistant/bin/akam`, or
`AKAM_USE_BIN=1 ./akam ARGS...`. Never run the bash wizard and the Go program at the same time (they share the state files).

## Tested platforms

The bash wizard and the Go program were developed and tested on **Arch Linux**. "Built" means the binary compiles and
passes the host tests of its operating-system layer. Nothing is called tested unless it ran against a real camera.

| Platform | Binary built | Run on a real camera | What was tested | Never tested |
|---|---|---|---|---|
| Linux amd64 (Go program) | yes | partly | SSH, 8091 camera options, `video show`, `led`, `ntp`, `talk` | update, restore, telnet backup, flash, SD card writer, first install |
| Linux arm64 | yes | no | host tests only | everything on hardware |
| Windows amd64 | yes | an earlier build | discovery, options over SSH, talk with the PC microphone, clock offer | the current full-screen interface (test pending), SD card writer and UAC prompt, flash, first install |
| Windows arm64 | yes | no | host tests only | everything on hardware |
| macOS Intel (amd64) | yes | no | host tests only | everything on hardware |
| macOS Apple silicon (arm64) | yes | no | host tests only | everything on hardware |
| Go program, 2K commands (any OS) | yes | **no** | host tests with a fake camera and a fake card (`backup-2k`, `prepare-2k-sd`, `key-2k`, `seed-2k`, `appfs-build`, `appfs-flash`, `update`, `update --rollback`) | everything on hardware; a first install from macOS or Windows |
| **Bash wizard, Linux** | n/a (scripts) | **yes, the reference** | real first installs (backup, image, flash), updates over SSH, verify, on two cameras | restore to stock (never rehearsed), Windows and macOS (not supported) |

A **first install from Windows or macOS has never been done**. Until it is, install from Linux (or WSL2 for the build
steps): the Go program is less tested than the bash wizard on every system. Checklists for testers: [hardware test checklists](hardware-test-checklists.md#assistant-checklists).
Evidence: [testing status](testing-status.md).

## Files of a release

The assistant has its own releases, tagged `assistant-vX.Y.Z` (the one marked "Latest" on GitHub). The camera firmware is
a separate release, `firmware-vX.Y.Z`, with its own version: one kit per camera model, the source, the licences and its own
`SHA256SUMS` and `SHA256SUMS.sig`; `akam get-kit` (1080p) or, for the 2K, `prepare-2k-sd`, `appfs-build` and `update` download and verify it for you. Names of the assistant files carry no version:

| Target | Raw binary | Archive |
|---|---|---|
| Linux amd64, arm64 | `akam-linux-amd64`, `akam-linux-arm64` | `.tar.gz` |
| macOS Intel, Apple silicon | `akam-darwin-amd64`, `akam-darwin-arm64` | `.tar.gz` |
| Windows amd64, arm64 | `akam-windows-amd64.exe`, `akam-windows-arm64.exe` | `.zip` |

Also `LICENSE`, `THIRD_PARTY_NOTICES.md`, and ONE `SHA256SUMS` (all files of the release) with `SHA256SUMS.sig`. An archive keeps the executable bit on Unix;
a raw download does not. `akam licenses` prints the notices even for a binary downloaded alone.

## Check the download

Compare the SHA256 of your file with the line in `SHA256SUMS`:

```bash
sha256sum -c --ignore-missing SHA256SUMS        # Linux
shasum -a 256 -c --ignore-missing SHA256SUMS    # macOS
```

```powershell
Get-FileHash -Algorithm SHA256 .\akam-windows-amd64.exe    # Windows: compare by eye with SHA256SUMS
```

Signature check (same scheme as the kit, namespace and principal `akam-release`). Download `SHA256SUMS` and `SHA256SUMS.sig`
next to your file. Take the public key from the release notes (and compare it with the README and `tools/release/allowed_signers`
in the source), then:

```bash
echo 'akam-release namespaces="akam-release" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC2qtKAhTkbHvkklqSiZAdGqq98kQiMmRzoOIBlmjPG1 akam-release' > allowed_signers
ssh-keygen -Y verify -f allowed_signers -I akam-release -n akam-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

The first command must print `Good "akam-release" signature`; the second shows `OK` for your file. `SHA256SUMS` lists every file of the
release. GitHub's automatic "Source code" archives are **not** covered: use `akam-firmware-X.Y.Z-src.tar.gz` of the
firmware release. A camera kit is checked the same way with the `SHA256SUMS` and `SHA256SUMS.sig` of its own
(firmware) release, for example `sha256sum -c --ignore-missing SHA256SUMS` next to `akam-kit-<model>-X.Y.Z.tar.xz`;
`tools/release/verify_kit.sh akam-kit-<model>-X.Y.Z.tar.xz` (`--expect-model <model>` to also check the camera model)
needs GNU tools, so it runs on Linux; `akam get-kit` does the same checks on every system.

## Unsigned binaries

The binaries are not signed with a Windows or Apple certificate yet, so the systems warn. This is expected. Only go ahead
when the SHA256 matches the release.

**Windows (SmartScreen, "Windows protected your PC").** Click More info, then Run anyway. Or right-click the file,
Properties, tick Unblock. Or in PowerShell: `Unblock-File .\akam-windows-amd64.exe`. The UAC prompt of the SD card step says
"Unknown publisher" until the binary is signed. Microsoft Defender may flag a new unsigned program: compare the SHA256, and
report a false positive to Microsoft.

**macOS (Gatekeeper).** A file downloaded with a browser carries the quarantine attribute. Any one of:

- download with `curl -L -o akam-darwin-arm64 <url>` (no quarantine);
- `xattr -d com.apple.quarantine ./akam-darwin-arm64`;
- Control-click the file, Open, once;
- System Settings, Privacy & Security, Allow Anyway.

Then `chmod +x ./akam-darwin-arm64` for a raw download. The arm64 binary carries the ad-hoc signature that the kernel
requires: do not repack or modify it.

**Linux.** `chmod +x ./akam-linux-amd64`.

## Camera type

When you create a camera in the assistant (the first run of the menu, or a new camera chosen in "Change camera"), it
asks the **type** first, from the model registry: "LSC Smart Indoor 1080p" or "LSC Smart Rotatable Camera 2K (Dual
Band)". The answer is stored in the camera's state as `camera_model`, and backup, flash, options, config and health
dispatch on it. From a script, `akam --camera NAME --model ID ...` types a camera that is created by that run (an
existing camera is never retyped; `akam cameras` lists them).

Migration: a camera saved before the type existed has no `camera_model`; it is a **1080p** and nothing is rewritten for it
(its files stay byte for byte as they were). The assistant records the type once it recognises the stock firmware of a
backup, as before.

The installer is chosen by the **firmware kind** of the model in the registry, never by the model id
(`internal/installer`): `native-app` (the 1080p: the APP partition is replaced) is served by `nativeapp` (image, flash,
sdcard), `cardless-appfs` (the 2K: Akam runs from the flash with no SD card) by `appfs`. Any other kind has no installer.
The 1080p backup, restore, flash, SD preparation, kit, option and config commands and menu entries (and the 1080p `update`) stay
refused on a 2K ("not yet supported for this model"): the 1080p installer would brick it. The 2K has its own `update`, below.

### The card-less 2K installer (`appfs-build`, `appfs-flash`)

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
akam_flash_boot.sh akam_ntp.sh akam_hostname.sh`) and `appfs/bin/` (`akam2k_rtsp`, `akam2k_sntp`, `akam2k_rtsp.BUILDINFO`,
`dropbearmulti`, `dropbearmulti.BUILDINFO`, the pinned static `busybox`). The vendor part of the image comes from your backup.
Optional, all hashed in `SHA256SUMS`: `card/` (`yy_debug.ini akam_boot.sh akam/enable_local.sh akam/restore_param.sh`, all or none:
the files of the first-install SD card that are not in `appfs/`), `supported_hardware.txt` and `licenses/*`. The kit is built by
`tools/release/make_kit_2k.sh` (reproducible tarball, `tests/kit2k_test.sh`), signed like the 1080p one, and verified by the same
`internal/kit` code with the 2K layout (`appfs.KitLayout`) before `appfs.LoadKit` applies the 2K checks.

### Install and update of the 2K from the assistant (`prepare-2k-sd`, `update`)

Same on Linux, macOS and Windows, no bash. The journey and the status of each step are in
[assistant/install-update-journey.md](assistant/install-update-journey.md).

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
  in a new folder (the camera's state folder by default), verified from the files, and recorded for `update`.
* `akam seed-2k` (once, after booting from the card, **before** `appfs-flash`): copies `akam/wpa.conf` and
  `akam/authorized_keys` of the card into `/config/akam` of the camera (jffs2, kept across the flash), over SSH. The copy is
  done by the camera: no secret reaches this PC or the screen. It refuses a camera that does not run from the card, or a
  card without both files; a file already in `/config/akam` is left alone (identical: "already seeded"; different: kept
  and reported, never overwritten), so it can be run again. Without it the card-less camera has no Wi-Fi and no SSH
  ([card-less appfs, section 4](porting/lsc-rotatable-2k/cardless-appfs.md)). Host-tested only.
* `akam key-2k` creates the control-API key for this assistant: the camera makes it (`akam_api_key.sh new --scope admin --name
  assistant`), the secret comes back once over SSH, is stored in the camera's state folder (`api_key`, 0600) and is never printed;
  `update` then reads the model and version. It asks before it writes `/config/akam/api_keys` on the camera.
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
  Back to stock on a 2K: there is no `akam restore-2k`; see [back to stock](porting/lsc-rotatable-2k/README.md#back-to-stock).

**Byte for byte.** The compressed streams of this program's squashfs writer and of `mksquashfs` differ (two xz encoders),
so the images are not the same byte string; what is tested equal against `build_appfs.sh` on the same fixture is the whole
tree (names, types, modes, owners, links, contents), the squashfs parameters, the superblock in kind, the size, the padding,
the creation time and the `appfs_sign` recipe (a test runs the script when `mksquashfs` is installed). The squashfs writer
also had to learn the flags of the stock image (`mksquashfs -xattrs` without xattrs: `NO_XATTRS` clear).

## First run

```text
./akam version     # shows the version and GOOS/GOARCH
./akam doctor      # what works on this computer; changes nothing
./akam             # home screen
```

### Audio: what is built in

`./akam say FILE` and `./akam talk` need no external program for audio on Linux and Windows:

| | Linux | Windows | macOS |
|---|---|---|---|
| `say`: WAV, MP3, FLAC, Ogg Vorbis files | built in (found by content, not by file name; any rate or channel count) | built in | built in |
| `talk`: live microphone | built in: PulseAudio protocol client, also served by PipeWire (`pipewire-pulse`) | built in: WASAPI, shared mode, default capture device | needs `ffmpeg` (CoreAudio cannot be used without cgo) |
| Other file formats for `say` | optional `ffmpeg` | optional `ffmpeg` | optional `ffmpeg` |
| Microphone fallback | `ffmpeg` or `arecord` when no PulseAudio/PipeWire server is reachable | `ffmpeg` if WASAPI fails | - |

The microphone source is chosen in this order: `AKAM_TALK_CAPTURE_CMD` (your own command, raw 8 kHz mono s16le on stdout),
the built-in backend, `ffmpeg`, `arecord`. `./akam doctor` prints an `Audio:` line with what will be used. Files longer than
30 minutes are refused by the built-in decoders. On Windows the first use may trigger the microphone privacy prompt
(Settings, Privacy, Microphone, "Let desktop apps access your microphone").

`doctor` names the package of each optional tool for your system (for example `alsa-utils` for `arecord`). The squashfs tools
have no native Windows package: use `--builder native` (experimental, needs none).

The camera options screen (`./akam`, item 3) also shows the camera's Wi-Fi signal and clock. A weak or poor signal (below
-70 dBm) prints advice (move the camera or the access point, or lower the bitrate and key frame interval with
`./akam video bitrate 1024 gop 1`); nothing is changed for you. If the clock is unset (the camera has no battery clock) or off
by more than 2 minutes it offers to set it from this computer (default No; lost at the next reboot unless time sync is on:
`./akam ntp set ntp.example.net`: you name the server, there is no default; a server on your network works too). The end of a first install or update offers time sync too (default No; it asks for the server, and an empty answer changes nothing).

On Windows and macOS the 1080p image can be built with `--builder native` (experimental, not yet validated on hardware), or
built on Linux or in WSL and given with `--image FILE` (see the README of the assistant). Writing a card asks for administrator rights: see "Writing the SD card,
per OS" there.
