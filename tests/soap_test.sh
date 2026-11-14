#!/usr/bin/env bash
# Host test of src/ak_rtsp/soap.c (SOAP scanner, templates, faults) under ASan/UBSan, with a fuzz loop. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/soap_test" \
    "$REPO/tests/native/soap_test.c" "$REPO/src/ak_rtsp/soap.c"
"$WORK/soap_test"
