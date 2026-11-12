#!/usr/bin/env bash
# shellcheck disable=SC2012  # ls -ld is what the camera script uses too (no stat on BusyBox): the test mirrors it
# Host tests for akam_mqtt.sh (camera script, run under busybox sh when available) in a fake root ($AKAM_ROOT). A fake
# `ak_rtsp` plays "ak_rtsp ctl <line>". Nothing touches hardware or the network. The broker password is a sentinel
# that must never appear in any output or in the settings file.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
MQ=$REPO/tools/firmware_patch_templates/akam_mqtt.sh

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
contains() { case $2 in *"$3"*) ok "$1" ;; *) ko "$1 (no '$3' in: $2)" ;; esac; }
lacks() { case $2 in *"$3"*) ko "$1 (found '$3' in: $2)" ;; *) ok "$1" ;; esac; }
perm() { ls -ld "$1" | cut -c1-10; }
exists() { if [ -e "$1" ]; then echo yes; else echo no; fi; }
SECRET='S3NTINEL-br0ker-pw'

# fake ak_rtsp: FAKE_MODE = new (answers), old (ERR unknown command), dead (connection refused); FAKE_TEST = the
# results of successive "GET mqtt.test" calls, one word per call (the last one repeats)
cat >"$WORK/ak_rtsp" <<'EOF2'
#!/bin/sh
echo "$*" >>"$FAKE_LOG"
[ "$1" = ctl ] || exit 9
case "${FAKE_MODE:-new}" in
    dead) echo "ctl: connect 127.0.0.1: Connection refused" >&2; exit 1 ;;
    old) echo "ERR unknown command"; exit 1 ;;
esac
case "$2 $3" in
    "MQTT reload") echo "OK mqtt.state=connecting" ;;
    "MQTT test") echo "OK mqtt.test=running" ;;
    "GET mqtt.state") echo "mqtt.state=connected" ;;
    "GET mqtt.test")
        n=$(cat "$FAKE_LOG.n" 2>/dev/null || echo 0)
        n=$((n + 1))
        echo "$n" >"$FAKE_LOG.n"
        # shellcheck disable=SC2086
        set -- $FAKE_TEST
        [ "$n" -le "$#" ] || n=$#
        eval "r=\${$n}"
        echo "mqtt.test=$r" ;;
    *) echo "ERR unknown command"; exit 1 ;;
esac
EOF2
chmod +x "$WORK/ak_rtsp"
# no real sleeping in the poll loop
mkdir "$WORK/bin"
printf '#!/bin/sh\nexit 0\n' >"$WORK/bin/sleep"
chmod +x "$WORK/bin/sleep"
export FAKE_LOG=$WORK/log AKAM_RTSP_BIN=$WORK/ak_rtsp
R=$WORK/root
export AKAM_ROOT=$R
CFG=$R/etc/config/akam/mqtt
PW=$R/etc/config/akam/mqtt_pass
D=$R/etc/config/akam
mkdir -p "$R/etc/config"
run() { PATH="$WORK/bin:$PATH" "${SHCMD[@]}" "$MQ" "$@" 2>&1; }
# run_quiet ARGS...: no stdin, no output, exit status of the helper
run_quiet() { PATH="$WORK/bin:$PATH" "${SHCMD[@]}" "$MQ" "$@" </dev/null >/dev/null 2>&1; }
# runpw PASSWORD ARGS...: the password goes on stdin, like the wizard does
runpw() { local pw=$1; shift; printf '%s\n' "$pw" | PATH="$WORK/bin:$PATH" "${SHCMD[@]}" "$MQ" "$@" 2>&1; }

# --- status with nothing
out=$(run status); rc=$?
eq "status, no file: exit 0" "$rc" 0
contains "status, no file: disabled" "$out" "MQTT: disabled (default)"
contains "status: no password" "$out" "password: none"
contains "status: ak_rtsp state shown" "$out" "state: connected"

# --- set
: >"$FAKE_LOG"
mkdir -p "$D"; echo stale-secret >"$PW.new.4242"; echo stale >"$CFG.new.4242"    # leftovers of an interrupted run
out=$(runpw "$SECRET" set 192.0.2.10 1883 bob); rc=$?
eq "set: exit 0" "$rc" 0
eq "set: stale temp files of an interrupted run are removed" "$(exists "$PW.new.4242")$(exists "$CFG.new.4242")" "nono"
eq "set: file content" "$(cat "$CFG")" "$(printf 'enabled=1\nhost=192.0.2.10\nport=1883\nuser=bob')"
eq "set: file mode 0600" "$(perm "$CFG")" "-rw-------"
eq "set: password file mode 0600" "$(perm "$PW")" "-rw-------"
eq "set: directory mode 0700" "$(ls -ld "$D" | cut -c1-10)" "drwx------"
eq "set: password file holds the password" "$(cat "$PW")" "$SECRET"
eq "set: reload sent" "$(cat "$FAKE_LOG")" "ctl MQTT reload"
contains "set: says applied" "$out" "Applied now"
contains "set: reports the real state, not a blind OK" "$out" "the MQTT state is connecting"
contains "set: warns about plain TCP" "$out" "plain TCP"
lacks "set: the password is not printed" "$out" "$SECRET"
lacks "set: the password is not in the settings file" "$(cat "$CFG")" "$SECRET"
eq "set: no temp file left" "$(ls "$D" | sort | paste -sd' ' -)" "mqtt mqtt_pass"

# --- status after set
out=$(run status); rc=$?
eq "status: exit 0" "$rc" 0
contains "status: enabled" "$out" "MQTT: enabled"
contains "status: broker" "$out" "broker: 192.0.2.10:1883, user bob"
contains "status: password set" "$out" "password: set"
contains "status: options defaults" "$out" "discovery on, prefix homeassistant, base topic akam/<hostname>"
lacks "status: no password in the output" "$out" "$SECRET"

# --- options are kept across set, and written by option
out=$(run option base_topic home/cam1); rc=$?
eq "option base_topic: exit 0" "$rc" 0
run option discovery off >/dev/null
run option interval 60 >/dev/null
run option client_id cam_one >/dev/null
run option discovery_prefix ha >/dev/null
eq "options: file content" "$(cat "$CFG")" "$(printf 'enabled=1\nhost=192.0.2.10\nport=1883\nuser=bob\nclient_id=cam_one\nbase_topic=home/cam1\ndiscovery=off\ndiscovery_prefix=ha\ninterval=60')"
eq "options: password untouched" "$(cat "$PW")" "$SECRET"
out=$(runpw "$SECRET" set broker.lan 1884 alice)
eq "set again keeps the options" "$(cat "$CFG")" "$(printf 'enabled=1\nhost=broker.lan\nport=1884\nuser=alice\nclient_id=cam_one\nbase_topic=home/cam1\ndiscovery=off\ndiscovery_prefix=ha\ninterval=60')"
for bad in "base_topic /x" "base_topic a//b" "base_topic a+b" "base_topic a/" "client_id a.b" "discovery yes" "interval 9" "interval 301" "interval x" "bogus 1" "enabled 0" "host x" "base_topic"; do
    # shellcheck disable=SC2086
    run option $bad >/dev/null
    rc=$?
    eq "option '$bad': usage error 2" "$rc" 2
done
eq "refused options left the file alone" "$(grep -c . "$CFG")" 9

# --- disable keeps the settings, forget deletes them
: >"$FAKE_LOG"
out=$(run disable); rc=$?
eq "disable: exit 0" "$rc" 0
eq "disable: enabled=0, rest kept" "$(sed -n 1,3p "$CFG" | paste -sd' ' -)" "enabled=0 host=broker.lan port=1884"
eq "disable: password kept" "$(cat "$PW")" "$SECRET"
eq "disable: mode 0600" "$(perm "$CFG")" "-rw-------"
eq "disable: reload sent" "$(cat "$FAKE_LOG")" "ctl MQTT reload"
contains "status after disable" "$(run status)" "MQTT: disabled"
out=$(run forget); rc=$?
eq "forget: exit 0" "$rc" 0
eq "forget: settings gone" "$(exists "$CFG")" no
eq "forget: password file gone" "$(exists "$PW")" no
out=$(run disable); rc=$?
eq "disable with no file: exit 0" "$rc" 0
contains "disable with no file: already disabled" "$out" "already disabled"
eq "disable with no file: no file created" "$(exists "$CFG")" no

# --- no password, no user
out=$(runpw "" set broker.lan 1883 -); rc=$?
eq "set without user or password: exit 0" "$rc" 0
eq "set without user: user= empty" "$(cat "$CFG")" "$(printf 'enabled=1\nhost=broker.lan\nport=1883\nuser=')"
eq "set without password: no password file" "$(exists "$PW")" no
contains "status: user none" "$(run status)" "user none"
out=$(PATH="$WORK/bin:$PATH" "${SHCMD[@]}" "$MQ" set broker.lan 1883 bob 2>&1 </dev/null); rc=$?
eq "set with closed stdin: exit 0, no password" "$rc" 0
eq "...no password file" "$(exists "$PW")" no
# a stale password file is removed when the new password is empty
printf 'old\n' >"$PW"
chmod 600 "$PW"
runpw "" set broker.lan 1883 bob >/dev/null
eq "an empty password removes the old file" "$(exists "$PW")" no
out=$(runpw "$SECRET" set broker.lan 1883 -); rc=$?
eq "a password without a user name: exit 2" "$rc" 2
lacks "...not echoed" "$out" "$SECRET"
eq "...nothing written" "$(exists "$PW")" no

# --- argument validation (nothing written on a refusal)
run forget >/dev/null
for badargs in "set" "set h" "set h 1883" "set h 0 u" "set h 65536 u" "set h abc u" "set h 123456 u" "set -h 1883 u" "set .h 1883 u" "set a/b 1883 u" \
    "set h 1883 a:b" "set h 1883 a/b" "set h 1883 u extra"; do
    # shellcheck disable=SC2086
    run $badargs >/dev/null </dev/null
    rc=$?
    eq "bad arguments '$badargs': exit 2" "$rc" 2
done
run_quiet set "a b" 1883 u
eq "host with a space: exit 2" "$?" 2
run_quiet set h 1883 "a b"
eq "user with a space: exit 2" "$?" 2
eq "bad arguments wrote nothing" "$(exists "$CFG")$(exists "$PW")" nono
long=$(printf 'a%.0s' $(seq 1 64))
run_quiet set "$long" 1883 u
eq "host of 64 characters: exit 2" "$?" 2
long33=$(printf 'u%.0s' $(seq 1 33))
run_quiet set h 1883 "$long33"
eq "user of 33 characters: exit 2" "$?" 2
out=$(runpw "$(printf 'p%.0s' $(seq 1 129))" set h 1883 u); rc=$?
eq "password of 129 characters: exit 2" "$rc" 2
out=$(runpw "$(printf 'p%.0s' $(seq 1 128))" set h 1883 u); rc=$?
eq "password of 128 characters: accepted" "$rc" 0
out=$(runpw "$(printf 'ab\tc')" set h 1883 u); rc=$?
eq "password with a control character: exit 2" "$rc" 2
eq "...the previous password is untouched" "$(wc -c <"$PW" | paste -sd' ' -)" 129
out=$(runpw "pa ss:w/ord'\"\$x" set h 1883 u); rc=$?
eq "password with spaces and shell characters: accepted" "$rc" 0
eq "...stored byte for byte" "$(cat "$PW")" "pa ss:w/ord'\"\$x"
run forget >/dev/null

# --- option and test need a saved broker
out=$(run option interval 30); rc=$?
eq "option without a saved broker: exit 1" "$rc" 1
out=$(run test); rc=$?
eq "test without a saved broker: exit 1" "$rc" 1

# --- test
runpw "$SECRET" set 192.0.2.10 1883 bob >/dev/null
: >"$FAKE_LOG"
rm -f "$FAKE_LOG.n"
out=$(FAKE_TEST="running running ok" run test); rc=$?
eq "test ok: exit 0" "$rc" 0
contains "test ok: says so" "$out" "MQTT test: ok"
contains "test: polled until the result" "$(cat "$FAKE_LOG")" "GET mqtt.test"
eq "test: three polls" "$(cat "$FAKE_LOG.n")" 3
for pair in "refused_credentials:refused the user name or password" "connect:nothing answered" "timeout:did not answer in time" "closed:closed the connection" \
    "protocol:not MQTT" "dns:could not be resolved" "refused_not_authorized:not authorised" "weird:refused (weird)"; do
    rm -f "$FAKE_LOG.n"
    out=$(FAKE_TEST="running error:${pair%%:*}" run test); rc=$?
    eq "test ${pair%%:*}: exit 1" "$rc" 1
    contains "test ${pair%%:*}: reason" "$out" "${pair#*:}"
done
rm -f "$FAKE_LOG.n"
out=$(AKAM_MQTT_TEST_TRIES=3 FAKE_TEST="running" run test); rc=$?
eq "test that never ends: exit 1" "$rc" 1
contains "test that never ends: says so" "$out" "no result after 3 seconds"
export FAKE_MODE=old
out=$(run test); rc=$?
eq "test on an older ak_rtsp: exit 1" "$rc" 1
contains "test on an older ak_rtsp: update hint" "$out" "older"
unset FAKE_MODE

# --- older firmware: reboot to apply, still exit 0, settings saved
export FAKE_MODE=old
out=$(runpw "$SECRET" set h 1883 bob); rc=$?
eq "old firmware: set exit 0" "$rc" 0
contains "old firmware: reboot to apply" "$out" "reboot the camera to apply"
eq "old firmware: file saved" "$(sed -n 1p "$CFG")" "enabled=1"
contains "old firmware: state unknown" "$(run status)" "state: unknown"
export FAKE_MODE=dead
out=$(run disable); rc=$?
eq "ak_rtsp down: disable exit 0" "$rc" 0
contains "ak_rtsp down: applies at next start" "$out" "next time ak_rtsp starts"
unset FAKE_MODE

# --- invalid files
printf 'enabled=2\nhost=h\n' >"$CFG"
out=$(run status); rc=$?
eq "invalid file: status exit 3" "$rc" 3
contains "invalid file: INVALID shown" "$out" "INVALID"
for bad in 'enabled=1\n' 'host=h\n' 'enabled=1\nhost=h\nport=0\n' 'enabled=1\nhost=h\nport=99999\n' 'enabled=1\nenabled=1\nhost=h\n' \
    'enabled=1\nhost=h\nbogus=1\n' 'enabled=1\nhost=h\n\n' 'enabled=1\nhost=h\nuser=a b\n' 'enabled=1\nhost=h\ninterval=5\n' \
    'enabled=1\nhost=h\ndiscovery=1\n' 'enabled=1\nhost=h\nbase_topic=/x\n' 'enabled=1\nhost=h\npass=secret\n' 'enabled=1\nhost=h\nuser=-\n'; do
    # shellcheck disable=SC2059
    printf "$bad" >"$CFG"
    run status >/dev/null
    rc=$?
    eq "invalid file '${bad//\\n/|}': status 3" "$rc" 3
done
printf 'enabled=1\r\nhost=h\r\nport=1883\r\n' >"$CFG"
run status >/dev/null
eq "CRLF file tolerated" "$?" 0
printf 'junk\n' >"$CFG"
out=$(run option interval 60); rc=$?
eq "option over an invalid file: exit 1" "$rc" 1
runpw "" set h 1883 - >/dev/null
eq "set over an invalid file: clean content" "$(cat "$CFG")" "$(printf 'enabled=1\nhost=h\nport=1883\nuser=')"

# --- a password file that others can read
printf 'x\n' >"$PW"
chmod 644 "$PW"
contains "status: world-readable password file flagged" "$(run status)" "not private"

# --- usage
for u in "" bogus "disable now" "status x" "forget now" "test now" "on"; do
    # shellcheck disable=SC2086
    run $u >/dev/null </dev/null
    rc=$?
    eq "usage '$u': exit 2" "$rc" 2
done

echo "akam_mqtt_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
