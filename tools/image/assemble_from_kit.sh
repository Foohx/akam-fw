#!/usr/bin/env bash
# Assemble the Akam APP image from YOUR backup and a prebuilt Akam kit (no compiler needed).
#
#   tools/image/assemble_from_kit.sh --backup-dir DIR --kit KIT --out DIR [options]
#
#   --backup-dir DIR   your backup (mtdblock0.bin ... mtdblock7.bin); never modified
#   --kit KIT          akam-kit-<version>.tar.xz (with SHA256SUMS next to it) or an extracted kit (a directory needs --allow-unsigned)
#   --out DIR          where the image, its .sha256 and build_report.txt are written
# Options passed to src/ak_rtsp/build_firmware.sh --kit:
#   --allow-unsigned, --allow-test-kit, --recheck-time64, --comp gzip|xz,
#   --allow-unknown-stock-comp, --i-understand-untested
#
# What it does: unpacks mtdblock7.bin (the stock APP squashfs) with unsquashfs as a normal
# user, then runs build_firmware.sh --kit, which verifies the kit, compares your dump with
# tools/release/supported_hardware.txt, builds the image with the SAME squashfs compression as
# your stock APP, checks it and pads it to the 5242880 byte APP partition.
# File owners: the image is built with -all-root (mksquashfs), so the owner ids of the stock tree
# (often 1000, lost when unpacking as a normal user) do not matter. Device nodes cannot be
# created without root: if the stock APP has any, this script refuses (run it as root).
# Only squashfs-tools and the tools the kit verifier needs (tar, xz, ssh-keygen) are required.
# The stock files are proprietary: they stay in a temporary directory and in OUT/app_patched/
# (the build tree); never publish them.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "assemble_from_kit: error: $*" >&2; exit 1; }
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//' >&2; exit 2; }

BACKUP="" KIT="" OUT=""
PASS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --backup-dir) [ $# -ge 2 ] || usage; BACKUP=$2; shift ;;
        --kit) [ $# -ge 2 ] || usage; KIT=$2; shift ;;
        --out) [ $# -ge 2 ] || usage; OUT=$2; shift ;;
        --comp) [ $# -ge 2 ] || usage; PASS+=(--comp "$2"); shift ;;
        --allow-unsigned | --allow-test-kit | --recheck-time64 | --allow-unknown-stock-comp | --i-understand-untested) PASS+=("$1") ;;
        -h | --help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
    shift
done
{ [ -n "$BACKUP" ] && [ -n "$KIT" ] && [ -n "$OUT" ]; } || usage
[ -d "$BACKUP" ] || die "$BACKUP is not a directory"
[ -f "$BACKUP/mtdblock7.bin" ] || die "$BACKUP/mtdblock7.bin not found (is this your backup directory?)"
[ -e "$KIT" ] || die "kit $KIT not found"
for t in unsquashfs mksquashfs sha256sum md5sum; do
    command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found (squashfs-tools / coreutils)"
done

WORK="$(mktemp -d)"
# unsquashfs as a normal user keeps read-only directories read-only: make the tree removable first.
trap 'chmod -R u+rwX "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

LIST="$(LC_ALL=C unsquashfs -lln "$BACKUP/mtdblock7.bin" 2>&1)" || die "mtdblock7.bin is not a readable squashfs: $LIST"
NODES="$(awk '/^[cb]/' <<<"$LIST" | wc -l | tr -d ' ')"
if [ "$NODES" != 0 ] && [ "$(id -u)" != 0 ]; then
    die "the stock APP has $NODES device node(s); they cannot be recreated without root. Run this script as root (sudo) or under fakeroot."
fi
OWNERS="$(awk '/^[-dlcb]/{print $2}' <<<"$LIST" | sort -u | tr '\n' ' ')"
echo "Stock APP owners (uid/gid): ${OWNERS:-?}; device nodes: $NODES. The image is built with -all-root."
DUPS="$(awk '/^[-dlcb]/{ $1=$2=$3=$4=$5=""; print tolower($0) }' <<<"$LIST" | sort | uniq -d | wc -l | tr -d ' ')"
[ "$DUPS" = 0 ] || echo "WARNING: $DUPS file names differ only by case; do not run this on a case-insensitive filesystem." >&2

echo "Unpacking the stock APP ..."
unsquashfs -no-xattrs -d "$WORK/app_extracted" "$BACKUP/mtdblock7.bin" >/dev/null 2>"$WORK/unsq.err" ||
    die "unsquashfs failed: $(head -3 "$WORK/unsq.err")"

mkdir -p "$OUT"
APP_EXTRACTED="$WORK/app_extracted" FW_DIR="$OUT" \
    bash "$ROOT/src/ak_rtsp/build_firmware.sh" --kit "$KIT" --backup-dir "$BACKUP" "${PASS[@]+"${PASS[@]}"}"

IMG="$(find "$OUT" -maxdepth 1 -name 'ak_rtsp_firmware_*.squashfs' | LC_ALL=C sort | tail -1)"
[ -n "$IMG" ] || die "no image found in $OUT"
rep() { sed -n "s/^$1: *//p" "$OUT/build_report.txt" | head -1; }
echo
echo "=============================== SUMMARY ==============================="
echo "image:        $IMG"
echo "md5:          $(md5sum "$IMG" | cut -d' ' -f1)"
echo "sha256:       $(sha256sum "$IMG" | cut -d' ' -f1)"
echo "free bytes:   $(rep 'free in mtd7')"
echo "compression:  $(rep compression)"
echo "vs stock:     $(rep 'compression vs stock')"
echo "hardware:     $(rep 'hardware verdict')"
echo "kit:          $(rep kit) / $(rep 'kit authentication')"
echo "report:       $OUT/build_report.txt"
echo "======================================================================="
