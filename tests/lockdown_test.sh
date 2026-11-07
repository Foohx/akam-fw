#!/usr/bin/env bash
# Host-side tests for patch_service_sh.sh (T4 layer 1) and camyn_lockdown.sh
# (layer 2). Camera scripts run with $SH_UNDER_TEST (default: "busybox sh" if
# installed, else "sh"). `ps` is stubbed (PATH); `kill` is the real one, aimed
# at harmless `sleep` processes. Nothing here touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TPL=$REPO/tools/firmware_patch_templates
FIXTURES=$REPO/tests/fixtures
PATCH=$TPL/patch_service_sh.sh
LOCKDOWN=$TPL/camyn_lockdown.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${CAMYN_BUSYBOX:-}" ]; then SH_UNDER_TEST="$CAMYN_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
PASS=0
FAIL=0
cleanup() {
    local p
    for p in "$WORK"/pids/*; do
        [ -f "$p" ] && kill -9 "$(cat "$p")" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK/pids"

ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}
rc_is() { # rc_is <expected> <command...>
    local want=$1 got
    shift
    "$@" >/dev/null 2>&1
    got=$?
    [ "$got" -eq "$want" ]
}

# ---------------------------------------------------------------- patch tests
# active_matches <file>: non-comment text that still launches a daemon.
active_daemon_lines() {
    sed -e 's/#.*//' "$1" | grep -E '(^|[^A-Za-z0-9_])(utelnetd|telnetd|bftpd|vsftpd|ftpd)([^A-Za-z0-9_-]|$)'
}
dead() { ! alive "$1"; }
no_active_daemon() { ! active_daemon_lines "$1" >/dev/null; }
changed_lines() { # number of removed lines in the report diff
    grep -c '^-[^-]' "$1"
}
new_tree() { # new_tree <name> <fixture-dir>
    T=$WORK/$1
    rm -rf "$T"
    mkdir -p "$T"
    cp -a "$2/." "$T/"
    export CAMYN_PATCH_REPORT=$WORK/$1.diff
}

echo "== P1. patch applies to fixture and variants"
for spec in "app_fake:5" "app_fake_variants/a:6" "app_fake_variants/b:5" "app_fake_variants/c:4"; do
    name=${spec%%:*}
    want=${spec##*:}
    tag=$(echo "$name" | tr / _)
    new_tree "$tag" "$FIXTURES/$name"
    cp "$T/sbin/service.sh" "$WORK/$tag.orig"
    chmod 755 "$T/sbin/service.sh"
    check "$name: exit 0" "$PATCH" "$T" >/dev/null 2>&1
    check "$name: no active daemon launch left" no_active_daemon "$T/sbin/service.sh"
    check "$name: bash -n" bash -n "$T/sbin/service.sh"
    check "$name: sh -n" "${SHCMD[@]}" -n "$T/sbin/service.sh"
    check "$name: exactly $want lines changed" test "$(changed_lines "$CAMYN_PATCH_REPORT")" = "$want"
    check "$name: line count unchanged" test "$(wc -l <"$T/sbin/service.sh")" = "$(wc -l <"$WORK/$tag.orig")"
    check "$name: file mode preserved" test "$(stat -c %a "$T/sbin/service.sh")" = 755
    check "$name: hook guarded by flag" grep -q 'CAMYN_MAINTENANCE' "$T/sbin/service.sh"
    # idempotent
    cp "$T/sbin/service.sh" "$WORK/$tag.once"
    check "$name: 2nd pass exit 0" "$PATCH" "$T" >/dev/null 2>&1
    check "$name: 2nd pass no diff" cmp -s "$T/sbin/service.sh" "$WORK/$tag.once"
done
check "variant c: comment line untouched" grep -q '^# telnetd is started below' "$WORK/app_fake_variants_c/sbin/service.sh"
check "variant b: one-liner if body still valid" grep -q ': # CAMYN-DISABLED:.*bftpd' "$WORK/app_fake_variants_b/sbin/service.sh"

echo "== P2. patched script behaves: nothing starts without the flag"
FAKE=$WORK/behav
mkdir -p "$FAKE/mnt" "$FAKE/bin" "$FAKE/app/sbin"
sed -e "s#/mnt/#$FAKE/mnt/#g" "$FIXTURES/app_fake_variants/a/sbin/service.sh" >"$FAKE/app/sbin/service.sh"
for b in telnetd utelnetd tcpsvd; do
    # shellcheck disable=SC2016
    printf '#!/bin/sh\necho "$0 $*" >>"%s/launched"\n' "$FAKE" >"$FAKE/bin/$b"
    chmod 755 "$FAKE/bin/$b"
done
printf 'echo hook-ran >>"%s/hook_ran"\n' "$FAKE" >"$FAKE/mnt/hack.sh"
printf '#!/bin/sh\necho hostapd-ran >>"%s/hook_ran"\n' "$FAKE" >"$FAKE/mnt/hostapd"
chmod 755 "$FAKE/mnt/hostapd"
: >"$FAKE/mnt/_ht_ap_mode.conf"
CAMYN_MAINT_FLAG=$FAKE/mnt/CAMYN_MAINTENANCE CAMYN_PATCH_REPORT=$WORK/behav.diff "$PATCH" "$FAKE/app" >/dev/null 2>&1
run_service() { PATH="$FAKE/bin:$PATH" "${SHCMD[@]}" "$FAKE/app/sbin/service.sh" start >/dev/null 2>&1; sleep 0.3; }
run_service
check "no flag: no telnetd/ftpd launched" test ! -e "$FAKE/launched"
check "no flag: SD hook (hack.sh/hostapd) not run" test ! -e "$FAKE/hook_ran"
: >"$FAKE/mnt/CAMYN_MAINTENANCE"
run_service
check "flag: SD hook runs (maintenance)" test -s "$FAKE/hook_ran"
check "flag: telnetd/ftpd still not launched by service.sh" test ! -e "$FAKE/launched"

echo "== P3. patch fails loudly on unexpected input"
mkdir -p "$WORK/n1/sbin"
check "service.sh missing: exit 1" rc_is 1 "$PATCH" "$WORK/n1"
check "no argument: exit 1" rc_is 1 "$PATCH"
printf '#!/bin/sh\necho hello\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "no pattern at all: exit 3" rc_is 3 "$PATCH" "$WORK/n1"
check "no pattern: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "no pattern: warning on stderr" bash -c "'$PATCH' '$WORK/n1' 2>&1 >/dev/null | grep -q WARNING"
printf '#!/bin/sh\ntelnetd -p 23 &\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "daemon but no SD hook: exit 0" rc_is 0 "$PATCH" "$WORK/n1"
check "daemon but no SD hook: daemon line disabled" no_active_daemon "$WORK/n1/sbin/service.sh"
check "daemon but no SD hook: syntax ok" bash -n "$WORK/n1/sbin/service.sh"
printf '#!/bin/sh\ntelnetd -p 23 &\n' >"$WORK/n1/sbin/service.sh"
check "daemon but no SD hook: WARNING on stderr" bash -c "'$PATCH' '$WORK/n1' 2>&1 >/dev/null | grep -q 'no SD hook'"
printf '#!/bin/sh\nsh /mnt/hack.sh\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "hook but no daemon line: exit 3" rc_is 3 "$PATCH" "$WORK/n1"
check "hook but no daemon line: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
printf '#!/bin/sh\ntelnetd -p 23 \\\n  -l /bin/sh &\n[ -f /mnt/hack.sh ] && sh /mnt/hack.sh\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "continuation line: exit 2" rc_is 2 "$PATCH" "$WORK/n1"
check "continuation line: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
printf '#!/bin/sh\nif [ -x /usr/sbin/ftpd ]; then\n  ftpd &\nfi\nsh /mnt/hack.sh\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "condition naming a daemon (would break syntax): exit 2" rc_is 2 "$PATCH" "$WORK/n1"
check "syntax failure: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
printf '#!/bin/sh\ntelnetd &\nsync; sh /mnt/hack.sh\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "compound hook line: exit 2" rc_is 2 "$PATCH" "$WORK/n1"
check "compound hook: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
printf '#!/bin/sh\ntelnetd &\nif [ -f /mnt/_ht_ap_mode.conf ] || [ -f /mnt/x ]; then\n  :\nfi\n' >"$WORK/n1/sbin/service.sh"
cp "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "hook condition with ||: exit 2" rc_is 2 "$PATCH" "$WORK/n1"
check "hook condition with ||: file untouched" cmp -s "$WORK/n1/sbin/service.sh" "$WORK/n1.orig"
check "relative CAMYN_MAINT_FLAG rejected" bash -c "CAMYN_MAINT_FLAG=mnt/x '$PATCH' '$FIXTURES/app_fake' >/dev/null 2>&1; [ \$? -eq 1 ]"

# ------------------------------------------------------------- lockdown tests
STUBS=$WORK/stubs
mkdir -p "$STUBS"
cat >"$STUBS/ps" <<'EOF'
#!/bin/sh
echo "  PID USER       VSZ STAT COMMAND"
cat "$REG"
EOF
chmod 755 "$STUBS/ps"

alive() {
    local st
    st=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null) || return 1
    [ -n "$st" ] && [ "$st" != Z ]
}
# spawn <name> <ps-command-line> [ignore-term]: start a sleeper, register it
# in the fake ps table, remember its pid in $WORK/pids/<name>.
spawn() {
    local pid
    if [ "${3:-}" = ignore-term ]; then
        bash -c 'trap "" TERM; while :; do sleep 0.2; done' >/dev/null 2>&1 &
    else
        sleep 600 >/dev/null 2>&1 &
    fi
    pid=$!
    echo "$pid" >"$WORK/pids/$1"
    printf '%5d root      1234 S    %s\n' "$pid" "$2" >>"$REG"
}
pid_of() { cat "$WORK/pids/$1"; }
LD_ENV=(CAMYN_FAST_ROUNDS=3 CAMYN_FAST_PERIOD=0 CAMYN_SLOW_ROUNDS=2 CAMYN_SLOW_PERIOD=0)
run_lockdown() { # run_lockdown <root>
    env "CAMYN_ROOT=$1" "REG=$REG" "PATH=$STUBS:$PATH" "${LD_ENV[@]}" timeout 30 "${SHCMD[@]}" "$LOCKDOWN"
}
new_ld_root() {
    LR=$WORK/$1
    mkdir -p "$LR/tmp" "$LR/mnt"
    REG=$LR/pstable
    : >"$REG"
    rm -f "$WORK"/pids/*
}

echo "== L1. kills telnet/ftp, spares the rest"
new_ld_root l1
spawn telnetd "/usr/sbin/telnetd -p 24 -l /bin/sh"
spawn utelnetd "utelnetd -d -p 23"
spawn ftpd "tcpsvd 0 21 ftpd -w /mnt"
spawn bftpd "/usr/sbin/bftpd -d"
spawn respawn "sh /mnt/telnet.sh"
spawn stubborn "telnetd -p 2323" ignore-term
spawn ak_rtsp "/usr/bin/ak_rtsp"
spawn dropbear "dropbear -r /etc/config/dropbear/k -p 22 -P /tmp/dropbear.pid"
spawn dbtrick "dropbear -p 22 telnetd"
spawn wpa "wpa_supplicant -Dnl80211 -i wlan0 -c /etc/config/wpa_supplicant.conf -B"
spawn cmdserver "/usr/bin/cmdserver"
spawn wrapper "/bin/sh /usr/bin/ak_rtsp_wrapper.sh"
spawn other "udhcpc -i wlan0"
spawn ttrick "sh /tmp/telnetd_notes.sh"
sleep 0.2
check "lockdown exits 0 (terminates)" run_lockdown "$LR"
sleep 0.3
for n in telnetd utelnetd ftpd bftpd respawn stubborn; do
    check "$n killed" dead "$(pid_of "$n")"
done
for n in ak_rtsp dropbear dbtrick wpa cmdserver wrapper other ttrick; do
    check "$n untouched" alive "$(pid_of "$n")"
done
check "log records kills" grep -q 'SIGTERM pid' "$LR/tmp/camyn_lockdown.log"
check "log records SIGKILL escalation" grep -q 'SIGKILL pid' "$LR/tmp/camyn_lockdown.log"
check "lock released" test ! -d "$LR/tmp/camyn_lockdown.lock"

echo "== L2. maintenance flag: nothing killed, loop goes on"
new_ld_root l2
spawn telnetd "telnetd -p 24 -l /bin/sh"
spawn ftpd "tcpsvd 0 21 ftpd -w /mnt"
: >"$LR/mnt/CAMYN_MAINTENANCE"
check "bounded loop exits 0" run_lockdown "$LR"
check "telnetd alive" alive "$(pid_of telnetd)"
check "ftpd alive" alive "$(pid_of ftpd)"
check "log says maintenance mode" grep -q 'maintenance mode: telnet not touched' "$LR/tmp/camyn_lockdown.log"
check "loop ran to its end" grep -q 'bounded loop finished' "$LR/tmp/camyn_lockdown.log"

echo "== L3. flag present then removed: killing resumes, no exit in between"
new_ld_root l3
LD_ENV=(CAMYN_FAST_ROUNDS=50 CAMYN_FAST_PERIOD=1 CAMYN_SLOW_ROUNDS=0 CAMYN_SLOW_PERIOD=1)
: >"$LR/mnt/CAMYN_MAINTENANCE"
spawn telnetd "telnetd -p 24"
(run_lockdown "$LR" >/dev/null 2>&1 &)
sleep 2.5
check "telnetd alive while flag present" alive "$(pid_of telnetd)"
check "loop still running (lock held)" test -d "$LR/tmp/camyn_lockdown.lock"
rm -f "$LR/mnt/CAMYN_MAINTENANCE"
sleep 3
check "telnetd killed after flag removal" dead "$(pid_of telnetd)"
check "log says lockdown resumed" grep -q 'lockdown resumed' "$LR/tmp/camyn_lockdown.log"
kill "$(cat "$LR/tmp/camyn_lockdown.lock/pid")" 2>/dev/null
LD_ENV=(CAMYN_FAST_ROUNDS=3 CAMYN_FAST_PERIOD=0 CAMYN_SLOW_ROUNDS=2 CAMYN_SLOW_PERIOD=0)

echo "== L3b. slow loop relaunches camyn_ssh.sh start, detached"
new_ld_root l3b
mkdir -p "$LR/usr/sbin"
cat >"$LR/usr/sbin/camyn_ssh.sh" <<'EOF'
#!/bin/sh
echo "$*" >>"$CAMYN_ROOT/tmp/ssh_calls"
sleep 20
EOF
chmod 755 "$LR/usr/sbin/camyn_ssh.sh"
start=$(date +%s)
check "lockdown exits 0 even with a slow helper" run_lockdown "$LR"
check "loop not blocked by helper" test $(($(date +%s) - start)) -lt 10
sleep 0.5
check "helper called with start" grep -q '^start$' "$LR/tmp/ssh_calls"
check "helper called once per slow round" test "$(wc -l <"$LR/tmp/ssh_calls")" = 2

echo "== L4. late telnetd killed during the loop, then bounded exit"
new_ld_root l4
LD_ENV=(CAMYN_FAST_ROUNDS=6 CAMYN_FAST_PERIOD=1 CAMYN_SLOW_ROUNDS=0 CAMYN_SLOW_PERIOD=1)
(run_lockdown "$LR" >/dev/null 2>&1 &)
sleep 1.5
spawn telnetd "telnetd -p 24"
sleep 3
check "late telnetd killed" dead "$(pid_of telnetd)"
sleep 3.5
check "loop ended by itself (lock released)" test ! -d "$LR/tmp/camyn_lockdown.lock"
check "log says bounded loop finished" grep -q 'bounded loop finished' "$LR/tmp/camyn_lockdown.log"
LD_ENV=(CAMYN_FAST_ROUNDS=3 CAMYN_FAST_PERIOD=0 CAMYN_SLOW_ROUNDS=2 CAMYN_SLOW_PERIOD=0)

echo "== L5. anti-duplicate lock and stale lock"
new_ld_root l5
spawn telnetd "telnetd -p 24"
spawn holder "sleep 600"
mkdir "$LR/tmp/camyn_lockdown.lock"
pid_of holder >"$LR/tmp/camyn_lockdown.lock/pid"
check "live lock: exits 0" run_lockdown "$LR"
check "live lock: second instance did nothing" alive "$(pid_of telnetd)"
check "live lock: lock kept for holder" test -d "$LR/tmp/camyn_lockdown.lock"
echo 999999 >"$LR/tmp/camyn_lockdown.lock/pid"
check "stale lock: exits 0" run_lockdown "$LR"
check "stale lock: taken over, telnetd killed" dead "$(pid_of telnetd)"

echo "== L6. robustness"
new_ld_root l6
rm -f "$STUBS/ps"
check "no ps available: exits 0" run_lockdown "$LR"

echo
echo "passed: $PASS  failed: $FAIL  (shell under test: $SH_UNDER_TEST)"
[ "$FAIL" -eq 0 ]
