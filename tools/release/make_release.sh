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
# Build mode refuses: a dirty tree; a version that is not plain X.Y.Z; a tag vX.Y.Z that already exists; an
# output directory that holds a signature; a wizard (`akam`) or assistant (cmd/akam/main.go) default version
# that is not X.Y.Z (set it in the release commit); an allowed_signers file with no ed25519 key line; an embedded
# copy (tools/assistant/internal/kit/allowed_signers) that differs from it (the assistant must be built AFTER the
# real key is committed); and the build overrides AKAM_ALLOW_ANY_GO, AKAM_STRIP, ZIG, AKAM_TARGETS, ...
# Then it runs make_kit.sh (AKAM_KIT_VERSION=X.Y.Z) and build_assistant.sh (AKAM_VERSION=X.Y.Z) and gathers, in
# DIR (default dist/release/vX.Y.Z/): the kit tarball, the assistant binaries and archives, LICENSE,
# THIRD_PARTY_NOTICES.md, the source tarball (git archive --prefix=akam-X.Y.Z/ HEAD) and ONE combined SHA256SUMS
# (see gather_release.sh). It checks every line, runs verify_kit.sh --allow-unsigned on the kit, refuses any camera
# image or dump, writes the filled release notes next to DIR (vX.Y.Z-notes.md, from docs/release_notes_draft.md:
# version, kit sha256, date, key, absolute links) and prints the next commands.
# It never signs, never tags and never publishes.
#
# --test: development run. Allows a dirty tree, any version, an existing tag and a placeholder key. Everything is
# stamped X.Y.Z-test (kit, binaries, directory dist/release/vX.Y.Z-test/), the kit is kit_grade=test, and the
# directory holds TEST_BUILD_NOT_PUBLISHABLE.txt (listed in SHA256SUMS): sign_kit.sh and --check-signed refuse it.
#
# --check-signed X.Y.Z: after tools/release/sign_kit.sh <key> DIR, verify the signature, the embedded key, the
# binaries, every SHA256SUMS line, the source commit, the filled notes and verify_kit.sh (see gather_release.sh).
#
# Environment: SOURCE_DATE_EPOCH (default: commit date), and whatever make_kit.sh / build_assistant.sh read.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ALLOWED="$HERE/allowed_signers"
EMBEDDED="$ROOT/tools/assistant/internal/kit/allowed_signers"
DRAFT="$ROOT/docs/release_notes_draft.md"
REPO_URL="https://github.com/Foohx/akam-fw"
KEY_LINE_RE='^akam-release namespaces="akam-release" ssh-ed25519 AAAA'
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
# A test run is stamped X.Y.Z-test everywhere, so that nothing built by it can pass for the release.
STAMP="$VERSION"
if [ "$TEST" = 1 ]; then
    case "$STAMP" in *-test) ;; *) STAMP="$STAMP-test" ;; esac
fi
if [ -z "$OUT" ]; then
    OUT="$ROOT/dist/release/v$STAMP"
else
    case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
    OUT="${OUT%/}"
fi
NOTES="$(dirname "$OUT")/v$STAMP-notes.md"

if [ "$CHECK" = 1 ]; then
    [ "$(basename "$OUT")" = "v$VERSION" ] || die "--check-signed expects the directory dist/release/vX.Y.Z (the notes file is looked up next to it)"
    exec bash "$HERE/gather_release.sh" --check-signed "$OUT"
fi

for t in git tar gzip sha256sum; do command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"; done
git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1 || die "not a git checkout"

GRADE="release"
if [ "$TEST" = 1 ]; then
    GRADE="test"
    export AKAM_KIT_ALLOW_DIRTY=1 AKAM_ALLOW_DIRTY=1
    log "TEST mode: everything is stamped $STAMP and can never be published"
else
    [ -z "$(git -C "$ROOT" status --porcelain)" ] || die "the working tree is not clean (commit or remove changes; --test for a development run)"
    if git -C "$ROOT" rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then
        die "the tag v$VERSION already exists: never rebuild a published release, use a new version"
    fi
    [ ! -e "$OUT/SHA256SUMS.sig" ] || die "$OUT holds a signature: never rebuild a signed release in place, use a new version"
    for v in AKAM_ALLOW_ANY_GO AKAM_STRIP ZIG AKAM_TARGETS AKAM_KIT_NO_BUILD AKAM_KIT_ALLOW_DIRTY AKAM_ALLOW_DIRTY \
        AKAM_KIT_CHECK_TIME32 AK_RTSP_BIN FLASH_TOOL_BIN DROPBEAR_MULTI; do
        [ -z "${!v:-}" ] || die "$v is set: build overrides are for test builds, never for a release (unset it)"
    done
    # The version shown by `akam` (wizard) and `akam version` (assistant) must be the release version.
    # shellcheck disable=SC2016  # the pattern is literal text of ./akam
    WIZ="$(sed -n 's/^AKAM_WIZARD_VERSION=\${AKAM_WIZARD_VERSION:-\(.*\)}$/\1/p' "$ROOT/akam" | head -1)"
    ASS="$(sed -n 's/^var version = "\(.*\)"$/\1/p' "$ROOT/tools/assistant/cmd/akam/main.go" | head -1)"
    [ "$WIZ" = "$VERSION" ] || die "the wizard default version in ./akam is '$WIZ', not $VERSION: set the version in the release commit"
    [ "$ASS" = "$VERSION" ] || die "the assistant default version in tools/assistant/cmd/akam/main.go is '$ASS', not $VERSION: set the version in the release commit"
    grep -qE "$KEY_LINE_RE" "$ALLOWED" \
        || die "tools/release/allowed_signers has no ssh-ed25519 key line. Create the release key, commit its public key to allowed_signers (and the embedded copy) first: the assistant binaries embed that file, so they must be built after it. See tools/release/README.md."
    cmp -s "$ALLOWED" "$EMBEDDED" || die "tools/assistant/internal/kit/allowed_signers differs from tools/release/allowed_signers: copy it (cp tools/release/allowed_signers tools/assistant/internal/kit/allowed_signers), commit, then build"
    [ -f "$DRAFT" ] || die "$DRAFT missing"
fi

EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
export SOURCE_DATE_EPOCH="$EPOCH"
mkdir -p "$ROOT/dist"
WORK="$(mktemp -d "$ROOT/dist/.release-work.XXXXXX")"
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

log "kit $STAMP"
AKAM_KIT_VERSION="$STAMP" bash "$HERE/make_kit.sh" --out "$WORK/kit"
log "assistant $STAMP"
AKAM_VERSION="$STAMP" bash "$HERE/build_assistant.sh" --out "$WORK/assistant"
log "source tarball (HEAD $(git -C "$ROOT" rev-parse --short HEAD))"
git -C "$ROOT" archive --format=tar --prefix="akam-$STAMP/" HEAD | gzip -n -9 >"$WORK/source.tar.gz"

mkdir -p "$(dirname "$OUT")"
bash "$HERE/gather_release.sh" "$STAMP" --kit-dir "$WORK/kit" --assistant-dir "$WORK/assistant" \
    --source "$WORK/source.tar.gz" --out "$OUT" --grade "$GRADE"

if [ "$TEST" = 1 ]; then
    log "test build done in $OUT (not publishable). Nothing else to do."
    exit 0
fi

# --- filled release notes (next to the directory, never inside it) ----------------------------------
KSHA="$(awk -v f="akam-kit-$VERSION.tar.xz" '$2 == f {print $1}' "$OUT/SHA256SUMS")"
KEYTXT="$(grep -E "$KEY_LINE_RE" "$ALLOWED" | head -1 | awk '{print $3 " " $4}')"
DATE="$(date -u -d "@$EPOCH" +%Y-%m-%d)"
BLOB="$REPO_URL/blob/v$VERSION/docs/"
grep -v '^Draft\. Not published' "$DRAFT" \
    | sed -e "s|](\([A-Za-z0-9_.-]*\.md\)|]($BLOB\1|g" \
    | awk -v v="$VERSION" -v sha="$KSHA" -v d="$DATE" -v k="$KEYTXT" '{
        gsub(/<version>/, v); gsub(/<sha256>/, sha); gsub(/<date>/, d)
        i = index($0, "<key>"); while (i > 0) { $0 = substr($0, 1, i - 1) k substr($0, i + 5); i = index($0, "<key>") }
        print }' >"$NOTES"
if grep -qE '<(sha256|key|date|version)>' "$NOTES"; then die "placeholders left in $NOTES"; fi
log "wrote $NOTES"

q() { printf '%q' "$1"; }
FILES=""
while IFS= read -r f; do FILES="$FILES    $(q "$OUT/$f") \\"$'\n'; done < <(cd "$OUT" && { sed 's/^[0-9a-f]*  //' SHA256SUMS; echo SHA256SUMS; echo SHA256SUMS.sig; })
FILES="${FILES%$' \\\n'}"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
cat <<EOF

make_release: built $OUT. Next steps (by hand, nothing was signed or published):

  1. Sign (asks for the key passphrase):
       tools/release/sign_kit.sh ~/.ssh/akam_release $(q "$OUT")
  2. Verify the signed release (key, embedded copy, binaries, commit, notes, every checksum, the signed kit):
       tools/release/make_release.sh --check-signed $VERSION
       tools/release/verify_kit.sh $(q "$OUT/akam-kit-$VERSION.tar.xz")
  3. Hardware check by hand (see tools/release/README.md, step 5): image from this kit, ONE validated camera, hwcheck.
  4. Push the release commit (${COMMIT:0:12}), then create a DRAFT release; the tag v$VERSION is created on that commit at publish time:
       gh release create v$VERSION --draft --title "Akam $VERSION" --notes-file $(q "$NOTES") \\
           --target $COMMIT \\
$FILES

  5. Review the draft on GitHub (file list, notes, key and kit sha256 quoted), then publish it.
  6. After the tag: bump the default versions to the next -dev, and put the kit sha256 in CHANGELOG.md (follow-up commit).
EOF
