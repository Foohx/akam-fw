# Camyn V1: Build the Image From Your Own Dump

All build steps run on the PC (Arch Linux x86-64 is the reference host) and never touch the camera.
Nothing here ships a vendor image: the APP tree comes from **your own** `mtdblock7.bin`
([backup guide](camyn_v1_backup.md)).

Status: the scripts below compile and run on the build host, and `tests/build_firmware_test.sh` builds
an image from a **synthetic fixture** with stub binaries. No image has been built from a real dump and
nothing has been booted on a camera.

---

## 1. Host tools

```bash
sudo pacman -S --needed squashfs-tools shellcheck file binutils make git curl tar xz bzip2
bash tools/build/check_host.sh
```

`check_host.sh` checks for `make git curl tar xz bzip2 mksquashfs unsquashfs shellcheck file readelf
sha256sum` and prints the missing `pacman` line; it never calls `sudo`. `cmake` is not needed.
`qemu-user` is optional and not required by any script.

## 2. Pinned toolchain and sources

```bash
bash tools/build/bootstrap.sh
```

Reads `tools/build/deps.lock` and downloads, with a SHA-256 check on every download (a mismatch aborts):

| Item | Pin | Location |
|---|---|---|
| Zig | 0.14.1 (x86_64-linux) | `tools/.toolchain/` (git-ignored) |
| SmolRTSP | commit `8ae3b5115f7a77c383080b51cb7fdba1046f862f` | `src/smolrtsp` |
| slice99 / metalang99 / datatype99 / interface99 | v0.7.8 / v1.13.5 / v1.6.5 / v1.0.2 | `src/smolrtsp/build/_deps/` |
| Dropbear archive | 2026.94 | `tools/.cache/` |
| static BusyBox | 1.35.0 (host-side tests only) | `tools/.toolchain/` |

The script is idempotent and refuses to overwrite a locally modified `src/smolrtsp`.

## 3. Build `ak_rtsp` and `flash_tool`

```bash
. tools/build/env.sh                      # exports ZIG, CAMYN_CC, ... and puts the pinned zig on PATH
make -C src/ak_rtsp ak_rtsp flash_tool
file src/ak_rtsp/ak_rtsp                  # expect: ELF 32-bit LSB executable, ARM, EABI5, statically linked
readelf -A src/ak_rtsp/ak_rtsp            # expect ARM926EJ-S / v5TE and no VFP attributes (soft-float)
```

`env.sh` must be sourced in the same shell as `make` (it is also how `make` finds `zig`). The Makefile
default is `zig cc -target arm-linux-musleabi -mcpu=arm926ej_s` and `make CC=...` still overrides it.
`ak_rtsp` is built with `-g`; `build_firmware.sh` installs a **stripped copy** (the build output is
not modified).

## 4. Build Dropbear

```bash
bash tools/dropbear/build_dropbear.sh
```

Output (git-ignored): `dist/dropbear/dropbearmulti`, `dist/dropbear/BUILDINFO.txt`,
`dist/dropbear/LICENSE.dropbear`. The multi-call binary contains `dropbear` and `dropbearkey` only;
`scp` and SFTP are intentionally absent (see the
[SSH guide](camyn_v1_ssh_and_lockdown.md#5-file-transfer-without-scp)). Options
are fixed in `tools/dropbear/localoptions.h` (no password authentication, no forwarding, ed25519 only).
Set `DROPBEAR_PROGRAMS` to change the program list. If you skip this step, `build_firmware.sh` runs it
for you when `dist/dropbear/dropbearmulti` is missing.

## 5. Extract your APP partition

```bash
mkdir -p ~/camyn-work
unsquashfs -d ~/camyn-work/app_extracted ~/camyn-backup/mtdblock7.bin
```

This is `APP_EXTRACTED`: the tree the camera mounts on `/usr` (`bin/`, `sbin/`, `local/`, `lib/`, ...).
`build_firmware.sh` refuses a tree without `bin/`, `sbin/` and `local/`. Read any `unsquashfs` warning
(for example about special files when you are not root): the image is packed with `-all-root`, but a
tree that failed to extract completely is not a valid input.

Before building, look at how the stock firmware launches network daemons. The repository's planned
audit helper `audit_remote_services.sh` is **not in the tree yet** (`build_firmware.sh` skips it when
absent), so do it by hand:

```bash
grep -rnE 'telnetd|utelnetd|ftpd|tcpsvd|bftpd|vsftpd|dropbear|sshd|httpd|lighttpd' ~/camyn-work/app_extracted/sbin ~/camyn-work/app_extracted/local
```

The lockdown patch assumes the daemons are started from `sbin/service.sh` in the APP. Whether that is
true on your unit is a hypothesis ([test plan](camyn_v1_test_plan.md#3-hypotheses-to-verify)).

## 6. Compression: gzip or xz

`CAMYN_SQUASHFS_COMP` selects the SquashFS compressor (`CAMYN_SQUASH_COMP` is accepted as an alias):

| Value | Notes |
|---|---|
| `gzip` (default) | The format the upstream project and users of the upstream flasher report having flashed and booted. The upstream docs describe the **stock** APP as XZ, so the kernel is expected to support both, but only a boot of **your** image proves it. |
| `xz` | Smaller image (built with `-Xbcj arm`), same format as the stock APP. Not exercised on a camera by Camyn. |

Check what your own dump uses: `unsquashfs -s ~/camyn-backup/mtdblock7.bin`. Start with `gzip`
unless the size guard (below) complains. Building both and comparing sizes in `build_report.txt`
is cheap: `CAMYN_SQUASHFS_COMP=xz` with a different `FW_DIR`.

## 7. Build the image

```bash
APP_EXTRACTED=~/camyn-work/app_extracted \
FW_DIR=~/camyn-work/out \
    bash src/ak_rtsp/build_firmware.sh
```

(`make -C src/ak_rtsp firmware` is the same script, with the default `APP_EXTRACTED=src/ak_rtsp/app_extracted`
and `FW_DIR=src/ak_rtsp`; prefer setting `FW_DIR` outside the repository.)

Environment variables (all optional), from the script header:

| Variable | Meaning |
|---|---|
| `APP_EXTRACTED` | Your unsquashed APP tree (default `src/ak_rtsp/app_extracted`) |
| `FW_DIR` | Output directory (default `src/ak_rtsp`) |
| `AK_RTSP_BIN` | `ak_rtsp` binary (default `src/ak_rtsp/ak_rtsp`) |
| `DROPBEAR_MULTI` | Prebuilt `dropbearmulti` (default `dist/dropbear/dropbearmulti`) |
| `CAMYN_SQUASHFS_COMP` | `gzip` (default) or `xz` |
| `CAMYN_ALLOW_UNPATCHED_SERVICE=1` | Build even if `service.sh` could not be patched (Telnet/FTP may then still be launched; read the warning) |
| `CAMYN_ALLOW_TIGHT=1` | Accept an image within one block (131072 bytes) of the partition size |
| `KEEP_PATCHED=1` | Reuse an existing `app_patched/` instead of copying the tree again |
| `SOURCE_DATE_EPOCH` | Fixed timestamp for the SquashFS (default: now) |

What the script does, in order: copies the tree to `$FW_DIR/app_patched/`; removes `bin/anyka_ipc`;
installs the wrapper and the single-entry `_ht_process.ini`, the stripped `ak_rtsp`, the stripped
`dropbearmulti` (plus `dropbear` / `dropbearkey` symlinks), `camyn_ssh.sh`, `camyn_lockdown.sh`,
`LICENSE.dropbear` and a `MANIFEST.txt`; patches `sbin/service.sh`
([lockdown guide](camyn_v1_ssh_and_lockdown.md#6-telnet-and-ftp-lockdown)); checks modes and refuses any
private-key material in the tree; runs `mksquashfs -b 131072 -all-root -noappend`; unpacks the result
and compares type, mode, symlink target and SHA-256 of every entry with the source tree; applies the
size guard; pads with `0xFF` to the partition size; verifies the padding.

**No SSH key of any kind is placed in the image.** Keys come from the microSD at boot.

### Size guard

The APP partition is 5 242 880 bytes. The check runs on the SquashFS **before** padding:

- larger than 5 242 880: fatal, nothing is padded;
- less than 131 072 bytes (one block) free: fatal unless `CAMYN_ALLOW_TIGHT=1`.

The padded image is then checked to be exactly 5 242 880 bytes, to start with the SquashFS bytes and to
contain nothing but `0xFF` afterwards. The free space on a real APP is **unknown until you build from
your dump** (the stock `anyka_ipc`, about 5.9 MB uncompressed, is removed; `ak_rtsp` stripped is about
129 KB and `dropbearmulti` about 213 KB, see the [resource report](camyn_v1_test_plan.md#4-resource-report)).

### Outputs in `FW_DIR`

| File | Content |
|---|---|
| `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` | The padded image, exactly 5 242 880 bytes. This is what gets flashed. |
| `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs.sha256` | `sha256sum` line for the image |
| `build_report.txt` | Compression, SquashFS size, free bytes and %, size of each added file, 10 largest files, SHA-256 of image and binaries, warnings |
| `service_sh.diff` | Exact diff the lockdown patch applied to `sbin/service.sh` |
| `new_app_<comp>.squashfs`, `app_patched/` | Intermediate files |

Read `build_report.txt` and `service_sh.diff` completely. In particular check the `warnings:` count and
that the diff shows the telnet/FTP launch lines commented out with `# CAMYN-DISABLED:`.

### If the build stops on `service.sh`

`patch_service_sh.sh` exits 3 when it finds no telnet/FTP launch line, and 2 when a matching line has a
shape it refuses to guess about. `build_firmware.sh` treats both as fatal unless
`CAMYN_ALLOW_UNPATCHED_SERVICE=1`. Do not set that flag before reading the real `sbin/service.sh` and
understanding why: it means the build-time half of the lockdown did not apply (the runtime half,
`camyn_lockdown.sh`, still runs, but leaves an exposure window).

## 8. Record the hashes

```bash
cd ~/camyn-work/out
sha256sum -c ak_rtsp_firmware_*.squashfs.sha256
```

Copy the SHA-256 somewhere safe (it is checked again before flashing). Also keep the first
`md5sum` the flashing script prints (see [flash guide](camyn_v1_flash_and_restore.md)). The image
embeds your stock APP tree: **do not publish it.**

Next: [SSH provisioning and Telnet/FTP lockdown](camyn_v1_ssh_and_lockdown.md).
