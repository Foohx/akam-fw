# Release: maintainer procedure

Two kinds of release, each with its own version, tag and assets. Both start at 0.1.0.

| | Tag | Assets | Version lives in |
|---|---|---|---|
| **Firmware** | `firmware-vX.Y.Z` | one kit per camera model `akam-kit-<model>-X.Y.Z.tar.xz`, `akam-firmware-X.Y.Z-src.tar.gz` (the source, with the bash wizard), `LICENSE`, `THIRD_PARTY_NOTICES.md`, `SHA256SUMS`, `SHA256SUMS.sig` | `tools/release/firmware_version`; recorded as `akam_version` in every kit `MANIFEST.txt` and in the image (`/usr/local/akam/MANIFEST.txt`) |
| **Assistant** | `assistant-vX.Y.Z` | the six binaries `akam-<os>-<arch>[.exe]` and archives, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `SHA256SUMS`, `SHA256SUMS.sig` | `var version` in `tools/assistant/cmd/akam/main.go`; the bash wizard `./akam` follows it (`AKAM_WIZARD_VERSION`) |

The version is written **without** the prefix everywhere except the tag. `akam get-kit` downloads
`https://github.com/Foohx/akam-fw/releases/download/firmware-vX.Y.Z/akam-kit-<model>-X.Y.Z.tar.xz`, `SHA256SUMS` and
`SHA256SUMS.sig`; the firmware version is independent of the assistant's, and the assistant lists only the
`firmware-v*` releases that carry a kit for the camera's model. The "Latest" mark on GitHub goes to the assistant
release (users download it); firmware releases are published with `--latest=false`.

A kit contains only Akam's own files (ARM binaries built here, overlay scripts, licences). It never contains a camera
image or any vendor content: the user supplies their own APP dump. User-facing description:
[docs/manual/kit.md](../../docs/manual/kit.md) and [docs/assistant.md](../../docs/assistant.md).

There is no CI for releases. Everything below runs on the maintainer's Linux machine, except that an AI agent may
prepare the commit, the builds and the draft: it cannot sign, because the key has a passphrase and never leaves the
maintainer's PC.

## The whole process

1. **Prepare** the release commit and push it to `origin/main` (details below).
2. **Build**: `tools/release/make_release.sh firmware X.Y.Z` and/or `tools/release/make_release.sh assistant X.Y.Z`
   (output `dist/release/<family>-vX.Y.Z/` and `<family>-vX.Y.Z-notes.md`, git-ignored).
3. **Hardware check** by hand (firmware: an image from each kit on ONE validated camera of that model, then
   `./akam verify`; assistant: the binary of your platform against a camera).
4. **Draft**: `tools/release/publish.sh draft dist/release/<family>-vX.Y.Z` (as many times as needed, it is idempotent).
   Review the draft on GitHub.
5. **Sign and publish**:
   `tools/release/publish.sh sign-and-publish dist/release/firmware-vX.Y.Z dist/release/assistant-vX.Y.Z`
   (the passphrase is asked by `ssh-keygen`; `--key FILE` instead of `~/.ssh/akam_release`).

Nothing publishes by itself: `make_release.sh` never signs, tags or uploads, and `publish.sh draft` never signs and
never publishes.

## Prerequisites

`tools/build/bootstrap.sh` done (pinned Zig, SmolRTSP and its 4 dependencies, Dropbear archive, the official Go
of `GO_VERSION` in `tools/build/deps.lock` and the Go modules), plus `gdb` (time64 gate), `make`, `patch`,
GNU `tar`, `xz`, `ssh-keygen`, and `gh` logged in (the draft and the publication). The tree must be clean (no
uncommitted and no untracked file) and on the commit to release, which must be `origin/main`. A kit built any other
way (`AKAM_KIT_ALLOW_DIRTY`, `--no-build`, a gate override) is stamped `kit_grade=test` in `MANIFEST.txt`, and
`verify_kit.sh` refuses it unless `--allow-test-kit` is given: never publish one.

## Details

### Release key (once)

```sh
ssh-keygen -t ed25519 -f ~/.ssh/akam_release -C akam-release     # choose a passphrase
```

Keep the private key outside the repository (`sign_kit.sh` refuses a key inside it) and back it up. The public key is
in `tools/release/allowed_signers` and, as an identical copy, in `tools/assistant/internal/kit/allowed_signers` (the
assistant embeds it; a Go test fails when the two differ). Quote it in the root `README.md` too, so it can be
cross-checked from another channel. The assistant binaries embed `allowed_signers`, so they must be built after the
key is committed: `make_release.sh` refuses a release build while the file has no key line or the embedded copy differs.

### 1. Prepare the commit

- Set the version of the family in the release commit: `tools/release/firmware_version` (firmware), or `var version` in
  `tools/assistant/cmd/akam/main.go` AND the default of `AKAM_WIZARD_VERSION` in `./akam` (assistant).
  `make_release.sh` refuses a different value.
- Write the section `## [firmware X.Y.Z] - YYYY-MM-DD` and/or `## [assistant X.Y.Z] - YYYY-MM-DD` in `CHANGELOG.md`: a
  short user-facing summary, with links to `docs/features.md` and `docs/testing-status.md`. `make_release.sh` puts that
  section in the release notes (`<changelog>` in `release_notes_firmware.md` / `release_notes_assistant.md`) and refuses
  a release without it.
- Review the model files in `tools/release/models/` (a record becomes `validated` only after an Akam image for that
  fingerprint was flashed and run on real hardware) and the two notes templates (`<version>`, `<date>`, `<key>`, `<kits>`
  and `<changelog>` are filled by the build).
- Commit and push to `origin/main`. The tag `<family>-vX.Y.Z` must not exist yet.

### 2. Build

```sh
tools/release/make_release.sh firmware  X.Y.Z      # dist/release/firmware-vX.Y.Z/
tools/release/make_release.sh assistant X.Y.Z      # dist/release/assistant-vX.Y.Z/
```

`X.Y.Z` is plain semver, no prefix. The script refuses a dirty tree, a bad version, an existing tag of THAT family, a
default version of the family that is not `X.Y.Z`, a placeholder key, an embedded key copy that differs, a missing
`CHANGELOG.md` section, a directory that already holds a signature, and the build overrides (`AKAM_ALLOW_ANY_GO`,
`AKAM_STRIP`, `ZIG`, `AKAM_TARGETS`, ...).

- *firmware*: runs `make_kit.sh --model <id>` (`AKAM_KIT_VERSION=X.Y.Z`) for every file of `tools/release/models/`, and
  gathers the kits, the source (`git archive --prefix=akam-firmware-X.Y.Z/ HEAD`), `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and ONE `SHA256SUMS`.
- *assistant*: runs `build_assistant.sh` (`AKAM_VERSION=X.Y.Z`) and gathers the six binaries and archives, `LICENSE`,
  `THIRD_PARTY_NOTICES.md` and ONE `SHA256SUMS`.

Both check every line of `SHA256SUMS` (sorted, `sha256sum` format), refuse any camera image or dump (`*.squashfs`,
`*.sqfs`, `*.bin`, `*.img`, `mtd*`, ...), run `verify_kit.sh --allow-unsigned` on every kit, and write the filled notes to
`dist/release/<family>-vX.Y.Z-notes.md` (it ends with an invisible comment holding the commit, which `publish.sh` checks
against `origin/main`).

`make_release.sh <family> X.Y.Z --test` is a development run: a dirty tree and any version string are allowed, everything
is stamped `X.Y.Z-test` (kits, binaries, directory `dist/release/<family>-vX.Y.Z-test/`), the kit is `kit_grade=test`, and
the directory holds `TEST_BUILD_NOT_PUBLISHABLE.txt` (listed in `SHA256SUMS`): `sign_kit.sh` and `publish.sh` refuse it and
`--check-signed` can never pass. The gathering and the refusals are covered by `tests/release_test.sh` on synthetic
fixtures (`tools/release/gather_release.sh`), `publish.sh` by `tests/publish_test.sh` with a fake `gh`.

### 3. Hardware check (maintainer, by hand)

- *firmware*: for each kit, build an image from THIS kit (not from a development tree):
  `./akam get-kit --kit dist/release/firmware-vX.Y.Z/akam-kit-<model>-X.Y.Z.tar.xz`, then `./akam image`. Flash ONE
  validated camera of that model, with a verified stock backup and a UART adapter at hand, only on the maintainer's
  explicit go. Then run `./akam verify` (it runs `tools/hw_tests/akam_hwcheck.sh` on the camera: ports, processes, no UDP
  listener) and check the RTSP stream.
- *assistant*: run the binary of your platform from `dist/release/assistant-vX.Y.Z/` against a camera (`status`, an
  option, a backup).

Put what was really validated in the `CHANGELOG.md` section (committed before step 2). If anything fails, fix it and
start over with a new version: do not touch a built or signed release.

### 4. Draft

```sh
tools/release/publish.sh draft dist/release/firmware-vX.Y.Z
tools/release/publish.sh draft dist/release/assistant-vX.Y.Z
```

Creates or updates the GitHub DRAFT release (title `Akam firmware X.Y.Z`, the notes, every file except
`SHA256SUMS.sig`), then downloads every asset back and compares its sha256 with the local file. The tag is only named
(the target is the commit, which must be the current `origin/main` with a clean tree); it is created when the release is
published. Run it again after a rebuild: a changed asset is replaced, a stray one removed. It refuses a published release,
a test build, a dirty tree and a commit that is not `origin/main`. Never attach a firmware image or a dump.

### 5. Sign and publish

```sh
tools/release/publish.sh sign-and-publish dist/release/firmware-vX.Y.Z dist/release/assistant-vX.Y.Z
```

For each directory: `sign_kit.sh ~/.ssh/akam_release DIR` (ssh-keygen asks for the passphrase), then
`gather_release.sh --check-signed` (signature against `allowed_signers`, embedded copy, binaries or kits, every
`SHA256SUMS` line, commit of the kits and the source, filled notes, `verify_kit.sh` without `--allow-unsigned`).
When all directories are signed and consistent: the draft's assets are compared again with the local files,
`SHA256SUMS.sig` is uploaded, everything is downloaded back and compared, and the release is published
(`gh release edit --draft=false`, `--latest` for the assistant, `--latest=false` for the firmware; the firmware goes
first). The URLs are printed. Review the draft before this step: a published release is never modified, a problem
means a new version (`make_release.sh` refuses an existing tag and a directory holding a signature).

After the tags exist: a follow-up commit dates the `CHANGELOG.md` headings and bumps the default versions to the next
`-dev` (`tools/release/firmware_version`, `./akam`, `tools/assistant/cmd/akam/main.go`).

## The kit (`make_kit.sh`)

Run by `make_release.sh`; alone: `AKAM_KIT_VERSION=X.Y.Z tools/release/make_kit.sh [--model ID]` (output `dist/kit/`;
`--model` defaults to the only model of the registry). The version has no prefix: the kit is named
`akam-kit-<model>-<AKAM_KIT_VERSION>` and `akam get-kit` looks for that exact name. `MANIFEST.txt` carries `model=<id>`.

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

## The assistant (`build_assistant.sh`)

Run by `make_release.sh`; alone: `AKAM_VERSION=X.Y.Z tools/release/build_assistant.sh` (output `dist/assistant/`,
git-ignored). Prerequisites: `tools/build/bootstrap.sh` done (the official Go of `GO_VERSION` in
`tools/build/deps.lock`, installed in `tools/.toolchain/go-<version>-<os>-<arch>/`, and the Go modules in
`tools/.toolchain/go/mod`); **exactly** that Go is used (any other `go version` is refused; `AKAM_ALLOW_ANY_GO=1` overrides
for a test build, never for a release), clean tree on the commit to release. No network is used.

It builds the six targets through `tools/assistant/build.sh flat` (the single place of the flags:
`CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."`,
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
Measured with `go1.27.3-X:nodwarf5` (not the pinned Go; re-measure with the pinned one), project flags:

| target | size (MiB) |
|---|---|
| linux/amd64 | 10.34 |
| linux/arm64 | 9.50 |
| darwin/amd64 | 10.57 |
| darwin/arm64 | 9.77 |
| windows/amd64 | 10.79 |
| windows/arm64 | 9.79 |

The figure of 14.6 MiB seen once for windows/amd64 comes from a build without `-trimpath -ldflags "-s -w"`.
The binaries grew past 11 MiB (windows/amd64 11.35, darwin/amd64 11.14) with the TUI, the audio decoders and the
Windows audio layer; the downloads no longer use `net/http` (`kit.Client`, about 0.6 MiB: no HTTP/1 stack, no IDNA
or Unicode normalisation tables). The biggest remaining parts are the runtime, `crypto/tls` and `x/crypto/ssh`,
`encoding/json` (about 0.4 MiB, used by the SD helper frames), `regexp` and the CLI code itself.
Do not use UPX.

Unlike the kit, the binaries are all published, including the platforms that no hardware test has covered yet:
the release notes label each one `tested` or `untested` (spec 11, 7.3).

## Camera models

`tools/release/models/<model-id>.txt` is the registry: one file per camera model, with the header (id, display name,
article numbers, SoC, flash, layout) and the records (status, device, kernel, MTD layout, stock APP squashfs format,
stock APP sha256, BusyBox) described in the file's own comment. The model id is the file name: lowercase `a-z`, `0-9` and
`-`, 3 to 40 characters, stable for ever (it is part of the asset names and of every kit). The kit of a model carries ITS
file as `supported_hardware.txt` and its id as `model=<id>`; the assistant embeds every file of the directory. How a new
model is added: [docs/compatible-cameras.md](../../docs/compatible-cameras.md#adding-a-new-model).

A record becomes `validated` only after an Akam image for that fingerprint was flashed and run on real hardware. The hash
of the file is recorded in `MANIFEST.txt`. Anything not marked validated may not work or may brick the camera: the wizard
and the assistant warn and ask for an explicit confirmation, and a different MTD layout is refused. A model missing from
a newer firmware release simply does not see that release.

## Files

| File | Role |
|---|---|
| `make_release.sh` | one release family (`firmware` or `assistant`): build, gather, one directory, one `SHA256SUMS`, notes; `--test`, `--check-signed` |
| `gather_release.sh` | gathers and checks the release directory (used by `make_release.sh`, tested on fixtures) |
| `publish.sh` | `draft` and `sign-and-publish` with `gh` (tested with a fake `gh`) |
| `make_kit.sh` | build, gate, MANIFEST, SHA256SUMS, tarball of ONE model's kit |
| `build_assistant.sh` | the six assistant binaries, archives, notices |
| `image_files.txt` | single list of the Akam files installed into the image |
| `models/<id>.txt` | the camera model registry: stock firmware fingerprints and their status |
| `firmware_version` | the firmware version of the tree (checked by `make_release.sh firmware`) |
| `release_notes_firmware.md`, `release_notes_assistant.md` | notes templates, filled from `CHANGELOG.md` |
| `sign_kit.sh` | `ssh-keygen -Y sign` of `SHA256SUMS` |
| `verify_kit.sh` | user-side verification (checksums, signature, MANIFEST, `kit_format`, `--expect-model`) |
| `allowed_signers` | the release public key (the assistant embeds a copy) |

`kit_format` stays 1: `model=` was added before the first public release, so no kit without it exists. Bump `KIT_FORMAT`
in `make_kit.sh`, `SUPPORTED_KIT_FORMATS` in `verify_kit.sh` and the assistant together when the kit layout changes
incompatibly.
