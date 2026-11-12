#!/usr/bin/env bash
# Gather one Akam release directory and check it (called by make_release.sh; testable on fake inputs).
#
#   tools/release/gather_release.sh X.Y.Z --kit-dir DIR --assistant-dir DIR --source FILE --out DIR [--grade release|test]
#   tools/release/gather_release.sh --check-signed DIR
#
# Gather mode builds, in DIR.tmp and then renames to DIR: the kit tarball akam-kit-X.Y.Z.tar.xz (from --kit-dir),
# the assistant files (akam-<os>-<arch>[.exe], archives, LICENSE, THIRD_PARTY_NOTICES.md; from --assistant-dir),
# and the source tarball (--source, copied as akam-X.Y.Z-src.tar.gz). ONE combined SHA256SUMS (sorted by name,
# `sha256sum` format) covers every file. Then it re-checks every line, runs verify_kit.sh --allow-unsigned on the
# kit and refuses anything that looks like a camera image or dump. An existing DIR is replaced only if it holds
# nothing but known release file names (no subdirectory) and no signature. It never signs and never publishes.
# Grade "release" (default) needs all six assistant targets and a release-grade kit; grade "test" is lenient and
# adds TEST_BUILD_NOT_PUBLISHABLE.txt (listed in SHA256SUMS), which makes sign_kit.sh and --check-signed refuse DIR.
#
# --check-signed DIR (after tools/release/sign_kit.sh KEY DIR): DIR must hold SHA256SUMS.sig, which must verify
# against tools/release/allowed_signers; the embedded copy of that file must be identical; all six binaries must
# contain the key text and the Linux amd64 one (on such a host) must report X.Y.Z; every SHA256SUMS line must
# match; the kit MANIFEST source_commit and the source tarball's commit id must equal HEAD (or
# AKAM_EXPECT_COMMIT); the filled notes file next to DIR (vX.Y.Z-notes.md) must have no placeholder left and
# quote the kit sha256 and the key; verify_kit.sh must pass WITHOUT --allow-unsigned.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ALLOWED="${AKAM_ALLOWED_SIGNERS:-$HERE/allowed_signers}"
EMBEDDED="${AKAM_EMBEDDED_SIGNERS:-$ROOT/tools/assistant/internal/kit/allowed_signers}"
MARKER=TEST_BUILD_NOT_PUBLISHABLE.txt
TARGETS="linux-amd64 linux-arm64 darwin-amd64 darwin-arm64 windows-amd64 windows-arm64"
KEY_LINE_RE='^akam-release namespaces="akam-release" ssh-ed25519 AAAA'

die() { echo "gather_release: error: $*" >&2; exit 1; }
log() { echo "gather_release: $*"; }
has_key() { grep -qE "$KEY_LINE_RE" "$1"; }

# The raw binary and the archive file names of the six targets.
target_files() {
    local t
    for t in $TARGETS; do
        case $t in
            windows-*) echo "akam-$t.exe"; echo "akam-$t.zip" ;;
            *) echo "akam-$t"; echo "akam-$t.tar.gz" ;;
        esac
    done
}

# Names that must never be shipped: camera images, dumps and keys.
forbidden_name() {
    case "$(basename "$1")" in
        *.squashfs | *.sqfs | *.bin | *.img | *.dump | *.jffs2 | *.ko | mtd* | *mtdblock* | *.pem | id_rsa* | id_ed25519* | *_host_key*) return 0 ;;
    esac
    return 1
}
refuse_images_in_tar() { # $1 tarball, $2 tar compression flag: no entry may look like an image or dump
    local f=$1 flag=$2 l list
    list="$(tar "-t${flag}f" "$f")" || die "cannot list $(basename "$f") (damaged or not a tar archive)"
    while IFS= read -r l; do
        l="${l%/}"
        # Tiny synthetic test fixtures of the source tree (testdata/, tests/fixtures/) are not camera images.
        if [ "$flag" = z ]; then case "$l" in */testdata/* | */tests/fixtures/*) continue ;; esac; fi
        if forbidden_name "$l"; then die "$(basename "$f") contains a forbidden entry (camera image, dump or key): $l"; fi
    done <<<"$list"
}

check_sums_dir() { # every SHA256SUMS line matches; the file set equals the (name-sorted) list
    local d=$1 have want
    (cd "$d" && sha256sum -c --quiet SHA256SUMS >/dev/null) || die "SHA256SUMS check failed in $d"
    have="$(cd "$d" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort)"
    want="$(sed 's/^[0-9a-f]*  //' "$d/SHA256SUMS")"
    [ "$have" = "$want" ] || die "files and SHA256SUMS differ in $d (or the list is not sorted)"
}

# --- check-signed mode ----------------------------------------------------------------------
if [ "${1:-}" = --check-signed ]; then
    [ $# = 2 ] || die "usage: $0 --check-signed DIR"
    D=${2%/}
    [ -f "$D/SHA256SUMS" ] || die "$D/SHA256SUMS not found"
    [ ! -e "$D/$MARKER" ] || die "$D is a TEST build ($MARKER): it can never be signed or published"
    [ -f "$D/SHA256SUMS.sig" ] || die "$D/SHA256SUMS.sig not found: run tools/release/sign_kit.sh <key> $D"
    has_key "$ALLOWED" || die "tools/release/allowed_signers has no ssh-ed25519 key line: commit the public key, then rebuild"
    cmp -s "$ALLOWED" "$EMBEDDED" || die "the embedded copy ($EMBEDDED) differs from allowed_signers: copy it, then rebuild the assistant"
    command -v ssh-keygen >/dev/null 2>&1 || die "ssh-keygen not found"
    ssh-keygen -Y verify -f "$ALLOWED" -I akam-release -n akam-release -s "$D/SHA256SUMS.sig" <"$D/SHA256SUMS" >/dev/null 2>&1 \
        || die "SHA256SUMS.sig does not verify against tools/release/allowed_signers"
    log "ok: signature verifies against tools/release/allowed_signers"
    check_sums_dir "$D"
    log "ok: every SHA256SUMS line matches"
    # The assistant must have been built after the key was committed: each binary carries the key text.
    blob="$(grep -E "$KEY_LINE_RE" "$ALLOWED" | head -1 | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^AAAA/) {print $i; exit}}')"
    [ -n "$blob" ] || die "cannot read the key blob from allowed_signers"
    for t in $TARGETS; do
        f="$D/akam-$t"
        case $t in windows-*) f="$f.exe" ;; esac
        [ -f "$f" ] || die "missing $(basename "$f")"
        grep -aqF -e "$blob" "$f" || die "$(basename "$f") does not embed the release key: it was built before the key was committed"
    done
    log "ok: the six binaries embed the release key"
    set -- "$D"/akam-kit-*.tar.xz
    [ -f "$1" ] || die "no kit tarball in $D"
    KITF=$1
    VER="$(basename "$KITF")"
    VER="${VER#akam-kit-}"
    VER="${VER%.tar.xz}"
    if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] && [ -x "$D/akam-linux-amd64" ]; then
        "$D/akam-linux-amd64" version 2>&1 | grep -qF "akam $VER " || die "akam-linux-amd64 does not report version $VER"
        log "ok: akam-linux-amd64 reports $VER"
    fi
    WANTC="${AKAM_EXPECT_COMMIT:-$(git -C "$ROOT" rev-parse HEAD)}"
    KC="$(tar -xJOf "$KITF" "akam-kit-$VER/MANIFEST.txt" | sed -n 's/^source_commit=//p')"
    [ "$KC" = "$WANTC" ] || die "the kit was built from commit $KC, not $WANTC"
    SC="$(gzip -dc "$D/akam-$VER-src.tar.gz" | git get-tar-commit-id 2>/dev/null || true)"
    [ "$SC" = "$WANTC" ] || die "the source tarball was made from commit '$SC', not $WANTC"
    log "ok: kit and source tarball come from commit $WANTC"
    NOTES="$(dirname "$D")/v$VER-notes.md"
    [ -f "$NOTES" ] || die "$NOTES not found (make_release.sh writes it)"
    if grep -nE '<(sha256|key|date|version)>' "$NOTES" >/dev/null; then die "$NOTES still has a placeholder (<sha256>, <key>, <date> or <version>)"; fi
    KSHA="$(awk -v f="akam-kit-$VER.tar.xz" '$2 == f {print $1}' "$D/SHA256SUMS")"
    grep -qF "$KSHA" "$NOTES" || die "$NOTES does not quote the kit sha256 $KSHA"
    grep -qF "$blob" "$NOTES" || die "$NOTES does not quote the release public key"
    log "ok: $NOTES is filled"
    bash "$HERE/verify_kit.sh" --allowed-signers "$ALLOWED" "$KITF" >/dev/null || die "verify_kit.sh failed on the signed kit"
    log "ok: verify_kit.sh accepts the signed kit"
    echo "RESULT: $D is signed and consistent"
    exit 0
fi

# --- gather mode -------------------------------------------------------------------------------
KITDIR="" ASSDIR="" SOURCE="" OUT="" GRADE=release
[ $# -ge 1 ] || die "usage: $0 X.Y.Z --kit-dir DIR --assistant-dir DIR --source FILE --out DIR [--grade release|test]"
case "$1" in -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;; esac
VERSION=$1
shift
while [ $# -gt 0 ]; do
    case "$1" in
        --kit-dir | --assistant-dir | --source | --out | --grade) [ $# -ge 2 ] || die "$1 needs a value" ;;
    esac
    case "$1" in
        --kit-dir) KITDIR=$2; shift ;;
        --assistant-dir) ASSDIR=$2; shift ;;
        --source) SOURCE=$2; shift ;;
        --out) OUT=${2%/}; shift ;;
        --grade) GRADE=$2; shift ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done
[ -d "$KITDIR" ] || die "--kit-dir is not a directory"
[ -d "$ASSDIR" ] || die "--assistant-dir is not a directory"
[ -f "$SOURCE" ] || die "--source is not a file"
[ -n "$OUT" ] || die "--out is required"
case "$GRADE" in release | test) ;; *) die "--grade must be release or test" ;; esac
case "$VERSION" in
    '' | *[!A-Za-z0-9._+-]*) die "bad version '$VERSION'" ;;
esac
if [ "$GRADE" = release ]; then
    printf '%s\n' "$VERSION" | grep -qE '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' || die "release version must be plain X.Y.Z, got '$VERSION'"
fi
for t in tar xz gzip sha256sum; do command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"; done

KITFILE="akam-kit-$VERSION.tar.xz"
[ -f "$KITDIR/$KITFILE" ] || die "$KITDIR/$KITFILE not found (the kit must be built with AKAM_KIT_VERSION=$VERSION)"
[ "$(tar -tJf "$KITDIR/$KITFILE" | head -1 | cut -d/ -f1)" = "akam-kit-$VERSION" ] || die "kit top directory is not akam-kit-$VERSION"

# Assistant files: a whitelist. Anything else (an image, a dump, a stray file) stops the run.
mapfile -t WANT < <(target_files; echo LICENSE; echo THIRD_PARTY_NOTICES.md)
ASSFILES=()
while IFS= read -r f; do
    if forbidden_name "$f"; then die "forbidden file in the assistant directory (camera image, dump or key): $f"; fi
    case "$f" in SHA256SUMS | SHA256SUMS.sig) continue ;; esac
    ok=0
    for w in "${WANT[@]}"; do [ "$f" = "$w" ] && ok=1; done
    [ "$ok" = 1 ] || die "unexpected file in the assistant directory: $f"
    ASSFILES+=("$f")
done < <(cd "$ASSDIR" && LC_ALL=C find . -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
if [ "$GRADE" = release ]; then
    for w in "${WANT[@]}"; do
        [ -f "$ASSDIR/$w" ] || die "missing assistant file $w (a release builds all six targets)"
    done
fi
[ "${#ASSFILES[@]}" -gt 0 ] || die "no assistant file found"
refuse_images_in_tar "$KITDIR/$KITFILE" J
refuse_images_in_tar "$SOURCE" z

# An existing output directory is replaced only when it is a previous, unsigned release directory:
# every entry a known release file name, no subdirectory.
if [ -e "$OUT" ]; then
    [ -d "$OUT" ] || die "$OUT exists and is not a directory"
    if [ -n "$(ls -A "$OUT")" ]; then
        [ ! -e "$OUT/SHA256SUMS.sig" ] || die "$OUT holds a signature: never rebuild a signed release in place (use a new version, or delete the directory yourself)"
        mapfile -t KNOWN < <(target_files; echo LICENSE; echo THIRD_PARTY_NOTICES.md; echo SHA256SUMS; echo "$MARKER")
        while IFS= read -r e; do
            [ ! -d "$OUT/$e" ] || die "$OUT is not empty and contains a subdirectory ($e): refusing to replace it"
            ok=0
            case "$e" in akam-kit-*.tar.xz | akam-*-src.tar.gz) ok=1 ;; esac
            for w in "${KNOWN[@]}"; do [ "$e" = "$w" ] && ok=1; done
            [ "$ok" = 1 ] || die "$OUT is not empty and contains '$e', which is not a release file: refusing to replace it"
        done < <(ls -A "$OUT")
    fi
fi

NEW="$OUT.tmp"
[ ! -e "$NEW" ] || { case "$NEW" in *.tmp) rm -rf "$NEW" ;; esac; }
mkdir -p "$NEW"
trap 'rm -rf "$NEW"' EXIT
cp "$KITDIR/$KITFILE" "$NEW/"
for f in "${ASSFILES[@]}"; do cp "$ASSDIR/$f" "$NEW/"; done
cp "$SOURCE" "$NEW/akam-$VERSION-src.tar.gz"
if [ "$GRADE" = test ]; then
    printf 'TEST BUILD: made with make_release.sh --test (dirty tree allowed, stub inputs possible).\nNever sign, never publish.\n' >"$NEW/$MARKER"
fi
for f in "$NEW"/*; do
    if forbidden_name "$f"; then die "forbidden file in the release directory: $f"; fi
done

(cd "$NEW" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS)
check_sums_dir "$NEW"
VK=(--allow-unsigned)
[ "$GRADE" = release ] || VK+=(--allow-test-kit)
bash "$HERE/verify_kit.sh" "${VK[@]}" "$NEW/$KITFILE" >/dev/null || die "verify_kit.sh refused the gathered kit (see: verify_kit.sh ${VK[*]} $NEW/$KITFILE)"

# All checks passed: swap the directory in.
if [ -e "$OUT" ]; then
    rm -rf "$OUT.old"
    mv "$OUT" "$OUT.old"
fi
mv "$NEW" "$OUT"
rm -rf "$OUT.old"
trap - EXIT
log "wrote $(wc -l <"$OUT/SHA256SUMS" | tr -d ' ') files and one combined SHA256SUMS in $OUT"
log "ok: verify_kit.sh ${VK[*]} accepts the gathered kit against the combined SHA256SUMS"
cat "$OUT/SHA256SUMS"
