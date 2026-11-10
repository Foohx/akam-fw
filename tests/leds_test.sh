#!/usr/bin/env bash
# Host test of src/ak_rtsp/leds.c (status LED state machine, saved settings, 8091 keys,
# blink thread) against a fake sysfs directory, a fake network check and a fake IR hook.
# No camera, no real LED.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/leds_test" \
    "$REPO/tests/native/leds_test.c" "$REPO/src/ak_rtsp/leds.c"
"$WORK/leds_test"
