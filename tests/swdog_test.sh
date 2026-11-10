#!/usr/bin/env bash
# Host test of src/ak_rtsp/swdog.c (software stall watchdog): pure decision logic on an injected clock.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/swdog_test" \
    "$REPO/tests/native/swdog_test.c" "$REPO/src/ak_rtsp/swdog.c"
"$WORK/swdog_test"
