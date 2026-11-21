#!/usr/bin/env bash
# Fetch and verify the pinned uClibc cross toolchain of the own-app study (see toolchain.lock), project-local in
# tools/.toolchain/ (git-ignored). Nothing is installed system-wide. A sha256 mismatch aborts and removes the file.
#   platforms/anyka-ak3918av130/toolchain/fetch_toolchain.sh
# Offline: AKAM_ROT2K_TOOLCHAIN_TARBALL=/path/to/the.tar.bz2 (still verified, never deleted by this script).
# Prints the compiler path on success. Nothing from the tarball is executed here: it is only unpacked (after the
# hash check) into a temporary directory next to the destination, then renamed.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
die() { echo "fetch_toolchain: error: $*" >&2; exit 1; }

# shellcheck source=platforms/anyka-ak3918av130/toolchain/toolchain.lock
. "$here/toolchain.lock"
dest="$root/tools/.toolchain"
own_tarball=0
[ -n "${AKAM_ROT2K_TOOLCHAIN_TARBALL:-}" ] && own_tarball=1
tarball="${AKAM_ROT2K_TOOLCHAIN_TARBALL:-$dest/$TOOLCHAIN_NAME.tar.bz2}"
mkdir -p "$dest"

if [ ! -x "$dest/$TOOLCHAIN_NAME/bin/arm-linux-gcc" ]; then
    if [ ! -f "$tarball" ]; then
        command -v curl >/dev/null || die "curl not found (or set AKAM_ROT2K_TOOLCHAIN_TARBALL)"
        curl -fSL --proto '=https' --proto-redir '=https' --tlsv1.2 --retry 3 -o "$tarball.part" "$TOOLCHAIN_URL" || die "download failed"
        mv "$tarball.part" "$tarball"
    fi
    got="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [ "$got" != "$TOOLCHAIN_SHA256" ]; then
        [ "$own_tarball" = 1 ] || rm -f "$tarball"
        die "sha256 mismatch: expected $TOOLCHAIN_SHA256, got $got"
    fi
    tmp="$(mktemp -d "$dest/.extract.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    tar -xjf "$tarball" -C "$tmp" || die "extract failed"
    [ -x "$tmp/$TOOLCHAIN_NAME/bin/arm-linux-gcc" ] || die "unexpected tarball layout"
    rm -rf "${dest:?}/$TOOLCHAIN_NAME"
    mv "$tmp/$TOOLCHAIN_NAME" "$dest/$TOOLCHAIN_NAME"
fi
echo "$dest/$TOOLCHAIN_NAME/bin/arm-linux-gcc"
