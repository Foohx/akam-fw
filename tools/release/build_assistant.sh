#!/usr/bin/env bash
# Build the assistant release files: six binaries, six archives, notices, SHA256SUMS (spec 11, section 8.1).
#
#   tools/release/build_assistant.sh [--out DIR]
#
# Output (default dist/assistant/, git-ignored), names without version:
#   akam-linux-amd64  akam-linux-arm64  akam-darwin-amd64  akam-darwin-arm64  akam-windows-amd64.exe  akam-windows-arm64.exe
#   akam-<os>-<arch>.tar.gz (Linux, macOS) / .zip (Windows): <dir>/akam[.exe] + LICENSE + THIRD_PARTY_NOTICES.md, exec bit kept
#   LICENSE  THIRD_PARTY_NOTICES.md  SHA256SUMS      (SHA256SUMS.sig: tools/release/sign_kit.sh <key> dist/assistant)
#
# Builds offline, from a clean checkout, with the exact Go of tools/build/deps.lock, through tools/assistant/build.sh
# (same flags as every other build: CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w", never -tags akamtest).
# No user name, host name, absolute path or build time ends up in a file: archives use a fixed mtime,
# owner 0/0 and no user or group name. Running it twice gives identical files (the maintainer compares two runs,
# and a second machine's run, before the first public release).
#
# Environment (all optional):
#   AKAM_VERSION          version shown by `akam version` (default: git describe --always)
#   SOURCE_DATE_EPOCH     timestamp of the archives (default: commit date of HEAD)
#   AKAM_ALLOW_DIRTY=1    build from a dirty tree (never for a release)
#   AKAM_ALLOW_ANY_GO=1   accept a Go other than the pinned one (never for a release)
#   AKAM_TARGETS          space-separated os/arch subset (tests only: a release builds all six)
#   AKAM_SIGN_WINDOWS, AKAM_SIGN_MACOS   hooks for later Authenticode / Developer ID signing: nothing is done yet
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "build_assistant: error: $*" >&2; exit 1; }
log() { echo "build_assistant: $*"; }

OUT="$ROOT/dist/assistant"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) [ $# -ge 2 ] || die "--out needs a directory"; OUT=$2; shift ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

for t in go git sha256sum tar gzip find sort mktemp; do
    command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"
done

# --- identity --------------------------------------------------------------------
git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1 || die "not a git checkout: cannot record the source commit"
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && [ "${AKAM_ALLOW_DIRTY:-}" != 1 ]; then
    die "the tree is not clean (commit or remove changes; AKAM_ALLOW_DIRTY=1 only for a test build)"
fi
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
case "$EPOCH" in '' | *[!0-9]*) die "SOURCE_DATE_EPOCH must be a number of seconds" ;; esac
VERSION="${AKAM_VERSION:-$(git -C "$ROOT" describe --always 2>/dev/null || echo dev)}"
export VERSION
BUILD="$ROOT/tools/assistant/build.sh"
TARGETS="${AKAM_TARGETS:-linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64 windows/arm64}"
export AKAM_TARGETS="$TARGETS"

# --- build (build.sh refuses an unpinned Go) -----------------------------------------------------
case "$OUT" in /*) ;; *) die "--out must be an absolute path" ;; esac
mkdir -p "$OUT"
rm -f "$OUT"/akam-* "$OUT"/SHA256SUMS "$OUT"/SHA256SUMS.sig "$OUT"/LICENSE "$OUT"/THIRD_PARTY_NOTICES.md
log "version $VERSION, SOURCE_DATE_EPOCH $EPOCH, targets: $TARGETS"
"$BUILD" notices >/dev/null || die "THIRD_PARTY_NOTICES.md does not list every linked module (build.sh notices)"
"$BUILD" flat "$OUT"

# --- checks on the binaries --------------------------------------------------------------------
for t in $TARGETS; do
    os=${t%/*}
    arch=${t#*/}
    f="$OUT/akam-$os-$arch"
    [ "$os" = windows ] && f="$f.exe"
    [ -s "$f" ] || die "missing $f"
    if [ -n "$HOME" ] && [ "$HOME" != / ] && grep -aqF -e "$HOME" "$f"; then die "$f contains the home directory path"; fi
    if grep -aqF -e "$ROOT" "$f"; then die "$f contains the checkout path"; fi
    info=$(go version -m "$f")
    case "$info" in *akamtest*) die "$f was built with the akamtest tag" ;; esac
    case "$info" in *CGO_ENABLED=0*) ;; *) die "$f was not built with CGO_ENABLED=0" ;; esac
    case "$info" in *-trimpath=true*) ;; *) die "$f was not built with -trimpath" ;; esac
done

# --- archives ------------------------------------------------------------------------------------
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$OUT/"
TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
(cd "$HERE/mkarchive" && GOFLAGS='' CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off \
    GOPATH="$ROOT/tools/.toolchain/go/path" GOCACHE="$ROOT/tools/.toolchain/go/cache" \
    go build -trimpath -o "$TMP/mkarchive" main.go) || die "cannot build tools/release/mkarchive"
for t in $TARGETS; do
    os=${t%/*}
    arch=${t#*/}
    base="akam-$os-$arch"
    if [ "$os" = windows ]; then
        "$TMP/mkarchive" -format zip -epoch "$EPOCH" -o "$OUT/$base.zip" -dir "$base" \
            "akam.exe=$OUT/$base.exe:755" "LICENSE=$OUT/LICENSE:644" "THIRD_PARTY_NOTICES.md=$OUT/THIRD_PARTY_NOTICES.md:644"
    else
        "$TMP/mkarchive" -format tar.gz -epoch "$EPOCH" -o "$OUT/$base.tar.gz" -dir "$base" \
            "akam=$OUT/$base:755" "LICENSE=$OUT/LICENSE:644" "THIRD_PARTY_NOTICES.md=$OUT/THIRD_PARTY_NOTICES.md:644"
    fi
done

# The archive budget is 5 MiB per target (spec 09 7.4)
for a in "$OUT"/akam-*.tar.gz "$OUT"/akam-*.zip; do
    [ -f "$a" ] || continue
    [ "$(wc -c <"$a")" -le $((5 * 1024 * 1024)) ] || die "$(basename "$a") is larger than 5 MiB"
done

# --- signing hooks (not implemented: unsigned binaries, documented in the install guide) -----
for v in AKAM_SIGN_WINDOWS AKAM_SIGN_MACOS; do
    if [ -n "${!v:-}" ]; then log "note: $v is set but code signing is not implemented yet; binaries stay unsigned"; fi
done

# --- checksums -------------------------------------------------------------------------------------
(cd "$OUT" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >"$TMP/SHA256SUMS")
cp "$TMP/SHA256SUMS" "$OUT/SHA256SUMS"
log "wrote $(grep -c . "$OUT/SHA256SUMS") files, checksums in $OUT/SHA256SUMS:"
cat "$OUT/SHA256SUMS"
log "next: tools/release/sign_kit.sh <private-key> $OUT   (writes SHA256SUMS.sig)"
