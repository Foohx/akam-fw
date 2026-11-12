#!/usr/bin/env bash
# Build the whole Akam release (kit + assistant + source) into ONE directory, ready to sign.
#
#   tools/release/make_release.sh X.Y.Z [--out DIR] [--test]
#   tools/release/make_release.sh --check-signed X.Y.Z [--out DIR]
#
# One release, one tag vX.Y.Z; the version is written WITHOUT the "v" everywhere else (kit name, MANIFEST
# akam_version, `akam version`, source prefix). `akam get-kit` relies on this: it downloads
# https://github.com/Foohx/akam-fw/releases/download/vX.Y.Z/akam-kit-X.Y.Z.tar.xz (+ SHA256SUMS, SHA256SUMS.sig).
#
# Build mode: refuses a dirty tree, a version that is not plain X.Y.Z, an allowed_signers file with no key line,
# and an embedded copy (tools/assistant/internal/kit/allowed_signers) that differs from it (the assistant must
# be built AFTER the real key is committed). Then it runs make_kit.sh (AKAM_KIT_VERSION=X.Y.Z) and
# build_assistant.sh (AKAM_VERSION=X.Y.Z) and gathers, in DIR (default dist/release/vX.Y.Z/): the kit tarball,
# the assistant binaries and archives, LICENSE, THIRD_PARTY_NOTICES.md, the source tarball
# (git archive --prefix=akam-X.Y.Z/ HEAD) and ONE combined SHA256SUMS. It checks every line, runs
# verify_kit.sh --allow-unsigned on the kit, refuses any camera image or dump, and prints the next commands.
# It never signs, never tags and never publishes.
#
# --test: development run. Allows a dirty tree, any version string and a placeholder key; output goes to
# dist/release/vX.Y.Z-test/ by default, the kit is stamped kit_grade=test and the directory gets
# TEST_BUILD_NOT_PUBLISHABLE.txt: sign_kit.sh must not be used on it and --check-signed refuses it.
#
# --check-signed X.Y.Z: after tools/release/sign_kit.sh <key> DIR, verify SHA256SUMS.sig against
# tools/release/allowed_signers, that the embedded copy is identical, that every binary embeds the key, every
# SHA256SUMS line, and verify_kit.sh without --allow-unsigned.
#
# Environment: SOURCE_DATE_EPOCH (default: commit date), and whatever make_kit.sh / build_assistant.sh read.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ALLOWED="$HERE/allowed_signers"
EMBEDDED="$ROOT/tools/assistant/internal/kit/allowed_signers"
die() { echo "make_release: error: $*" >&2; exit 1; }
log() { echo "make_release: $*"; }

VERSION="" OUT="" TEST=0 CHECK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --out) [ $# -ge 2 ] || die "--out needs a directory"; OUT=$2; shift ;;
        --test) TEST=1 ;;
        --check-signed) CHECK=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *) [ -z "$VERSION" ] || die "one version only"; VERSION=$1 ;;
    esac
    shift
done
[ -n "$VERSION" ] || die "usage: $0 X.Y.Z [--out DIR] [--test]   |   $0 --check-signed X.Y.Z [--out DIR]"
if [ "$TEST" = 1 ] && [ "$CHECK" = 1 ]; then die "--test and --check-signed exclude each other: a test build is never signed"; fi
case "$VERSION" in '' | *[!A-Za-z0-9._+-]*) die "bad version '$VERSION'" ;; esac
if [ "$TEST" = 0 ]; then
    printf '%s\n' "$VERSION" | grep -qE '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' \
        || die "version must be plain semver X.Y.Z without a leading v (got '$VERSION'); --test accepts anything"
fi
if [ -z "$OUT" ]; then
    OUT="$ROOT/dist/release/v$VERSION"
    [ "$TEST" = 0 ] || OUT="$OUT-test"
fi
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"

if [ "$CHECK" = 1 ]; then
    exec bash "$HERE/gather_release.sh" --check-signed "$OUT"
fi

for t in git tar gzip sha256sum; do command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"; done
git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1 || die "not a git checkout"

GRADE=release
if [ "$TEST" = 1 ]; then
    GRADE="test"
    export AKAM_KIT_ALLOW_DIRTY=1 AKAM_ALLOW_DIRTY=1
    log "TEST mode: the output is stamped as a test build and can never be published"
else
    [ -z "$(git -C "$ROOT" status --porcelain)" ] || die "the working tree is not clean (commit or remove changes; --test for a development run)"
    grep -qvE '^[[:space:]]*(#|$)' "$ALLOWED" \
        || die "tools/release/allowed_signers has no key line. Create the release key, commit its public key to allowed_signers (and the embedded copy) first: the assistant binaries embed that file, so they must be built after it. See tools/release/README.md."
    cmp -s "$ALLOWED" "$EMBEDDED" || die "tools/assistant/internal/kit/allowed_signers differs from tools/release/allowed_signers: copy it (cp tools/release/allowed_signers tools/assistant/internal/kit/allowed_signers), commit, then build"
fi

EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
export SOURCE_DATE_EPOCH="$EPOCH"
WORK="$(mktemp -d)"
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

log "kit $VERSION"
AKAM_KIT_VERSION="$VERSION" bash "$HERE/make_kit.sh" --out "$WORK/kit"
log "assistant $VERSION"
AKAM_VERSION="$VERSION" bash "$HERE/build_assistant.sh" --out "$WORK/assistant"
log "source tarball (HEAD $(git -C "$ROOT" rev-parse --short HEAD))"
git -C "$ROOT" archive --format=tar --prefix="akam-$VERSION/" HEAD | gzip -n -9 >"$WORK/source.tar.gz"

bash "$HERE/gather_release.sh" "$VERSION" --kit-dir "$WORK/kit" --assistant-dir "$WORK/assistant" \
    --source "$WORK/source.tar.gz" --out "$OUT" --grade "$GRADE"

if [ "$TEST" = 1 ]; then
    log "test build done in $OUT (not publishable). Nothing else to do."
    exit 0
fi

FILES="$(cd "$OUT" && { sed 's/^[0-9a-f]*  //' SHA256SUMS; echo SHA256SUMS; echo SHA256SUMS.sig; } | sed "s|^|    $OUT/|" | sed 's/$/ \\/' | sed '$ s/ \\$//')"
cat <<EOF

make_release: built $OUT. Next steps (by hand, nothing was signed or published):

  1. Sign (asks for the key passphrase):
       tools/release/sign_kit.sh ~/.ssh/akam_release $OUT
  2. Verify the signed release (key, embedded copy, binaries, every checksum, the signed kit):
       tools/release/make_release.sh --check-signed $VERSION
       tools/release/verify_kit.sh $OUT/akam-kit-$VERSION.tar.xz
  3. Push the release commit ($(git -C "$ROOT" rev-parse --short HEAD)), then create a DRAFT release; the tag v$VERSION is created at publish time on that commit:
       gh release create v$VERSION --draft --title "Akam $VERSION" --notes-file docs/release_notes_draft.md \\
           --target $(git -C "$ROOT" rev-parse HEAD) \\
$FILES

  4. Review the draft on GitHub (file list, notes, the key and the kit sha256 quoted from step 2), then publish it.
EOF
