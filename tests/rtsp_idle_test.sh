#!/usr/bin/env bash
# Host test of src/net/rtsp/rtsp_idle.c (pre-PLAY idle timeout and PLAY cap of the
# single-client RTSP server). Pure state machine on a fake clock: no sleep.
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
    -o "$WORK/rtsp_idle_test" \
    "$REPO/tests/native/rtsp_idle_test.c" "$REPO/src/net/rtsp/rtsp_idle.c"
"$WORK/rtsp_idle_test"
