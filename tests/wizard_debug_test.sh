#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam debug (on [MINUTES], off, status) against a fake ssh that runs the command line the wizard
# sends: the real akam_debug.sh with a fake `ak_rtsp ctl`. The real camera is never contacted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
HELPER="$ROOT/tools/firmware_patch_templates/akam_debug.sh"

T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { local n=$1; shift; if "$@"; then ok "$n"; else ko "$n"; fi; }
has() { grep -qF -- "$2" "$1"; }

export HOME=$T/home NO_COLOR=1
mkdir -p "$HOME"
unset AKAM_STATE_DIR AKAM_SSH

export STUB_DIR=$T/stub CAM=$T/cam HELPER
mkdir -p "$STUB_DIR" "$CAM"
echo 0 >"$CAM/state"
cat >"$STUB_DIR/ak_rtsp" <<'EOF'
#!/bin/sh
echo "$*" >>"$CAM/ctl.log"
case "$2 $3" in
    "DEBUG on") echo "OK ctl.debug=1 minutes=$4"; echo 1 >"$CAM/state"; echo "$4" >"$CAM/min" ;;
    "DEBUG off") echo "OK ctl.debug=0"; echo 0 >"$CAM/state" ;;
    "DEBUG status")
        if [ "$(cat "$CAM/state")" = 1 ]; then echo "ctl.debug=1 remaining=$(( $(cat "$CAM/min") * 60 ))"; else echo "ctl.debug=0"; fi ;;
    *) echo "ERR unknown command"; exit 1 ;;
esac
EOF
chmod +x "$STUB_DIR/ak_rtsp"
cat >"$STUB_DIR/ssh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
[ "${STUB_SSH_RC:-0}" = 0 ] || exit "$STUB_SSH_RC"
case $cmd in
    true) exit 0 ;;
    "/usr/sbin/akam_debug.sh "*)
        [ -z "${STUB_OLD:-}" ] || { echo "sh: /usr/sbin/akam_debug.sh: not found"; exit 127; }
        # shellcheck disable=SC2086
        exec env AKAM_RTSP_BIN="$STUB_DIR/ak_rtsp" sh "$HELPER" ${cmd#/usr/sbin/akam_debug.sh } ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
S=$(new_state d)

a "$S" debug </dev/null >"$T/o1" 2>&1
check "debug (default status): off" bash -c '[ "$1" -eq 0 ] && grep -q "debug off" "$2"' _ $? "$T/o1"
a "$S" debug status </dev/null >"$T/o2" 2>&1
check "debug status: off" has "$T/o2" "debug off"
a "$S" debug on </dev/null >"$T/o3" 2>&1
check "debug on: default 60 minutes" bash -c '[ "$1" -eq 0 ] && grep -q "60 minutes" "$2"' _ $? "$T/o3"
check "debug on: warns the network can write" has "$T/o3" "can now change the camera settings"
check "debug on: flag set on the camera" test "$(cat "$CAM/state")" = 1
a "$S" debug status </dev/null >"$T/o4" 2>&1
check "debug status: on with time left" has "$T/o4" "debug on, 3600 s left"
a "$S" debug on 5 </dev/null >"$T/o5" 2>&1
check "debug on 5: sent 5" bash -c '[ "$1" -eq 0 ] && grep -q "5 minutes" "$2" && [ "$(cat "$3")" = 5 ]' _ $? "$T/o5" "$CAM/min"
a "$S" debug off </dev/null >"$T/o6" 2>&1
check "debug off: ok, flag cleared" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = 0 ]' _ $? "$CAM/state"
check "debug off: says read-only again" has "$T/o6" "read-only for the network again"

: >"$STUB_DIR/ssh.log"
for bad in "on 0" "on 1441" "on abc" "on 5 6" "on -1" "off 3" "status 1" "maybe" "on 5;reboot"; do
    # shellcheck disable=SC2086
    a "$S" debug $bad </dev/null >"$T/bad" 2>&1
    check "refused (exit 2): debug $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"

STUB_OLD=1 a "$S" debug on </dev/null >"$T/old" 2>&1
check "old image: fails and says to update" bash -c '[ "$1" -ne 0 ] && grep -q "./akam update" "$2"' _ $? "$T/old"
STUB_SSH_RC=255 a "$S" debug off </dev/null >"$T/down" 2>&1
check "camera unreachable: fails with a clear message" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2"' _ $? "$T/down"

: >"$STUB_DIR/ssh.log"
a "$S" --dry-run debug on 7 </dev/null >"$T/dry" 2>&1
check "dry-run: WOULD line, camera untouched" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD run over SSH on the camera: /usr/sbin/akam_debug.sh on 7" "$2" && [ ! -s "$3" ]' _ $? "$T/dry" "$STUB_DIR/ssh.log"

a "$S" help >"$T/help" 2>&1
check "help lists debug" has "$T/help" "debug [on [MINUTES] | off | status]"

echo "wizard_debug_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
