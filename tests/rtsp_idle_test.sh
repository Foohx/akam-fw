#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtsp_idle.c (pre-PLAY idle timeout and PLAY cap of the
# single-client RTSP server). Pure state machine on a fake clock: no sleep.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_idle_test" \
    "$REPO/tests/native/rtsp_idle_test.c" "$REPO/src/ak_rtsp/rtsp_idle.c"
"$WORK/rtsp_idle_test"
