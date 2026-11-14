#!/usr/bin/env bash
# Build the Akam release kit: akam-kit-<version>.tar.xz + SHA256SUMS.
#
#   tools/release/make_kit.sh [--out DIR] [--no-build]
#
# The kit holds ONLY Akam's own files: ARM binaries built here (ak_rtsp,
# dropbearmulti, flash_tool), the image overlay (wrapper, ini, akam_*.sh), the
# service.sh patch, flash/hwcheck scripts and the licence texts. It never
# contains anything from a camera dump or from the vendor firmware. The user
# supplies their own APP dump and assembles the image locally (spec section 4).
#
# Steps: clean build -> strip copies -> time64 machine-code gate on ak_rtsp AND
# dropbearmulti (blocking) -> normalised tree -> MANIFEST.txt + SHA256SUMS
# inside the kit -> reproducible tarball -> SHA256SUMS next to the tarball.
# Signing is a separate step (tools/release/sign_kit.sh).
#
# Which Akam files go into the image (overlay, patch, links) is listed ONCE, in
# tools/release/image_files.txt; tests/kit_test.sh checks build_firmware.sh against it.
# tools/release/supported_hardware.txt is copied into the kit and hashed in MANIFEST.txt.
#
# Environment (all optional):
#   SOURCE_DATE_EPOCH     build timestamp (default: commit date of HEAD)
#   AKAM_KIT_VERSION      version string (default: git describe --always)
#   AKAM_KIT_ALLOW_DIRTY=1  build from a dirty tree (never for a release)
#   AKAM_KIT_NO_BUILD=1   same as --no-build: use the prebuilt inputs below
#   Any of these, or AKAM_KIT_CHECK_TIME32, marks the kit "kit_grade=test" in
#   MANIFEST.txt; verify_kit.sh refuses such a kit unless --allow-test-kit is given.
#   AK_RTSP_BIN, FLASH_TOOL_BIN, DROPBEAR_MULTI (+ LICENSE.dropbear next to it)
#   SMOLRTSP_DIR, MUSL_COPYRIGHT, ZIG_LICENSE    licence sources (as build_firmware.sh)
#   AKAM_KIT_CHECK_TIME32 alternative gate command (tests only)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
KIT_FORMAT=1
TARGET=arm-linux-musleabi

die() { echo "make_kit: error: $*" >&2; exit 1; }
log() { echo "make_kit: $*"; }

OUT="$ROOT/dist/kit"
NO_BUILD="${AKAM_KIT_NO_BUILD:-0}"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) [ $# -ge 2 ] || die "--out needs a directory"; OUT=$2; shift ;;
        --no-build) NO_BUILD=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

for t in tar xz sha256sum git install find sort awk; do
    command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"
done
tar --version 2>/dev/null | grep -q 'GNU tar' || die "GNU tar is required (--sort/--mtime/--owner)"

# --- identity --------------------------------------------------------------------
COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)" || die "not a git checkout: cannot record the source commit"
# A release kit is built from a clean tree: untracked (not ignored) files count as
# dirty too, they could be picked up by the build.
KIT_GRADE=release
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]; then
    [ "${AKAM_KIT_ALLOW_DIRTY:-0}" = 1 ] || die "working tree is not clean (uncommitted or untracked files; set AKAM_KIT_ALLOW_DIRTY=1 for a non-release test kit)"
    KIT_GRADE="test"
fi
[ "${AKAM_KIT_ALLOW_DIRTY:-0}" != 1 ] || KIT_GRADE="test"
[ "$NO_BUILD" != 1 ] || KIT_GRADE="test"
[ -z "${AKAM_KIT_CHECK_TIME32:-}" ] || KIT_GRADE="test"
VERSION="${AKAM_KIT_VERSION:-$(git -C "$ROOT" describe --always 2>/dev/null)}"
case "$VERSION" in '' | *[!A-Za-z0-9._+-]*) die "bad version string '$VERSION' (allowed: A-Z a-z 0-9 . _ + -)" ;; esac
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
case "$EPOCH" in '' | *[!0-9]*) die "SOURCE_DATE_EPOCH must be an integer" ;; esac
BUILT_UTC="$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')"
# shellcheck source=tools/build/deps.lock
. "$ROOT/tools/build/deps.lock"
KITNAME="akam-kit-$VERSION"
# Always the pinned toolchain, never "whatever zig-* directory happens to be there".
PINNED_ZIG_DIR="$ROOT/tools/.toolchain/zig-$ZIG_VERSION-x86_64-linux"
PINNED_ZIG="$PINNED_ZIG_DIR/zig"

# --- inputs ----------------------------------------------------------------------
if [ "$NO_BUILD" != 1 ]; then
    command -v make >/dev/null 2>&1 || die "make not found"
    [ -x "$PINNED_ZIG" ] || die "pinned Zig missing: run tools/build/bootstrap.sh"
    # shellcheck source=tools/build/env.sh
    . "$ROOT/tools/build/env.sh"
    log "building ak_rtsp and flash_tool from a clean tree"
    make -C "$ROOT/src/ak_rtsp" clean >/dev/null
    make -C "$ROOT/src/ak_rtsp" ak_rtsp flash_tool >/dev/null
    AK_RTSP_BIN="$ROOT/src/ak_rtsp/ak_rtsp"
    FLASH_TOOL_BIN="$ROOT/src/ak_rtsp/flash_tool"
    log "building dropbearmulti"
    rm -rf "$ROOT/dist/dropbear"
    bash "$ROOT/tools/dropbear/build_dropbear.sh" >/dev/null
    DROPBEAR_MULTI="$ROOT/dist/dropbear/dropbearmulti"
fi
AK_RTSP_BIN="${AK_RTSP_BIN:-}"
FLASH_TOOL_BIN="${FLASH_TOOL_BIN:-}"
DROPBEAR_MULTI="${DROPBEAR_MULTI:-}"
for v in AK_RTSP_BIN FLASH_TOOL_BIN DROPBEAR_MULTI; do
    [ -f "${!v}" ] || die "$v is not a file (build first, or set it with --no-build)"
done
DROPBEAR_DIR="$(dirname "$DROPBEAR_MULTI")"
DROPBEAR_LICENSE="$DROPBEAR_DIR/LICENSE.dropbear"
SMOLRTSP_DIR="${SMOLRTSP_DIR:-$ROOT/src/smolrtsp}"
MUSL_COPYRIGHT="${MUSL_COPYRIGHT:-$PINNED_ZIG_DIR/lib/libc/musl/COPYRIGHT}"
ZIG_LICENSE="${ZIG_LICENSE:-$PINNED_ZIG_DIR/LICENSE}"
LICENSES=(
    "LICENSE.akam:$ROOT/LICENSE"
    "THIRD_PARTY_NOTICES.md:$ROOT/THIRD_PARTY_NOTICES.md"
    "LICENSE.dropbear:$DROPBEAR_LICENSE"
    "LICENSE.musl:$MUSL_COPYRIGHT"
    "LICENSE.zig:$ZIG_LICENSE"
    "LICENSE.smolrtsp:$SMOLRTSP_DIR/LICENSE"
)
for d in slice99 metalang99 datatype99 interface99; do
    LICENSES+=("LICENSE.$d:$SMOLRTSP_DIR/build/_deps/$d-src/LICENSE")
done
for e in "${LICENSES[@]}"; do
    [ -s "${e#*:}" ] || die "licence source for ${e%%:*} missing or empty: ${e#*:} (run tools/build/bootstrap.sh)"
done
IMAGE_FILES="$HERE/image_files.txt"
SUPPORTED_HW="$HERE/supported_hardware.txt"
[ -s "$IMAGE_FILES" ] || die "$IMAGE_FILES missing"
[ -s "$SUPPORTED_HW" ] || die "$SUPPORTED_HW missing (the kit must say which hardware it is validated for)"
grep -qE '^validated\|' "$SUPPORTED_HW" || die "$SUPPORTED_HW lists no validated target"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
KIT="$TMP/$KITNAME"
mkdir -p "$KIT/bin" "$KIT/scripts/flash" "$KIT/licenses"

# --- stripped binary copies (build outputs are never modified) ---------------------
strip_copy() {
    local src=$1 dst=$2
    if [ "$(head -c4 "$src" | od -An -c | tr -d ' ')" != '177ELF' ]; then
        install -m 755 "$src" "$dst" # not an ELF (test stub)
        return 0
    fi
    if [ -n "${AKAM_STRIP:-}" ]; then
        $AKAM_STRIP "$src" "$dst.tmp" || die "AKAM_STRIP failed on $src"
    else
        local z="${ZIG:-$PINNED_ZIG}"
        [ -x "$z" ] || die "no strip tool (zig objcopy): run tools/build/bootstrap.sh"
        "$z" objcopy --strip-all "$src" "$dst.tmp" || die "zig objcopy failed on $src"
    fi
    [ -s "$dst.tmp" ] || die "stripped copy of $src is empty"
    install -m 755 "$dst.tmp" "$dst"
    rm -f "$dst.tmp"
}
strip_copy "$AK_RTSP_BIN" "$KIT/bin/ak_rtsp"
strip_copy "$DROPBEAR_MULTI" "$KIT/bin/dropbearmulti"
strip_copy "$FLASH_TOOL_BIN" "$KIT/bin/flash_tool"

# --- time64 gate (blocking), on the files that go into the kit ------------------------
GATE_TOOL="${AKAM_KIT_CHECK_TIME32:-$ROOT/tools/build/check_time32.sh}"
log "time64 gate on ak_rtsp and dropbearmulti"
if ! bash "$GATE_TOOL" --require-263 "$KIT/bin/ak_rtsp" "$KIT/bin/dropbearmulti"; then
    die "time64 gate FAILED: the binaries would misbehave on the camera's kernel (docs/how-it-works.md). No kit written."
fi
GATE_DISASM="$(gdb --version 2>/dev/null | head -1 || true)"
[ -n "$GATE_DISASM" ] || GATE_DISASM=unknown

# --- overlay, patch, links, hwcheck: from the single list (image_files.txt) -------------
while read -r kind a b c d; do
    case "$kind" in
        '' | '#'*) continue ;;
        file) # a=mode b=kit path c=image path d=source in the repository
            case "$b$d" in /* | *..* | *' '*) die "image_files.txt: unsafe path in '$kind $a $b $c $d'" ;; esac
            [ -f "$ROOT/$d" ] || die "image_files.txt: source $d missing"
            if ! git -C "$ROOT" ls-files --error-unmatch -- "$d" >/dev/null 2>&1; then
                [ "$KIT_GRADE" = test ] || die "$d is not tracked by git"
            fi
            mkdir -p "$KIT/$(dirname "$b")"
            install -m "$a" "$ROOT/$d" "$KIT/$b"
            ;;
        link) # a=target b=kit path c=image path
            case "$a$b" in /* | *..* | *' '*) die "image_files.txt: unsafe link in '$kind $a $b'" ;; esac
            mkdir -p "$KIT/$(dirname "$b")"
            ln -s "$a" "$KIT/$b"
            ;;
        *) die "image_files.txt: unknown entry kind '$kind'" ;;
    esac
done <"$IMAGE_FILES"
# Flash scripts: only the tracked ones (a stray local file must not ship).
while IFS= read -r f; do
    install -m 755 "$ROOT/$f" "$KIT/scripts/flash/$(basename "$f")"
done < <(git -C "$ROOT" ls-files -- 'tools/flash_scripts/*.sh')
[ -n "$(find "$KIT/scripts/flash" -type f)" ] || die "no tracked flash scripts found"
install -m 644 "$SUPPORTED_HW" "$KIT/supported_hardware.txt"
for e in "${LICENSES[@]}"; do
    install -m 644 "${e#*:}" "$KIT/licenses/${e%%:*}"
done

# SOURCES.txt: where every input comes from (source offer).
{
    echo "Akam source offer"
    echo "akam_repository=https://github.com/Foohx/akam-fw"
    echo "source_commit=$COMMIT"
    echo "The complete corresponding sources are the git tree at that commit."
    echo
    echo "# Pinned third-party inputs (tools/build/deps.lock)"
    grep -E '^(ZIG|SMOLRTSP|SLICE99|METALANG99|DATATYPE99|INTERFACE99|DROPBEAR)_(VERSION|URL|COMMIT|TAG|SHA256)=' "$ROOT/tools/build/deps.lock"
    echo
    echo "# Dropbear patches applied (tools/dropbear/patches)"
    for p in "$ROOT"/tools/dropbear/patches/*.patch; do
        [ -f "$p" ] && echo "$(sha256sum "$p" | cut -d' ' -f1)  tools/dropbear/patches/$(basename "$p")"
    done
    if [ -f "$DROPBEAR_DIR/BUILDINFO.txt" ]; then
        echo
        echo "# Dropbear build info"
        cat "$DROPBEAR_DIR/BUILDINFO.txt"
    fi
} >"$KIT/SOURCES.txt"
chmod 644 "$KIT/SOURCES.txt"

# --- content guard: no key, no image, no vendor blob, no third-party toolkit file ------------------------------------------
BAD="$( (grep -rIlE -e 'BEGIN [A-Z0-9 ]*PRIVATE KEY' "$KIT" 2>/dev/null || true
    find "$KIT" \( -name '*_host_key*' -o -name 'id_rsa*' -o -name 'id_ed25519*' -o -name '*.pem' -o -name '*.squashfs' -o -name '*.img' -o -name '*.ko' \)
    # lsc-tuya-toolkit (tasarren) has no licence file: it is never redistributed (THIRD_PARTY_NOTICES.md)
    find "$KIT" \( -ipath '*tuya-toolkit*' -o -ipath '*/toolkit/*' -o -name 'toolkit' -o -ipath '*/sd_card/*' \)
    grep -rl --binary-files=binary -e 'hsqs' "$KIT/scripts" "$KIT/overlay" "$KIT/patches" "$KIT/licenses" 2>/dev/null || true) | sort -u)"
[ -z "$BAD" ] || die "forbidden content in the kit (key, image, vendor or toolkit file):
$BAD"
[ "$(find "$KIT/bin" -type f -printf "%f\n" | LC_ALL=C sort | tr "\n" " ")" = 'ak_rtsp dropbearmulti flash_tool ' ] || die "unexpected content in bin/"

# --- normalise ----------------------------------------------------------------------------
find "$KIT" -type d -exec chmod 755 {} +

# --- MANIFEST.txt and inner SHA256SUMS -------------------------------------------------------
sha() { sha256sum "$1" | cut -d' ' -f1; }
{
    echo "kit_format=$KIT_FORMAT"
    echo "kit_grade=$KIT_GRADE"
    echo "akam_version=$VERSION"
    echo "source_commit=$COMMIT"
    echo "built_utc=$BUILT_UTC"
    echo "deps_lock_sha256=$(sha "$ROOT/tools/build/deps.lock")"
    echo "zig_version=$ZIG_VERSION"
    echo "target=$TARGET"
    echo "time64_gate=pass"
    echo "time64_gate_tool=tools/build/check_time32.sh sha256=$(sha "$ROOT/tools/build/check_time32.sh")"
    echo "time64_gate_disassembler=$GATE_DISASM"
    echo "time64_gate_date=$BUILT_UTC"
    echo "supported_hardware_sha256=$(sha "$SUPPORTED_HW")"
    echo "supported_hardware=supported_hardware.txt ($(grep -c '^validated|' "$SUPPORTED_HW") validated, $(grep -c '^reported|' "$SUPPORTED_HW") reported, $(grep -c '^untested|' "$SUPPORTED_HW") untested records)"
    echo "# file <sha256> <mode> <path>"
    echo "# link <target> <mode> <path>"
    (cd "$KIT" && find . -type f ! -name MANIFEST.txt | LC_ALL=C sort | sed 's|^\./||') | while IFS= read -r f; do
        echo "file $(sha "$KIT/$f") $(stat -c %a "$KIT/$f") $f"
    done
    (cd "$KIT" && find . -type l | LC_ALL=C sort | sed 's|^\./||') | while IFS= read -r f; do
        echo "link $(readlink "$KIT/$f") 777 $f"
    done
} >"$KIT/MANIFEST.txt"
chmod 644 "$KIT/MANIFEST.txt"
(cd "$KIT" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | while IFS= read -r f; do
    echo "$(sha256sum "$f" | cut -d' ' -f1)  $f"
done) >"$TMP/SHA256SUMS.inner"
install -m 644 "$TMP/SHA256SUMS.inner" "$KIT/SHA256SUMS"

# --- reproducible tarball -----------------------------------------------------------------------
mkdir -p "$OUT"
TARBALL="$OUT/$KITNAME.tar.xz"
rm -f "$TARBALL" "$OUT/SHA256SUMS" "$OUT/SHA256SUMS.sig"
# every entry gets the fixed epoch as mtime; names are sorted.
(cd "$TMP" && LC_ALL=C tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner \
    --format=gnu -cf - "$KITNAME") | xz -9e -T1 >"$TARBALL"
(cd "$OUT" && sha256sum "$KITNAME.tar.xz" >SHA256SUMS)
log "wrote $TARBALL (kit_grade=$KIT_GRADE)"
cat "$OUT/SHA256SUMS"
