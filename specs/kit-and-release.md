# Kit and release

Akam ships as a **kit** of its own files plus PC-side assistant binaries. The kit never contains a camera image or any
vendor content. The user's PC combines the kit with the user's own backup to build the image locally. This file covers the
kit, its checksums and signature, how the installer verifies it, how the image is assembled from it, the release
pipeline, and the size budgets.

Status: **host-tested only**. No release is published yet. A kit built from a development tree is stamped
`kit_grade=test`.

There are two release families with independent versions and tags (both start at 0.1.0): the **firmware** (`firmware-vX.Y.Z`:
one kit per camera model, the source) and the **assistant** (`assistant-vX.Y.Z`: the six `akam` binaries). The bash wizard
ships with the source and follows the assistant version; the firmware version is recorded in every kit `MANIFEST.txt` and in
the image.

## 1. Kit contents

A kit is the tarball `akam-kit-<model>-X.Y.Z.tar.xz` for ONE camera model (`<model>` is the model id, see section 4; X.Y.Z is
the firmware version, plain semver, without a prefix; `get-kit` depends on that exact name). Its top directory is
`akam-kit-<model>-X.Y.Z`.

| Path in the kit | Content |
|---|---|
| `bin/ak_rtsp`, `bin/dropbearmulti`, `bin/flash_tool` | Stripped ARM binaries (static musl, built with the pinned Zig). Exactly these three |
| `overlay/` | Files copied into the APP tree: `sbin/ak_rtsp_wrapper.sh`, `sbin/akam_*.sh`, `local/_ht_process.ini`, links `bin/dropbear`, `bin/dropbearkey` |
| `patches/` | `patch_service_sh.sh`, `patch_ethernet_hostname.sh` (host tools run at image build time) |
| `scripts/flash/` | The tracked scripts of `tools/flash_scripts/` (flash and restore helpers) |
| `scripts/hwcheck/akam_hwcheck.sh` | The read-only hardware check |
| `licenses/` | `LICENSE.akam`, `THIRD_PARTY_NOTICES.md`, Dropbear, musl, Zig, SmolRTSP and its four dependencies |
| `supported_hardware.txt` | The registry entry of the kit's model (`tools/release/models/<model>.txt`): validated, reported and untested stock firmware fingerprints |
| `SOURCES.txt` | Source offer: repository, source commit, pinned third-party inputs and Dropbear patches with hashes |
| `MANIFEST.txt` | Build record (below) |
| `SHA256SUMS` | Inner list: every file except `MANIFEST.txt` |

Which Akam files go into the image is listed once, in `tools/release/image_files.txt` (`file <mode> <kit path> <image
path> <source>` and `link <target> <kit path> <image path>`). `tests/kit_test.sh` builds an image with
`build_firmware.sh` and fails if it disagrees with the list.

The kit build refuses forbidden content: any private key, `*_host_key*`, `id_rsa*`, `id_ed25519*`, `*.pem`, `*.squashfs`,
`*.img`, `*.ko`, or a squashfs magic (`hsqs`) in the scripts, overlay, patches or licences.

### MANIFEST.txt

`key=value` lines: `kit_format` (1), `kit_grade` (`release` or `test`), `akam_version` (the firmware version), `model` (the
camera model id), `source_commit`, `built_utc`, `deps_lock_sha256`, `zig_version`, `target` (`arm-linux-musleabi`),
`time64_gate=pass`, the gate tool hash and disassembler, `supported_hardware_sha256`, `supported_hardware` (counts per
status). `model` is required, appears exactly once, follows the model id rule of section 4, must equal the `model=` header of
`supported_hardware.txt` and gives the top directory name (`akam-kit-<model>-<akam_version>`).

`kit_format` stays 1: `model=` was added before the first public release, so no kit without it was ever published, and the
assistant and the verifiers simply require it. Bump `KIT_FORMAT` in `make_kit.sh` and `SUPPORTED_KIT_FORMATS` in
`verify_kit.sh` together when the layout changes incompatibly; installers accept only the formats listed in
`tools/wizard/kit_compat.txt` (and the assistant's own list). `kit_format` is the compatibility contract between an
installer and a kit; the model decides which camera a kit is for.

### Build gates

- `ak_rtsp`, `flash_tool` are rebuilt from clean; `dropbearmulti` is rebuilt with `tools/dropbear/build_dropbear.sh`.
- **time64 gate** (blocking): `tools/build/check_time32.sh --require-263` on `ak_rtsp` and `dropbearmulti`. If it fails no
  kit is written. See [architecture.md](architecture.md) section 8.
- The tree MUST be clean (no uncommitted and no untracked file) and on the commit to release. Anything else
  (`AKAM_KIT_ALLOW_DIRTY`, `--no-build`, a gate override) yields `kit_grade=test`.
- The tarball is reproducible for identical inputs: sorted names, fixed mtime from `SOURCE_DATE_EPOCH`, owner 0/0,
  `xz -9e -T1`. Two separate compilations of `ak_rtsp` are not byte-identical because it embeds a build number and
  timestamp.

## 2. Checksums and signature

A release has **one** `SHA256SUMS` (sorted, `sha256sum` format) covering all its files, and **one** signature
`SHA256SUMS.sig`. A firmware release lists every model's kit, the source tarball and the licences; an assistant release
lists the six binaries and their archives and the licences. A single release cannot hold two files named `SHA256SUMS`, so
the kit check looks up only the kit's own line.

- Signature: `ssh-keygen -Y sign -n akam-release` with a dedicated Ed25519 release key kept outside the repository
  (`sign_kit.sh` refuses a key inside it and never reads or copies it).
- Trust: `tools/release/allowed_signers` in `ssh-keygen` format, principal and namespace both `akam-release`:
  `akam-release namespaces="akam-release" ssh-ed25519 AAAA... akam-release`. The Go assistant embeds a copy; a test fails
  when the two differ. The public key is also quoted in the README and the release notes so it can be cross-checked from
  another channel.

## 3. Verification by the installer (`get-kit`, `verify_kit.sh`)

`get-kit` downloads `akam-kit-<model>-X.Y.Z.tar.xz`, `SHA256SUMS` and `SHA256SUMS.sig` from the GitHub release
`firmware-vX.Y.Z` (HTTPS only; `SHA256SUMS` and the signature are limited to 1 MiB), or uses `--kit FILE`. The model is the
camera's recorded model (or `--model`); without `--version` the newest firmware release that has a kit for it is used.
`verify_kit.sh` (bash 3.2 compatible; nothing from the kit is executed) then checks, for a tarball:

1. The tarball's sha256 matches its line in `SHA256SUMS`.
2. `SHA256SUMS.sig` verifies against `allowed_signers`. An **invalid** signature always fails. No signature fails too unless
   `--allow-unsigned` is given (a loud warning is printed either way).
3. The archive is listed first (only regular files, directories and the two Dropbear links; names limited to
   `A-Z a-z 0-9 . _ + - /`; no absolute or `..` paths; no duplicates; sane modes and total size), then extracted into a
   private temporary directory, and checked as in directory mode. The tarball is copied there first, so what is hashed,
   listed and extracted is one and the same file.

Directory mode: the kit's own `SHA256SUMS` and `MANIFEST.txt` must agree with every file (content, mode, link target), with
nothing missing or extra; `kit_format` supported; one valid `model`, matching the archive directory and the `model=` header
of `supported_hardware.txt`; `time64_gate=pass`; `kit_grade=release` (`--allow-test-kit` otherwise);
`supported_hardware.txt` present. A directory cannot be authenticated, so it needs `--allow-unsigned`.

Options: `--allowed-signers FILE`, `--sums FILE`, `--sig FILE`, `--allow-unsigned`, `--expect-sha256 HEX` (at least 8 hex
digits, cross-checked against the release notes), `--expect-model ID` (the kit must be for that camera model),
`--allow-test-kit`, `--extract-to DIR`. Exit: 0 verified, 1 failed, 2 usage or missing tool, 3 unsigned without
`--allow-unsigned` (everything else fine).

After the kit is verified, image assembly runs ([installer.md](installer.md) section 6): `tools/image/assemble_from_kit.sh`
calls `build_firmware.sh --kit`, which verifies the kit again, fingerprints the user's stock APP against
`supported_hardware.txt`, and installs the files of `image_files.txt`. In kit mode the machine-code time64 check is
replayed on the installed binaries when `gdb` exists (`--recheck-time64`); otherwise the MANIFEST and the hashes are
trusted.

## 4. Camera models and the supported hardware list

Each camera model has a file `tools/release/models/<model-id>.txt` (the registry; the old single file
`supported_hardware.txt` is now the entry of the kit's model, shipped inside the kit under that name). The **model id** is
the file name: lowercase `a-z`, `0-9` and single `-`, 3 to 40 characters, of the shape `<brand>-<form>-<resolution>-<soc>`
(today `lsc-indoor-1080p-ak3918`: LSC Smart Indoor 1080p, article `3215672.3`, Anyka AK3918AV130). It is stable for ever (it
is in the asset names and in every kit); a different board, SoC, partition table or stock firmware family gets a new id, the
same hardware under another article number keeps its id.

A model file has header lines `key=value` (`supported_hardware_format=1`, `model`, `name`, `articles`, `soc`, `flash`,
`layout`; a tool ignores keys it does not know), then one record per line, fields separated by `|`:

`status|device|soc|kernel|mtd_layout|app_squashfs|app_sha256|busybox`

| Status | Meaning |
|---|---|
| `validated` | An image built for this exact fingerprint was flashed and ran on real hardware (boot, SSH, RTSP, Telnet off) |
| `reported` | Someone else reported it working; not reproduced |
| `untested` | May or may not work; the tool warns and asks for an explicit yes |

Anything not listed is unknown and treated as `untested`. Today: one `validated` record (LSC Smart Indoor 1080p
`3215672.3`, kernel `4.4.302-cip94`, 8 partitions with `mtd7=APP 00500000`, squashfs xz block 131072 no BCJ, a recorded
stock APP sha256, BusyBox 1.30.1), one `untested` for the `3215672.2` boards, and a catch-all `untested` line.
The verdict is decided by the stock APP sha256; the kernel line only warns; the partition table must always match.
A model with no validated record gets no kit. A model missing from a newer firmware release simply does not see that
release: the assistant lists only the `firmware-v*` releases that carry `akam-kit-<model>-X.Y.Z.tar.xz` for the camera's
model.

## 5. Release pipeline

Two release families, tags `firmware-vX.Y.Z` and `assistant-vX.Y.Z`, versions independent. Nothing in the pipeline publishes
by itself: `make_release.sh` never signs, tags or uploads, and `publish.sh draft` never signs or publishes.
There is no CI; everything runs on the maintainer's Linux machine, and `tools/ci/local_check.sh` is the local
equivalent (shellcheck, every `tests/*_test.sh`, tracked-file hygiene; it prints `LOCAL CHECK: OK (N tests, M skipped)`).

| Step | Tool | Result |
|---|---|---|
| Release key (once) | `ssh-keygen -t ed25519`, then one commit adding the key to `allowed_signers` and the assistant's embedded copy, and quoting it in the README and release notes | Trust anchor |
| Prepare the commit | Set the version of the family (`tools/release/firmware_version`; or `var version` in `tools/assistant/cmd/akam/main.go` and `AKAM_WIZARD_VERSION` in `./akam`); write the `CHANGELOG.md` section `## [<family> X.Y.Z]`; review `tools/release/models/` and the two notes templates; push to `origin/main` | Clean tree, tag not yet existing |
| Build | `tools/release/make_release.sh firmware X.Y.Z` and/or `assistant X.Y.Z` | `dist/release/<family>-vX.Y.Z/` and `<family>-vX.Y.Z-notes.md` |
| Hardware check | Build an image from each kit (not a development tree); flash one validated camera of that model with a verified stock backup and a UART adapter at hand; run `verify`. Assistant: run the binary against a camera | Real results go into the `CHANGELOG.md` section |
| Draft | `tools/release/publish.sh draft dist/release/<family>-vX.Y.Z` | Draft release with every file except the signature, sha256 checked against the local files |
| Sign and publish | `tools/release/publish.sh sign-and-publish dist/release/firmware-vX.Y.Z dist/release/assistant-vX.Y.Z` | Signed (`sign_kit.sh`, passphrase asked by `ssh-keygen`), checked (`--check-signed`), signature uploaded, assets compared again, published; the assistant release is "Latest", the firmware release is not |

`make_release.sh <family> X.Y.Z` refuses: a dirty tree, a bad version, an existing tag of that family, a default version of
the family that is not `X.Y.Z`, a placeholder or missing key, an embedded `allowed_signers` that differs, a `CHANGELOG.md`
without the section, a directory that already holds a signature, and the build overrides (`AKAM_ALLOW_ANY_GO`,
`AKAM_STRIP`, `ZIG`, `AKAM_TARGETS`, ...). The combined single-tag mode no longer exists.

- *firmware*: runs `make_kit.sh --model <id>` (`AKAM_KIT_VERSION=X.Y.Z`) for every file of `tools/release/models/`, and
  gathers (`gather_release.sh`) the kits, `akam-firmware-X.Y.Z-src.tar.gz` (`git archive`), `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and `SHA256SUMS`; runs `verify_kit.sh --allow-unsigned` on every kit.
- *assistant*: runs `build_assistant.sh` (`AKAM_VERSION=X.Y.Z`) and gathers the six binaries and archives
  (`akam-linux-amd64`, `akam-linux-arm64`, `akam-darwin-amd64`, `akam-darwin-arm64`, `akam-windows-amd64.exe`,
  `akam-windows-arm64.exe`; `.tar.gz` for Linux and macOS, `.zip` for Windows, holding `akam[.exe]`, `LICENSE`,
  `THIRD_PARTY_NOTICES.md`), `LICENSE`, `THIRD_PARTY_NOTICES.md` and `SHA256SUMS`.

Both check every line, refuse any camera image or dump (`*.squashfs`, `*.sqfs`, `*.bin`, `*.img`, `mtd*`), and write the
release notes (template per family, filled with the version, date, key, kit list and the matching `CHANGELOG.md` section).
`make_release.sh <family> X.Y.Z --test` is a development run: any version string and a dirty tree are allowed, everything is
stamped `X.Y.Z-test`, the directory holds `TEST_BUILD_NOT_PUBLISHABLE.txt`, and `sign_kit.sh` and `publish.sh` refuse it.

`--check-signed` (run by `publish.sh sign-and-publish`, or alone: `make_release.sh --check-signed <family> X.Y.Z`) verifies that
the signature checks against `allowed_signers`, that the embedded copy is identical, every `SHA256SUMS` line, that the notes
have no placeholder left and quote the key, and, for the assistant, that all six binaries contain the key and that
`akam-linux-amd64 version` reports `X.Y.Z` (on a Linux amd64 host); for the firmware, that every kit and the source tarball
come from the current commit, that the notes quote each kit sha256 and that `verify_kit.sh` accepts every kit without
`--allow-unsigned`.

`publish.sh` needs the `gh` CLI. It refuses a tree that is dirty, a release not built from the current `origin/main` commit
(the notes record the commit), a test build and an already published release. The release key has a passphrase and never
leaves the maintainer's PC: an agent prepares everything up to the draft, the maintainer runs `sign-and-publish`.

A published release is never modified: a problem means a new version. Never attach a firmware image or a dump.

### Pinned toolchain

`tools/build/deps.lock` pins, with URL and SHA-256: Zig 0.14.1 (linux x86-64 and aarch64), SmolRTSP (commit
`8ae3b5115f7a`) and its four dependencies (Slice99, Metalang99, Datatype99, Interface99), Dropbear 2026.94 (tag commit
`28216cd9af82`), BusyBox for host-side camera-script tests, and the exact Go release `GO_VERSION` (1.27.4) used for the
assistant. `tools/build/bootstrap.sh` installs them into `tools/.toolchain/` (git-ignored). Release builds refuse any other
Go version.

### Assistant build

`tools/release/build_assistant.sh` calls `tools/assistant/build.sh flat` for the six targets with
`CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."` and never the test build tag. It checks that
no checkout or home path is embedded, records the flags, and writes archives with `mkarchive` (standard library only):
fixed mtime from `SOURCE_DATE_EPOCH` (default the commit date), owner 0/0, no names, sorted entries. Reproducibility:
two builds give the same `SHA256SUMS` (for a given Go release, since `compress/flate` output may change between releases).
Signing of Windows and macOS binaries is reserved (`AKAM_SIGN_WINDOWS`, `AKAM_SIGN_MACOS`) and not done. The release notes
label each platform `tested` or `untested`.

## 6. Size budgets

| Artifact | Budget | Notes |
|---|---|---|
| APP image | Exactly 5,242,880 bytes | The squashfs content is about 2.6 MB; the build fails when the image comes within one block of the limit (unless `AKAM_ALLOW_TIGHT=1`) |
| `ak_rtsp` stripped | About 0.33 MB | Features are measured: ONVIF +60 to 110 KB, MQTT about +39 KB (+23 KB without DNS), backchannel about +13 KB. A feature that costs more than twice its estimate, or 200 KB of RAM, stops for a decision |
| Stack protector | Costs +3.1 % on every object (+10.2 KB), over the 2 % budget; applied only to the objects that parse untrusted input (ONVIF, SOAP, HTTP, `rtsp_auth`, `control`): +1.06 % | `_FORTIFY_SOURCE`, RELRO and PIE do not apply to static non-PIE musl |
| RAM | Idle RSS and per-feature deltas are recorded per feature; video frame rate and bitrate must not change beyond measurement noise with all optional features idle | Not yet measured on a camera |
| Go assistant binary | 12 MiB hard, 11 MiB soft per target | Measured 9.5 to 10.8 MiB with go1.27.3 (re-measure with the pinned Go); later features pushed windows/amd64 and darwin/amd64 past 11 MiB |
| Assistant archive | 5 MiB per target | |
| Assistant start | Menu in under 100 ms (`help` under 50 ms); idle RSS under 40 MiB; `talk` under 5 % of one core | Start in about 2 ms measured on the first build |

`tools/assistant/build.sh sizes` checks the binary budget; `sizes --by-package [os/arch]` shows the largest packages. UPX is
not used.

## 7. Tests

`tests/kit_test.sh`, `release_test.sh`, `publish_test.sh` (a fake `gh`), `build_from_kit_test.sh`, `build_firmware_test.sh`, `bootstrap_test.sh`,
`dropbear_patch_test.sh`, `time32_shim_test.sh`, `assistant_test.sh` (six targets, size, reproducibility on two targets),
`readme_links_test.sh`.
