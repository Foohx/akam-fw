#!/usr/bin/env bash
# Sign the release checksums with the maintainer's SSH key.
#
#   tools/release/sign_kit.sh <private-key-path> [dir-with-SHA256SUMS]
#
# Writes SHA256SUMS.sig next to SHA256SUMS (a release: dist/release/vX.Y.Z; default directory: dist/kit) using
# `ssh-keygen -Y sign -n akam-release`. The key path is only passed to
# ssh-keygen: this script never reads, copies or stores the key, and it refuses
# a key path inside the repository. ssh-keygen asks for the passphrase itself
# (or uses ssh-agent). If tools/release/allowed_signers holds a key, the fresh
# signature is verified against it (tests point AKAM_ALLOWED_SIGNERS at their own file).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "sign_kit: error: $*" >&2; exit 1; }
ALLOWED="${AKAM_ALLOWED_SIGNERS:-$HERE/allowed_signers}"

{ [ $# -ge 1 ] && [ $# -le 2 ]; } || { echo "usage: $0 <private-key-path> [dir-with-SHA256SUMS]" >&2; exit 2; }
KEY=$1
DIR="${2:-$ROOT/dist/kit}"
command -v ssh-keygen >/dev/null 2>&1 || die "ssh-keygen not found"
[ -f "$KEY" ] || die "key file $KEY not found"
[ -f "$DIR/SHA256SUMS" ] || die "$DIR/SHA256SUMS not found (run tools/release/make_release.sh X.Y.Z first, then sign dist/release/vX.Y.Z)"
[ ! -e "$DIR/TEST_BUILD_NOT_PUBLISHABLE.txt" ] || die "$DIR is a TEST build (TEST_BUILD_NOT_PUBLISHABLE.txt): it is never signed or published"
KEYABS="$(cd "$(dirname "$KEY")" && pwd)/$(basename "$KEY")"
case "$KEYABS" in "$ROOT"/*) die "refusing a key stored inside the repository ($KEYABS)" ;; esac

rm -f "$DIR/SHA256SUMS.sig"
ssh-keygen -Y sign -f "$KEY" -n akam-release "$DIR/SHA256SUMS" || die "ssh-keygen -Y sign failed"
[ -s "$DIR/SHA256SUMS.sig" ] || die "no signature produced"
echo "sign_kit: wrote $DIR/SHA256SUMS.sig"

if grep -qvE '^[[:space:]]*(#|$)' "$ALLOWED"; then
    ssh-keygen -Y verify -f "$ALLOWED" -I akam-release -n akam-release \
        -s "$DIR/SHA256SUMS.sig" <"$DIR/SHA256SUMS" >/dev/null \
        || die "the signature does not verify against tools/release/allowed_signers (wrong key?)"
    echo "sign_kit: signature verifies against tools/release/allowed_signers"
else
    echo "sign_kit: WARNING: tools/release/allowed_signers has no key yet; signature not cross-checked" >&2
fi
