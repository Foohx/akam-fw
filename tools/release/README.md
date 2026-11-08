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
