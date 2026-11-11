# Release kit: maintainer procedure

The kit is what a user downloads instead of compiling Akam. It contains only
Akam's own files (ARM binaries built here, overlay scripts, licences). It never
contains a camera image or any vendor content: the user supplies their own APP
dump. User-facing description: [docs/akam_v1_kit.md](../../docs/akam_v1_kit.md).

There is no CI. Everything below runs on the maintainer's Linux machine.

## Prerequisites

`tools/build/bootstrap.sh` done (pinned Zig, SmolRTSP and its 4 dependencies,
Dropbear archive), plus `gdb` (time64 gate), `make`, `patch`, GNU `tar`, `xz`,
`ssh-keygen`. The tree must be clean (no uncommitted and no untracked file) and on the commit to
release. A kit built any other way (`AKAM_KIT_ALLOW_DIRTY`, `--no-build`, a gate
override) is stamped `kit_grade=test` in `MANIFEST.txt`, and `verify_kit.sh` refuses it
unless `--allow-test-kit` is given: never publish one.

## 1. Build the kit

```sh
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)   # optional: this is the default
AKAM_KIT_VERSION=v1.0.0 tools/release/make_kit.sh      # default version: git describe
# output: dist/kit/akam-kit-<version>.tar.xz and dist/kit/SHA256SUMS
```

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

## 2. Verify it yourself, unsigned

```sh
tools/release/verify_kit.sh --allow-unsigned dist/kit/akam-kit-<version>.tar.xz
```

## 3. Sign

```sh
tools/release/sign_kit.sh ~/.ssh/akam_release          # writes dist/kit/SHA256SUMS.sig
tools/release/verify_kit.sh dist/kit/akam-kit-<version>.tar.xz
```

`sign_kit.sh` only hands the key path to `ssh-keygen -Y sign -n akam-release`;
it never copies or stores the key and refuses a key inside the repository. The
matching public key must be in `tools/release/allowed_signers` (committed before
the release, see the comments in that file) and quoted in the README and the
release notes. Use a dedicated Ed25519 key with a passphrase.

## Assistant release (`akam` binaries, tag `assistant-vX.Y.Z`)

Separate from the kit. Prerequisites: `tools/build/bootstrap.sh` done (the official Go of `GO_VERSION` in
`tools/build/deps.lock`, installed in `tools/.toolchain/go-<version>-<os>-<arch>/`, and the Go modules in
`tools/.toolchain/go/mod`); **exactly** that Go is used (any other `go version` is refused; `AKAM_ALLOW_ANY_GO=1` overrides
for a test build, never for a release), clean tree on the commit to release. No network is used.

```sh
AKAM_VERSION=1.0.0 tools/release/build_assistant.sh     # output: dist/assistant/ (git-ignored)
tools/release/sign_kit.sh ~/.ssh/akam_release dist/assistant   # SHA256SUMS.sig (same key and scheme as the kit)
```

It builds the six targets through `tools/assistant/build.sh flat` (the single place of the flags:
`CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."`, `-tags nethttpomithttp2`,
never `-tags akamtest`), checks the binaries (no checkout or home path inside, flags recorded by
`go version -m`), then writes, with names without version:
`akam-<os>-<arch>[.exe]`, `akam-<os>-<arch>.tar.gz` (Linux, macOS) or `.zip` (Windows) holding
`akam[.exe]` + `LICENSE` + `THIRD_PARTY_NOTICES.md` (exec bit kept), `LICENSE`,
`THIRD_PARTY_NOTICES.md` and `SHA256SUMS`. Archives come from `mkarchive/` (standard library only):
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

Files: `build_assistant.sh` (this pipeline), `mkarchive/main.go` (deterministic tar.gz and zip).

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

## 4. Publish (by hand)

```sh
git archive --format=tar.gz --prefix=akam-<version>/ -o dist/kit/akam-<version>-src.tar.gz <tag>
gh release create <tag> --draft --title "Akam <version>" --notes-file notes.md \
    dist/kit/akam-kit-<version>.tar.xz dist/kit/SHA256SUMS dist/kit/SHA256SUMS.sig \
    dist/kit/akam-<version>-src.tar.gz
# later additions:  gh release upload <tag> <file>
```

Put the kit's sha256 (`cat dist/kit/SHA256SUMS`) in the release notes,
`CHANGELOG.md` and the annotated tag message, so that users can cross-check it
from another channel (`verify_kit.sh --expect-sha256 <first 12 hex>`). Review the
draft, then publish it. Never attach a firmware image or a dump.

## Files

| File | Role |
|---|---|
| `make_kit.sh` | build, gate, MANIFEST, SHA256SUMS, tarball |
| `image_files.txt` | single list of the Akam files installed into the image |
| `supported_hardware.txt` | validated / reported / untested hardware and firmware fingerprints |
| `sign_kit.sh` | `ssh-keygen -Y sign` of `SHA256SUMS` |
| `verify_kit.sh` | user-side verification (checksums, signature, MANIFEST, `kit_format`) |
| `allowed_signers` | trusted public keys (placeholder until the maintainer adds one) |

Bump `KIT_FORMAT` in `make_kit.sh` and `SUPPORTED_KIT_FORMATS` in
`verify_kit.sh` together when the kit layout changes incompatibly.
