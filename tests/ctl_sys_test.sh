#!/usr/bin/env bash
# Host test of src/ak_rtsp/ctl_sys.c (metrics) on a fake /proc and /sys tree. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/ctl_sys_test" \
    "$REPO/tests/native/ctl_sys_test.c" "$REPO/src/ak_rtsp/ctl_sys.c" -pthread
"$WORK/ctl_sys_test"
