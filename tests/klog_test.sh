#!/usr/bin/env bash
# Host test of the kernel log filter (src/ak_rtsp/klog.c, klog_run.c): drop, collapse, priorities, syslogd absent.
# Synthetic input only; never reads the real kernel log.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/klog_test" \
    "$REPO/tests/native/klog_test.c" "$REPO/src/ak_rtsp/klog.c" "$REPO/src/ak_rtsp/klog_run.c"
"$WORK/klog_test" "$WORK"
