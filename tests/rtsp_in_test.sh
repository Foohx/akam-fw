#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtsp_in.c (RTSP/TCP input demultiplexer: RTSP
# requests mixed with interleaved '$' RTCP frames split across reads, 30
# minutes of receiver reports and keepalives, bounded buffer, overflow
# handling). Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_in_test" \
    "$REPO/tests/native/rtsp_in_test.c" "$REPO/src/ak_rtsp/rtsp_in.c"
"$WORK/rtsp_in_test"
