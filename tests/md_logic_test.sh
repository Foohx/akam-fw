#!/bin/sh
# Host test of src/ak_rtsp/md_logic.c (motion detection: block reduction, score, state machine).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap "rm -rf \"$WORK\"" EXIT
${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -O1 -I"$REPO/src/ak_rtsp" \
    -o "$WORK/md_logic_test" "$REPO/tests/native/md_logic_test.c" "$REPO/src/ak_rtsp/md_logic.c"
"$WORK/md_logic_test"
