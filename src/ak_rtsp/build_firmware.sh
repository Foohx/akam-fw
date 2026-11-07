#!/bin/bash
# Assembles the patched APP squashfs tree and builds the flashable, padded
# firmware image. Invoked by `make firmware` after ak_rtsp itself has been
# built, and by the flasher as
#     APP_EXTRACTED=<tree> FW_DIR=<out> bash src/ak_rtsp/build_firmware.sh
#
# This script does NOT ship a stock APP partition dump -- that's your
# camera's proprietary Tuya firmware and can't be redistributed. You must
# supply your own unsquashed dump as app_extracted/ (see
# docs/building_your_own_firmware.md). Steps:
#   1. copy APP_EXTRACTED to $FW_DIR/app_patched/ (always from scratch, unless
#      KEEP_PATCHED=1; the Camyn install/patch steps below run in both cases)
#   2. de-Tuyafication: remove anyka_ipc, install ak_rtsp + wrapper + ini
#   3. Camyn: install dropbearmulti (+ dropbear/dropbearkey symlinks),
#      camyn_ssh.sh, camyn_lockdown.sh, LICENSE.dropbear, MANIFEST.txt;
#      patch sbin/service.sh (telnet/ftp off)
#   4. safety checks (modes, no private key), mksquashfs, round-trip verify,
#      size guard (BEFORE padding), pad to exactly 5242880 bytes, report,
#      SHA-256 file.
# No key of any kind is ever put in the image: authorized_keys comes from the
# microSD at runtime (spec O-5).
#
# Env vars (all optional):
#   APP_EXTRACTED   unsquashed stock APP dump (default: ./app_extracted)
#   FW_DIR          output directory (default: this script's directory)
#   AK_RTSP_BIN     ak_rtsp binary (default: ./ak_rtsp next to this script)
#   SMOLRTSP_DIR    SmolRTSP checkout for licence texts (default: src/smolrtsp)
#   MUSL_COPYRIGHT  musl COPYRIGHT (default: from the pinned Zig tarball)
#   DROPBEAR_MULTI  prebuilt dropbearmulti (default: dist/dropbear/dropbearmulti,
#                   built with tools/dropbear/build_dropbear.sh if missing)
#   CAMYN_SQUASHFS_COMP  gzip (default) | xz
#   CAMYN_ALLOW_UNPATCHED_SERVICE=1  continue when service.sh could not be patched
#   CAMYN_ALLOW_UNSTRIPPED=1  install ELF binaries unstripped when no strip tool is found
#   CAMYN_ALLOW_TIGHT=1  allow an image within one block of the partition size
#   KEEP_PATCHED=1  reuse an existing app_patched/ instead of starting over
#   SOURCE_DATE_EPOCH  fixed timestamp for the squashfs (reproducible content)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMPLATES="$ROOT/tools/firmware_patch_templates"
PARTITION=5242880   # mtd7 (APP) size
BLOCK=131072

APP_EXTRACTED="${APP_EXTRACTED:-$SCRIPT_DIR/app_extracted}"
FW_DIR="${FW_DIR:-$SCRIPT_DIR}"
PATCHED="$FW_DIR/app_patched"
BINARY="${AK_RTSP_BIN:-$SCRIPT_DIR/ak_rtsp}"
COMP="${CAMYN_SQUASHFS_COMP:-${CAMYN_SQUASH_COMP:-gzip}}"
EPOCH="${SOURCE_DATE_EPOCH:-$(date +%s)}"

die() { echo "FATAL: $*" >&2; exit 1; }
WARNINGS=()
warn() { echo "WARNING: $*" >&2; WARNINGS+=("$*"); }

case "$COMP" in gzip | xz) ;; *) die "CAMYN_SQUASHFS_COMP must be gzip or xz (got '$COMP')" ;; esac
case "$EPOCH" in '' | *[!0-9]*) die "SOURCE_DATE_EPOCH must be an integer" ;; esac
for t in mksquashfs unsquashfs sha256sum; do
    command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"
done

if [ ! -d "$APP_EXTRACTED" ]; then
    echo "FATAL: $APP_EXTRACTED not found." >&2
    echo "This must be your own unsquashed dump of the camera's stock APP" >&2
    echo "partition (mtdblock7) -- it is NOT included in this repo since it's" >&2
    echo "the vendor's proprietary firmware. See docs/building_your_own_firmware.md" >&2
    echo "for how to dump and unsquash your own, or set APP_EXTRACTED to point" >&2
    echo "at an existing extraction." >&2
    exit 1
fi
for d in bin sbin local; do
    [ -d "$APP_EXTRACTED/$d" ] || die "$APP_EXTRACTED has no $d/ -- not an APP tree"
done
[ -f "$BINARY" ] || die "ak_rtsp binary not found at $BINARY (build it first: make -C src/ak_rtsp ak_rtsp, or set AK_RTSP_BIN)"

# Dropbear: prebuilt, or build it with the pinned toolchain.
DROPBEAR_MULTI="${DROPBEAR_MULTI:-$ROOT/dist/dropbear/dropbearmulti}"
if [ ! -f "$DROPBEAR_MULTI" ]; then
    echo "dropbearmulti not found, building it..."
    bash "$ROOT/tools/dropbear/build_dropbear.sh"
fi
[ -f "$DROPBEAR_MULTI" ] || die "dropbearmulti not found at $DROPBEAR_MULTI"
DROPBEAR_DIR="$(dirname "$DROPBEAR_MULTI")"
DROPBEAR_LICENSE="${DROPBEAR_LICENSE:-$DROPBEAR_DIR/LICENSE.dropbear}"
[ -f "$DROPBEAR_LICENSE" ] || die "LICENSE.dropbear not found at $DROPBEAR_LICENSE (the licence must ship with the binary)"
# Licence texts of statically linked / macro-expanded third-party code, copied
# from the pinned sources (never hand-written). Missing = fatal.
SMOLRTSP_DIR="${SMOLRTSP_DIR:-$ROOT/src/smolrtsp}"
if [ -z "${MUSL_COPYRIGHT:-}" ]; then
    MUSL_COPYRIGHT=""
    for f in "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/lib/libc/musl/COPYRIGHT; do
        [ -f "$f" ] && MUSL_COPYRIGHT="$f"
    done
fi
[ -f "${MUSL_COPYRIGHT:-}" ] || die "musl COPYRIGHT not found (run tools/build/bootstrap.sh or set MUSL_COPYRIGHT)"
LIC_SRC=("$SMOLRTSP_DIR/LICENSE")
for d in slice99 metalang99 datatype99 interface99; do LIC_SRC+=("$SMOLRTSP_DIR/build/_deps/$d-src/LICENSE"); done
for f in "${LIC_SRC[@]}"; do [ -s "$f" ] || die "licence file $f missing (run tools/build/bootstrap.sh)"; done
for f in camyn_ssh.sh camyn_lockdown.sh patch_service_sh.sh ak_rtsp_wrapper.sh _ht_process.ini; do
    [ -f "$TEMPLATES/$f" ] || die "template $TEMPLATES/$f missing"
done

mkdir -p "$FW_DIR"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Flash budget: the Makefile builds ak_rtsp with -g (~1.6 MB vs ~130 KB
# stripped). Install a stripped COPY; the build output is never modified.
# Uses `zig objcopy --strip-all` (pinned toolchain: $ZIG or zig on PATH), or
# $CAMYN_STRIP (a command taking "<in> <out>") if set.
# Use the pinned toolchain from tools/.toolchain when zig is not on PATH.
zig_pinned() {
    local z
    for z in "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/zig; do
        [ -x "$z" ] && { ZIG="$z"; return 0; }
    done
    return 1
}
install_stripped() {
    local src=$1 dst=$2 tmp="$TMP/strip.tmp"
    if [ "$(head -c4 "$src" | od -An -c | tr -d ' ')" != '177ELF' ]; then
        install -m 755 "$src" "$dst" # not an ELF (test stub): nothing to strip
        return 0
    fi
    if [ -n "${CAMYN_STRIP:-}" ]; then
        $CAMYN_STRIP "$src" "$tmp" || die "CAMYN_STRIP failed on $src"
    elif [ -x "${ZIG:-}" ] || command -v zig >/dev/null 2>&1 || zig_pinned; then
        "${ZIG:-zig}" objcopy --strip-all "$src" "$tmp" || die "zig objcopy --strip-all failed on $src"
    elif [ "${CAMYN_ALLOW_UNSTRIPPED:-0}" = 1 ]; then
        warn "no strip tool (zig objcopy): $(basename "$dst") installed UNSTRIPPED, image will be larger (CAMYN_ALLOW_UNSTRIPPED=1)"
        install -m 755 "$src" "$dst"
        return 0
    else
        die "no strip tool for $(basename "$dst"): run bootstrap.sh and source tools/build/env.sh (or set CAMYN_STRIP, or CAMYN_ALLOW_UNSTRIPPED=1)"
    fi
    [ -s "$tmp" ] || die "stripped copy of $src is empty"
    install -m 755 "$tmp" "$dst"
    rm -f "$tmp"
}

# --- 1. tree -----------------------------------------------------------------
if [ "${KEEP_PATCHED:-0}" = 1 ] && [ -d "$PATCHED" ]; then
    echo "KEEP_PATCHED=1: re-using $PATCHED (Camyn install/patch steps are re-applied)."
else
    rm -rf "$PATCHED"
    echo "Creating app_patched/ from $APP_EXTRACTED..."
    cp -a "$APP_EXTRACTED" "$PATCHED"
fi

# --- 2. de-Tuyafication --------------------------------------------------------
echo "Applying de-Tuyafication patch..."
rm -f "$PATCHED/bin/anyka_ipc"
install -m 755 "$TEMPLATES/ak_rtsp_wrapper.sh" "$PATCHED/sbin/ak_rtsp_wrapper.sh"
install -m 644 "$TEMPLATES/_ht_process.ini" "$PATCHED/local/_ht_process.ini"
install_stripped "$BINARY" "$PATCHED/bin/ak_rtsp"

# --- 3. Camyn ------------------------------------------------------------------
echo "Installing Camyn components..."
rm -f "$PATCHED/bin/dropbearmulti" "$PATCHED/bin/dropbear" "$PATCHED/bin/dropbearkey"
install_stripped "$DROPBEAR_MULTI" "$PATCHED/bin/dropbearmulti"
ln -s dropbearmulti "$PATCHED/bin/dropbear"
ln -s dropbearmulti "$PATCHED/bin/dropbearkey"
install -m 755 "$TEMPLATES/camyn_ssh.sh" "$PATCHED/sbin/camyn_ssh.sh"
install -m 755 "$TEMPLATES/camyn_lockdown.sh" "$PATCHED/sbin/camyn_lockdown.sh"
mkdir -p "$PATCHED/local/camyn"
chmod 755 "$PATCHED/local/camyn"
install -m 644 "$DROPBEAR_LICENSE" "$PATCHED/local/camyn/LICENSE.dropbear"
install -m 644 "$MUSL_COPYRIGHT" "$PATCHED/local/camyn/LICENSE.musl"
install -m 644 "$SMOLRTSP_DIR/LICENSE" "$PATCHED/local/camyn/LICENSE.smolrtsp"
for d in slice99 metalang99 datatype99 interface99; do
    install -m 644 "$SMOLRTSP_DIR/build/_deps/$d-src/LICENSE" "$PATCHED/local/camyn/LICENSE.$d"
done

echo "Patching sbin/service.sh (telnet/ftp off, SD hook guarded)..."
SVC_RC=0
CAMYN_PATCH_REPORT="$FW_DIR/service_sh.diff" bash "$TEMPLATES/patch_service_sh.sh" "$PATCHED" || SVC_RC=$?
case "$SVC_RC" in
    0) ;;
    2 | 3)
        if [ "${CAMYN_ALLOW_UNPATCHED_SERVICE:-0}" = 1 ]; then
            warn "*** service.sh NOT patched (exit $SVC_RC): Telnet/FTP may still be launched by the stock boot script. CAMYN_ALLOW_UNPATCHED_SERVICE=1 set, continuing. ***"
        else
            die "patch_service_sh.sh exit $SVC_RC: service.sh not patched, image would not be locked down. Fix the patterns, or set CAMYN_ALLOW_UNPATCHED_SERVICE=1 to build anyway."
        fi
        ;;
    *) die "patch_service_sh.sh failed (exit $SVC_RC)" ;;
esac

AUDIT_OUT=""
if [ -f "$TEMPLATES/audit_remote_services.sh" ]; then
    (cd "$FW_DIR" && bash "$TEMPLATES/audit_remote_services.sh" "$PATCHED") || warn "audit_remote_services.sh failed"
    AUDIT_OUT="audit_remote_services.txt"
fi

SHA_AK="$(sha256sum "$PATCHED/bin/ak_rtsp" | cut -d' ' -f1)"
SHA_DB="$(sha256sum "$PATCHED/bin/dropbearmulti" | cut -d' ' -f1)"
GIT_DESC="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
{
    echo "camyn_version=$GIT_DESC"
    echo "build_date=$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')"
    echo "ak_rtsp_sha256=$SHA_AK"
    echo "dropbearmulti_sha256=$SHA_DB"
    for l in "$PATCHED"/local/camyn/LICENSE.*; do
        echo "licence: $(basename "$l") sha256=$(sha256sum "$l" | cut -d' ' -f1)"
    done
    if [ -f "$DROPBEAR_DIR/BUILDINFO.txt" ]; then
        sed 's/^/dropbear_buildinfo: /' "$DROPBEAR_DIR/BUILDINFO.txt"
    fi
} >"$PATCHED/local/camyn/MANIFEST.txt"
chmod 644 "$PATCHED/local/camyn/MANIFEST.txt"

# --- 4. safety checks -------------------------------------------------------------
echo "Checking tree..."
BADKEYS="$( (grep -rlE -e 'BEGIN [A-Z0-9 ]*PRIVATE KEY' "$PATCHED" 2>/dev/null || true
    find "$PATCHED" \( -name '*_host_key*' -o -name 'id_rsa*' -o -name 'id_ed25519*' \) ) | sort -u)"
[ -z "$BADKEYS" ] || die "private key material found in the image tree (O-5: none allowed):
$BADKEYS"
for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/camyn_ssh.sh sbin/camyn_lockdown.sh; do
    [ "$(stat -c %a "$PATCHED/$f")" = 755 ] || die "$f is not mode 755"
done
BADW="$(find "$PATCHED/local/camyn" \( -type f -o -type d \) -perm /022 2>/dev/null)"
[ -z "$BADW" ] || die "group/other-writable files in local/camyn: $BADW"
for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/camyn_ssh.sh sbin/camyn_lockdown.sh sbin/service.sh; do
    if [ ! -e "$PATCHED/$f" ] || [ -n "$(find "$PATCHED/$f" -perm /022)" ]; then
        die "$f is group/other-writable or missing"
    fi
done
STOCKW="$(find "$PATCHED/sbin" "$PATCHED/bin" -type f -perm /022 2>/dev/null | head -5)"
if [ -n "$STOCKW" ]; then
    warn "stock files writable by group/others in bin/ or sbin/ (not changed): $(echo "$STOCKW" | tr '\n' ' ')"
fi

# --- 5. squashfs ----------------------------------------------------------------------
SQ="$FW_DIR/new_app_${COMP}.squashfs"
rm -f "$SQ"
MKOPTS=(-comp "$COMP" -b "$BLOCK" -all-root -noappend -no-xattrs -no-progress -mkfs-time "$EPOCH" -all-time "$EPOCH")
[ "$COMP" = xz ] && MKOPTS+=(-Xbcj arm)
# mksquashfs refuses explicit time options when SOURCE_DATE_EPOCH is in the env.
env -u SOURCE_DATE_EPOCH mksquashfs "$PATCHED" "$SQ" "${MKOPTS[@]}" >/dev/null || die "mksquashfs failed (unsupported option? tool version: $(mksquashfs -version | head -1))"

# --- 6. round-trip ---------------------------------------------------------------------
echo "Round-trip verifying..."
INFO="$(unsquashfs -s "$SQ")"
grep -qi "^Compression $COMP\$" <<<"$INFO" || die "squashfs compression is not $COMP"
grep -q "^Block size $BLOCK\$" <<<"$INFO" || die "squashfs block size is not $BLOCK"
unsquashfs -no-xattrs -d "$TMP/rt" "$SQ" >/dev/null || die "unsquashfs failed"
# type, mode, symlink target and (for files) sha256 of every entry. Owner is
# excluded: the image is -all-root, the source tree is not.
tree_sig() {
    (cd "$1" && find . -printf '%y %m %p -> %l\n' | LC_ALL=C sort &&
        find . -type f -exec sha256sum {} + | LC_ALL=C sort -k2)
}
tree_sig "$PATCHED" >"$TMP/sig.src"
tree_sig "$TMP/rt" >"$TMP/sig.img"
if ! diff "$TMP/sig.src" "$TMP/sig.img"; then
    die "round-trip verification mismatch (types/modes/links/contents), aborting."
fi
echo "Round-trip verify OK ($(wc -l <"$TMP/sig.src") signature lines)."

# --- 7. size guard (before padding) --------------------------------------------------------
SQ_SIZE="$(stat -c %s "$SQ")"
FREE=$((PARTITION - SQ_SIZE))
if [ "$SQ_SIZE" -gt "$PARTITION" ]; then
    die "squashfs is $SQ_SIZE bytes, larger than the $PARTITION byte APP partition (over by $((-FREE))). Nothing padded."
fi
if [ "$FREE" -lt "$BLOCK" ]; then
    if [ "${CAMYN_ALLOW_TIGHT:-0}" = 1 ]; then
        warn "only $FREE bytes free in the APP partition (< one block); CAMYN_ALLOW_TIGHT=1"
    else
        die "only $FREE bytes free in the APP partition (< $BLOCK, one block of margin). Set CAMYN_ALLOW_TIGHT=1 to accept."
    fi
fi

# --- 8. pad ---------------------------------------------------------------------------------
bash "$ROOT/tools/flash_scripts/pad_build.sh" "$SQ"
OUT="$(find "$FW_DIR" -maxdepth 1 -name 'ak_rtsp_firmware_*.squashfs' | LC_ALL=C sort | tail -1)"
[ -n "$OUT" ] || die "padded image not found in $FW_DIR"
[ "$(stat -c %s "$OUT")" -eq "$PARTITION" ] || die "padded image is not exactly $PARTITION bytes"
cmp -n "$SQ_SIZE" "$SQ" "$OUT" || die "padded image does not start with the squashfs"
[ "$(tail -c +"$((SQ_SIZE + 1))" "$OUT" | tr -d '\377' | wc -c)" -eq 0 ] || die "padding is not all 0xFF"
SHA_OUT="$(cd "$FW_DIR" && sha256sum "$(basename "$OUT")" | tee "$(basename "$OUT").sha256" | cut -d' ' -f1)"

# --- 9. report -----------------------------------------------------------------------------------
REPORT="$FW_DIR/build_report.txt"
{
    echo "Camyn firmware build report"
    echo "image:              $(basename "$OUT")"
    echo "image sha256:       $SHA_OUT"
    echo "squashfs sha256:    $(sha256sum "$SQ" | cut -d' ' -f1)  (content, before padding)"
    echo "compression:        $COMP, block $BLOCK"
    echo "squashfs size:      $SQ_SIZE bytes"
    echo "free in mtd7:       $FREE bytes ($((FREE * 100 / PARTITION))% of $PARTITION)"
    echo "added components (uncompressed file size):"
    for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/camyn_ssh.sh sbin/camyn_lockdown.sh local/camyn/LICENSE.dropbear local/camyn/LICENSE.musl local/camyn/LICENSE.smolrtsp local/camyn/MANIFEST.txt; do
        printf '  %-36s %9s bytes\n' "$f" "$(stat -c %s "$PATCHED/$f")"
    done
    echo "ak_rtsp sha256:     $SHA_AK"
    echo "dropbearmulti sha256: $SHA_DB"
    echo "10 largest files in the image:"
    (cd "$PATCHED" && find . -type f -printf '%s %p\n' | sort -rn | head -10 | sed 's/^/  /')
    [ -z "$AUDIT_OUT" ] || echo "remote-services audit: $AUDIT_OUT"
    echo "service.sh patch diff: service_sh.diff"
    echo "warnings: ${#WARNINGS[@]}"
    for w in "${WARNINGS[@]+"${WARNINGS[@]}"}"; do echo "  - $w"; done
} >"$REPORT"
echo
cat "$REPORT"
echo "(the free-space figure is only meaningful for the real APP dump)"
echo "Wrote $OUT and $OUT.sha256"
