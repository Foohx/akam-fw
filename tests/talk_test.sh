#!/usr/bin/env bash
# Host test of src/ak_rtsp/talk.c (talk session of the audio backchannel: RTP in, decode,
# speech gate / half duplex, speaker queue, tail flush, 8091 counters) with a fake speaker.
# Needs only a host C compiler; touches no hardware.
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

SRC="$REPO/src/ak_rtsp"
"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -pthread -I"$SRC" \
    -o "$WORK/talk_test" \
    "$REPO/tests/native/talk_test.c" "$SRC/talk.c" "$SRC/rtp_in.c" "$SRC/audio_vad.c" \
    "$SRC/g711.c" "$SRC/audio_out.c" "$SRC/audio_jitter.c" "$SRC/rtp_clock.c" -lm
"$WORK/talk_test"
