#!/usr/bin/env bash
# Build one Akam release (firmware OR assistant) into ONE directory, ready to sign.
#
#   tools/release/make_release.sh firmware  X.Y.Z [--out DIR] [--test]
#   tools/release/make_release.sh assistant X.Y.Z [--out DIR] [--test]
#   tools/release/make_release.sh --check-signed firmware|assistant X.Y.Z [--out DIR]
#
# Two release families with their own versions, tags and assets (both start at 0.1.0):
#   firmware   tag firmware-vX.Y.Z: one kit per camera model akam-kit-<model>-X.Y.Z.tar.xz (one per file of
#              tools/release/models/), the source tarball akam-firmware-X.Y.Z-src.tar.gz, LICENSE,
#              THIRD_PARTY_NOTICES.md, SHA256SUMS, SHA256SUMS.sig. The version is in tools/release/firmware_version,
#              recorded as akam_version in every kit MANIFEST and in the image (/usr/local/akam/MANIFEST.txt).
#   assistant  tag assistant-vX.Y.Z: the six binaries and archives, LICENSE, THIRD_PARTY_NOTICES.md, SHA256SUMS,
#              SHA256SUMS.sig. The version is `var version` of tools/assistant/cmd/akam/main.go; the bash wizard
#              ./akam (shipped with the source) follows it: AKAM_WIZARD_VERSION must be the same.
# The tag carries the family prefix, the version is written WITHOUT the "v" everywhere else (kit name, MANIFEST
# akam_version, `akam version`, asset names). `akam get-kit` downloads
# https://github.com/Foohx/akam-fw/releases/download/firmware-vX.Y.Z/akam-kit-<model>-X.Y.Z.tar.xz (+ SHA256SUMS, .sig).
#
# Build mode refuses: a dirty tree; a version that is not plain X.Y.Z; a tag <family>-vX.Y.Z that already exists;
# an output directory that holds a signature; a default version of the family that is not X.Y.Z (set it in the
# release commit); an allowed_signers file with no ed25519 key line; an embedded copy
# (tools/assistant/internal/kit/allowed_signers) that differs from it (the assistant must be built AFTER the real
# key is committed); a CHANGELOG.md without the section "## [<family> X.Y.Z]"; and the build overrides
# AKAM_ALLOW_ANY_GO, AKAM_STRIP, ZIG, AKAM_TARGETS, ...
# Then it builds (firmware: make_kit.sh per model with AKAM_KIT_VERSION=X.Y.Z; assistant: build_assistant.sh with
# AKAM_VERSION=X.Y.Z), gathers the files in DIR (default dist/release/<family>-vX.Y.Z/) with ONE SHA256SUMS (see
# gather_release.sh), refuses any camera image or dump, and writes the filled release notes next to DIR
# (<family>-vX.Y.Z-notes.md, from tools/release/release_notes_<family>.md: version, kit sha256, date, key, the matching
# CHANGELOG.md section, absolute links). It never signs, never tags and never publishes: see tools/release/publish.sh.
#
# --test: development run. Allows a dirty tree, any version, an existing tag and a placeholder key. Everything is
# stamped X.Y.Z-test (kit, binaries, directory dist/release/<family>-vX.Y.Z-test/), the kit is kit_grade=test, and the
# directory holds TEST_BUILD_NOT_PUBLISHABLE.txt (listed in SHA256SUMS): sign_kit.sh and --check-signed refuse it.
#
# --check-signed FAMILY X.Y.Z: after tools/release/sign_kit.sh <key> DIR, verify the signature, the embedded key, the
# binaries or kits, every SHA256SUMS line, the source commit and the filled notes (see gather_release.sh).
#
# Environment: SOURCE_DATE_EPOCH (default: commit date), and whatever make_kit.sh / build_assistant.sh read.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ALLOWED="$HERE/allowed_signers"
EMBEDDED="$ROOT/tools/assistant/internal/kit/allowed_signers"
REPO_URL="https://github.com/Foohx/akam-fw"
KEY_LINE_RE='^akam-release namespaces="akam-release" ssh-ed25519 AAAA'
die() { echo "make_release: error: $*" >&2; exit 1; }
log() { echo "make_release: $*"; }

FAMILY="" VERSION="" OUT="" TEST=0 CHECK=0
USAGE="usage: $0 firmware|assistant X.Y.Z [--out DIR] [--test]   |   $0 --check-signed firmware|assistant X.Y.Z [--out DIR]"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) [ $# -ge 2 ] || die "--out needs a directory"; OUT=$2; shift ;;
        --test) TEST=1 ;;
        --check-signed) CHECK=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option: $1" ;;
        *)
            if [ -z "$FAMILY" ]; then FAMILY=$1
            elif [ -z "$VERSION" ]; then VERSION=$1
            else die "one family and one version only"; fi
            ;;
    esac
    shift
done
[ -n "$FAMILY" ] && [ -n "$VERSION" ] || die "$USAGE"
case "$FAMILY" in firmware | assistant) ;; *) die "the first argument is the release family: firmware or assistant (got '$FAMILY')" ;; esac
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
TAG="$FAMILY-v$VERSION"
if [ -z "$OUT" ]; then
    OUT="$ROOT/dist/release/$FAMILY-v$STAMP"
else
    case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
    OUT="${OUT%/}"
fi
NOTES="$(dirname "$OUT")/$FAMILY-v$STAMP-notes.md"
DRAFT="$ROOT/tools/release/release_notes_$FAMILY.md"

if [ "$CHECK" = 1 ]; then
    [ "$(basename "$OUT")" = "$FAMILY-v$VERSION" ] || die "--check-signed expects the directory dist/release/$FAMILY-vX.Y.Z (the notes file is looked up next to it)"
    exec bash "$HERE/gather_release.sh" --check-signed "$FAMILY" "$OUT"
fi

for t in git tar gzip sha256sum; do command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"; done
git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1 || die "not a git checkout"

# changelog_section: the body of "## [<family> X.Y.Z]" in CHANGELOG.md (up to the next "## " heading).
changelog_section() {
    local s
    s="$(awk -v h="## [$FAMILY $VERSION]" 'index($0, h) == 1 { on = 1; next } /^## / { on = 0 } on { print }' "$ROOT/CHANGELOG.md" 2>/dev/null)" || s=""
    printf '%s' "$s" | sed -e '/./,$!d'
}

GRADE="release"
if [ "$TEST" = 1 ]; then
    GRADE="test"
    export AKAM_KIT_ALLOW_DIRTY=1 AKAM_ALLOW_DIRTY=1
    log "TEST mode: everything is stamped $STAMP and can never be published"
else
    [ -z "$(git -C "$ROOT" status --porcelain)" ] || die "the working tree is not clean (commit or remove changes; --test for a development run)"
    if git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
        die "the tag $TAG already exists: never rebuild a published release, use a new version"
    fi
    [ ! -e "$OUT/SHA256SUMS.sig" ] || die "$OUT holds a signature: never rebuild a signed release in place, use a new version"
    # ZIG is exported by tools/build/env.sh: the pinned Zig is fine, any other one is an override.
    if [ -n "${ZIG:-}" ]; then
        zv="$(sed -n 's/^ZIG_VERSION=//p' "$ROOT/tools/build/deps.lock" 2>/dev/null | head -1)" || zv=""
        pz="$ROOT/tools/.toolchain/zig-$zv-x86_64-linux/zig"
        [ -n "$zv" ] && [ -e "$pz" ] && [ "$(readlink -f -- "$ZIG")" = "$(readlink -f -- "$pz")" ] \
            || die "ZIG is set to a Zig other than the pinned one ($ZIG): unset it for a release"
    fi
    for v in AKAM_ALLOW_ANY_GO AKAM_STRIP AKAM_TARGETS AKAM_KIT_NO_BUILD AKAM_KIT_ALLOW_DIRTY AKAM_ALLOW_DIRTY \
        AKAM_KIT_CHECK_TIME32 AK_RTSP_BIN FLASH_TOOL_BIN DROPBEAR_MULTI; do
        [ -z "${!v:-}" ] || die "$v is set: build overrides are for test builds, never for a release (unset it)"
    done
    # The default version of the family must be the release version.
    if [ "$FAMILY" = firmware ]; then
        FWV="$(head -1 "$ROOT/tools/release/firmware_version" 2>/dev/null | tr -d '[:space:]')"
        [ "$FWV" = "$VERSION" ] || die "the firmware version in tools/release/firmware_version is '$FWV', not $VERSION: set the version in the release commit"
    else
        # shellcheck disable=SC2016  # the pattern is literal text of ./akam
        WIZ="$(sed -n 's/^AKAM_WIZARD_VERSION=\${AKAM_WIZARD_VERSION:-\(.*\)}$/\1/p' "$ROOT/akam" | head -1)"
        ASS="$(sed -n 's/^var version = "\(.*\)"$/\1/p' "$ROOT/tools/assistant/cmd/akam/main.go" | head -1)"
        [ "$ASS" = "$VERSION" ] || die "the assistant default version in tools/assistant/cmd/akam/main.go is '$ASS', not $VERSION: set the version in the release commit"
        [ "$WIZ" = "$VERSION" ] || die "the wizard default version in ./akam is '$WIZ', not $VERSION (it follows the assistant version): set the version in the release commit"
    fi
    grep -qE "$KEY_LINE_RE" "$ALLOWED" \
        || die "tools/release/allowed_signers has no ssh-ed25519 key line. Create the release key, commit its public key to allowed_signers (and the embedded copy) first: the assistant binaries embed that file, so they must be built after it. See tools/release/README.md."
    cmp -s "$ALLOWED" "$EMBEDDED" || die "tools/assistant/internal/kit/allowed_signers differs from tools/release/allowed_signers: copy it (cp tools/release/allowed_signers tools/assistant/internal/kit/allowed_signers), commit, then build"
    [ -f "$DRAFT" ] || die "$DRAFT missing"
    [ -n "$(changelog_section)" ] || die "CHANGELOG.md has no section '## [$FAMILY $VERSION]' (or it is empty): write the release notes there first"
fi

EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
export SOURCE_DATE_EPOCH="$EPOCH"
mkdir -p "$ROOT/dist"
WORK="$(mktemp -d "$ROOT/dist/.release-work.XXXXXX")"
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

mkdir -p "$(dirname "$OUT")"
if [ "$FAMILY" = firmware ]; then
    mkdir -p "$WORK/kit"
    MODELS=()
    for f in "$HERE"/models/*.txt; do
        [ -f "$f" ] && MODELS+=("$(basename "$f" .txt)")
    done
    [ "${#MODELS[@]}" -gt 0 ] || die "no model in tools/release/models/"
    for m in "${MODELS[@]}"; do
        log "kit $m $STAMP"
        AKAM_KIT_VERSION="$STAMP" bash "$HERE/make_kit.sh" --model "$m" --out "$WORK/kit.$m"
        cp "$WORK/kit.$m/akam-kit-$m-$STAMP.tar.xz" "$WORK/kit/"
    done
    log "source tarball (HEAD $(git -C "$ROOT" rev-parse --short HEAD))"
    git -C "$ROOT" archive --format=tar --prefix="akam-firmware-$STAMP/" HEAD | gzip -n -9 >"$WORK/source.tar.gz"
    bash "$HERE/gather_release.sh" firmware "$STAMP" --kit-dir "$WORK/kit" --source "$WORK/source.tar.gz" --out "$OUT" --grade "$GRADE"
else
    log "assistant $STAMP"
    AKAM_VERSION="$STAMP" bash "$HERE/build_assistant.sh" --out "$WORK/assistant"
    bash "$HERE/gather_release.sh" assistant "$STAMP" --assistant-dir "$WORK/assistant" --out "$OUT" --grade "$GRADE"
fi

if [ "$TEST" = 1 ]; then
    log "test build done in $OUT (not publishable). Nothing else to do."
    exit 0
fi

# --- filled release notes (next to the directory, never inside it) ----------------------------------
KEYTXT="$(grep -E "$KEY_LINE_RE" "$ALLOWED" | head -1 | awk '{print $3 " " $4}')"
DATE="$(date -u -d "@$EPOCH" +%Y-%m-%d)"
BLOB="$REPO_URL/blob/$TAG/docs/"
KITLINES=""
if [ "$FAMILY" = firmware ]; then
    for m in "${MODELS[@]}"; do
        ksha="$(awk -v f="akam-kit-$m-$VERSION.tar.xz" '$2 == f {print $1}' "$OUT/SHA256SUMS")"
        mname="$(sed -n 's/^name=//p' "$HERE/models/$m.txt" | head -1)"
        KITLINES="$KITLINES- \`akam-kit-$m-$VERSION.tar.xz\` ($mname): sha256 \`$ksha\`"$'\n'
    done
    KITLINES="${KITLINES%$'\n'}"
fi
changelog_section | sed -e "s|](docs/|]($BLOB|g" >"$WORK/changelog.txt"
printf '%s' "$KITLINES" >"$WORK/kits.txt"
grep -v '^Draft\. Not published' "$DRAFT" \
    | sed -e "s|](\([A-Za-z0-9_./-]*\.md\)|]($BLOB\1|g" \
    | awk -v v="$VERSION" -v d="$DATE" -v k="$KEYTXT" -v kf="$WORK/kits.txt" -v cf="$WORK/changelog.txt" '
        function slurp(f,   l, s) { s = ""; while ((getline l < f) > 0) s = s (s == "" ? "" : "\n") l; close(f); return s }
        BEGIN { kits = slurp(kf); chg = slurp(cf) }
        { gsub(/<version>/, v); gsub(/<date>/, d)
          i = index($0, "<key>"); while (i > 0) { $0 = substr($0, 1, i - 1) k substr($0, i + 5); i = index($0, "<key>") }
          if ($0 == "<kits>") $0 = kits
          if ($0 == "<changelog>") $0 = chg
          print }' >"$NOTES"
if grep -qE '<(sha256|key|date|version|kits|changelog)>' "$NOTES"; then die "placeholders left in $NOTES"; fi
log "wrote $NOTES"

HWSTEP="the akam binary of your platform against a camera (backup, status, an option)"
[ "$FAMILY" != firmware ] || HWSTEP="build an image from each kit, flash ONE validated camera of that model, run hwcheck"
q() { printf '%q' "$1"; }
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
# publish.sh reads the commit back from here (an HTML comment: invisible on GitHub).
printf '\n<!-- akam-release-commit: %s -->\n' "$COMMIT" >>"$NOTES"
cat <<EOF

make_release: built $OUT. Next steps (by hand, nothing was signed, tagged or published):

  1. Hardware check by hand (see tools/release/README.md, step 2): $HWSTEP.
  2. Push the release commit (${COMMIT:0:12}) to origin/main, then create or refresh the GitHub DRAFT (no signature yet; the tag $TAG is created on that commit at publish time):
       tools/release/publish.sh draft $(q "$OUT")
  3. Sign, verify and publish (asks for the key passphrase; several directories can be given at once):
       tools/release/publish.sh sign-and-publish $(q "$OUT")
  4. After the tag: bump the default version of the family to the next -dev, and set the date in the CHANGELOG.md heading (follow-up commit).
EOF
