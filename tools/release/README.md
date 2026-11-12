# Release: maintainer procedure

One release, one tag `vX.Y.Z`. A release holds the camera firmware kit, the `akam` assistant binaries
and the source, with ONE `SHA256SUMS` and ONE signature. The version is written **without** the `v`
everywhere except the tag: `akam-kit-X.Y.Z.tar.xz`, `akam_version=X.Y.Z` in `MANIFEST.txt`, `akam version`.
`akam get-kit` depends on it: it downloads
`https://github.com/Foohx/akam-fw/releases/download/vX.Y.Z/akam-kit-X.Y.Z.tar.xz`, `SHA256SUMS` and
`SHA256SUMS.sig`, where `X.Y.Z` is the assistant's own version. A single release cannot hold two files named
`SHA256SUMS`, so the combined list covers the kit and the assistant files; the kit check only looks up the
kit's own line in it.

The kit contains only Akam's own files (ARM binaries built here, overlay scripts, licences). It never
contains a camera image or any vendor content: the user supplies their own APP dump. User-facing
description: [docs/akam_v1_kit.md](../../docs/akam_v1_kit.md) and
[docs/assistant_install.md](../../docs/assistant_install.md).

There is no CI. Everything below runs on the maintainer's Linux machine. Nothing here publishes by itself:
`make_release.sh` never signs, tags or uploads.

## Prerequisites

`tools/build/bootstrap.sh` done (pinned Zig, SmolRTSP and its 4 dependencies, Dropbear archive, the official Go
of `GO_VERSION` in `tools/build/deps.lock` and the Go modules), plus `gdb` (time64 gate), `make`, `patch`,
GNU `tar`, `xz`, `ssh-keygen`, and `gh` (to create the draft). The tree must be clean (no uncommitted and no
untracked file) and on the commit to release. A kit built any other way (`AKAM_KIT_ALLOW_DIRTY`,
`--no-build`, a gate override) is stamped `kit_grade=test` in `MANIFEST.txt`, and `verify_kit.sh` refuses it
unless `--allow-test-kit` is given: never publish one.

## Procedure

### 0. Release key (once)

```sh
ssh-keygen -t ed25519 -f ~/.ssh/akam_release -C akam-release     # choose a passphrase
```

Keep the private key outside the repository (`sign_kit.sh` refuses a key inside it) and back it up. Then, in ONE
commit before the release build:

1. add one line to `tools/release/allowed_signers`:
   `akam-release namespaces="akam-release" ssh-ed25519 AAAA...base64... akam-release`
   (the key text comes from `cat ~/.ssh/akam_release.pub`);
2. copy it: `cp tools/release/allowed_signers tools/assistant/internal/kit/allowed_signers` (the assistant
   embeds this file; a Go test fails when the two differ);
3. quote the public key in the root `README.md` and in the release notes (`<key>` in
   `docs/release_notes_draft.md`), so it can be cross-checked from another channel.

The assistant binaries embed `allowed_signers`, so they must be built after this commit. `make_release.sh`
refuses a release build while the file has no key line or the embedded copy differs.

### 1. Prepare the commit

Date the `CHANGELOG.md` section (`## X.Y.Z (YYYY-MM-DD)`), finish `docs/release_notes_draft.md`, review
`supported_hardware.txt`, commit. The tree must then be clean.

### 2. Build everything

```sh
tools/release/make_release.sh X.Y.Z          # output: dist/release/vX.Y.Z/ (git-ignored)
```

`X.Y.Z` is plain semver, no `v`. The script refuses a dirty tree, a bad version and a placeholder key; runs
`make_kit.sh` (`AKAM_KIT_VERSION=X.Y.Z`) and `build_assistant.sh` (`AKAM_VERSION=X.Y.Z`); gathers in
`dist/release/vX.Y.Z/` the kit tarball, the six assistant binaries and archives, `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `akam-X.Y.Z-src.tar.gz` (`git archive --prefix=akam-X.Y.Z/ HEAD`) and ONE combined
`SHA256SUMS` (sorted, `sha256sum` format); checks every line; runs `verify_kit.sh --allow-unsigned` on the kit;
refuses any camera image or dump (`*.squashfs`, `*.bin`, `*.img`, `mtd*`, ...); and prints the next commands.

`tools/release/make_release.sh X.Y.Z --test` is a development run: a dirty tree and any version string are
allowed, the output goes to `dist/release/vX.Y.Z-test/` with `TEST_BUILD_NOT_PUBLISHABLE.txt`, the kit is
`kit_grade=test`, and the directory can never be checked as signed. The gathering and the refusals are covered
by `tests/release_test.sh` on synthetic fixtures (`tools/release/gather_release.sh`).

### 3. Sign

```sh
tools/release/sign_kit.sh ~/.ssh/akam_release dist/release/vX.Y.Z      # writes SHA256SUMS.sig
```

`sign_kit.sh` only hands the key path to `ssh-keygen -Y sign -n akam-release`; it never copies or stores the key.

### 4. Verify the signed release

```sh
tools/release/make_release.sh --check-signed X.Y.Z
tools/release/verify_kit.sh dist/release/vX.Y.Z/akam-kit-X.Y.Z.tar.xz
```

`--check-signed` checks that `SHA256SUMS.sig` verifies against `tools/release/allowed_signers`, that the embedded
copy is identical, that every binary contains the key (so it was built after the key was committed), every
`SHA256SUMS` line, and that `verify_kit.sh` accepts the kit without `--allow-unsigned`.

### 5. Draft release

Push the release commit, then create a draft; the tag `vX.Y.Z` is created on that commit when you publish.
`make_release.sh` prints this command with the exact file list:

```sh
gh release create vX.Y.Z --draft --title "Akam X.Y.Z" --notes-file docs/release_notes_draft.md \
    --target <release commit sha> <every file of dist/release/vX.Y.Z/, including SHA256SUMS and SHA256SUMS.sig>
```

Put the kit's sha256 (`grep akam-kit dist/release/vX.Y.Z/SHA256SUMS`) in the release notes and `CHANGELOG.md`, so
users can cross-check it from another channel (`verify_kit.sh --expect-sha256 <first 12 hex>`). Never attach a
firmware image or a dump.

### 6. Review and publish

Review the draft on GitHub: file list (kit, 6 binaries, 6 archives, `LICENSE`, `THIRD_PARTY_NOTICES.md`, source,
`SHA256SUMS`, `SHA256SUMS.sig`), notes, the key and the sha256 quoted. Download one file and check it as a user
would ([docs/assistant_install.md](../../docs/assistant_install.md)). Publish the draft. Later additions:
`gh release upload vX.Y.Z <file>` (then re-sign: the combined `SHA256SUMS` must list every file).

## Details: the kit (`make_kit.sh`)

Run by `make_release.sh`; alone: `AKAM_KIT_VERSION=X.Y.Z tools/release/make_kit.sh` (output `dist/kit/`). The
version has no `v`: the kit is named `akam-kit-<AKAM_KIT_VERSION>` and `akam get-kit` looks for that exact name.

`make_kit.sh` rebuilds `ak_rtsp` and `flash_tool` (`make clean` first) and
`dropbearmulti` (`build_dropbear.sh`, fresh work tree), strips copies, and runs
`tools/build/check_time32.sh --require-263` on `ak_rtsp` and `dropbearmulti`.
If that gate fails, no kit is written. The verdict is recorded in
`MANIFEST.txt` (`time64_gate=pass`).

The tarball is reproducible for identical inputs: sorted names, fixed mtime from
`SOURCE_DATE_EPOCH`, owner 0/0, `xz -9e -T1`. Note that `ak_rtsp` embeds a build
number and timestamp (`gen_version.sh`), so two separate compilations of
`ak_rtsp` are not byte-identical; reproducibility holds for the packaging of
given binaries (this is what `tests/kit_test.sh` checks).

Which Akam files end up in the image (overlay, `service.sh` patch, links) is listed once,
in `image_files.txt`; `tests/kit_test.sh` builds an image with `build_firmware.sh` and
fails if it disagrees with that list. Edit the list, not two scripts.

## Details: the assistant (`build_assistant.sh`)

Run by `make_release.sh`; alone: `AKAM_VERSION=X.Y.Z tools/release/build_assistant.sh` (output `dist/assistant/`,
git-ignored). Prerequisites: `tools/build/bootstrap.sh` done (the official Go of `GO_VERSION` in
`tools/build/deps.lock`, installed in `tools/.toolchain/go-<version>-<os>-<arch>/`, and the Go modules in
`tools/.toolchain/go/mod`); **exactly** that Go is used (any other `go version` is refused; `AKAM_ALLOW_ANY_GO=1` overrides
for a test build, never for a release), clean tree on the commit to release. No network is used.

It builds the six targets through `tools/assistant/build.sh flat` (the single place of the flags:
`CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."`, `-tags nethttpomithttp2`,
never `-tags akamtest`), checks the binaries (no checkout or home path inside, flags recorded by
`go version -m`), then writes, with names without version:
`akam-<os>-<arch>[.exe]`, `akam-<os>-<arch>.tar.gz` (Linux, macOS) or `.zip` (Windows) holding
`akam[.exe]` + `LICENSE` + `THIRD_PARTY_NOTICES.md` (exec bit kept), `LICENSE`,
`THIRD_PARTY_NOTICES.md` and its own `SHA256SUMS`, which `make_release.sh` replaces by the combined one.
Archives come from `mkarchive/` (standard library only):
fixed mtime from `SOURCE_DATE_EPOCH` (default: commit date), owner 0/0, no user or group name, sorted entries.
`AKAM_SIGN_WINDOWS` / `AKAM_SIGN_MACOS` are reserved hooks: nothing is signed yet (see spec 11, 8.3).

Reproducibility: run it twice (second time with `--out` elsewhere) and compare the two `SHA256SUMS`
(`tests/assistant_test.sh` does this for two targets; `tools/assistant/build.sh repro` compares two builds of
each of the six targets; `AKAM_REPRO_FRESH=1` gives the second build an empty build cache). Before the first
public release also compare a run on a second machine. Archives are reproducible for a given Go release
(`compress/flate` output may change between Go releases, hence the exact pin).

Size budget (`tools/assistant/build.sh sizes`): 12 MiB hard, 11 MiB soft per target; archives 5 MiB.
`tools/assistant/build.sh sizes --by-package [os/arch]` shows the biggest Go packages of an unstripped build.
Measured with `go1.27.3-X:nodwarf5` (not the pinned Go; re-measure with the pinned one): about 8.9 to 9.8 MiB per
target. The figure of 14.6 MiB seen once for windows/amd64 comes from a build without `-trimpath -ldflags "-s -w"`
(plain `go build`: 15.65 MB = 14.93 MiB); with the project flags it is 10.1 MiB (9.76 MiB with `nethttpomithttp2`).
Do not use UPX.

Unlike the kit, the binaries are all published, including the platforms that no hardware test has covered yet:
the release notes label each one `tested` or `untested` (spec 11, 7.3).

## Supported hardware (every release)

`supported_hardware.txt` says exactly which cameras and firmware the kit is validated
for. Review it before building: a record becomes `validated` only after an Akam image
for that fingerprint was flashed and run on real hardware. It is copied into the kit and
its hash is recorded in `MANIFEST.txt`. Put the same table in the release notes:

```markdown
## Supported hardware

| Status | Device | Fingerprint |
|---|---|---|
| validated | LSC Smart Indoor 1080p 3215672.3 (board SI D26228), AK3918AV130 | kernel `4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025`; 8 MTD partitions, mtd7 = APP (`00500000`); stock APP squashfs xz, block 128 KiB, no BCJ; stock APP sha256 `14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294`; BusyBox 1.30.1 |
| untested | LSC Smart Indoor 1080p 3215672.2 (boards SI B26101 / C26101) | never run with Akam |
| untested | any other camera, kernel build or stock APP | unknown |

Anything not marked validated may not work or may brick your camera. The setup wizard
warns and asks for an explicit confirmation. A different MTD layout is refused.

## Verify
sha256 of akam-kit-<version>.tar.xz: <64 hex digits>
signature: ssh-keygen -Y verify (public key in tools/release/allowed_signers: <key>)
```

## Files

| File | Role |
|---|---|
| `make_release.sh` | the whole release: kit + assistant + source, one directory, one `SHA256SUMS`; `--test`, `--check-signed` |
| `gather_release.sh` | gathers and checks the release directory (used by `make_release.sh`, tested on fixtures) |
| `make_kit.sh` | build, gate, MANIFEST, SHA256SUMS, tarball |
| `build_assistant.sh` | the six assistant binaries, archives, notices |
| `image_files.txt` | single list of the Akam files installed into the image |
| `supported_hardware.txt` | validated / reported / untested hardware and firmware fingerprints |
| `sign_kit.sh` | `ssh-keygen -Y sign` of `SHA256SUMS` |
| `verify_kit.sh` | user-side verification (checksums, signature, MANIFEST, `kit_format`) |
| `allowed_signers` | trusted public keys (placeholder until the maintainer adds one; the assistant embeds a copy) |

Bump `KIT_FORMAT` in `make_kit.sh` and `SUPPORTED_KIT_FORMATS` in
`verify_kit.sh` together when the kit layout changes incompatibly.
