#!/usr/bin/env bash
# Host test of src/core/control/ctl_sys.c (metrics) on a fake /proc and /sys tree. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/ctl_sys_test" \
    "$REPO/tests/native/ctl_sys_test.c" "$REPO/src/core/control/ctl_sys.c" "$REPO/src/hal/hal_sys.c" -pthread
"$WORK/ctl_sys_test"
