#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam ntp (status, set SERVER..., off) against a fake ssh that runs the real akam_ntp.sh in a fake
# camera root with stub ntpd/date. The real camera is never contacted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
HELPER="$ROOT/tools/firmware_patch_templates/akam_ntp.sh"

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
mkdir -p "$STUB_DIR" "$CAM/root/etc/config" "$CAM/root/tmp"
echo 1970 >"$CAM/year"
cat >"$STUB_DIR/date" <<'EOS'
#!/bin/sh
case "$*" in "-u +%Y") cat "$CAM/year" ;; *) echo "$(cat "$CAM/year")-01-01 00:00:00" ;; esac
EOS
cat >"$STUB_DIR/ntpd" <<'EOS'
#!/bin/sh
echo "ntpd $*" >>"$CAM/ntpd.log"
EOS
cat >"$STUB_DIR/ssh" <<'EOS'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
export AKAM_ROOT="$CAM/root" AKAM_NTPD="$STUB_DIR/ntpd" AKAM_NTP_NO_START=1 PATH="$STUB_DIR:$PATH"
case $cmd in
    true) exit 0 ;;
    "/usr/sbin/akam_ntp.sh "*)
        [ -z "${STUB_OLD:-}" ] || { echo "sh: /usr/sbin/akam_ntp.sh: not found"; exit 127; }
        # shellcheck disable=SC2086
        exec sh "$HELPER" ${cmd#/usr/sbin/akam_ntp.sh } ;;
esac
exit 0
EOS
chmod +x "$STUB_DIR"/*
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"
FILE=$CAM/root/etc/config/akam/ntp

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
S=$(new_state n)

a "$S" ntp </dev/null >"$T/s1" 2>&1
check "ntp (default status): off, exit 0" bash -c '[ "$1" -eq 0 ] && grep -q "time sync: off" "$2"' _ $? "$T/s1"
check "status: the clock is reported as not set" has "$T/s1" "sys.time_set: 0"
check "status: nothing written" test ! -e "$FILE"

a "$S" ntp set pool.ntp.org 192.0.2.1 </dev/null >"$T/s2" 2>&1
check "set: succeeds" bash -c '[ "$1" -eq 0 ] && grep -q "Time sync on" "$2"' _ $? "$T/s2"
check "set: says what is sent and that it is opt-in" bash -c 'grep -q "UDP 123, outgoing only" "$1" && grep -q "Nothing is sent unless you do this" "$1"' _ "$T/s2"
check "set: file on the camera" test "$(cat "$FILE")" = "$(printf 'server=pool.ntp.org\nserver=192.0.2.1')"
a "$S" ntp status </dev/null >"$T/s3" 2>&1
check "status after set: servers shown" has "$T/s3" "servers: pool.ntp.org 192.0.2.1"

a "$S" ntp off </dev/null >"$T/s4" 2>&1
check "off: succeeds, file gone" bash -c '[ "$1" -eq 0 ] && [ ! -e "$2" ]' _ $? "$FILE"

# refusals: nothing reaches the camera
: >"$STUB_DIR/ssh.log"
for bad in "set" "set a b c d" "set -x" 'set a;b' 'set $(id)' "set bad_name" "set a..b" "set pool.ntp.org:123" "off now" "status now" "frob" "SET x.org" "set x.org --"; do
    # shellcheck disable=SC2086  # word list on purpose
    a "$S" ntp $bad </dev/null >"$T/bad.out" 2>&1
    check "refused (exit 2): ntp $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"

# dry run
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run ntp set pool.ntp.org </dev/null >"$T/d.out" 2>&1
check "dry-run: WOULD run" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD run over SSH on the camera: /usr/sbin/akam_ntp.sh set pool.ntp.org" "$2"' _ $? "$T/d.out"
check "dry-run: nothing contacted, nothing written" bash -c '[ ! -s "$1" ] && [ ! -e "$2" ]' _ "$STUB_DIR/ssh.log" "$FILE"

# an older image without the helper
STUB_OLD=1 a "$S" ntp status </dev/null >"$T/old.out" 2>&1
check "older image: clear message, non-zero" bash -c '[ "$1" -ne 0 ] && grep -q "older Akam image" "$2"' _ $? "$T/old.out"

a "$S" help >"$T/help.out" 2>&1
check "help lists ntp" has "$T/help.out" "ntp [status | set SERVER... | off]"

echo "wizard_ntp_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
