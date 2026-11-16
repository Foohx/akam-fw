#!/usr/bin/env bash
# Extract the vendor shared libraries (link-time only) from YOUR OWN flash backup into dist/rot2k-app/vendor/.
# They are proprietary: the output directory is git-ignored and must never be committed or shared.
#   firmware/lsc-rotatable-2k-ak3918av130/app/prepare_vendor.sh ~/akam-backup-lsc-rotatable-2k-<date>
# Needs unsquashfs (squashfs-tools) on the host. Checks mtd3.bin against the backup's SHA256SUMS first.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
die() { echo "prepare_vendor: error: $*" >&2; exit 1; }

backup="${1:-}"
[ -n "$backup" ] && [ -f "$backup/mtd3.bin" ] || die "usage: $0 <backup dir containing mtd3.bin and SHA256SUMS>"
command -v unsquashfs >/dev/null || die "unsquashfs not found (squashfs-tools)"
[ -f "$backup/SHA256SUMS" ] || die "no SHA256SUMS in $backup"
(cd "$backup" && grep -E '[ *]mtd3\.bin$' SHA256SUMS | sha256sum -c --quiet -) || die "mtd3.bin does not match SHA256SUMS"

out="${AKAM_ROT2K_VENDOR_DIR:-$root/dist/rot2k-app/vendor}"
case "$out" in "$root"/dist/?*) ;; *) die "output directory must be under $root/dist/ (got: $out)" ;; esac
rm -rf "$out"
mkdir -p "$out"
unsquashfs -q -f -d "$out/appfs" "$backup/mtd3.bin" lib >/dev/null || die "unsquashfs failed"
echo "prepare_vendor: $(find "$out/appfs/lib" -name 'lib*.so' | wc -l) libraries in $out/appfs/lib (never commit)"
