# The Akam release kit

The kit lets you build an Akam firmware image without installing a compiler,
Zig, CMake or gdb. You only need `squashfs-tools` and your own dump of the
camera's APP partition.

## What it is, and what it is not

The kit (`akam-kit-<version>.tar.xz`) holds **only Akam's own files**:

| Path | Content |
|---|---|
| `bin/ak_rtsp`, `bin/dropbearmulti`, `bin/flash_tool` | static ARM EABI5 soft-float binaries, stripped |
| `overlay/` | files copied into the image: `ak_rtsp_wrapper.sh`, `akam_ssh.sh`, `akam_lockdown.sh`, `_ht_process.ini`, `dropbear`/`dropbearkey` links |
| `patches/patch_service_sh.sh` | turns Telnet/FTP off in the stock `sbin/service.sh` |
| `scripts/flash/`, `scripts/hwcheck/` | flash, restore and hardware check scripts |
| `licenses/` | Akam (MIT), `THIRD_PARTY_NOTICES.md`, Dropbear, musl, Zig (compiler-rt), SmolRTSP and its four header-only dependencies |
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
3. the archive layout (regular files and two fixed symlinks only, no absolute or
   `..` paths), then every file in `MANIFEST.txt` and the kit's inner
   `SHA256SUMS`: content, mode, nothing missing, nothing extra;
4. `kit_format` is one this version understands, and the MANIFEST records
   `time64_gate=pass`.

Outcomes:

| Result | Exit |
|---|---|
| everything fine, signature valid | 0 |
| any mismatch, invalid signature, unsupported `kit_format` | 1 |
| fine but **unsigned** (a clear warning is printed) | 3, or 0 with `--allow-unsigned` |

An unsigned kit is only protected against download corruption, not against a
malicious replacement on the same server. Compare the kit's sha256 with the one
in the release notes, the `CHANGELOG.md` and the tag message (another channel):
`--expect-sha256 <first 12 hex digits>`. The signature is only worth what the
copy of the repository and public key you hold is worth.

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

The kit format and tooling are tested on the build host. v1.0 will ship the
kit together with the setup wizard; macOS is not supported yet.
