#!/usr/bin/env bash
# Host test of src/ak_rtsp/audio_vad.c (energy gate of the half-duplex rule of the audio
# backchannel: threshold, 300 ms hold, jitter delay). Needs only a host C compiler.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$REPO/src/ak_rtsp" \
    -o "$WORK/audio_vad_test" \
    "$REPO/tests/native/audio_vad_test.c" "$REPO/src/ak_rtsp/audio_vad.c" -lm
"$WORK/audio_vad_test"
