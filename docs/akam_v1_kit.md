# The Akam release kit

`./akam get-kit` downloads and verifies the kit for you; this page explains what is inside and how the
verification works, for people who want to check it by hand.

The kit lets you build an Akam firmware image without installing a compiler,
Zig, CMake or gdb. You only need `squashfs-tools` and your own dump of the
camera's APP partition.

## What it is, and what it is not

The kit (`akam-kit-<version>.tar.xz`) holds **only Akam's own files**:

| Path | Content |
|---|---|
| `bin/ak_rtsp`, `bin/dropbearmulti`, `bin/flash_tool` | static ARM EABI5 soft-float binaries, stripped |
| `overlay/` | files copied into the image: `ak_rtsp_wrapper.sh`, `akam_ssh.sh`, `akam_lockdown.sh`, `akam_hostname.sh`, `akam_rtsp_auth.sh`, `akam_debug.sh`, `akam_net.sh`, `akam_onvif.sh`, `_ht_process.ini`, `dropbear`/`dropbearkey` links |
| `patches/` | `patch_service_sh.sh` turns Telnet/FTP off in the stock `sbin/service.sh`; `patch_ethernet_hostname.sh` makes DHCP announce the camera's name |
| `scripts/flash/`, `scripts/hwcheck/` | flash, restore and hardware check scripts |
| `licenses/` | Akam (MIT), `THIRD_PARTY_NOTICES.md`, Dropbear, musl, Zig (compiler-rt), SmolRTSP and its four header-only dependencies |
| `supported_hardware.txt` | which cameras and firmware Akam is validated for (see below) |
| `SOURCES.txt` | source offer: commit, pinned upstream URLs and sha256, Dropbear patches |
| `MANIFEST.txt`, `SHA256SUMS` | the checks described below |

It contains **no firmware image, no dump and no vendor file** (Anyka/Tuya
binaries, kernel modules, stock scripts). Your camera's APP partition is
proprietary and cannot be redistributed: you dump it from your own camera, and
the image is assembled on your machine. `flash_tool` is Akam's own MIT-licensed
program (`src/ak_rtsp/flash_tool.c`), covered by the repository's `LICENSE`.

Nothing in the kit is executed on your PC: the binaries are for the camera's
ARM CPU, and the scripts are only copied into the image or used on the camera.

## Verify before use

```sh
tools/release/verify_kit.sh akam-kit-<version>.tar.xz
```

Download `SHA256SUMS` and `SHA256SUMS.sig` into the same folder as the kit. The
script checks, in this order:

1. the kit's sha256 against `SHA256SUMS`;
2. the signature `SHA256SUMS.sig` (`ssh-keygen -Y verify`, identity and namespace
   `akam-release`) against the public key in `tools/release/allowed_signers`;
3. the archive layout, checked **before** anything is extracted: regular files and
   directories plus the two fixed `dropbear` symlinks; no hard link, device, fifo,
   absolute or `..` path, duplicate, odd character in a name, setuid mode or oversized
   content; then every file in `MANIFEST.txt` and the kit's inner `SHA256SUMS`:
   content, mode, nothing missing, nothing extra;
4. `kit_format` is one this version understands, the MANIFEST records
   `time64_gate=pass` and `kit_grade=release`, and `supported_hardware.txt` is the one
   the MANIFEST hashes.

Nothing from the kit is executed. The tarball is copied to a private temporary
directory first, so the file that was hashed is the file that is extracted.

Outcomes:

| Result | Exit |
|---|---|
| everything fine, signature valid | 0 |
| any mismatch, invalid signature, unsupported `kit_format` | 1 |
| fine but **unsigned** (a clear warning is printed) | 3, or 0 with `--allow-unsigned` |

An unsigned kit is only protected against download corruption, not against a
malicious replacement on the same server. Compare the kit's sha256 with the one
in the release notes, the `CHANGELOG.md` and the tag message (another channel):
`--expect-sha256 <first 12 hex digits>` (at least 8). The signature is only worth what the
copy of the repository and public key you hold is worth.

## Supported hardware

Akam is validated for **one** hardware and firmware combination. The machine-readable
list is `supported_hardware.txt` (shipped in the kit, hash recorded in `MANIFEST.txt`;
source: `tools/release/supported_hardware.txt`).

| Status | Device | Fingerprint |
|---|---|---|
| **validated** | LSC Smart Indoor 1080p **3215672.3** (board SI D26228), SoC AK3918AV130 | kernel `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`; 8 MTD partitions, mtd7 = APP (`00500000`); stock APP squashfs **xz**, block 128 KiB, no BCJ filter; stock APP sha256 `14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294`; BusyBox 1.30.1 |
| untested | LSC Smart Indoor 1080p **3215672.2** (boards SI B26101 / C26101) | upstream ak_rtsp ran there; Akam never did |
| untested | any other camera, kernel build or stock APP (including a gzip-compressed stock APP) | unknown |

What the words mean:

- **validated**: an Akam image built for this exact fingerprint was flashed and run on a
  real camera by the maintainer (boot, SSH, RTSP, low CPU, Telnet off); two units so far.
- **untested**: it may work or brick the camera. Akam promises nothing. The setup
  wizard warns and asks an explicit yes/no (red box, default No) before going on.
- A different MTD layout is refused outright.

The stock APP sha256 is that of one camera's dump. It is expected to be identical on
other units with the same firmware, which has not been confirmed yet. The image is
built with the same squashfs compression as your stock APP (xz or gzip).

## About the time64 check

The camera's kernel acknowledges musl's 64-bit time syscalls without executing
them, which would break every clock read ([kernel_time64](akam_v1_kernel_time64.md)).
The maintainer's build refuses to produce a kit unless `ak_rtsp` and
`dropbearmulti` pass `tools/build/check_time32.sh --require-263`; the verdict is
written in `MANIFEST.txt`. You do not need `gdb` to use the kit.

## Build the kit yourself

`tools/release/make_kit.sh` needs the full toolchain (see
[akam_v1_build.md](akam_v1_build.md)). Two builds of the same commit with the same
`SOURCE_DATE_EPOCH` and the same binaries give a byte-identical tarball. The
binaries themselves embed a build number and timestamp, so recompiling gives
different `ak_rtsp` bytes. Maintainer procedure:
[tools/release/README.md](../tools/release/README.md).

## Status

The kit format and tooling are tested on the build host, and kits were used on two cameras.
The kit is used on Linux only (Arch, Debian 12, Ubuntu 24.04); macOS and Windows are not supported.
