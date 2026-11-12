#!/usr/bin/env bash
# Gather one Akam release directory and check it (called by make_release.sh; testable on fake inputs).
#
#   tools/release/gather_release.sh X.Y.Z --kit-dir DIR --assistant-dir DIR --source FILE --out DIR [--grade release|test]
#   tools/release/gather_release.sh --check-signed DIR
#
# Gather mode copies into --out: the kit tarball akam-kit-X.Y.Z.tar.xz (from --kit-dir), the assistant files
# (akam-<os>-<arch>[.exe], archives, LICENSE, THIRD_PARTY_NOTICES.md; from --assistant-dir), and the source
# tarball (--source, copied under the name akam-X.Y.Z-src.tar.gz). It writes ONE combined SHA256SUMS (sorted by
# name, `sha256sum` format) over every file, then re-checks every line, runs verify_kit.sh --allow-unsigned on
# the kit, and refuses anything that looks like a camera image or dump. It never signs and never publishes.
# Grade "release" (default) needs all six assistant targets and a release-grade kit; grade "test" is lenient
# and writes TEST_BUILD_NOT_PUBLISHABLE.txt, which makes every later step refuse the directory.
#
# --check-signed DIR (after tools/release/sign_kit.sh KEY DIR): DIR must hold SHA256SUMS.sig, which must verify
# against tools/release/allowed_signers; the embedded copy of that file must be identical; every binary must
# contain the key text; every SHA256SUMS line must match; verify_kit.sh must pass WITHOUT --allow-unsigned.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ALLOWED="${AKAM_ALLOWED_SIGNERS:-$HERE/allowed_signers}"
EMBEDDED="${AKAM_EMBEDDED_SIGNERS:-$ROOT/tools/assistant/internal/kit/allowed_signers}"
MARKER=TEST_BUILD_NOT_PUBLISHABLE.txt
TARGETS="linux-amd64 linux-arm64 darwin-amd64 darwin-arm64 windows-amd64 windows-arm64"

die() { echo "gather_release: error: $*" >&2; exit 1; }
log() { echo "gather_release: $*"; }
has_key() { grep -qvE '^[[:space:]]*(#|$)' "$1"; }

# Names that must never be shipped: camera images, dumps and keys.
forbidden_name() {
    case "$(basename "$1")" in
        *.squashfs | *.bin | *.img | *.dump | *.jffs2 | *.ko | mtd* | *mtdblock* | *.pem | id_rsa* | id_ed25519* | *_host_key*) return 0 ;;
    esac
    return 1
}
refuse_images_in_tar() { # $1 tarball, $2 tar compression flag: no entry may look like an image or dump
    local f=$1 flag=$2 l
    while IFS= read -r l; do
        if forbidden_name "$l"; then die "$(basename "$f") contains a forbidden entry (camera image, dump or key): $l"; fi
    done < <(tar "-t${flag}f" "$f" | sed 's|/$||')
}

check_sums_dir() { # every SHA256SUMS line matches; the file set equals the (name-sorted) list
    local d=$1 have want
    (cd "$d" && sha256sum -c --quiet SHA256SUMS >/dev/null) || die "SHA256SUMS check failed in $d"
    have="$(cd "$d" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' ! -name "$MARKER" -printf '%f\n' | LC_ALL=C sort)"
    want="$(sed 's/^[0-9a-f]*  //' "$d/SHA256SUMS")"
    [ "$have" = "$want" ] || die "files and SHA256SUMS differ in $d (or the list is not sorted)"
}

# --- check-signed mode ----------------------------------------------------------------------
if [ "${1:-}" = --check-signed ]; then
    [ $# = 2 ] || die "usage: $0 --check-signed DIR"
    D=$2
    [ -f "$D/SHA256SUMS" ] || die "$D/SHA256SUMS not found"
    [ ! -e "$D/$MARKER" ] || die "$D is a TEST build ($MARKER): it can never be signed or published"
    [ -f "$D/SHA256SUMS.sig" ] || die "$D/SHA256SUMS.sig not found: run tools/release/sign_kit.sh <key> $D"
    has_key "$ALLOWED" || die "tools/release/allowed_signers has no key line: commit the public key, then rebuild"
    cmp -s "$ALLOWED" "$EMBEDDED" || die "the embedded copy ($EMBEDDED) differs from allowed_signers: copy it, then rebuild the assistant"
    command -v ssh-keygen >/dev/null 2>&1 || die "ssh-keygen not found"
    ssh-keygen -Y verify -f "$ALLOWED" -I akam-release -n akam-release -s "$D/SHA256SUMS.sig" <"$D/SHA256SUMS" >/dev/null 2>&1 \
        || die "SHA256SUMS.sig does not verify against tools/release/allowed_signers"
    log "ok: signature verifies against tools/release/allowed_signers"
    check_sums_dir "$D"
    log "ok: every SHA256SUMS line matches"
    # The assistant must have been built after the key was committed: each binary carries the key text.
    blob="$(grep -vE '^[[:space:]]*(#|$)' "$ALLOWED" | head -1 | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^AAAA/) {print $i; exit}}')"
    [ -n "$blob" ] || die "cannot read the key blob from allowed_signers"
    n=0
    for f in "$D"/akam-*-*; do
        case "$f" in *.tar.gz | *.zip) continue ;; esac
        [ -f "$f" ] || continue
        grep -aqF -e "$blob" "$f" || die "$(basename "$f") does not embed the release key: it was built before the key was committed"
        n=$((n + 1))
    done
    [ "$n" -ge 1 ] || die "no assistant binary found in $D"
    log "ok: $n binaries embed the release key"
    set -- "$D"/akam-kit-*.tar.xz
    [ -f "$1" ] || die "no kit tarball in $D"
    bash "$HERE/verify_kit.sh" --allowed-signers "$ALLOWED" "$1" >/dev/null || die "verify_kit.sh failed on the signed kit"
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
        --out) OUT=$2; shift ;;
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
for t in tar xz sha256sum; do command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"; done

KITFILE="akam-kit-$VERSION.tar.xz"
[ -f "$KITDIR/$KITFILE" ] || die "$KITDIR/$KITFILE not found (the kit must be built with AKAM_KIT_VERSION=$VERSION)"
[ "$(tar -tJf "$KITDIR/$KITFILE" | head -1 | cut -d/ -f1)" = "akam-kit-$VERSION" ] || die "kit top directory is not akam-kit-$VERSION"

# Assistant files: a whitelist. Anything else (an image, a dump, a stray file) stops the run.
WANT=()
for t in $TARGETS; do
    case $t in
        windows-*) WANT+=("akam-$t.exe" "akam-$t.zip") ;;
        *) WANT+=("akam-$t" "akam-$t.tar.gz") ;;
    esac
done
WANT+=(LICENSE THIRD_PARTY_NOTICES.md)
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

# Output directory: created, or a previous release directory that is emptied first.
if [ -e "$OUT" ]; then
    [ -d "$OUT" ] || die "$OUT exists and is not a directory"
    if [ -n "$(ls -A "$OUT")" ]; then
        { [ -f "$OUT/SHA256SUMS" ] || [ -f "$OUT/$MARKER" ]; } || die "$OUT is not empty and does not look like a release directory: refusing to clear it"
        find "$OUT" -mindepth 1 -delete
    fi
fi
mkdir -p "$OUT"
cp "$KITDIR/$KITFILE" "$OUT/"
for f in "${ASSFILES[@]}"; do cp "$ASSDIR/$f" "$OUT/"; done
cp "$SOURCE" "$OUT/akam-$VERSION-src.tar.gz"
if [ "$GRADE" = test ]; then
    printf 'TEST BUILD: made with make_release.sh --test (dirty tree allowed, stub inputs possible).\nNever sign, never publish.\n' >"$OUT/$MARKER"
fi
for f in "$OUT"/*; do
    if forbidden_name "$f"; then die "forbidden file in the release directory: $f"; fi
done

(cd "$OUT" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' ! -name "$MARKER" -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS)
check_sums_dir "$OUT"
log "wrote $(wc -l <"$OUT/SHA256SUMS" | tr -d ' ') files and one combined SHA256SUMS in $OUT"

VK=(--allow-unsigned)
[ "$GRADE" = release ] || VK+=(--allow-test-kit)
bash "$HERE/verify_kit.sh" "${VK[@]}" "$OUT/$KITFILE" >/dev/null || die "verify_kit.sh refused the gathered kit (see: verify_kit.sh ${VK[*]} $OUT/$KITFILE)"
log "ok: verify_kit.sh ${VK[*]} accepts the gathered kit against the combined SHA256SUMS"
cat "$OUT/SHA256SUMS"
