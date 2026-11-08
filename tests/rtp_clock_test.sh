#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtp_clock.c (video capture clock, audio mapping,
# NTP/RTP pairing for RTCP Sender Reports) on simulated time. Needs only a
# host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtp_clock_test" \
    "$REPO/tests/native/rtp_clock_test.c" "$REPO/src/ak_rtsp/rtp_clock.c" -lm
"$WORK/rtp_clock_test"
