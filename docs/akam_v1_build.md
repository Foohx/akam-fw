# Akam V1: Build the Image From Your Own Dump (advanced, from source)

**Advanced / manual procedure, and optional.** Most people use the prebuilt kit through `./akam` (see the
[README](../README.md#quick-start)): `./akam get-kit` then `./akam image`. Build from source only to audit
or change the code, or if no kit is available (`./akam build` drives the same steps).

All build steps run on the PC (Arch Linux x86-64 is the reference host) and never touch the camera.
Nothing here ships a vendor image: the APP tree comes from **your own** `mtdblock7.bin`
([backup guide](akam_v1_backup.md)).

Status: `tests/build_firmware_test.sh` builds an image from a **synthetic fixture** with stub binaries. Images
built from a real dump (from the kit and from source) were flashed and validated on two cameras between
2026-11-07 and 11-09 (see the [README status](../README.md#status-and-limits)).

---

## 1. Host tools

```bash
bash tools/build/check_host.sh            # checks the host and prints the exact install command
bash tools/build/check_host.sh --json     # same, machine readable (used by the wizard)
```

Supported hosts: Arch Linux and Debian/Ubuntu (x86_64; aarch64 Linux builds but is untested). The
script never calls `sudo`; it prints one of

```bash
sudo pacman -S --needed <packages>        # Arch
sudo apt-get install -y <packages>        # Debian 12 / Ubuntu 24.04
```

`--profile kit|build|all` limits the check (kit: prebuilt-kit path; build: from sources; all, the
default, adds `shellcheck`). `gdb` is a hard requirement of the time64 gate
(`tools/build/check_time32.sh`) and must be able to disassemble ARM: `gdb` on Arch,
`gdb-multiarch` on Debian/Ubuntu (their plain `gdb` is native-only). `cmake`, `python3` and `telnet`
are optional. Debian does not put `/usr/sbin` in a user's `PATH`; the SD-card tools are looked up
there too, since the scripts run them through `sudo`. macOS is not supported yet.

Tested on 2026-11-08 in clean Docker containers (`debian:12`, `ubuntu:24.04`, mawk as `awk`,
shellcheck 0.9.x): the packages `check_host.sh` asks for, then `bootstrap.sh`, `make` in
`src/ak_rtsp`, `tools/dropbear/build_dropbear.sh`, `tools/build/check_time32.sh --require-263` on
`ak_rtsp` and `dropbearmulti`, and `tools/ci/local_check.sh` (ends with `LOCAL CHECK: OK`).
Maintainers can repeat it with `tools/ci/distro_check.sh` (docker or podman, no sudo, read-only
source mount, `--rm`). Host scripts use POSIX awk only, so mawk and busybox awk work.

## 2. Pinned toolchain and sources

```bash
bash tools/build/bootstrap.sh
```

Reads `tools/build/deps.lock` and downloads, with a SHA-256 check on every download (a mismatch aborts):

| Item | Pin | Location |
|---|---|---|
| Zig | 0.14.1 (linux x86_64; aarch64 also pinned) | `tools/.toolchain/` (git-ignored) |
| SmolRTSP | commit `8ae3b5115f7a77c383080b51cb7fdba1046f862f` | `src/smolrtsp` |
| slice99 / metalang99 / datatype99 / interface99 | v0.7.8 / v1.13.5 / v1.6.5 / v1.0.2 | `src/smolrtsp/build/_deps/` |
| Dropbear archive | 2026.94 | `tools/.cache/` |
| static BusyBox | 1.35.0 (host-side tests only) | `tools/.toolchain/` |

The script is idempotent and refuses to overwrite a locally modified `src/smolrtsp`.
`--print-plan` shows what would be fetched for this host without downloading; `--offline` (or
`AKAM_OFFLINE=1`) uses `tools/.cache` only and fails clearly if something is missing. Transient
failures (HTTP 5xx/429, network errors) are retried with exponential backoff.

## 3. Build `ak_rtsp` and `flash_tool`

```bash
. tools/build/env.sh                      # exports ZIG, AKAM_CC, ... and puts the pinned zig on PATH
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
[SSH guide](akam_v1_ssh_and_lockdown.md#5-file-transfer-without-scp)). Options
are fixed in `tools/dropbear/localoptions.h` (no password authentication, no forwarding, ed25519 only).
Set `DROPBEAR_PROGRAMS` to change the program list. If you skip this step, `build_firmware.sh` runs it
for you when `dist/dropbear/dropbearmulti` is missing.

## 5. Extract your APP partition

```bash
mkdir -p ~/akam-work
unsquashfs -d ~/akam-work/app_extracted ~/akam-backup/mtdblock7.bin
```

This is `APP_EXTRACTED`: the tree the camera mounts on `/usr` (`bin/`, `sbin/`, `local/`, `lib/`, ...).
`build_firmware.sh` refuses a tree without `bin/`, `sbin/` and `local/`. Read any `unsquashfs` warning
(for example about special files when you are not root): the image is packed with `-all-root`, but a
tree that failed to extract completely is not a valid input.

Before building, look at how the stock firmware launches network daemons. The repository's planned
audit helper `audit_remote_services.sh` is **not in the tree yet** (`build_firmware.sh` skips it when
absent), so do it by hand:

```bash
grep -rnE 'telnetd|utelnetd|ftpd|tcpsvd|bftpd|vsftpd|dropbear|sshd|httpd|lighttpd' ~/akam-work/app_extracted/sbin ~/akam-work/app_extracted/local
```

The lockdown patch assumes the daemons are started from `sbin/service.sh` in the APP. That holds on the
validated units (Telnet and FTP were closed after boot); on another unit read the output above
([test plan](akam_v1_test_plan.md#3-hypotheses-to-verify)).

## 6. Compression: xz (default) or gzip

`AKAM_SQUASHFS_COMP` selects the SquashFS compressor (`AKAM_SQUASH_COMP` is accepted as an alias):

| Value | Notes |
|---|---|
| `xz` (default) | Same format as the stock APP: xz, 128 KiB blocks, **no BCJ filter** (verified with `unsquashfs -s` on a real dump; ROOTFS is xz too and the running kernel mounts it, so xz SquashFS support is certain). Chosen as the default so the flash uses exactly the stock format, and validated on hardware. `AKAM_XZ_BCJ=1` adds `-Xbcj arm` (about 45 KB smaller, but the stock image does not use it, so kernel support is unproven). |
| `gzip` | Opt-in. The format the upstream project reports having flashed and booted. Kernel analysis of the real KERNEL dump (Linux 4.4.302, `mtdblock4`) found the SquashFS `zlib`, `lzo`, `xz` and `lz4` decompressor tables and `zlib_inflate`, so gzip is expected to work too, but only xz is proven on this unit. |

Sizes measured against a real APP dump (4,739,612 bytes used in the stock xz image): gzip 3,067,904 bytes
(2,174,976 free of 5,242,880), xz with BCJ 2,449,408 (2,793,472 free), xz without BCJ about 2,494,000.

Check what your own dump uses: `unsquashfs -s ~/akam-backup/mtdblock7.bin`. Use the default `xz`
(same format as the stock APP). Building both and comparing sizes in `build_report.txt`
is cheap: `AKAM_SQUASHFS_COMP=gzip` with a different `FW_DIR`.

## 7. Build the image

```bash
APP_EXTRACTED=~/akam-work/app_extracted \
FW_DIR=~/akam-work/out \
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
| `ZIG_LICENSE`, `MUSL_COPYRIGHT` | Licence texts shipped in the image (default: from the pinned Zig tarball) |
| `DROPBEAR_MULTI` | Prebuilt `dropbearmulti` (default `dist/dropbear/dropbearmulti`) |
| `AKAM_SQUASHFS_COMP` | `xz` (default) or `gzip` |
| `AKAM_ALLOW_UNPATCHED_SERVICE=1` | Build even if `service.sh` could not be patched (Telnet/FTP may then still be launched; read the warning) |
| `AKAM_ALLOW_UNSTRIPPED=1` | Install ELF binaries unstripped when no strip tool is found (default: fatal; the pinned `zig` from `tools/.toolchain/` is found automatically) |
| `AKAM_ALLOW_TIGHT=1` | Accept an image within one block (131072 bytes) of the partition size |
| `KEEP_PATCHED=1` | Reuse an existing `app_patched/` instead of copying the tree again |
| `SOURCE_DATE_EPOCH` | Fixed timestamp for the SquashFS (default: now) |

What the script does, in order: copies the tree to `$FW_DIR/app_patched/`; removes `bin/anyka_ipc`;
installs the wrapper and the single-entry `_ht_process.ini`, the stripped `ak_rtsp`, the stripped
`dropbearmulti` (plus `dropbear` / `dropbearkey` symlinks), `akam_ssh.sh`, `akam_lockdown.sh`,
the licence texts under `/usr/local/akam/` (`LICENSE.dropbear`, `.musl`, `.zig`, `.smolrtsp`, its four dependencies, `.akam`, `THIRD_PARTY_NOTICES.md`) and a `MANIFEST.txt`; patches `sbin/service.sh`
([lockdown guide](akam_v1_ssh_and_lockdown.md#6-telnet-and-ftp-lockdown)); checks modes and refuses any
private-key material in the tree; runs `mksquashfs -b 131072 -all-root -noappend`; unpacks the result
and compares type, mode, symlink target and SHA-256 of every entry with the source tree; applies the
size guard; pads with `0xFF` to the partition size; verifies the padding.

**No SSH key of any kind is placed in the image.** Keys come from the microSD at boot.

### Size guard

The APP partition is 5 242 880 bytes. The check runs on the SquashFS **before** padding:

- larger than 5 242 880: fatal, nothing is padded;
- less than 131 072 bytes (one block) free: fatal unless `AKAM_ALLOW_TIGHT=1`.

The padded image is then checked to be exactly 5 242 880 bytes, to start with the SquashFS bytes and to
contain nothing but `0xFF` afterwards. The free space on a real APP is **unknown until you build from
your dump** (the stock `anyka_ipc`, about 5.9 MB uncompressed, is removed; `ak_rtsp` stripped is about
129 KB and `dropbearmulti` about 213 KB, see the [resource report](akam_v1_test_plan.md#4-resource-report)).

### Outputs in `FW_DIR`

| File | Content |
|---|---|
| `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` | The padded image, exactly 5 242 880 bytes. This is what gets flashed. |
| `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs.sha256` | `sha256sum` line for the image |
| `build_report.txt` | Compression, SquashFS size, free bytes and %, size of each added file, 10 largest files, SHA-256 of image and binaries, warnings |
| `service_sh.diff` | Exact diff the lockdown patch applied to `sbin/service.sh` |
| `new_app_<comp>.squashfs`, `app_patched/` | Intermediate files |

Read `build_report.txt` and `service_sh.diff` completely. In particular check the `warnings:` count and
that the diff shows the telnet/FTP launch lines commented out with `# AKAM-DISABLED:`.

### If the build stops on `service.sh`

`patch_service_sh.sh` exits 3 when it finds no telnet/FTP launch line, and 2 when a matching line has a
shape it refuses to guess about. `build_firmware.sh` treats both as fatal unless
`AKAM_ALLOW_UNPATCHED_SERVICE=1`. Do not set that flag before reading the real `sbin/service.sh` and
understanding why: it means the build-time half of the lockdown did not apply (the runtime half,
`akam_lockdown.sh`, still runs, but leaves an exposure window).

## 8. Record the hashes

```bash
cd ~/akam-work/out
sha256sum -c ak_rtsp_firmware_*.squashfs.sha256
```

Copy the SHA-256 somewhere safe (it is checked again before flashing). Also keep the first
`md5sum` the flashing script prints (see [flash guide](akam_v1_flash_and_restore.md)). The image
embeds your stock APP tree: **do not publish it.**

Next: [SSH provisioning and Telnet/FTP lockdown](akam_v1_ssh_and_lockdown.md).
