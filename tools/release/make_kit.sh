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
# Environment (all optional):
#   SOURCE_DATE_EPOCH     build timestamp (default: commit date of HEAD)
#   AKAM_KIT_VERSION      version string (default: git describe --always)
#   AKAM_KIT_ALLOW_DIRTY=1  build from a dirty tree (never for a release)
#   AKAM_KIT_NO_BUILD=1   same as --no-build: use the prebuilt inputs below
#   AK_RTSP_BIN, FLASH_TOOL_BIN, DROPBEAR_MULTI (+ LICENSE.dropbear next to it)
#   SMOLRTSP_DIR, MUSL_COPYRIGHT, ZIG_LICENSE    licence sources (as build_firmware.sh)
#   AKAM_KIT_CHECK_TIME32 alternative gate command (tests only)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TEMPLATES="$ROOT/tools/firmware_patch_templates"
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
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ] && [ "${AKAM_KIT_ALLOW_DIRTY:-0}" != 1 ]; then
    die "working tree has uncommitted changes (set AKAM_KIT_ALLOW_DIRTY=1 for a non-release test kit)"
fi
VERSION="${AKAM_KIT_VERSION:-$(git -C "$ROOT" describe --always 2>/dev/null)}"
case "$VERSION" in '' | *[!A-Za-z0-9._+-]*) die "bad version string '$VERSION' (allowed: A-Z a-z 0-9 . _ + -)" ;; esac
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
case "$EPOCH" in '' | *[!0-9]*) die "SOURCE_DATE_EPOCH must be an integer" ;; esac
BUILT_UTC="$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')"
# shellcheck source=tools/build/deps.lock
. "$ROOT/tools/build/deps.lock"
KITNAME="akam-kit-$VERSION"

# --- inputs ----------------------------------------------------------------------
if [ "$NO_BUILD" != 1 ]; then
    command -v make >/dev/null 2>&1 || die "make not found"
    [ -x "$ROOT/tools/.toolchain/zig-$ZIG_VERSION-x86_64-linux/zig" ] || die "pinned Zig missing: run tools/build/bootstrap.sh"
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
first_glob() { local f r=""; for f in "$@"; do [ -f "$f" ] && r=$f; done; printf '%s' "$r"; }
MUSL_COPYRIGHT="${MUSL_COPYRIGHT:-$(first_glob "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/lib/libc/musl/COPYRIGHT)}"
ZIG_LICENSE="${ZIG_LICENSE:-$(first_glob "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/LICENSE)}"
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
for f in akam_ssh.sh akam_lockdown.sh patch_service_sh.sh ak_rtsp_wrapper.sh _ht_process.ini; do
    [ -f "$TEMPLATES/$f" ] || die "template $TEMPLATES/$f missing"
done
[ -f "$ROOT/tools/hw_tests/akam_hwcheck.sh" ] || die "akam_hwcheck.sh missing"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
KIT="$TMP/$KITNAME"
mkdir -p "$KIT/bin" "$KIT/overlay/sbin" "$KIT/overlay/local" "$KIT/overlay/bin" \
    "$KIT/patches" "$KIT/scripts/flash" "$KIT/scripts/hwcheck" "$KIT/licenses"

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
        local z="${ZIG:-}"
        [ -x "$z" ] || z="$(first_glob "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/zig)"
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
    die "time64 gate FAILED: the binaries would misbehave on the camera's kernel (docs/akam_v1_kernel_time64.md). No kit written."
fi
GATE_DISASM="$(gdb --version 2>/dev/null | head -1 || true)"
[ -n "$GATE_DISASM" ] || GATE_DISASM=unknown

# --- overlay, patches, scripts, licences -------------------------------------------------
install -m 755 "$TEMPLATES/ak_rtsp_wrapper.sh" "$KIT/overlay/sbin/ak_rtsp_wrapper.sh"
install -m 755 "$TEMPLATES/akam_ssh.sh" "$KIT/overlay/sbin/akam_ssh.sh"
install -m 755 "$TEMPLATES/akam_lockdown.sh" "$KIT/overlay/sbin/akam_lockdown.sh"
install -m 644 "$TEMPLATES/_ht_process.ini" "$KIT/overlay/local/_ht_process.ini"
ln -s dropbearmulti "$KIT/overlay/bin/dropbear"
ln -s dropbearmulti "$KIT/overlay/bin/dropbearkey"
install -m 755 "$TEMPLATES/patch_service_sh.sh" "$KIT/patches/patch_service_sh.sh"
for f in "$ROOT"/tools/flash_scripts/*.sh; do
    install -m 755 "$f" "$KIT/scripts/flash/$(basename "$f")"
done
install -m 755 "$ROOT/tools/hw_tests/akam_hwcheck.sh" "$KIT/scripts/hwcheck/akam_hwcheck.sh"
for e in "${LICENSES[@]}"; do
    install -m 644 "${e#*:}" "$KIT/licenses/${e%%:*}"
done

# SOURCES.txt: where every input comes from (source offer).
{
    echo "Akam source offer"
    echo "akam_repository=https://github.com/Foohx/akam"
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

# --- normalise ----------------------------------------------------------------------------
find "$KIT" -type d -exec chmod 755 {} +

# --- MANIFEST.txt and inner SHA256SUMS -------------------------------------------------------
sha() { sha256sum "$1" | cut -d' ' -f1; }
{
    echo "kit_format=$KIT_FORMAT"
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
    echo "hardware_validation=see README.md (Status) of the source commit"
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
log "wrote $TARBALL"
cat "$OUT/SHA256SUMS"
