#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam mqtt (status, set, disable, forget, test) against a fake ssh that runs the command line the wizard
# sends: the real akam_mqtt.sh in a fake camera root, with a fake `ak_rtsp ctl`. The real camera is never contacted.
# The broker password is a sentinel that must never reach a command line, the output, the log or the state.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
HELPER="$ROOT/tools/firmware_patch_templates/akam_mqtt.sh"

T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { local n=$1; shift; if "$@"; then ok "$n"; else ko "$n"; fi; }
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }
SECRET=S3NTINEL-br0ker-pw

export HOME=$T/home NO_COLOR=1
mkdir -p "$HOME"
unset AKAM_STATE_DIR AKAM_SSH

export STUB_DIR=$T/stub CAM=$T/cam HELPER
mkdir -p "$STUB_DIR" "$CAM/root/etc/config"
CFG=$CAM/root/etc/config/akam/mqtt
PW=$CAM/root/etc/config/akam/mqtt_pass
cat >"$STUB_DIR/ak_rtsp" <<'EOF'
#!/bin/sh
echo "$*" >>"$CAM/ctl.log"
case "$2 $3" in
    "MQTT reload") echo "OK mqtt.state=connecting" ;;
    "MQTT test") echo "OK mqtt.test=running" ;;
    "GET mqtt.state") echo "mqtt.state=connected" ;;
    "GET mqtt.test")
        if [ "$(cat "$CAM/root/etc/config/akam/mqtt_pass" 2>/dev/null)" = "reject-me-please" ]; then echo "mqtt.test=error:refused_credentials"
        else echo "mqtt.test=ok"; fi ;;
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
export AKAM_ROOT="$CAM/root" AKAM_RTSP_BIN="$STUB_DIR/ak_rtsp"
case $cmd in
    true) exit 0 ;;
    "/usr/sbin/akam_mqtt.sh "*)
        [ -z "${STUB_OLD:-}" ] || { echo "sh: /usr/sbin/akam_mqtt.sh: not found"; exit 127; }
        # shellcheck disable=SC2086
        exec sh "$HELPER" ${cmd#/usr/sbin/akam_mqtt.sh } ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
a_tty() { local st=$1; shift; AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
S=$(new_state m)

a "$S" mqtt </dev/null >"$T/s0" 2>&1
check "mqtt (default status): disabled, exit 0" bash -c '[ "$1" -eq 0 ] && grep -q "MQTT: disabled (default)" "$2"' _ $? "$T/s0"

# set: asks, default No, never without a terminal
: >"$STUB_DIR/ssh.log"
printf '192.0.2.10\n1883\nbob\n%s\ny\n' "$SECRET" | a "$S" mqtt set >"$T/n0" 2>&1
check "set: refused without a terminal, nothing written" bash -c '[ "$1" -ne 0 ] && [ ! -e "$2" ]' _ $? "$CFG"
for last in "n" "" "N" "q" "no" "maybe"; do
    printf '192.0.2.10\n1883\nbob\n%s\n%s\n' "$SECRET" "$last" | a_tty "$S" mqtt set >"$T/n1" 2>&1
    check "set: answer '$last' to the last question leaves it off" bash -c '[ "$1" -eq 0 ] && [ ! -e "$2" ] && [ ! -e "$3" ] && grep -q "Left as it was (MQTT off)" "$4"' _ $? "$CFG" "$PW" "$T/n1"
done
check "set without a yes: no set call reached the camera" bash -c '! grep -q "akam_mqtt.sh set" "$1"' _ "$STUB_DIR/ssh.log"

# set: yes, then no test
printf '192.0.2.10\n\nbob\n%s\ny\n\n' "$SECRET" | a_tty "$S" mqtt set >"$T/e1" 2>&1
check "set y: exit 0, settings written" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = "$(printf "enabled=1\nhost=192.0.2.10\nport=1883\nuser=bob")" ]' _ $? "$CFG"
check "set: the password file holds the password" bash -c '[ "$(cat "$1")" = "$2" ]' _ "$PW" "$SECRET"
check "set: password file mode 0600" bash -c '[ "$(ls -ld "$1" | cut -c1-10)" = "-rw-------" ]' _ "$PW"
check "set: the camera was told to reload" has "$CAM/ctl.log" "ctl MQTT reload"
check "set: warns about plain TCP" has "$T/e1" "plain TCP"
check "set: the password is not in the output" hasnt "$T/e1" "$SECRET"
check "set: the password never reaches ssh's command line" bash -c '! grep -q S3NTINEL "$1"' _ "$STUB_DIR/ssh.log"
check "set: the password is not in the log or the state" bash -c '! grep -rq S3NTINEL "$1"' _ "$S"
a "$S" mqtt status </dev/null >"$T/s1" 2>&1
check "status: enabled, broker, password set, state" bash -c 'grep -q "MQTT: enabled" "$1" && grep -q "broker: 192.0.2.10:1883, user bob" "$1" && grep -q "password: set" "$1" && grep -q "state: connected" "$1"' _ "$T/s1"

# test
a "$S" mqtt test </dev/null >"$T/t1" 2>&1
check "test: ok, exit 0" bash -c '[ "$1" -eq 0 ] && grep -q "MQTT test: ok" "$2"' _ $? "$T/t1"
printf '192.0.2.10\n1883\nbob\nreject-me-please\ny\nn\n' | a_tty "$S" mqtt set >/dev/null 2>&1
a "$S" mqtt test </dev/null >"$T/t2" 2>&1
check "test: a refused login fails with the reason" bash -c '[ "$1" -ne 0 ] && grep -q "refused the user name or password" "$2"' _ $? "$T/t2"
printf '192.0.2.10\n1883\nbob\n%s\ny\ny\n' "$SECRET" | a_tty "$S" mqtt set >"$T/e2" 2>&1
check "set then test now (y): the test ran and passed" bash -c 'grep -q "MQTT test: ok" "$1"' _ "$T/e2"

# no user: no password asked
printf 'broker.lan\n\n\ny\nn\n' | a_tty "$S" mqtt set >"$T/e3" 2>&1
check "no user: no password question, user none" bash -c '[ "$1" -eq 0 ] && ! grep -q "Broker password" "$2" && grep -q "user none, password none" "$2"' _ $? "$T/e3"

# validation: nothing sent
: >"$STUB_DIR/ssh.log"
for in in 'bad host\n' '-h\n' 'h\n0\n' 'h\n65536\n' 'h\nabc\n' 'h\n1883\nbad user\n' 'h\n1883\n-\n' 'h\n1883\nbob\nab\tc\n' '\n'; do
    # shellcheck disable=SC2059
    printf -- "$in"'y\n' | a_tty "$S" mqtt set >"$T/v" 2>&1
    check "invalid input refused: ${in//\\n/|}" test $? -ne 0
done
check "invalid input: nothing was sent to the camera" bash -c '! grep -q "akam_mqtt.sh set" "$1"' _ "$STUB_DIR/ssh.log"
for bad in "frob" "set x" "status x" "disable now" "forget x" "test x"; do
    # shellcheck disable=SC2086
    a "$S" mqtt $bad </dev/null >"$T/bad" 2>&1
    check "refused (exit 2): mqtt $bad" test $? -eq 2
done

# disable keeps the settings; forget asks (default No) and deletes
printf '192.0.2.10\n1883\nbob\n%s\ny\nn\n' "$SECRET" | a_tty "$S" mqtt set >/dev/null 2>&1
a "$S" mqtt disable </dev/null >"$T/d1" 2>&1
check "disable: no question, enabled=0, password kept" bash -c '[ "$1" -eq 0 ] && grep -qx "enabled=0" "$2" && [ -s "$3" ]' _ $? "$CFG" "$PW"
for last in "" "n" "q"; do
    printf '%s\n' "$last" | a_tty "$S" mqtt forget >"$T/f0" 2>&1
    check "forget: answer '$last' leaves the files" bash -c '[ "$1" -eq 0 ] && [ -s "$2" ] && [ -s "$3" ] && grep -q "Left as it was" "$4"' _ $? "$CFG" "$PW" "$T/f0"
done
printf 'y\n' | a_tty "$S" mqtt forget >"$T/f1" 2>&1
check "forget y: both files deleted" bash -c '[ "$1" -eq 0 ] && [ ! -e "$2" ] && [ ! -e "$3" ]' _ $? "$CFG" "$PW"

# older image, unreachable camera
printf '192.0.2.10\n1883\nbob\n%s\ny\n' "$SECRET" | STUB_OLD=1 a_tty "$S" mqtt set >"$T/old" 2>&1
check "old image: fails and says to update" bash -c '[ "$1" -ne 0 ] && grep -q "./akam update" "$2"' _ $? "$T/old"
check "old image: the password is not printed" hasnt "$T/old" "$SECRET"
STUB_SSH_RC=255 a "$S" mqtt disable </dev/null >"$T/down" 2>&1
check "camera unreachable: fails with a clear message" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2"' _ $? "$T/down"

# dry-run
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run mqtt set </dev/null >"$T/dry" 2>&1
check "dry-run set: WOULD lines, camera untouched" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD ask: Turn MQTT on with these settings? \[y/N\]" "$2" && grep -q "WOULD run over SSH on the camera: /usr/sbin/akam_mqtt.sh set <host> <port> <user> (password on stdin)" "$2" && [ ! -s "$3" ]' _ $? "$T/dry" "$STUB_DIR/ssh.log"

a "$S" help >"$T/help" 2>&1
check "help lists mqtt" has "$T/help" "mqtt [status | set | disable | forget | test]"

echo "wizard_mqtt_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
