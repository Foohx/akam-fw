#!/usr/bin/env bash
# Host test of src/ak_rtsp/md5.c and rtsp_auth.c (MD5 and Digest vectors, the
# authentication state machine, credentials file reload, rate limiting, no
# secret in the logs). Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_auth_test" \
    "$REPO/tests/native/rtsp_auth_test.c" "$REPO/src/ak_rtsp/rtsp_auth.c" "$REPO/src/ak_rtsp/md5.c"
"$WORK/rtsp_auth_test"
