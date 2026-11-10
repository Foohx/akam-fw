#!/usr/bin/env bash
# Host tests for akam_onvif.sh (camera script, run under busybox sh when available) in a fake root
# ($AKAM_ROOT). A fake `ak_rtsp` plays "ak_rtsp ctl <line>". Nothing touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
ONV=$REPO/tools/firmware_patch_templates/akam_onvif.sh

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
perm() { ls -ld "$1" | cut -c1-10; }
exists() { if [ -e "$1" ]; then echo yes; else echo no; fi; }

# fake ak_rtsp: FAKE_MODE = new (answers), old (ERR unknown command), dead (connection refused)
cat >"$WORK/ak_rtsp" <<'EOF2'
#!/bin/sh
echo "$*" >>"$FAKE_LOG"
[ "$1" = ctl ] || exit 9
case "${FAKE_MODE:-new}" in
    dead) echo "ctl: connect 127.0.0.1: Connection refused" >&2; exit 1 ;;
    old) echo "ERR unknown command"; exit 1 ;;
esac
case "$2 $3" in
    "ONVIF reload") echo "OK onvif reloaded" ;;
    "GET onvif.state") echo "onvif.state=listening" ;;
    *) echo "ERR unknown command"; exit 1 ;;
esac
EOF2
chmod +x "$WORK/ak_rtsp"
export FAKE_LOG=$WORK/log AKAM_RTSP_BIN=$WORK/ak_rtsp
R=$WORK/root
export AKAM_ROOT=$R
FLAG=$R/etc/config/akam/onvif
CRED=$R/etc/config/akam/credentials
mkdir -p "$R/etc/config"
run() { "${SHCMD[@]}" "$ONV" "$@" 2>&1; }
good_creds() { mkdir -p "$R/etc/config/akam"; chmod 700 "$R/etc/config/akam"; printf 'user=viewer\npass=S3cret-pass-1\n' >"$CRED"; chmod 600 "$CRED"; }

# --- status with nothing
out=$(run status); rc=$?
eq "status, no flag: exit 0" "$rc" 0
contains "status, no flag: disabled" "$out" "ONVIF: disabled (default)"
contains "status: no password" "$out" "password: no password set"
contains "status: ak_rtsp state shown" "$out" "state: listening"

# --- enable refused without credentials
: >"$FAKE_LOG"
out=$(run enable); rc=$?
eq "enable without credentials: exit 1" "$rc" 1
contains "enable without credentials: says rtsp-auth set" "$out" "./akam rtsp-auth set"
eq "enable without credentials: no flag written" "$(exists "$FLAG")" no
eq "enable without credentials: no reload sent" "$(cat "$FAKE_LOG")" ""

# --- invalid credentials files are refused too
mkdir -p "$R/etc/config/akam"
for bad in 'garbage' 'user=viewer\npass=short\n' 'user=bad name\npass=S3cret-pass-1\n' 'user=viewer\npass=S3cret-pass-1\nextra=1\n' 'pass=S3cret-pass-1\nuser=viewer\n'; do
    # shellcheck disable=SC2059
    printf "$bad" >"$CRED"
    chmod 600 "$CRED"
    run enable >/dev/null
    rc=$?
    eq "enable refused with credentials '${bad//\\n/|}': exit 1" "$rc" 1
done
printf 'user=viewer\npass=S3cret-pass-1\n' >"$CRED"
chmod 644 "$CRED"
out=$(run enable); rc=$?
eq "enable refused when the credentials file is world-readable" "$rc" 1
contains "...with a clear reason" "$out" "not private"
eq "no flag after refusals" "$(exists "$FLAG")" no

# --- enable
good_creds
: >"$FAKE_LOG"
out=$(run enable); rc=$?
eq "enable: exit 0" "$rc" 0
eq "enable: file content (default port 80)" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=80')"
eq "enable: file mode 0600" "$(perm "$FLAG")" "-rw-------"
eq "enable: directory mode 0700" "$(ls -ld "$R/etc/config/akam" | cut -c1-10)" "drwx------"
eq "enable: reload sent" "$(cat "$FAKE_LOG")" "ctl ONVIF reload"
contains "enable: says applied" "$out" "Applied now"
eq "enable: no temp file left" "$(ls "$R/etc/config/akam" | sort | paste -sd' ' -)" "credentials onvif"
case $out in *S3cret*) ko "enable: the password is not printed" ;; *) ok "enable: the password is not printed" ;; esac

# --- status after enable
out=$(run status); rc=$?
eq "status enabled: exit 0" "$rc" 0
contains "status enabled: text" "$out" "ONVIF: enabled, port 80"
contains "status: password set" "$out" "password: set"

# --- port argument
out=$(run enable 8080); rc=$?
eq "enable 8080: exit 0" "$rc" 0
eq "enable 8080: file" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=8080')"
out=$(run enable)
eq "enable again keeps the saved port" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=8080')"
for badp in 0 65536 abc -1 '' 123456 80x; do
    out=$(run enable "$badp"); rc=$?
    eq "enable '$badp': usage error 2" "$rc" 2
done
for resp in 21 22 23 24 554 8091; do
    out=$(run enable "$resp"); rc=$?
    eq "enable $resp (used by another service): usage error 2" "$rc" 2
done
out=$(run enable 1 2); rc=$?
eq "enable with two arguments: 2" "$rc" 2
out=$(run enable 65535)
eq "enable 65535 accepted" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=65535')"
run enable 80 >/dev/null

# --- disable
: >"$FAKE_LOG"
out=$(run disable); rc=$?
eq "disable: exit 0" "$rc" 0
eq "disable: file content" "$(cat "$FLAG")" "$(printf 'enabled=0\nport=80')"
eq "disable: mode 0600" "$(perm "$FLAG")" "-rw-------"
eq "disable: reload sent" "$(cat "$FAKE_LOG")" "ctl ONVIF reload"
out=$(run status)
contains "status after disable" "$out" "ONVIF: disabled, port 80"
rm -f "$FLAG"
: >"$FAKE_LOG"
out=$(run disable); rc=$?
eq "disable with no flag: exit 0" "$rc" 0
contains "disable with no flag: already disabled" "$out" "already disabled"
eq "disable with no flag: no file created" "$(exists "$FLAG")" no

# --- older firmware: ERR unknown command -> reboot to apply, still exit 0, flag saved
export FAKE_MODE=old
out=$(run enable); rc=$?
eq "old firmware: enable exit 0" "$rc" 0
contains "old firmware: reboot to apply" "$out" "reboot the camera to apply"
eq "old firmware: flag saved" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=80')"
out=$(run status)
contains "old firmware: state unknown" "$out" "state: unknown"
export FAKE_MODE=dead
out=$(run disable); rc=$?
eq "ak_rtsp down: disable exit 0" "$rc" 0
contains "ak_rtsp down: applies at next start" "$out" "next time ak_rtsp starts"
unset FAKE_MODE

# --- invalid flag file
printf 'enabled=2\nport=80\n' >"$FLAG"
out=$(run status); rc=$?
eq "invalid flag: status exit 3" "$rc" 3
contains "invalid flag: INVALID shown" "$out" "INVALID"
for bad in 'enabled=1\n' 'port=80\nenabled=1\nx=1\n' 'enabled=1\nport=0\n' 'enabled=1\nport=99999\n' 'enabled=1\nenabled=1\n'; do
    # shellcheck disable=SC2059
    printf "$bad" >"$FLAG"
    run status >/dev/null
    rc=$?
    eq "invalid flag '${bad//\\n/|}': status 3" "$rc" 3
done
printf 'enabled=1\r\nport=80\r\n' >"$FLAG"
run status >/dev/null
rc=$?
eq "CRLF flag file tolerated" "$rc" 0
printf 'junk\n' >"$FLAG"
run enable >/dev/null
eq "enable over an invalid file: clean content" "$(cat "$FLAG")" "$(printf 'enabled=1\nport=80')"

# --- usage
for u in "" bogus "disable now" "status x" "on"; do
    # shellcheck disable=SC2086
    run $u >/dev/null
    rc=$?
    eq "usage '$u': exit 2" "$rc" 2
done

echo "akam_onvif_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
