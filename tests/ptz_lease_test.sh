#!/usr/bin/env bash
# Host test of src/features/ptz/ptz_lease.c (dead-man lease with a fake clock; ASan/UBSan). No hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 "${SHARED_INC[@]}" -o "$WORK/ptz_lease_test" "$REPO/tests/native/ptz_lease_test.c" "$REPO/src/features/ptz/ptz_lease.c"
"$WORK/ptz_lease_test"
