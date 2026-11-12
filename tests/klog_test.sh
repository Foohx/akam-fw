#!/usr/bin/env bash
# shellcheck disable=SC2016  # the stub scripts are written with single quotes on purpose
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
# filter cannot start or crashed 5 times in a row. ak_rtsp, sleep, klogd and /proc/uptime are stubs; nothing real
# is started. Each RCS entry is "exit-code" or "exit-code:seconds-the-run-lasted" (the stub advances the fake uptime).
LINE=$(sed -n "s/^    KLOG_NEW='\(.*\)'\$/\1/p" "$REPO/src/ak_rtsp/build_firmware.sh")
[ -n "$LINE" ] || { echo "FAIL: KLOG_NEW not found in build_firmware.sh"; exit 1; }
LINE=${LINE//\/usr\/bin\/ak_rtsp/$WORK/fake_ak_rtsp}
LINE=${LINE//\/proc\/uptime/$WORK/uptime}
mkdir -p "$WORK/stubs"
printf '#!/bin/sh\nexit 0\n' >"$WORK/stubs/sleep"
printf '#!/bin/sh\necho klogd >>"$WORK/calls"\n' >"$WORK/stubs/klogd"
cat >"$WORK/fake_ak_rtsp" <<'EOF'
#!/bin/sh
echo ak >>"$WORK/calls"
n=$(cat "$WORK/n" 2>/dev/null || echo 0)
n=$((n + 1))
echo $n >"$WORK/n"
set -- $RCS
shift $((n - 1))
spec=${1:-1}
rc=${spec%%:*}
dur=0
case $spec in *:*) dur=${spec#*:} ;; esac
read up _ <"$WORK/uptime"
echo "$(( ${up%.*} + dur )).00 0.00" >"$WORK/uptime"
exit "$rc"
EOF
chmod +x "$WORK/stubs/sleep" "$WORK/stubs/klogd" "$WORK/fake_ak_rtsp"
sh_fail=0
fallback_case() { # name rc-list expected-call-sequence
    rm -f "$WORK/calls" "$WORK/n"
    echo "1000.00 0.00" >"$WORK/uptime"
    export RCS=$2 WORK
    PATH="$WORK/stubs:$PATH" sh -c "$LINE"$'\nwait' >/dev/null 2>&1
    got=$(paste -sd' ' "$WORK/calls")
    if [ "$got" = "$3" ]; then echo "  ok   - $1"; else echo "  FAIL - $1: got '$got', want '$3'"; sh_fail=1; fi
}
fallback_case "cannot start (exit 1): stock klogd at once" "1" "ak klogd"
fallback_case "binary missing (127): stock klogd at once" "127" "ak klogd"
fallback_case "killed (143), then cannot start: restarted, then klogd" "143 137 1" "ak ak ak klogd"
fallback_case "five crashes in a row (139): klogd after the fifth" "139 139 139 139 139 139" "ak ak ak ak ak klogd"
fallback_case "SIGSEGV once then clean end: restarted, then klogd" "139 0" "ak ak klogd"
# the reviewed bug: `kill $(pidof ak_rtsp)` used to hit the filter; every kill counted as a crash and after 5 the
# stock klogd took over for good
fallback_case "killed by pidof 8 times (143): never counted, never falls back to klogd" "143 143 143 143 143 143 143 143 1" "ak ak ak ak ak ak ak ak ak klogd"
fallback_case "SIGINT (130) is a stop request too" "130 130 130 130 130 130 1" "ak ak ak ak ak ak ak klogd"
fallback_case "four crashes, then a run of 61 s resets the count (the 5th crash would have fallen back)" "139 139 139 139 139:61 139 139 139 139" "ak ak ak ak ak ak ak ak ak klogd"
fallback_case "a long run (61 s) that ends in a crash counts as one" "139:61 139:61 139:61 139:61 139:61 139:61 1" "ak ak ak ak ak ak ak klogd"
fallback_case "short runs (59 s) do not reset: klogd after the fifth" "139:59 139:59 139:59 139:59 139:59 139:59" "ak ak ak ak ak klogd"
[ "$sh_fail" = 0 ]

# The restart command of the assistant and the wizard (restartVideoCmd) kills the streamer but never the filter,
# although `pidof ak_rtsp` lists both (same exe). Two real processes: one shows akam_klog in its cmdline.
RESTART=$(sed -n 's/^const restartVideoCmd = `\(.*\)`$/\1/p' "$REPO/tools/assistant/internal/cli/remote.go")
[ -n "$RESTART" ] || { echo "FAIL: restartVideoCmd not found"; exit 1; }
printf '#!/bin/sh\necho "$PIDS"\n' >"$WORK/stubs/pidof"
chmod +x "$WORK/stubs/pidof"
( exec -a ak_rtsp sleep 120 ) & STREAMER=$!
( exec -a akam_klog sleep 120 ) & FILTER=$!
cleanup_pids() { kill "$STREAMER" "$FILTER" 2>/dev/null || true; wait 2>/dev/null || true; }
trap 'cleanup_pids; rm -rf "$WORK"' EXIT
sleep 0.3
PIDS="$STREAMER $FILTER" PATH="$WORK/stubs:$PATH" sh -c "$RESTART"
sleep 0.3
if kill -0 "$STREAMER" 2>/dev/null; then echo "  FAIL - restart command did not stop the streamer"; exit 1; fi
if kill -0 "$FILTER" 2>/dev/null; then echo "  ok   - restart command killed the streamer and left the akam_klog process alone"; else echo "  FAIL - restart command killed the kernel log filter"; exit 1; fi
cleanup_pids
