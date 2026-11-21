#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/talk.c (talk session of the audio backchannel: RTP in, decode,
# speech gate / half duplex, speaker queue, tail flush, 8091 counters) with a fake speaker.
# Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SRC="$REPO/platforms/anyka-ak3918"
"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-sanitize-recover=undefined -pthread -I"$SRC" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/talk_test" \
    "$REPO/tests/native/talk_test.c" "$SRC/talk.c" "$REPO/src/net/rtsp/rtp_in.c" "$REPO/src/features/audio/audio_vad.c" \
    "$REPO/src/features/audio/g711.c" "$SRC/audio_out.c" "$REPO/src/features/audio/audio_jitter.c" "$REPO/src/net/rtsp/rtp_clock.c" -lm
"$WORK/talk_test"
