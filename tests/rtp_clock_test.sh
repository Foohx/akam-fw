#!/usr/bin/env bash
# Host test of src/net/rtsp/rtp_clock.c (video capture clock, audio mapping,
# NTP/RTP pairing for RTCP Sender Reports) on simulated time. Needs only a
# host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/rtp_clock_test" \
    "$REPO/tests/native/rtp_clock_test.c" "$REPO/src/net/rtsp/rtp_clock.c" -lm
"$WORK/rtp_clock_test"
