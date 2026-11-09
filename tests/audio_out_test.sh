#!/usr/bin/env bash
# Host test of src/ak_rtsp/audio_out.c (speaker output: device sequence, crash guard,
# amplifier policy, software volume, beep, 8091 keys, playback thread) against a fake
# PCM device and a fake clock. No camera, no sleeping on the amp timers.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/audio_out_test" \
    "$REPO/tests/native/audio_out_test.c" "$REPO/src/ak_rtsp/audio_out.c"
"$WORK/audio_out_test"
