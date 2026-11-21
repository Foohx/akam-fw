#!/usr/bin/env bash
# Build the static ARM akam_ptz tool of the LSC Rotatable 2K with the pinned Zig toolchain.
#
#   tools/build/bootstrap.sh                                          # once: Zig
#   firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/build_akam_ptz.sh
#
# Output (git-ignored): dist/akam_ptz/{akam_ptz,BUILDINFO.txt}. build_card.sh copies it to the card as
# akam/akam_ptz after checking BUILDINFO.txt against the binary and the current source.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
# shellcheck source=tools/build/env.sh
. "$root/tools/build/env.sh"

die() { echo "build_akam_ptz: error: $*" >&2; exit 1; }

src="$here/akam_ptz.c"
dist="${AKAM_PTZ_DIST:-$root/dist/akam_ptz}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$dist"

[ -n "${ZIG:-}" ] || die "pinned Zig not found; run tools/build/bootstrap.sh"
# shellcheck disable=SC2086  # AKAM_CC is a word list by design
$AKAM_CC -Os -mfloat-abi=soft -Wall -Wextra -static -Wl,--gc-sections -ffile-prefix-map="$root"=akam \
    -o "$work/akam_ptz" "$src" || die "compile failed"
if [ "${AKAM_ALLOW_UNCHECKED_TIME64:-0}" = 1 ]; then
    echo "build_akam_ptz: warning: time64 machine code check SKIPPED (AKAM_ALLOW_UNCHECKED_TIME64=1)" >&2
else
    "$root/tools/build/check_time32.sh" "$work/akam_ptz" || die "time64 check failed (gdb with ARM support is required)"
fi
"$ZIG" objcopy --strip-all "$work/akam_ptz" "$dist/akam_ptz"
chmod 755 "$dist/akam_ptz"
{
    echo "tool=akam_ptz"
    echo "source_sha256=$(cat "$src" "$here/akam_ptz_safe.h" | sha256sum | cut -d' ' -f1)"
    echo "sha256_stripped=$(sha256sum "$dist/akam_ptz" | cut -d' ' -f1)"
    echo "size=$(stat -c %s "$dist/akam_ptz")"
    echo "target=$AKAM_TARGET mcpu=$AKAM_MCPU"
} >"$dist/BUILDINFO.txt"
echo "build_akam_ptz: $dist/akam_ptz ($(stat -c %s "$dist/akam_ptz") bytes)"
