#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam onvif (enable [PORT], disable, status) against a fake ssh that runs the command line the
# wizard sends: the real akam_onvif.sh and akam_rtsp_auth.sh in a fake camera root, with a fake `ak_rtsp ctl`.
# The real camera is never contacted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
HELPER="$ROOT/tools/firmware_patch_templates/akam_onvif.sh"
AUTH="$ROOT/tools/firmware_patch_templates/akam_rtsp_auth.sh"

T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { local n=$1; shift; if "$@"; then ok "$n"; else ko "$n"; fi; }
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

export HOME=$T/home NO_COLOR=1
mkdir -p "$HOME"
unset AKAM_STATE_DIR AKAM_SSH

export STUB_DIR=$T/stub CAM=$T/cam HELPER AUTH
mkdir -p "$STUB_DIR" "$CAM/root/etc/config"
FLAG=$CAM/root/etc/config/akam/onvif
cat >"$STUB_DIR/ak_rtsp" <<'EOF'
#!/bin/sh
echo "$*" >>"$CAM/ctl.log"
case "$2 $3" in
    "ONVIF reload") echo "OK onvif reloaded" ;;
    "GET onvif.state") echo "onvif.state=listening" ;;
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
    "/usr/sbin/akam_onvif.sh "*)
        [ -z "${STUB_OLD:-}" ] || { echo "sh: /usr/sbin/akam_onvif.sh: not found"; exit 127; }
        # shellcheck disable=SC2086
        exec sh "$HELPER" ${cmd#/usr/sbin/akam_onvif.sh } ;;
    "/usr/sbin/akam_rtsp_auth.sh "*)
        # shellcheck disable=SC2086
        exec sh "$AUTH" ${cmd#/usr/sbin/akam_rtsp_auth.sh } ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
a_tty() { local st=$1; shift; AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
S=$(new_state o)

a "$S" onvif </dev/null >"$T/o1" 2>&1
check "onvif (default status): disabled, exit 0" bash -c '[ "$1" -eq 0 ] && grep -q "ONVIF: disabled" "$2"' _ $? "$T/o1"
a "$S" onvif status </dev/null >"$T/o2" 2>&1
check "onvif status: shows the camera's state" has "$T/o2" "state: listening"

# enable: asks, default No, never without a terminal
: >"$STUB_DIR/ssh.log"
a "$S" onvif enable </dev/null >"$T/n0" 2>&1
check "enable: refused without a terminal, nothing written" bash -c '[ "$1" -ne 0 ] && [ ! -e "$2" ]' _ $? "$FLAG"
check "enable refused without a terminal: no enable call" bash -c '! grep -q "akam_onvif.sh enable" "$1"' _ "$STUB_DIR/ssh.log"
printf 'n\n' | a_tty "$S" onvif enable >"$T/n1" 2>&1
check "enable: answering n leaves it off" bash -c '[ "$1" -eq 0 ] && [ ! -e "$2" ] && grep -q "Left as it was" "$3"' _ $? "$FLAG" "$T/n1"
printf '\n' | a_tty "$S" onvif enable >"$T/n2" 2>&1
check "enable: an empty answer is No" test ! -e "$FLAG"

# enable without a password: the camera refuses and says what to do
printf 'y\n' | a_tty "$S" onvif enable >"$T/e0" 2>&1
check "enable without a stream password: fails, points to rtsp-auth set" bash -c '[ "$1" -ne 0 ] && grep -q "./akam rtsp-auth set" "$2" && [ ! -e "$3" ]' _ $? "$T/e0" "$FLAG"

# set a password through the wizard (real rtsp-auth helper in the fake root), then enable
printf 'viewer\nS3NTINEL-pw-1\nS3NTINEL-pw-1\n' | a_tty "$S" rtsp-auth set >"$T/pw" 2>&1
check "rtsp-auth set wrote the credentials file" test -s "$CAM/root/etc/config/akam/credentials"
printf 'y\n' | a_tty "$S" onvif enable >"$T/e1" 2>&1
check "enable y: exit 0, flag enabled=1 port=80" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = "$(printf "enabled=1\nport=80")" ]' _ $? "$FLAG"
check "enable: the camera was told to reload" has "$CAM/ctl.log" "ctl ONVIF reload"
check "enable: says how to add the camera in an app" has "$T/e1" "add the camera by its address"
check "enable: the password is not in the output" hasnt "$T/e1" S3NTINEL
check "enable: the password never reaches ssh's command line" bash -c '! grep -q S3NTINEL "$1"' _ "$STUB_DIR/ssh.log"
a "$S" onvif status </dev/null >"$T/s1" 2>&1
check "status: enabled, port 80" has "$T/s1" "ONVIF: enabled, port 80"
printf 'y\n' | a_tty "$S" onvif enable 8080 >"$T/e2" 2>&1
check "enable 8080: flag port=8080" bash -c '[ "$1" -eq 0 ] && grep -qx "port=8080" "$2"' _ $? "$FLAG"

# disable and status never ask
a "$S" onvif disable </dev/null >"$T/d1" 2>&1
check "disable: no question, flag enabled=0" bash -c '[ "$1" -eq 0 ] && grep -qx "enabled=0" "$2"' _ $? "$FLAG"
check "disable: says the ports are closed again" has "$T/d1" "ONVIF is off"

# bad arguments: exit 2, camera not contacted
: >"$STUB_DIR/ssh.log"
for bad in "enable 0" "enable 65536" "enable abc" "enable 80 81" "enable -1" "disable 1" "status 1" "maybe" "enable 80;reboot"; do
    # shellcheck disable=SC2086
    a "$S" onvif $bad </dev/null >"$T/bad" 2>&1
    check "refused (exit 2): onvif $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"

# older image without the helper
printf 'y\n' | STUB_OLD=1 a_tty "$S" onvif enable >"$T/old" 2>&1
check "old image: fails and says to update" bash -c '[ "$1" -ne 0 ] && grep -q "./akam update" "$2"' _ $? "$T/old"
STUB_SSH_RC=255 a "$S" onvif disable </dev/null >"$T/down" 2>&1
check "camera unreachable: fails with a clear message" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2"' _ $? "$T/down"

# dry-run
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run onvif enable 8080 </dev/null >"$T/dry" 2>&1
check "dry-run: WOULD lines (question and command), camera untouched" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD ask: Make the camera visible to ONVIF apps (NVRs, ONVIF Device Manager)? \[y/N\]" "$2" && grep -q "WOULD run over SSH on the camera: /usr/sbin/akam_onvif.sh enable 8080" "$2" && [ ! -s "$3" ]' _ $? "$T/dry" "$STUB_DIR/ssh.log"

a "$S" help >"$T/help" 2>&1
check "help lists onvif" has "$T/help" "onvif [enable [PORT] | disable | status]"

echo "wizard_onvif_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
