#!/usr/bin/env bash
# Host test of src/net/rtsp/rtsp_in.c (RTSP/TCP input demultiplexer: RTSP
# requests mixed with interleaved '$' RTCP frames split across reads, 30
# minutes of receiver reports and keepalives, bounded buffer, overflow
# handling). Needs only a host C compiler; touches no hardware.
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
    -o "$WORK/rtsp_in_test" \
    "$REPO/tests/native/rtsp_in_test.c" "$REPO/src/net/rtsp/rtsp_in.c"
"$WORK/rtsp_in_test"
