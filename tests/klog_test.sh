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

# The one-line patch of main.sh (build_firmware.sh KLOG_NEW): restart after a crash, stock klogd only when the
# filter cannot start or crashed 5 times. ak_rtsp, sleep and klogd are stubs; nothing real is started.
LINE=$(sed -n "s/^    KLOG_NEW='\(.*\)'\$/\1/p" "$REPO/src/ak_rtsp/build_firmware.sh")
[ -n "$LINE" ] || { echo "FAIL: KLOG_NEW not found in build_firmware.sh"; exit 1; }
LINE=${LINE//\/usr\/bin\/ak_rtsp/$WORK/fake_ak_rtsp}
mkdir -p "$WORK/stubs"
printf '#!/bin/sh\nexit 0\n' >"$WORK/stubs/sleep"
printf '#!/bin/sh\necho klogd >>"$WORK/calls"\n' >"$WORK/stubs/klogd"
printf '#!/bin/sh\necho ak >>"$WORK/calls"\nn=$(cat "$WORK/n" 2>/dev/null || echo 0)\nn=$((n + 1))\necho $n >"$WORK/n"\nset -- $RCS\nshift $((n - 1))\nexit ${1:-1}\n' >"$WORK/fake_ak_rtsp"
chmod +x "$WORK/stubs/sleep" "$WORK/stubs/klogd" "$WORK/fake_ak_rtsp"
sh_fail=0
fallback_case() { # name rc-list expected-call-sequence
    rm -f "$WORK/calls" "$WORK/n"
    export RCS=$2 WORK
    PATH="$WORK/stubs:$PATH" sh -c "$LINE"$'\nwait' >/dev/null 2>&1
    got=$(paste -sd' ' "$WORK/calls")
    if [ "$got" = "$3" ]; then echo "  ok   - $1"; else echo "  FAIL - $1: got '$got', want '$3'"; sh_fail=1; fi
}
fallback_case "cannot start (exit 1): stock klogd at once" "1" "ak klogd"
fallback_case "binary missing (127): stock klogd at once" "127" "ak klogd"
fallback_case "killed (143), then cannot start: restarted, then klogd" "143 137 1" "ak ak ak klogd"
fallback_case "five crashes in a row: klogd after the fifth" "143 143 143 143 143 143" "ak ak ak ak ak klogd"
fallback_case "SIGSEGV once then clean end: restarted, then klogd" "139 0" "ak ak klogd"
[ "$sh_fail" = 0 ]
