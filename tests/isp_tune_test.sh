#!/usr/bin/env bash
# Host test of src/ak_rtsp/isp_tune.c (saved picture settings: load, validation, atomic save). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/isp_tune_test" \
    "$REPO/tests/native/isp_tune_test.c" "$REPO/src/ak_rtsp/isp_tune.c"
"$WORK/isp_tune_test" "$WORK"
