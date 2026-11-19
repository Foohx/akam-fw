#!/usr/bin/env bash
# Host test of src/ak_rtsp/orient.c (picture flip and mirror: when the ISP is told, and what). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/orient_test" \
    "$REPO/tests/native/orient_test.c" "$REPO/src/ak_rtsp/orient.c" -pthread
"$WORK/orient_test" | grep -v "^\[orient\]"
