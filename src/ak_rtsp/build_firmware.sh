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
#      KEEP_PATCHED=1; the Akam install/patch steps below run in both cases)
#   2. de-Tuyafication: remove anyka_ipc, install ak_rtsp + wrapper + ini
#   3. Akam: install dropbearmulti (+ dropbear/dropbearkey symlinks),
#      akam_ssh.sh, akam_lockdown.sh, akam_rtsp_auth.sh, akam_hostname.sh, akam_debug.sh, akam_net.sh, akam_onvif.sh, LICENSE.dropbear, MANIFEST.txt;
#      patch sbin/service.sh (telnet/ftp off)
#   4. safety checks (modes, no private key), mksquashfs, round-trip verify,
#      size guard (BEFORE padding), pad to exactly 5242880 bytes, report,
#      SHA-256 file.
#
# Two input modes, one assembly path (the Akam files come from
# tools/release/image_files.txt in both, so they cannot drift):
#   source mode (default)  binaries and licences from the build tree / env vars below
#   --kit DIR|TARBALL      binaries, overlay, service.sh patch and licences from a prebuilt kit,
#                          verified first with tools/release/verify_kit.sh (no compiler needed)
#
# Options:
#   --kit DIR|TARBALL       use a prebuilt kit (or AKAM_KIT=...)
#   --allow-unsigned        kit tarball without signature (checksum-only trust); also required for a
#                           kit DIRECTORY, which cannot be authenticated
#   --allow-test-kit        accept a kit_grade=test kit (development only)
#   --recheck-time64        kit mode: replay the time64 gate on the installed binaries (done by default
#                           for a release kit when gdb is installed; otherwise MANIFEST + sha256 are trusted)
#   --stock-app FILE        the dump of the stock APP partition (mtdblock7.bin): fingerprint it
#   --backup-dir DIR        the backup directory (mtdblock*.bin); implies --stock-app DIR/mtdblock7.bin
#                           and reads the kernel name from DIR/mtdblock4.bin
#   --stock-info FILE       key=value file (compression=, block_size=, bcj=, app_sha256=) recorded
#                           when only an extracted tree is available
#   --comp gzip|xz          override the compression (default: the stock APP's, else xz); a
#                           difference from the stock APP is reported loudly
#   --allow-unknown-stock-comp  the stock APP uses a compressor Akam cannot follow (lzo, lz4,
#                           zstd, ...): build anyway with --comp (check your kernel supports it!)
#   --i-understand-untested hardware/firmware not marked "validated" in supported_hardware.txt
#   Hardware check: with a stock fingerprint (--stock-app/--backup-dir/--stock-info) the dump is
#   compared with supported_hardware.txt; anything but "validated" is refused without
#   --i-understand-untested; a different MTD layout is always refused. In kit mode a missing
#   fingerprint counts as unknown. In source mode without fingerprint the check is skipped.
#
# No key of any kind is ever put in the image: authorized_keys comes from the
# microSD at runtime (spec O-5).
#
# Env vars (all optional):
#   APP_EXTRACTED   unsquashed stock APP dump (default: ./app_extracted)
#   FW_DIR          output directory (default: this script's directory)
#   AK_RTSP_BIN     ak_rtsp binary (default: ./ak_rtsp next to this script)
#   SMOLRTSP_DIR    SmolRTSP checkout for licence texts (default: src/smolrtsp)
#   MUSL_COPYRIGHT  musl COPYRIGHT (default: from the pinned Zig tarball)
#   ZIG_LICENSE     Zig LICENSE (compiler-rt is linked in; default: pinned Zig tarball)
#   DROPBEAR_MULTI  prebuilt dropbearmulti (default: dist/dropbear/dropbearmulti,
#                   built with tools/dropbear/build_dropbear.sh if missing)
#   AKAM_SQUASHFS_COMP  xz | gzip (same as --comp). Default: follow the stock APP, else xz
#                   (stock is xz, no BCJ, 128 KiB blocks)
#   AKAM_ALLOW_UNPATCHED_SERVICE=1  continue when service.sh could not be patched
#   AKAM_ALLOW_UNCHECKED_TIME64=1  skip the machine-code time64 gate (tools/build/check_time32.sh,
#                   needs gdb) on the installed ak_rtsp and dropbearmulti; for stub binaries only
#   AKAM_ALLOW_UNSTRIPPED=1  install ELF binaries unstripped when no strip tool is found
#   AKAM_ALLOW_TIGHT=1  allow an image within one block of the partition size
#   KEEP_PATCHED=1  reuse an existing app_patched/ instead of starting over
#   SOURCE_DATE_EPOCH  fixed timestamp for the squashfs (reproducible content)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMPLATES="$ROOT/tools/firmware_patch_templates"
PARTITION=5242880   # mtd7 (APP) size

APP_EXTRACTED="${APP_EXTRACTED:-$SCRIPT_DIR/app_extracted}"
FW_DIR="${FW_DIR:-$SCRIPT_DIR}"
PATCHED="$FW_DIR/app_patched"
BINARY="${AK_RTSP_BIN:-$SCRIPT_DIR/ak_rtsp}"
COMP_ARG="${AKAM_SQUASHFS_COMP:-${AKAM_SQUASH_COMP:-}}"
EPOCH="${SOURCE_DATE_EPOCH:-$(date +%s)}"
IMAGE_FILES="$ROOT/tools/release/image_files.txt"

die() { echo "FATAL: $*" >&2; exit 1; }
WARNINGS=()
warn() { echo "WARNING: $*" >&2; WARNINGS+=("$*"); }
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//' >&2; exit 2; }

KIT_ARG="${AKAM_KIT:-}"
ALLOW_UNSIGNED=0
ALLOW_TEST_KIT=0
RECHECK_TIME64=0
STOCK_APP=""
BACKUP_DIR=""
STOCK_INFO=""
ALLOW_UNKNOWN_STOCK_COMP=0
UNTESTED_OK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --kit) [ $# -ge 2 ] || usage; KIT_ARG=$2; shift ;;
        --allow-unsigned) ALLOW_UNSIGNED=1 ;;
        --allow-test-kit) ALLOW_TEST_KIT=1 ;;
        --recheck-time64) RECHECK_TIME64=1 ;;
        --stock-app) [ $# -ge 2 ] || usage; STOCK_APP=$2; shift ;;
        --backup-dir) [ $# -ge 2 ] || usage; BACKUP_DIR=$2; shift ;;
        --stock-info) [ $# -ge 2 ] || usage; STOCK_INFO=$2; shift ;;
        --comp) [ $# -ge 2 ] || usage; COMP_ARG=$2; shift ;;
        --allow-unknown-stock-comp) ALLOW_UNKNOWN_STOCK_COMP=1 ;;
        --i-understand-untested) UNTESTED_OK=1 ;;
        -h | --help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
    shift
done

case "$COMP_ARG" in '' | gzip | xz) ;; *) die "--comp / AKAM_SQUASHFS_COMP must be gzip or xz (got '$COMP_ARG')" ;; esac
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
[ -s "$IMAGE_FILES" ] || die "$IMAGE_FILES missing"

mkdir -p "$FW_DIR"
TMP="$(mktemp -d)"
# rm_tree <dir>...: rm -rf that also removes trees unpacked by a normal user from a squashfs with
# read-only directories (rm cannot delete inside them).
rm_tree() {
    local d
    for d in "$@"; do
        [ -e "$d" ] || continue
        chmod -R u+rwX "$d" 2>/dev/null || true
        rm -rf "$d"
    done
}
trap 'rm_tree "$TMP"' EXIT

# --- inputs: kit or sources ---------------------------------------------------------
# LICS: "<name in local/akam>:<source path>", installed 0644 by the common code below.
LICS=()
KIT=""
KIT_INFO=""
KIT_AUTH=""
if [ -n "$KIT_ARG" ]; then
    MODE=kit
    VERIFY_ARGS=()
    [ "$ALLOW_UNSIGNED" = 1 ] && VERIFY_ARGS+=(--allow-unsigned)
    [ "$ALLOW_TEST_KIT" = 1 ] && VERIFY_ARGS+=(--allow-test-kit)
    echo "Verifying the kit $KIT_ARG ..."
    if [ -d "$KIT_ARG" ]; then
        KIT="$(cd "$KIT_ARG" && pwd)"
        bash "$ROOT/tools/release/verify_kit.sh" "${VERIFY_ARGS[@]+"${VERIFY_ARGS[@]}"}" "$KIT" || die "kit verification failed (see above); nothing built"
        # A directory has no signature to check: it is exactly as (un)trusted as an unsigned tarball.
        [ "$ALLOW_UNSIGNED" = 1 ] || die "the kit is a DIRECTORY, which cannot be authenticated (no signature): refusing. Pass the signed .tar.xz, or --allow-unsigned if you trust where this directory came from."
        KIT_AUTH="directory (consistency checked, not authenticated; --allow-unsigned)"
    elif [ -f "$KIT_ARG" ]; then
        VRC=0
        bash "$ROOT/tools/release/verify_kit.sh" "${VERIFY_ARGS[@]+"${VERIFY_ARGS[@]}"}" --extract-to "$TMP/kit" "$KIT_ARG" || VRC=$?
        case "$VRC" in
            0) ;;
            3) die "the kit is UNSIGNED: refusing. Use --allow-unsigned only if you trust where it came from and its sha256 (checksum-only trust)." ;;
            *) die "kit verification failed (exit $VRC, see above); nothing built" ;;
        esac
        KIT="$TMP/kit"
        if [ -f "$(dirname "$KIT_ARG")/SHA256SUMS.sig" ]; then
            KIT_AUTH="signature verified"
        else
            KIT_AUTH="UNSIGNED (checksum-only trust, --allow-unsigned)"
        fi
        KIT_INFO="tarball sha256 $(sha256sum "$KIT_ARG" | cut -d' ' -f1)"
    else
        die "--kit $KIT_ARG: no such file or directory"
    fi
    kit_key() { sed -n "s/^$1=//p" "$KIT/MANIFEST.txt" | head -1; }
    KIT_VERSION="$(kit_key akam_version)"
    KIT_COMMIT="$(kit_key source_commit)"
    KIT_GRADE="$(kit_key kit_grade)"
    BINARY="$KIT/bin/ak_rtsp"
    DROPBEAR_MULTI="$KIT/bin/dropbearmulti"
    SUPPORTED_HW="$KIT/supported_hardware.txt"
    for n in LICENSE.akam THIRD_PARTY_NOTICES.md LICENSE.dropbear LICENSE.musl LICENSE.zig LICENSE.smolrtsp \
        LICENSE.slice99 LICENSE.metalang99 LICENSE.datatype99 LICENSE.interface99; do
        [ -s "$KIT/licenses/$n" ] || die "kit has no licenses/$n"
        LICS+=("$n:$KIT/licenses/$n")
    done
    DROPBEAR_DIR=""
else
    MODE=source
    SUPPORTED_HW="${AKAM_SUPPORTED_HARDWARE:-$ROOT/tools/release/supported_hardware.txt}" # env: tests only
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
    if [ -z "${ZIG_LICENSE:-}" ]; then
        ZIG_LICENSE=""
        for f in "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/LICENSE; do
            [ -f "$f" ] && ZIG_LICENSE="$f"
        done
    fi
    [ -f "${ZIG_LICENSE:-}" ] || die "Zig LICENSE not found (run tools/build/bootstrap.sh or set ZIG_LICENSE)"
    # Akam's own notices (ak_rtsp is FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware MIT code; the ported flasher fixes carry their own MIT notice).
    { [ -s "$ROOT/LICENSE" ] && [ -s "$ROOT/THIRD_PARTY_NOTICES.md" ]; } || die "LICENSE or THIRD_PARTY_NOTICES.md missing in $ROOT"
    LICS=("LICENSE.akam:$ROOT/LICENSE" "THIRD_PARTY_NOTICES.md:$ROOT/THIRD_PARTY_NOTICES.md"
        "LICENSE.dropbear:$DROPBEAR_LICENSE" "LICENSE.musl:$MUSL_COPYRIGHT" "LICENSE.zig:$ZIG_LICENSE"
        "LICENSE.smolrtsp:$SMOLRTSP_DIR/LICENSE")
    for d in slice99 metalang99 datatype99 interface99; do
        LICS+=("LICENSE.$d:$SMOLRTSP_DIR/build/_deps/$d-src/LICENSE")
    done
    for e in "${LICS[@]}"; do
        [ -s "${e#*:}" ] || die "licence file ${e#*:} missing (run tools/build/bootstrap.sh)"
    done
fi

# image_files.txt: the Akam files of the image. list_src <kit path> prints the source of an entry:
# "$KIT/<kit path>" in kit mode, "$ROOT/<repository source>" in source mode.
list_src() {
    local kind a b c d
    while read -r kind a b c d; do
        if [ "$kind" = file ] && [ "$b" = "$1" ]; then
            if [ "$MODE" = kit ]; then echo "$KIT/$b"; else echo "$ROOT/$d"; fi
            return 0
        fi
    done <"$IMAGE_FILES"
    return 1
}
# A kit newer than this script could carry overlay files the list does not know: they would be
# silently left out of the image. Every file/link of the kit's overlay/ and patches/ must be listed.
if [ "$MODE" = kit ]; then
    awk '$1 == "file" || $1 == "link" { print $3 }' "$IMAGE_FILES" | LC_ALL=C sort -u >"$TMP/listed.paths"
    (cd "$KIT" && find overlay patches \( -type f -o -type l \) 2>/dev/null | LC_ALL=C sort) >"$TMP/kit.paths"
    KIT_EXTRA="$(LC_ALL=C comm -13 "$TMP/listed.paths" "$TMP/kit.paths" | tr '\n' ' ')"
    [ -z "$KIT_EXTRA" ] || die "the kit has files this script does not install (kit newer than the scripts? update Akam): $KIT_EXTRA"
fi
PATCH_ETH="$(list_src patches/patch_ethernet_hostname.sh)" || die "image_files.txt has no patches/patch_ethernet_hostname.sh entry"
[ -f "$PATCH_ETH" ] || die "ethernet.sh patch script $PATCH_ETH missing"
PATCH_SERVICE="$(list_src patches/patch_service_sh.sh)" || die "image_files.txt has no patches/patch_service_sh.sh entry"
[ -f "$PATCH_SERVICE" ] || die "service.sh patch script $PATCH_SERVICE missing"
while read -r kind a b c d; do
    case "$kind" in
        '' | '#'*) continue ;;
        file)
            case "$a$b$c$d" in *..* | *' '*) die "image_files.txt: unsafe entry '$kind $a $b $c $d'" ;; esac
            case "$b$c" in /*) die "image_files.txt: absolute path in '$kind $a $b $c'" ;; esac
            src="$(list_src "$b")"
            [ -f "$src" ] || die "image file source $src missing"
            ;;
        link) ;;
        *) die "image_files.txt: unknown entry kind '$kind'" ;;
    esac
done <"$IMAGE_FILES"

# --- stock APP fingerprint, hardware verdict, compression ---------------------------------
STOCK_COMP="" STOCK_BLOCK="" STOCK_BCJ="" STOCK_SHA="" STOCK_KERNEL="" STOCK_SRC="" KERNEL_EXPECTED=0
if [ -n "$BACKUP_DIR" ]; then
    [ -d "$BACKUP_DIR" ] || die "--backup-dir $BACKUP_DIR is not a directory"
    [ -n "$STOCK_APP" ] || STOCK_APP="$BACKUP_DIR/mtdblock7.bin"
    # MTD layout: exactly the 8 dumps mtdblock0.bin ... mtdblock7.bin, none empty (their sizes are
    # not listed in supported_hardware.txt: only the APP size is compared, below).
    NPART="$(find "$BACKUP_DIR" -maxdepth 1 -name 'mtdblock*.bin' | wc -l | tr -d ' ')"
    [ "$NPART" = 8 ] || die "MTD layout differs from the supported one: $NPART mtdblock*.bin in $BACKUP_DIR, expected 8. Refusing."
    for i in 0 1 2 3 4 5 6 7; do
        [ -s "$BACKUP_DIR/mtdblock$i.bin" ] || die "MTD layout differs from the supported one: $BACKUP_DIR/mtdblock$i.bin is missing or empty. Refusing."
    done
    # u-boot legacy uImage header (64 bytes): magic 27051956 at offset 0, image name at offset 32
    # (32 bytes), e.g. "Linux-4.4.302-cip94". Anything else leaves the kernel unknown.
    KMAGIC="$(dd if="$BACKUP_DIR/mtdblock4.bin" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    if [ "$KMAGIC" = 27051956 ]; then
        STOCK_KERNEL="$(dd if="$BACKUP_DIR/mtdblock4.bin" bs=1 skip=32 count=32 2>/dev/null | tr -d '\000' | LC_ALL=C tr -cd 'A-Za-z0-9._+ -')"
        case "$STOCK_KERNEL" in Linux-?*) STOCK_KERNEL="${STOCK_KERNEL#Linux-}" ;; *) STOCK_KERNEL="" ;; esac
    fi
    KERNEL_EXPECTED=1
fi
if [ -n "$STOCK_APP" ]; then
    [ -f "$STOCK_APP" ] || die "stock APP dump $STOCK_APP not found"
    [ "$(stat -c %s "$STOCK_APP")" = "$PARTITION" ] || die "MTD layout differs: $STOCK_APP is $(stat -c %s "$STOCK_APP") bytes, the APP partition (mtd7) is $PARTITION. Refusing."
    STOCK_SHA="$(sha256sum "$STOCK_APP" | cut -d' ' -f1)"
    SQINFO="$(LC_ALL=C unsquashfs -s "$STOCK_APP" 2>&1)" || die "$STOCK_APP is not a readable squashfs (unsquashfs -s failed): $SQINFO"
    # unsquashfs -s (4.3 ... 4.7): "Compression xz", "Block size 131072", and for xz either
    # "Filters selected: arm" / "No filters specified" (when the options are stored) or nothing.
    STOCK_COMP="$(awk 'tolower($1) == "compression" && NF == 2 && tolower($2) != "options" { print tolower($2); exit }' <<<"$SQINFO")"
    STOCK_BLOCK="$(awk 'tolower($1) == "block" && tolower($2) == "size" { print $3; exit }' <<<"$SQINFO")"
    { [ -n "$STOCK_COMP" ] && [ -n "$STOCK_BLOCK" ]; } || die "cannot read the compression / block size of $STOCK_APP from 'unsquashfs -s' (unsupported squashfs-tools output?): $(head -8 <<<"$SQINFO" | tr '\n' '|')"
    FLINE="$(awk 'tolower($0) ~ /filter/ { print; exit }' <<<"$SQINFO" | sed 's/^[[:space:]]*//')"
    case "$FLINE" in
        '' | [Nn]o\ filter*) STOCK_BCJ=none ;;
        [Ff]ilters\ selected:*) STOCK_BCJ="$(tr '[:upper:]' '[:lower:]' <<<"${FLINE#*:}" | sed 's/^ *//')" ;;
        *) STOCK_BCJ="unrecognised ($FLINE)" ;;
    esac
    [ -n "$STOCK_BCJ" ] || STOCK_BCJ=none
    STOCK_SRC="$STOCK_APP"
elif [ -n "$STOCK_INFO" ]; then
    [ -f "$STOCK_INFO" ] || die "--stock-info $STOCK_INFO not found"
    si() { sed -n "s/^$1=//p" "$STOCK_INFO" | head -1; }
    STOCK_COMP="$(si compression | tr '[:upper:]' '[:lower:]')"
    STOCK_BLOCK="$(si block_size)"
    STOCK_BCJ="$(si bcj | tr '[:upper:]' '[:lower:]')"
    STOCK_BCJ="${STOCK_BCJ:-none}"
    STOCK_SHA="$(si app_sha256 | tr '[:upper:]' '[:lower:]')"
    if [ -n "$STOCK_SHA" ] && { [ "${#STOCK_SHA}" != 64 ] || [ -n "${STOCK_SHA//[0-9a-f]/}" ]; }; then
        die "$STOCK_INFO: app_sha256 must be 64 hex digits"
    fi
    STOCK_SRC="$STOCK_INFO"
    { [ -n "$STOCK_COMP" ] && [ -n "$STOCK_BLOCK" ]; } || die "$STOCK_INFO needs compression= and block_size="
fi
HAVE_STOCK=0
[ -n "$STOCK_COMP" ] && HAVE_STOCK=1
case "$STOCK_BLOCK" in *[!0-9]*) die "unreadable squashfs block size '$STOCK_BLOCK' in the stock APP" ;; esac
[ "$HAVE_STOCK" = 0 ] || [ -n "$STOCK_BLOCK" ] || die "no squashfs block size found for the stock APP"

# Hardware verdict against supported_hardware.txt.
HW_VERDICT="" HW_DETAIL=""
if [ "$HAVE_STOCK" = 0 ]; then
    if [ "$MODE" = kit ]; then
        HW_VERDICT=unknown
        HW_DETAIL="no stock APP fingerprint given (--stock-app / --backup-dir)"
    else
        HW_VERDICT=not-checked
        HW_DETAIL="source mode without stock APP fingerprint"
    fi
else
    [ -s "$SUPPORTED_HW" ] || die "$SUPPORTED_HW missing"
    HW_VERDICT=unknown
    HW_DETAIL="stock APP sha256 ${STOCK_SHA:-(none)} is not listed in supported_hardware.txt"
    STOCK_FMT="squashfs $STOCK_COMP block=$STOCK_BLOCK bcj=$STOCK_BCJ"
    while IFS='|' read -r st dev _soc kern _mtd sq sha _bb || [ -n "$st" ]; do
        case "$st" in '' | '#'* | supported_hardware_format=*) continue ;; esac
        { [ -n "$STOCK_SHA" ] && [ "$sha" = "$STOCK_SHA" ]; } || continue
        if [ "$sq" != "$STOCK_FMT" ]; then
            HW_DETAIL="stock APP sha256 matches '$dev' but its squashfs format differs ($STOCK_FMT, listed: $sq)"
            continue
        fi
        if [ "$KERNEL_EXPECTED" = 1 ] && [ -z "$STOCK_KERNEL" ] && [ "$kern" != - ]; then
            HW_DETAIL="stock APP matches '$dev' but the kernel of the backup could not be read (mtdblock4.bin is not a uImage)"
            continue
        fi
        if [ -n "$STOCK_KERNEL" ] && [ "$kern" != - ] && [ "${kern%% *}" != "$STOCK_KERNEL" ]; then
            HW_DETAIL="stock APP matches '$dev' but the kernel differs (dump: $STOCK_KERNEL, listed: ${kern%% *})"
            continue
        fi
        HW_VERDICT="$st"
        HW_DETAIL="matches '$dev'"
        break
    done <"$SUPPORTED_HW"
fi
echo "Hardware check: $HW_VERDICT ($HW_DETAIL)"
case "$HW_VERDICT" in
    validated | not-checked) ;;
    *)
        if [ "$UNTESTED_OK" = 1 ]; then
            warn "*** UNTESTED hardware/firmware ($HW_VERDICT: $HW_DETAIL). Building anyway (--i-understand-untested). Akam makes no promise that the result boots. ***"
        else
            die "hardware/firmware is not validated ($HW_VERDICT: $HW_DETAIL). Akam is only validated for the targets in tools/release/supported_hardware.txt. If you accept the risk, repeat with --i-understand-untested."
        fi
        ;;
esac

# Compression: follow the stock APP (same compressor and options = proof the kernel reads it).
COMP="" BLOCK=131072 BCJ=none
if [ "$HAVE_STOCK" = 1 ]; then
    case "$STOCK_COMP:$STOCK_BCJ" in
        gzip:none | xz:none | xz:arm) STOCK_FOLLOWABLE=1 ;;
        *) STOCK_FOLLOWABLE=0 ;;
    esac
    case "$STOCK_BLOCK" in 4096 | 8192 | 16384 | 32768 | 65536 | 131072 | 262144 | 524288 | 1048576) ;; *) STOCK_FOLLOWABLE=0 ;; esac
    if [ "$STOCK_FOLLOWABLE" = 1 ]; then
        COMP="$STOCK_COMP"
        BLOCK="$STOCK_BLOCK"
        BCJ="$STOCK_BCJ"
        if [ -n "$COMP_ARG" ] && [ "$COMP_ARG" != "$STOCK_COMP" ]; then
            warn "*** COMPRESSION DIFFERS FROM THE STOCK APP: stock is $STOCK_COMP, building $COMP_ARG (--comp). The stock kernel is only PROVEN to read $STOCK_COMP. ***"
            COMP="$COMP_ARG"
            BCJ=none
        fi
    else
        if [ "$ALLOW_UNKNOWN_STOCK_COMP" != 1 ] || [ -z "$COMP_ARG" ]; then
            die "the stock APP uses squashfs compression '$STOCK_COMP' (filter: $STOCK_BCJ, block $STOCK_BLOCK), which Akam cannot reproduce (only gzip and xz). Refusing. If your kernel (mtdblock4) supports gzip or xz squashfs, pass --comp gzip|xz together with --allow-unknown-stock-comp."
        fi
        warn "*** stock APP compression is '$STOCK_COMP', not reproducible: building $COMP_ARG as requested (--allow-unknown-stock-comp). Verify that your kernel supports it. ***"
        COMP="$COMP_ARG"
    fi
else
    COMP="${COMP_ARG:-xz}"
fi
if [ "$COMP" = xz ] && [ "${AKAM_XZ_BCJ:-0}" = 1 ] && [ "$BCJ" = none ]; then
    [ "$HAVE_STOCK" = 0 ] || warn "AKAM_XZ_BCJ=1: BCJ arm filter differs from the stock APP (no filter); not proven to boot"
    BCJ=arm
fi

# Use the pinned toolchain from tools/.toolchain when zig is not on PATH.
zig_pinned() {
    local z
    for z in "$ROOT"/tools/.toolchain/zig-*-x86_64-linux/zig; do
        [ -x "$z" ] && { ZIG="$z"; return 0; }
    done
    return 1
}
# Flash budget: the Makefile builds ak_rtsp with -g (~1.6 MB vs ~130 KB
# stripped). Install a stripped COPY; the build output is never modified.
# Uses `zig objcopy --strip-all` (pinned toolchain: $ZIG or zig on PATH), or
# $AKAM_STRIP (a command taking "<in> <out>") if set. Kit binaries are already stripped.
install_stripped() {
    local src=$1 dst=$2 tmp="$TMP/strip.tmp"
    if [ "$MODE" = kit ] || [ "$(head -c4 "$src" | od -An -c | tr -d ' ')" != '177ELF' ]; then
        install -m 755 "$src" "$dst" # kit (stripped at kit build time) or not an ELF (test stub)
        return 0
    fi
    if [ -n "${AKAM_STRIP:-}" ]; then
        $AKAM_STRIP "$src" "$tmp" || die "AKAM_STRIP failed on $src"
    elif [ -x "${ZIG:-}" ] || command -v zig >/dev/null 2>&1 || zig_pinned; then
        "${ZIG:-zig}" objcopy --strip-all "$src" "$tmp" || die "zig objcopy --strip-all failed on $src"
    elif [ "${AKAM_ALLOW_UNSTRIPPED:-0}" = 1 ]; then
        warn "no strip tool (zig objcopy): $(basename "$dst") installed UNSTRIPPED, image will be larger (AKAM_ALLOW_UNSTRIPPED=1)"
        install -m 755 "$src" "$dst"
        return 0
    else
        die "no strip tool for $(basename "$dst"): run bootstrap.sh and source tools/build/env.sh (or set AKAM_STRIP, or AKAM_ALLOW_UNSTRIPPED=1)"
    fi
    [ -s "$tmp" ] || die "stripped copy of $src is empty"
    install -m 755 "$tmp" "$dst"
    rm -f "$tmp"
}

# --- 1. tree -----------------------------------------------------------------
if [ "${KEEP_PATCHED:-0}" = 1 ] && [ -d "$PATCHED" ]; then
    echo "KEEP_PATCHED=1: re-using $PATCHED (Akam install/patch steps are re-applied)."
else
    rm_tree "$PATCHED"
    echo "Creating app_patched/ from $APP_EXTRACTED..."
    cp -a "$APP_EXTRACTED" "$PATCHED"
fi

# --- 2. de-Tuyafication + 3. Akam: ONE code path for both modes ------------------------------
echo "Applying de-Tuyafication patch..."
rm -f "$PATCHED/bin/anyka_ipc"
echo "Installing Akam components ($MODE mode)..."
rm -f "$PATCHED/bin/ak_rtsp" "$PATCHED/bin/dropbearmulti" "$PATCHED/bin/dropbear" "$PATCHED/bin/dropbearkey"
install_stripped "$BINARY" "$PATCHED/bin/ak_rtsp"
install_stripped "$DROPBEAR_MULTI" "$PATCHED/bin/dropbearmulti"
# Overlay files and links: exactly the entries of tools/release/image_files.txt.
while read -r kind a b c d; do
    case "$kind" in
        file)
            [ "$c" != - ] || continue # not installed in the image (service.sh patch, hwcheck)
            mkdir -p "$PATCHED/$(dirname "$c")"
            rm -f "$PATCHED/$c"
            install -m "$a" "$(list_src "$b")" "$PATCHED/$c"
            ;;
        link)
            mkdir -p "$PATCHED/$(dirname "$c")"
            rm -f "$PATCHED/$c"
            ln -s "$a" "$PATCHED/$c"
            ;;
    esac
done < <(grep -vE '^[[:space:]]*(#|$)' "$IMAGE_FILES")
mkdir -p "$PATCHED/local/akam"
chmod 755 "$PATCHED/local/akam"
for e in "${LICS[@]}"; do
    install -m 644 "${e#*:}" "$PATCHED/local/akam/${e%%:*}"
done

echo "Patching sbin/service.sh (telnet/ftp off, SD hook guarded)..."
SVC_RC=0
AKAM_PATCH_REPORT="$FW_DIR/service_sh.diff" bash "$PATCH_SERVICE" "$PATCHED" || SVC_RC=$?
case "$SVC_RC" in
    0) ;;
    2 | 3)
        if [ "${AKAM_ALLOW_UNPATCHED_SERVICE:-0}" = 1 ]; then
            warn "*** service.sh NOT patched (exit $SVC_RC): Telnet/FTP may still be launched by the stock boot script. AKAM_ALLOW_UNPATCHED_SERVICE=1 set, continuing. ***"
        else
            die "patch_service_sh.sh exit $SVC_RC: service.sh not patched, image would not be locked down. Fix the patterns, or set AKAM_ALLOW_UNPATCHED_SERVICE=1 to build anyway."
        fi
        ;;
    *) die "patch_service_sh.sh failed (exit $SVC_RC)" ;;
esac

echo "Patching sbin/ethernet.sh (DHCP hostname from akam_hostname.sh)..."
bash "$PATCH_ETH" "$PATCHED" || die "patch_ethernet_hostname.sh failed: the DHCP hostname of ethernet.sh would stay hard-coded"

# The stock service.sh copies sbin/drop_caches.sh to /tmp and runs it forever: "sleep 5; sync; echo 3 >
# /proc/sys/vm/drop_caches" (it also removes a stale /tmp/wpa_supplicant). Nothing depends on the 5 s
# period; each pass is a wakeup plus a sync, and drop_caches is a no-op on this kernel
# (see tools/flash_scripts/install_custom_firmware.sh). One pass per minute keeps the stock behaviour
# at a twelfth of the cost. A different layout is left untouched with a warning.
echo "Slowing sbin/drop_caches.sh (5 s -> 60 s)..."
DROPC="$PATCHED/sbin/drop_caches.sh"
if [ ! -f "$DROPC" ]; then
    warn "sbin/drop_caches.sh not found in the APP tree: not patched"
elif grep -q '^[[:space:]]*sleep 5[[:space:]]*$' "$DROPC"; then
    sed -i 's/^\([[:space:]]*\)sleep 5[[:space:]]*$/\1sleep 60/' "$DROPC"
elif grep -q '^[[:space:]]*sleep 60[[:space:]]*$' "$DROPC"; then
    :   # already patched (KEEP_PATCHED=1)
else
    warn "sbin/drop_caches.sh has no 'sleep 5' line: not patched"
fi

AUDIT_OUT=""
if [ -f "$TEMPLATES/audit_remote_services.sh" ]; then
    (cd "$FW_DIR" && bash "$TEMPLATES/audit_remote_services.sh" "$PATCHED") || warn "audit_remote_services.sh failed"
    AUDIT_OUT="audit_remote_services.txt"
fi

SHA_AK="$(sha256sum "$PATCHED/bin/ak_rtsp" | cut -d' ' -f1)"
SHA_DB="$(sha256sum "$PATCHED/bin/dropbearmulti" | cut -d' ' -f1)"
if [ "$MODE" = kit ]; then
    GIT_DESC="$KIT_VERSION"
    cp "$KIT/MANIFEST.txt" "$PATCHED/local/akam/MANIFEST.txt" # the kit's own manifest travels with the image
else
    GIT_DESC="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
    {
        echo "akam_version=$GIT_DESC"
        echo "build_date=$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')"
        echo "ak_rtsp_sha256=$SHA_AK"
        echo "dropbearmulti_sha256=$SHA_DB"
        for l in "$PATCHED"/local/akam/LICENSE.*; do
            echo "licence: $(basename "$l") sha256=$(sha256sum "$l" | cut -d' ' -f1)"
        done
        if [ -f "$DROPBEAR_DIR/BUILDINFO.txt" ]; then
            sed 's/^/dropbear_buildinfo: /' "$DROPBEAR_DIR/BUILDINFO.txt"
        fi
    } >"$PATCHED/local/akam/MANIFEST.txt"
fi
chmod 644 "$PATCHED/local/akam/MANIFEST.txt"

TIME64_NOTE=""
# --- 4. safety checks -------------------------------------------------------------
echo "Checking tree..."
# The camera's kernel acknowledges musl's time64 clock syscalls without executing
# them (docs/akam_v1_kernel_time64.md). Hard gate, run on the files that are
# actually in the image (stripped): no forbidden time64 syscall, and the 32-bit
# clock_gettime (263) of src/common/time32_shim.c present. A missing gdb is a failure.
# Kit mode: verify_kit.sh checked time64_gate=pass in the hashed MANIFEST and the sha256 of both
# binaries (--recheck-time64 forces the replay here; see below for the default).
# Kit mode replays the gate by default whenever gdb is available (the MANIFEST is only a claim by
# the kit builder); without gdb it relies on MANIFEST + sha256 and says so.
if [ "$MODE" = kit ] && [ "$RECHECK_TIME64" != 1 ] && [ "$KIT_GRADE" = release ] && command -v gdb >/dev/null 2>&1; then
    RECHECK_TIME64=1
fi
if [ "$MODE" = kit ] && [ "$RECHECK_TIME64" != 1 ]; then
    TIME64_NOTE="NOT replayed (no gdb, or a test kit): relies on the kit MANIFEST + sha256"
    echo "time64 gate: $TIME64_NOTE (--recheck-time64 to replay it)."
elif [ "$MODE" = source ] && [ "${AKAM_ALLOW_UNCHECKED_TIME64:-0}" = 1 ]; then
    TIME64_NOTE="SKIPPED (AKAM_ALLOW_UNCHECKED_TIME64=1)"
    warn "*** time64 machine code check SKIPPED (AKAM_ALLOW_UNCHECKED_TIME64=1): clocks may not work on the camera. ***"
else
    bash "$ROOT/tools/build/check_time32.sh" --require-263 "$PATCHED/bin/ak_rtsp" "$PATCHED/bin/dropbearmulti" \
        || die "time64 check failed on the installed binaries (see above; missing gdb also lands here)"
    TIME64_NOTE="replayed on the installed binaries: ok"
fi
# -I: text files only. The stock wpa_supplicant binary contains the marker as a
# string literal (not a key); a binary can't be a PEM key file anyway.
BADKEYS="$( (grep -rIlE -e 'BEGIN [A-Z0-9 ]*PRIVATE KEY' "$PATCHED" 2>/dev/null || true
    find "$PATCHED" \( -name '*_host_key*' -o -name 'id_rsa*' -o -name 'id_ed25519*' \) ) | sort -u)"
[ -z "$BADKEYS" ] || die "private key material found in the image tree (O-5: none allowed):
$BADKEYS"
for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/akam_ssh.sh sbin/akam_lockdown.sh sbin/akam_rtsp_auth.sh sbin/akam_hostname.sh sbin/akam_debug.sh sbin/akam_net.sh sbin/akam_onvif.sh; do
    [ "$(stat -c %a "$PATCHED/$f")" = 755 ] || die "$f is not mode 755"
done
BADW="$(find "$PATCHED/local/akam" \( -type f -o -type d \) -perm /022 2>/dev/null)"
[ -z "$BADW" ] || die "group/other-writable files in local/akam: $BADW"
for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/akam_ssh.sh sbin/akam_lockdown.sh sbin/akam_rtsp_auth.sh sbin/akam_hostname.sh sbin/akam_debug.sh sbin/akam_net.sh sbin/akam_onvif.sh sbin/service.sh; do
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
# Same compressor options as the stock APP: it is xz WITHOUT a BCJ filter (a BCJ-arm image needs
# CONFIG_XZ_DEC_ARM in the kernel, unproven). A BCJ filter only when the stock has one or
# AKAM_XZ_BCJ=1 (~45 KB smaller, not proven to boot; warned about above).
if [ "$COMP" = xz ] && [ "$BCJ" = arm ]; then MKOPTS+=(-Xbcj arm); fi
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
    if [ "${AKAM_ALLOW_TIGHT:-0}" = 1 ]; then
        warn "only $FREE bytes free in the APP partition (< one block); AKAM_ALLOW_TIGHT=1"
    else
        die "only $FREE bytes free in the APP partition (< $BLOCK, one block of margin). Set AKAM_ALLOW_TIGHT=1 to accept."
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
    echo "Akam firmware build report"
    echo "image:              $(basename "$OUT")"
    echo "image sha256:       $SHA_OUT"
    echo "squashfs sha256:    $(sha256sum "$SQ" | cut -d' ' -f1)  (content, before padding)"
    echo "compression:        $COMP, block $BLOCK, bcj $BCJ"
    echo "build mode:         $MODE"
    echo "time64 gate:        $TIME64_NOTE"
    if [ "$MODE" = kit ]; then
        echo "kit:                akam_version=$KIT_VERSION source_commit=$KIT_COMMIT kit_grade=$KIT_GRADE"
        echo "kit authentication: $KIT_AUTH${KIT_INFO:+; $KIT_INFO}"
    fi
    if [ "$HAVE_STOCK" = 1 ]; then
        echo "stock APP:          squashfs $STOCK_COMP, block $STOCK_BLOCK, bcj $STOCK_BCJ (from $(basename "$STOCK_SRC"))"
        echo "stock APP sha256:   ${STOCK_SHA:-unknown}"
        if [ "$COMP" = "$STOCK_COMP" ] && [ "$BLOCK" = "$STOCK_BLOCK" ] && [ "$BCJ" = "$STOCK_BCJ" ]; then
            echo "compression vs stock: SAME as the stock APP"
        else
            echo "compression vs stock: *** DIFFERENT from the stock APP ($STOCK_COMP/$STOCK_BLOCK/$STOCK_BCJ) ***"
        fi
    else
        echo "stock APP:          not fingerprinted (no dump given); compression not compared with the stock"
    fi
    echo "stock kernel:       ${STOCK_KERNEL:-unknown (no mtdblock4.bin given or not a uImage)}"
    echo "hardware verdict:   $HW_VERDICT ($HW_DETAIL)"
    echo "squashfs size:      $SQ_SIZE bytes"
    echo "free in mtd7:       $FREE bytes ($((FREE * 100 / PARTITION))% of $PARTITION)"
    echo "added components (uncompressed file size):"
    for f in bin/ak_rtsp bin/dropbearmulti sbin/ak_rtsp_wrapper.sh sbin/akam_ssh.sh sbin/akam_lockdown.sh sbin/akam_rtsp_auth.sh sbin/akam_hostname.sh sbin/akam_debug.sh sbin/akam_net.sh sbin/akam_onvif.sh local/akam/LICENSE.dropbear local/akam/LICENSE.musl local/akam/LICENSE.smolrtsp local/akam/MANIFEST.txt; do
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
