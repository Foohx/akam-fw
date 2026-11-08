#!/usr/bin/env bash
# Verify an Akam release kit (user side; bash 3.2 compatible, nothing from the
# kit is ever executed).
#
#   tools/release/verify_kit.sh [options] <akam-kit-X.tar.xz | extracted-kit-dir>
#
# Tarball mode (the real check):
#   1. the tarball's sha256 must match its line in SHA256SUMS (next to it, or --sums);
#   2. SHA256SUMS.sig, if present, must verify with ssh-keygen -Y against
#      tools/release/allowed_signers (identity and namespace "akam-release");
#      an INVALID signature always fails; no signature fails too unless
#      --allow-unsigned is given (a loud warning is printed either way);
#   3. the archive is listed first (only regular files, directories and the two
#      dropbear symlinks, no absolute or '..' paths), then extracted to a
#      private temporary directory and checked as in directory mode.
# Directory mode: the kit's own SHA256SUMS and MANIFEST.txt must agree with every
# file (content, mode, symlink target), no file may be missing or extra,
# kit_format must be supported and time64_gate must be "pass". The signature
# cannot be checked on a directory: verify the tarball it came from.
#
# Options:
#   --allowed-signers FILE  public keys to trust (default: allowed_signers next to this script)
#   --sums FILE             SHA256SUMS of the tarball (default: SHA256SUMS in its directory)
#   --sig FILE              signature file (default: <sums>.sig)
#   --allow-unsigned        accept a kit with no signature (checksum-only trust)
#   --expect-sha256 HEX     also require the tarball sha256 to start with HEX (cross-check
#                           against the release notes / CHANGELOG)
#   --extract-to DIR        keep the verified tree in DIR (must not exist yet)
# Exit: 0 verified, 1 verification failed, 2 usage/missing tool, 3 unsigned and
# --allow-unsigned not given (everything else was fine).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUPPORTED_KIT_FORMATS="1"
SIG_IDENTITY=akam-release
SIG_NAMESPACE=akam-release

die() { echo "verify_kit: FAIL: $*" >&2; exit 1; }
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//' >&2; exit 2; }

ALLOWED="$HERE/allowed_signers"
SUMS=""
SIG=""
ALLOW_UNSIGNED=0
EXPECT=""
EXTRACT_TO=""
INPUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --allowed-signers) [ $# -ge 2 ] || usage; ALLOWED=$2; shift ;;
        --sums) [ $# -ge 2 ] || usage; SUMS=$2; shift ;;
        --sig) [ $# -ge 2 ] || usage; SIG=$2; shift ;;
        --allow-unsigned) ALLOW_UNSIGNED=1 ;;
        --expect-sha256) [ $# -ge 2 ] || usage; EXPECT=$2; shift ;;
        --extract-to) [ $# -ge 2 ] || usage; EXTRACT_TO=$2; shift ;;
        -h | --help) usage ;;
        -*) echo "verify_kit: unknown option $1" >&2; usage ;;
        *) [ -z "$INPUT" ] || usage; INPUT=$1 ;;
    esac
    shift
done
[ -n "$INPUT" ] || usage

# sha256 helper: GNU sha256sum, or shasum on systems without it.
if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "verify_kit: neither sha256sum nor shasum found" >&2
    exit 2
fi
mode_of() { stat -c %a "$1" 2>/dev/null || stat -f %Lp "$1"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SIGNED_BY=""

# --- tarball mode -----------------------------------------------------------------------
verify_tarball() {
    local tb=$1 base got want dir
    command -v tar >/dev/null 2>&1 || { echo "verify_kit: tar not found" >&2; exit 2; }
    command -v xz >/dev/null 2>&1 || { echo "verify_kit: xz not found" >&2; exit 2; }
    base="$(basename "$tb")"
    dir="$(cd "$(dirname "$tb")" && pwd)"
    [ -n "$SUMS" ] || SUMS="$dir/SHA256SUMS"
    [ -f "$SUMS" ] || die "$SUMS not found (download SHA256SUMS next to the kit)"
    got="$(sha256_of "$tb")"
    want="$(awk -v f="$base" '{ n = $2; sub(/^\*/, "", n); if (n == f) print $1 }' "$SUMS")"
    [ -n "$want" ] || die "$base is not listed in $SUMS"
    [ "$(printf '%s\n' "$want" | wc -l | tr -d ' ')" = 1 ] || die "$base is listed more than once in $SUMS"
    [ "$got" = "$want" ] || die "sha256 mismatch for $base: expected $want, got $got"
    echo "ok: $base sha256 $got matches $(basename "$SUMS")"
    if [ -n "$EXPECT" ]; then
        case "$got" in "$EXPECT"*) echo "ok: sha256 starts with the expected $EXPECT" ;; *) die "sha256 $got does not start with --expect-sha256 $EXPECT" ;; esac
    fi

    check_signature
    list_and_extract "$tb"
}

check_signature() {
    [ -n "$SIG" ] || SIG="$SUMS.sig"
    if [ ! -f "$SIG" ]; then
        SIGNED_BY=""
        echo "WARNING: no signature ($SIG not found). This kit is NOT authenticated: only its checksum" >&2
        echo "WARNING: was compared, and a checksum from the same download place proves nothing." >&2
        return 0
    fi
    command -v ssh-keygen >/dev/null 2>&1 || { echo "verify_kit: ssh-keygen not found, cannot check $SIG" >&2; exit 2; }
    [ -f "$ALLOWED" ] || die "signature present but $ALLOWED not found"
    if ! grep -qvE '^[[:space:]]*(#|$)' "$ALLOWED"; then
        die "signature present but $ALLOWED contains no public key (placeholder not replaced): cannot authenticate"
    fi
    local out
    if out="$(ssh-keygen -Y verify -f "$ALLOWED" -I "$SIG_IDENTITY" -n "$SIG_NAMESPACE" -s "$SIG" <"$SUMS" 2>&1)"; then
        SIGNED_BY="$out"
        echo "ok: signature valid ($out)"
    else
        die "INVALID signature on $(basename "$SUMS"): $out"
    fi
}

list_and_extract() {
    local tb=$1 target="$TMP/tree" listing="$TMP/listing" names="$TMP/names"
    tar -tvJf "$tb" >"$listing" 2>"$TMP/tar.err" || die "cannot read the archive: $(head -1 "$TMP/tar.err")"
    tar -tJf "$tb" >"$names"
    # Entry types: '-' file, 'd' directory, 'l' symlink only for overlay/bin/dropbear{,key}.
    awk '
        { t = substr($1, 1, 1) }
        t == "-" || t == "d" { next }
        t == "l" {
            if ($0 ~ / akam-kit-[^ \/]+\/overlay\/bin\/dropbear(key)? -> dropbearmulti$/) next
        }
        { print "unexpected archive entry: " $0; bad = 1 }
        END { exit bad }' "$listing" >"$TMP/awk.out" || die "$(head -1 "$TMP/awk.out")"
    top="$(head -1 "$names" | cut -d/ -f1)"
    case "$top" in akam-kit-*) ;; *) die "archive top-level directory '$top' is not akam-kit-*" ;; esac
    if grep -qE '(^/|(^|/)\.\.(/|$))' "$names"; then die "archive contains absolute or '..' paths"; fi
    if grep -qvE "^$top(/|\$)" "$names"; then die "archive has entries outside $top/"; fi
    mkdir -p "$target"
    # -p keeps the archived modes (not the umask), so the MANIFEST modes are really compared.
    tar -xJf "$tb" -C "$target" --no-same-owner -p --no-overwrite-dir
    KIT_DIR="$target/$top"
    [ -d "$KIT_DIR" ] || die "extraction produced no $top directory"
    echo "ok: archive layout is safe ($(wc -l <"$names" | tr -d ' ') entries)"
    if [ -n "$EXTRACT_TO" ]; then
        [ ! -e "$EXTRACT_TO" ] || die "--extract-to $EXTRACT_TO already exists"
        mkdir -p "$EXTRACT_TO"
    fi
}

# --- directory checks -------------------------------------------------------------------------
verify_dir() {
    local d=$1 m="$1/MANIFEST.txt" kf want ver
    [ -f "$m" ] || die "MANIFEST.txt missing in $d"
    [ -f "$d/SHA256SUMS" ] || die "SHA256SUMS missing in $d"
    if find "$d" ! -type f ! -type d ! -type l | grep -q .; then die "unexpected file type in the kit"; fi
    kf="$(sed -n 's/^kit_format=//p' "$m")"
    [ "$(printf '%s\n' "$kf" | wc -l | tr -d ' ')" = 1 ] && [ -n "$kf" ] || die "MANIFEST.txt has no (single) kit_format"
    case " $SUPPORTED_KIT_FORMATS " in
        *" $kf "*) echo "ok: kit_format $kf is supported" ;;
        *) die "kit_format $kf is not supported by this verifier (supports: $SUPPORTED_KIT_FORMATS); use a matching Akam version" ;;
    esac
    ver="$(sed -n 's/^akam_version=//p' "$m")"
    [ -n "$ver" ] || die "MANIFEST.txt has no akam_version"
    grep -qx 'time64_gate=pass' "$m" || die "MANIFEST.txt does not record time64_gate=pass"
    grep -qx 'target=arm-linux-musleabi' "$m" || die "MANIFEST.txt target is not arm-linux-musleabi"

    # 1. inner SHA256SUMS: every listed file matches, nothing missing or extra.
    (cd "$d" && sha256sum -c --quiet SHA256SUMS >"$TMP/sc.out" 2>&1) || die "SHA256SUMS check failed: $(head -3 "$TMP/sc.out" | tr '\n' ' ')"
    (cd "$d" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||') >"$TMP/have.files"
    sed 's/^[0-9a-f]*  //' "$d/SHA256SUMS" | LC_ALL=C sort >"$TMP/sums.files"
    cmp -s "$TMP/have.files" "$TMP/sums.files" || die "files differ from SHA256SUMS: $(diff "$TMP/have.files" "$TMP/sums.files" | head -3 | tr '\n' ' ')"
    grep -qx 'MANIFEST.txt' "$TMP/sums.files" || die "SHA256SUMS does not cover MANIFEST.txt"

    # 2. MANIFEST file/link lines.
    : >"$TMP/man.files"
    local type a mode path got n=0
    while read -r type a mode path; do
        # a = sha256 (file) or target (link); mode is octal.
        case "$path" in '' | /* | ../* | */../* | */..) die "MANIFEST path '$path' is not safe" ;; esac
        case "$type" in
            file)
                [ -f "$d/$path" ] && [ ! -L "$d/$path" ] || die "MANIFEST lists $path but it is not a regular file"
                got="$(sha256_of "$d/$path")"
                [ "$got" = "$a" ] || die "sha256 mismatch for $path: MANIFEST $a, actual $got"
                [ "$(mode_of "$d/$path")" = "$mode" ] || die "mode mismatch for $path: MANIFEST $mode, actual $(mode_of "$d/$path")"
                echo "$path" >>"$TMP/man.files"
                ;;
            link)
                [ -L "$d/$path" ] || die "MANIFEST lists $path as a link but it is not"
                [ "$(readlink "$d/$path")" = "$a" ] || die "link target mismatch for $path"
                ;;
            *) die "MANIFEST has an unknown entry type '$type'" ;;
        esac
        n=$((n + 1))
    done < <(grep -E '^(file|link) ' "$m")
    LC_ALL=C sort "$TMP/man.files" >"$TMP/man.sorted"
    grep -vx 'MANIFEST.txt\|SHA256SUMS' "$TMP/have.files" >"$TMP/have.nomf" || true
    cmp -s "$TMP/man.sorted" "$TMP/have.nomf" || die "MANIFEST and the kit's files differ: $(diff "$TMP/man.sorted" "$TMP/have.nomf" | head -3 | tr '\n' ' ')"
    local find_links
    find_links="$(cd "$d" && find . -type l | wc -l | tr -d ' ')"
    [ "$find_links" = "$(grep -c '^link ' "$m" || true)" ] || die "symlinks in the kit are not all listed in MANIFEST.txt"

    # 3. the payload we promise.
    for path in bin/ak_rtsp bin/dropbearmulti bin/flash_tool patches/patch_service_sh.sh licenses/LICENSE.akam licenses/THIRD_PARTY_NOTICES.md licenses/LICENSE.dropbear; do
        grep -qx "$path" "$TMP/man.sorted" || die "required file $path is not in the kit"
    done
    echo "ok: $n MANIFEST entries match (akam_version $ver, source $(sed -n 's/^source_commit=//p' "$m" | cut -c1-12))"
}

KIT_DIR=""
if [ -d "$INPUT" ]; then
    KIT_DIR="$(cd "$INPUT" && pwd)"
    echo "note: directory mode: the tarball signature is not checked here; verify the .tar.xz it came from."
    verify_dir "$KIT_DIR"
    echo "RESULT: kit directory consistent (not authenticated)"
    exit 0
fi
[ -f "$INPUT" ] || { echo "verify_kit: $INPUT: no such file or directory" >&2; exit 2; }
verify_tarball "$INPUT"
verify_dir "$KIT_DIR"
if [ -n "$EXTRACT_TO" ]; then
    cp -a "$KIT_DIR/." "$EXTRACT_TO/"
    echo "extracted to $EXTRACT_TO"
fi
if [ -n "$SIGNED_BY" ]; then
    echo "RESULT: VERIFIED and signed"
    exit 0
fi
if [ "$ALLOW_UNSIGNED" = 1 ]; then
    echo "RESULT: checksums OK, UNSIGNED (accepted with --allow-unsigned)"
    exit 0
fi
echo "RESULT: checksums OK but the kit is UNSIGNED: refusing (use --allow-unsigned to accept checksum-only trust)" >&2
exit 3
