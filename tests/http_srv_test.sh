#!/usr/bin/env bash
# Host test of src/ak_rtsp/http_srv.c (+ soap.c for the template/producer path) over real localhost sockets under
# ASan/UBSan: routing, keep-alive, pipelining, limits, slowloris, connection cap, start/stop/start, fd and thread
# leaks. No camera. Takes about 12 s (one test waits out the real 5 s request timeout).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/http_srv_test" \
    "$REPO/tests/native/http_srv_test.c" "$REPO/src/ak_rtsp/http_srv.c" "$REPO/src/ak_rtsp/soap.c" -pthread
"$WORK/http_srv_test"
