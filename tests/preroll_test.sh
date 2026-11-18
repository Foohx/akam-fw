#!/bin/sh
# Host test of src/ak_rtsp/preroll.c (lead-in buffer: ring of encoded frames).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap "rm -rf \"$WORK\"" EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -O1 -I"$REPO/src/ak_rtsp" \
    -o "$WORK/preroll_test" "$REPO/tests/native/preroll_test.c" "$REPO/src/ak_rtsp/preroll.c"
"$WORK/preroll_test"
