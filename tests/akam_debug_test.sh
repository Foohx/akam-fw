#!/usr/bin/env bash
# Host tests for akam_debug.sh (camera script, run under busybox sh when available). A fake `ak_rtsp`
# plays "ak_rtsp ctl <line>" (it records the line and answers like the control port). Nothing touches
# hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
DBG=$REPO/tools/firmware_patch_templates/akam_debug.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else ko "$1 (got '$2', want '$3')"; fi; }

# fake ak_rtsp: "ctl DEBUG on N" -> OK, "ctl DEBUG off" -> OK, "ctl DEBUG status" -> state kept in a file
cat >"$WORK/ak_rtsp" <<'EOF'
#!/bin/sh
echo "$*" >>"$FAKE_LOG"
[ "$1" = ctl ] || exit 9
[ -n "${FAKE_DEAD:-}" ] && { echo "ctl: connect 127.0.0.1: Connection refused" >&2; exit 1; }
case "$2 $3" in
    "DEBUG on") echo "OK ctl.debug=1 minutes=$4"; echo 1 >"$FAKE_STATE"; echo "$4" >"$FAKE_STATE.min" ;;
    "DEBUG off") echo "OK ctl.debug=0"; echo 0 >"$FAKE_STATE" ;;
    "DEBUG status")
        if [ "$(cat "$FAKE_STATE" 2>/dev/null)" = 1 ]; then echo "ctl.debug=1 remaining=$(( $(cat "$FAKE_STATE.min") * 60 ))"
        else echo "ctl.debug=0"; fi ;;
    *) echo "ERR unknown command"; exit 1 ;;
esac
EOF
chmod +x "$WORK/ak_rtsp"
export FAKE_LOG=$WORK/log FAKE_STATE=$WORK/state AKAM_RTSP_BIN=$WORK/ak_rtsp
run() { "${SHCMD[@]}" "$DBG" "$@" 2>&1; }

out=$(run status); rc=$?
eq "status when off: exit 0" "$rc" 0
case $out in "debug off"*) ok "status when off: text" ;; *) ko "status when off: text ($out)" ;; esac

: >"$FAKE_LOG"
out=$(run on); rc=$?
eq "on: exit 0" "$rc" 0
eq "on: default 60 minutes sent" "$(cat "$FAKE_LOG")" "ctl DEBUG on 60"
case $out in "debug on:"*"60 minutes"*) ok "on: text" ;; *) ko "on: text ($out)" ;; esac
out=$(run status)
case $out in "debug on, 3600 s left"*) ok "status when on" ;; *) ko "status when on ($out)" ;; esac
: >"$FAKE_LOG"
out=$(run on 5); rc=$?
eq "on 5: exit 0" "$rc" 0
eq "on 5: sent" "$(cat "$FAKE_LOG")" "ctl DEBUG on 5"
run on 1440 >/dev/null; eq "on 1440: exit 0" "$?" 0
run on 1 >/dev/null; eq "on 1: exit 0" "$?" 0
out=$(run off); rc=$?
eq "off: exit 0" "$rc" 0
case $out in "debug off:"*) ok "off: text" ;; *) ko "off: text ($out)" ;; esac

: >"$FAKE_LOG"
for bad in "on 0" "on 1441" "on -3" "on abc" "on 5 6" "on 1.5" "on ''" "off now" "status x" "frob" ""; do
    # shellcheck disable=SC2086
    eval "run $bad" >/dev/null; rc=$?
    eq "refused (exit 2): '$bad'" "$rc" 2
done
eq "refusals never reached ak_rtsp" "$(wc -c <"$FAKE_LOG" | tr -d ' ')" 0

out=$(FAKE_DEAD=1 run on); rc=$?
eq "ak_rtsp not answering: exit 1" "$rc" 1
case $out in *"did not accept"*) ok "ak_rtsp not answering: message" ;; *) ko "ak_rtsp not answering: message ($out)" ;; esac
out=$(AKAM_RTSP_BIN=$WORK/missing run status); rc=$?
eq "ak_rtsp missing: exit 1" "$rc" 1

echo "akam_debug_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
