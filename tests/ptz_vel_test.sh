#!/usr/bin/env bash
# Host test of src/features/ptz/ptz_vel.c (velocity -> motor divisor mapping, parsers; ASan/UBSan). No hardware.
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
 "${SHARED_INC[@]}" -o "$WORK/ptz_vel_test" "$REPO/tests/native/ptz_vel_test.c" "$REPO/src/features/ptz/ptz_vel.c"
"$WORK/ptz_vel_test"
