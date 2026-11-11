#!/usr/bin/env bash
# Host test of src/ak_rtsp/audio.c (G07): the capture supervisor (retry with backoff after read errors or a failed
# open, prompt stop) and audio.mic_enable=0 (capture device never opened), against a faked device. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -DAUDIO_BACKOFF_UNIT_MS=20 -DAUDIO_FAIL_SLEEP_US=1000 \
    -Wl,--wrap=open,--wrap=read,--wrap=ioctl \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/audio_sup_test" \
    "$REPO/tests/native/audio_sup_test.c" "$REPO/src/ak_rtsp/audio.c" "$REPO/src/ak_rtsp/rtp_clock.c"
"$WORK/audio_sup_test" 2>"$WORK/err.txt" | grep -v '^\[ak_rtsp\]'
