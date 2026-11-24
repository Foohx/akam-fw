#!/usr/bin/env bash
# Host test of the video latency bounds (ring join/catch-up of the Rotatable 2K app, tx_pace latency bound, frame-age
# statistics) in virtual time with a fake encoder and a slow reader. No network, no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$PLATFORM_2K" "${SHARED_INC[@]}" \
    -o "$WORK/rot2k_latency_test" \
    "$REPO/tests/native/rot2k_latency_test.c" "$PLATFORM_2K/stream.c" "$SRC_NET/rtsp/tx_pace.c" \
    "$SRC_NET/rtsp/h264_params.c" "$SRC_NET/rtsp/rtp_clock.c" -lpthread
"$WORK/rot2k_latency_test"
