#!/usr/bin/env bash
# Host-side tests for patch_service_sh.sh (T4 layer 1) and akam_lockdown.sh
# (layer 2). Camera scripts run with $SH_UNDER_TEST (default: "busybox sh" if
# installed, else "sh"). `ps` is stubbed (PATH); `kill` is the real one, aimed
# at harmless `sleep` processes. Nothing here touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TPL=$REPO/tools/firmware_patch_templates
FIXTURES=$REPO/tests/fixtures
PATCH=$TPL/patch_service_sh.sh
LOCKDOWN=$TPL/akam_lockdown.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
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
    export AKAM_PATCH_REPORT=$WORK/$1.diff
}

echo "== P1. patch applies to fixture and variants"
for spec in "app_fake:5" "app_fake_variants/a:6" "app_fake_variants/b:5" "app_fake_variants/c:4" "app_fake_variants/d:3"; do
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
    check "$name: exactly $want lines changed" test "$(changed_lines "$AKAM_PATCH_REPORT")" = "$want"
    check "$name: exactly one line added (RAM marker)" test "$(wc -l <"$T/sbin/service.sh")" = "$(( $(wc -l <"$WORK/$tag.orig") + 1 ))"
    check "$name: one RAM marker line" test "$(grep -c ': > /tmp/akam_maintenance' "$T/sbin/service.sh")" = 1
    check "$name: marker precedes the first guarded hook" test "$(grep -n 'akam_maintenance' "$T/sbin/service.sh" | head -1 | cut -d: -f1)" -lt "$(grep -n 'if { \[ -f /mnt/AKAM_MAINTENANCE \]\|^[[:space:]]*{ \[ -f /mnt/AKAM_MAINTENANCE \] || \[ -f /mnt/CAMYN_MAINTENANCE \]; } && [^:]' "$T/sbin/service.sh" | head -1 | cut -d: -f1)"
    check "$name: file mode preserved" test "$(stat -c %a "$T/sbin/service.sh")" = 755
    check "$name: hook guarded by flag" grep -q 'AKAM_MAINTENANCE' "$T/sbin/service.sh"
    # idempotent
    cp "$T/sbin/service.sh" "$WORK/$tag.once"
    check "$name: 2nd pass exit 0" "$PATCH" "$T" >/dev/null 2>&1
    check "$name: 2nd pass no diff" cmp -s "$T/sbin/service.sh" "$WORK/$tag.once"
done
check "variant c: comment line untouched" grep -q '^# telnetd is started below' "$WORK/app_fake_variants_c/sbin/service.sh"
check "variant b: one-liner if body still valid" grep -q ': # AKAM-DISABLED:.*bftpd' "$WORK/app_fake_variants_b/sbin/service.sh"

check "variant d: assignment line untouched" grep -q '^AP_MODE_FILE=_ht_ap_mode.conf$' "$WORK/app_fake_variants_d/sbin/service.sh"
check "variant d: dd of the AP mode file guarded" grep -q '^	{ \[ -f /mnt/AKAM_MAINTENANCE \] || \[ -f /mnt/CAMYN_MAINTENANCE \]; } && dd if=' "$WORK/app_fake_variants_d/sbin/service.sh"
check "variant d: tf_update run guarded by flag" grep -q '^	{ \[ -f /mnt/AKAM_MAINTENANCE \] || \[ -f /mnt/CAMYN_MAINTENANCE \]; } && sh /tmp/tf_update.sh$' "$WORK/app_fake_variants_d/sbin/service.sh"
check "variant d: tf_update cp/rm untouched" grep -q '^	cp /usr/sbin/tf_update.sh /tmp/$' "$WORK/app_fake_variants_d/sbin/service.sh"
mkdir -p "$WORK/vd"
cp -a "$FIXTURES/app_fake_variants/d/." "$WORK/vd/"
check "variant d: NOTE about commented-only daemons" bash -c "AKAM_PATCH_REPORT='$WORK/vd.diff' '$PATCH' '$WORK/vd' 2>&1 | grep -q 'commented mention'"

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
AKAM_MAINT_FLAG=$FAKE/mnt/AKAM_MAINTENANCE AKAM_PATCH_REPORT=$WORK/behav.diff "$PATCH" "$FAKE/app" >/dev/null 2>&1
run_service() { PATH="$FAKE/bin:$PATH" "${SHCMD[@]}" "$FAKE/app/sbin/service.sh" start >/dev/null 2>&1; sleep 0.3; }
run_service
check "no flag: no telnetd/ftpd launched" test ! -e "$FAKE/launched"
check "no flag: SD hook (hack.sh/hostapd) not run" test ! -e "$FAKE/hook_ran"
: >"$FAKE/mnt/AKAM_MAINTENANCE"
run_service
check "flag: SD hook runs (maintenance)" test -s "$FAKE/hook_ran"
check "flag: telnetd/ftpd still not launched by service.sh" test ! -e "$FAKE/launched"

echo "== P2b. tf_update (SD firmware flash) only runs with the flag"
FU=$WORK/tfu
mkdir -p "$FU/mnt" "$FU/tmp"
sed -e "s#/mnt/#$FU/mnt/#g" -e "s#/tmp/tf_update.sh#$FU/tmp/tf_update.sh#g" \
    "$FIXTURES/app_fake_variants/d/sbin/service.sh" >"$FU/service.sh"
printf '#!/bin/sh\necho flashed >>"%s/flashed"\n' "$FU" >"$FU/tmp/tf_update.sh"
AKAM_MAINT_FLAG=$FU/mnt/AKAM_MAINTENANCE AKAM_PATCH_REPORT=$WORK/tfu.diff "$PATCH" "$FU" >/dev/null 2>&1 || true
mkdir -p "$FU/sbin"; cp "$FU/service.sh" "$FU/sbin/service.sh"
AKAM_MAINT_FLAG=$FU/mnt/AKAM_MAINTENANCE AKAM_PATCH_REPORT=$WORK/tfu.diff "$PATCH" "$FU" >/dev/null 2>&1
sed -e "s#^\(.*\)sh /usr/sbin/tf_mount.sh#\1:#" -e "s#cp /usr/sbin/tf_update.sh .*#:#" -e 's#^\(\s*\)rm .*#\1:#' "$FU/sbin/service.sh" >"$FU/run.sh"
"${SHCMD[@]}" "$FU/run.sh" >/dev/null 2>&1
check "no flag: tf_update not run" test ! -e "$FU/flashed"
: >"$FU/mnt/AKAM_MAINTENANCE"
"${SHCMD[@]}" "$FU/run.sh" >/dev/null 2>&1
check "flag: tf_update runs (maintenance)" test -s "$FU/flashed"

echo "== P2c. RAM marker is written while the SD is mounted, survives the unmount"
FM=$WORK/mark
mkdir -p "$FM/mnt" "$FM/tmp" "$FM/sbin"
sed -e "s#/mnt/#$FM/mnt/#g" -e "s#/tmp/tf_update.sh#$FM/tmp/tf_update.sh#g" \
    "$FIXTURES/app_fake_variants/d/sbin/service.sh" >"$FM/sbin/service.sh"
AKAM_MAINT_FLAG=$FM/mnt/AKAM_MAINTENANCE AKAM_MAINT_MARK=$FM/tmp/akam_maintenance AKAM_PATCH_REPORT=$WORK/mark.diff "$PATCH" "$FM" >/dev/null 2>&1
sed -e "s#^\(.*\)sh /usr/sbin/tf_mount.sh#\1:#" -e "s#cp /usr/sbin/tf_update.sh .*#:#" -e 's#^\(\s*\)rm .*#\1:#' "$FM/sbin/service.sh" >"$FM/run.sh"
"${SHCMD[@]}" "$FM/run.sh" >/dev/null 2>&1
check "no flag: no RAM marker" test ! -e "$FM/tmp/akam_maintenance"
: >"$FM/mnt/AKAM_MAINTENANCE"
"${SHCMD[@]}" "$FM/run.sh" >/dev/null 2>&1
check "flag: RAM marker written" test -e "$FM/tmp/akam_maintenance"
rm -f "$FM/mnt/AKAM_MAINTENANCE"
check "marker survives the SD removal" test -e "$FM/tmp/akam_maintenance"
# Already patched by an older version (no marker): the 2nd pass adds it once.
grep -v 'akam_maintenance' "$FM/sbin/service.sh" >"$FM/old.sh" && cp "$FM/old.sh" "$FM/sbin/service.sh"
AKAM_MAINT_FLAG=$FM/mnt/AKAM_MAINTENANCE AKAM_MAINT_MARK=$FM/tmp/akam_maintenance AKAM_PATCH_REPORT=$WORK/mark.diff "$PATCH" "$FM" >/dev/null 2>&1
check "old-patched file gets the marker once" test "$(grep -c ': > ' "$FM/sbin/service.sh")" = 1

echo "== P2d. deprecated CAMYN_MAINTENANCE alias (one release) in the patched service.sh"
FL=$WORK/legacy
mkdir -p "$FL/mnt" "$FL/tmp" "$FL/sbin"
sed -e "s#/mnt/#$FL/mnt/#g" -e "s#/tmp/tf_update.sh#$FL/tmp/tf_update.sh#g" \
    "$FIXTURES/app_fake_variants/d/sbin/service.sh" >"$FL/sbin/service.sh"
AKAM_MAINT_FLAG=$FL/mnt/AKAM_MAINTENANCE AKAM_MAINT_FLAG_LEGACY=$FL/mnt/CAMYN_MAINTENANCE \
    AKAM_MAINT_MARK=$FL/tmp/akam_maintenance AKAM_PATCH_REPORT=$WORK/legacy.diff "$PATCH" "$FL" >/dev/null 2>&1
sed -e "s#^\(.*\)sh /usr/sbin/tf_mount.sh#\1:#" -e "s#cp /usr/sbin/tf_update.sh .*#:#" -e 's#^\(\s*\)rm .*#\1:#' "$FL/sbin/service.sh" >"$FL/run.sh"
printf '#!/bin/sh\necho flashed >>"%s/flashed"\n' "$FL" >"$FL/tmp/tf_update.sh"
check "alias: guard names both flags" grep -q "{ \[ -f $FL/mnt/AKAM_MAINTENANCE \] || \[ -f $FL/mnt/CAMYN_MAINTENANCE \]; } && sh " "$FL/sbin/service.sh"
check "alias: patched file sh -n" "${SHCMD[@]}" -n "$FL/sbin/service.sh"
"${SHCMD[@]}" "$FL/run.sh" >/dev/null 2>&1
check "alias, no flag: tf_update not run, no marker" bash -c "! test -e '$FL/flashed' && ! test -e '$FL/tmp/akam_maintenance'"
: >"$FL/mnt/CAMYN_MAINTENANCE"
"${SHCMD[@]}" "$FL/run.sh" >/dev/null 2>&1
check "alias: old flag name still opens the SD hook" test -s "$FL/flashed"
check "alias: old flag name writes the new RAM marker" test -e "$FL/tmp/akam_maintenance"
rm -f "$FL/flashed" "$FL/tmp/akam_maintenance" "$FL/mnt/CAMYN_MAINTENANCE"
: >"$FL/mnt/AKAM_MAINTENANCE"
"${SHCMD[@]}" "$FL/run.sh" >/dev/null 2>&1
check "alias: new flag name works too" test -s "$FL/flashed"
# alias disabled with an empty AKAM_MAINT_FLAG_LEGACY: plain single-flag guard
mkdir -p "$WORK/nl/sbin"
cp "$FIXTURES/app_fake_variants/d/sbin/service.sh" "$WORK/nl/sbin/service.sh"
AKAM_MAINT_FLAG_LEGACY= AKAM_PATCH_REPORT=$WORK/nl.diff "$PATCH" "$WORK/nl" >/dev/null 2>&1
check "alias off: single-flag guard, no CAMYN text" bash -c "grep -q '^	\[ -f /mnt/AKAM_MAINTENANCE \] && sh /tmp/tf_update.sh\$' '$WORK/nl/sbin/service.sh' && ! grep -qi camyn '$WORK/nl/sbin/service.sh'"
# a service.sh whose daemon lines were already disabled by the old marker stays as is
mkdir -p "$WORK/od/sbin"
sed -e 's/AKAM-DISABLED/CAMYN-DISABLED/' "$WORK/app_fake_variants_b/sbin/service.sh" >"$WORK/od/sbin/service.sh"
cp "$WORK/od/sbin/service.sh" "$WORK/od.before"
AKAM_PATCH_REPORT=$WORK/od.diff "$PATCH" "$WORK/od" >/dev/null 2>&1
check "old DISABLED marker is recognised (no double disabling)" bash -c "! grep -q 'AKAM-DISABLED' '$WORK/od/sbin/service.sh'"

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
check "relative AKAM_MAINT_FLAG rejected" bash -c "AKAM_MAINT_FLAG=mnt/x '$PATCH' '$FIXTURES/app_fake' >/dev/null 2>&1; [ \$? -eq 1 ]"

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
LD_ENV=(AKAM_FAST_ROUNDS=3 AKAM_FAST_PERIOD=0 AKAM_SLOW_ROUNDS=2 AKAM_SLOW_PERIOD=0)
run_lockdown() { # run_lockdown <root>
    env "AKAM_ROOT=$1" "REG=$REG" "PATH=$STUBS:$PATH" "${LD_ENV[@]}" timeout 30 "${SHCMD[@]}" "$LOCKDOWN"
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
check "log records kills" grep -q 'SIGTERM pid' "$LR/tmp/akam_lockdown.log"
check "log records SIGKILL escalation" grep -q 'SIGKILL pid' "$LR/tmp/akam_lockdown.log"
check "lock released" test ! -d "$LR/tmp/akam_lockdown.lock"

echo "== L2. maintenance flag: nothing killed, loop goes on"
new_ld_root l2
spawn telnetd "telnetd -p 24 -l /bin/sh"
spawn ftpd "tcpsvd 0 21 ftpd -w /mnt"
: >"$LR/mnt/AKAM_MAINTENANCE"
check "bounded loop exits 0" run_lockdown "$LR"
check "telnetd alive" alive "$(pid_of telnetd)"
check "ftpd alive" alive "$(pid_of ftpd)"
check "log says maintenance mode" grep -q 'maintenance mode: telnet not touched' "$LR/tmp/akam_lockdown.log"
check "loop ran to its end" grep -q 'bounded loop finished' "$LR/tmp/akam_lockdown.log"

echo "== L2b. RAM marker only (SD gone): toolkit-style telnet survives, killing resumes when cleared"
new_ld_root l2b
spawn telnetd "telnetd -p 24 -l /bin/sh"
spawn hack "sh /mnt/custom/scripts/telnet.sh"
: >"$LR/tmp/akam_maintenance"
check "bounded loop exits 0" run_lockdown "$LR"
check "telnetd alive with marker" alive "$(pid_of telnetd)"
check "telnet.sh respawner alive with marker" alive "$(pid_of hack)"
check "log says maintenance mode" grep -q 'maintenance mode: telnet not touched' "$LR/tmp/akam_lockdown.log"
rm -f "$LR/tmp/akam_maintenance"
check "marker cleared: loop kills" run_lockdown "$LR"
check "telnetd dead after marker cleared" dead "$(pid_of telnetd)"

echo "== L2c. deprecated CAMYN_MAINTENANCE alias: nothing killed, deprecation logged"
new_ld_root l2c
spawn telnetd "telnetd -p 24 -l /bin/sh"
: >"$LR/mnt/CAMYN_MAINTENANCE"
check "bounded loop exits 0" run_lockdown "$LR"
check "telnetd alive with the old flag name" alive "$(pid_of telnetd)"
check "log says maintenance mode" grep -q 'maintenance mode: telnet not touched' "$LR/tmp/akam_lockdown.log"
check "log says the flag name is deprecated" grep -q 'DEPRECATED.*CAMYN_MAINTENANCE.*AKAM_MAINTENANCE' "$LR/tmp/akam_lockdown.log"
rm -f "$LR/mnt/CAMYN_MAINTENANCE"
check "old flag removed: loop kills" run_lockdown "$LR"
check "telnetd dead after the old flag is removed" dead "$(pid_of telnetd)"

echo "== L3. flag present then removed: killing resumes, no exit in between"
new_ld_root l3
LD_ENV=(AKAM_FAST_ROUNDS=50 AKAM_FAST_PERIOD=1 AKAM_SLOW_ROUNDS=0 AKAM_SLOW_PERIOD=1)
: >"$LR/mnt/AKAM_MAINTENANCE"
spawn telnetd "telnetd -p 24"
(run_lockdown "$LR" >/dev/null 2>&1 &)
sleep 2.5
check "telnetd alive while flag present" alive "$(pid_of telnetd)"
check "loop still running (lock held)" test -d "$LR/tmp/akam_lockdown.lock"
rm -f "$LR/mnt/AKAM_MAINTENANCE"
sleep 3
check "telnetd killed after flag removal" dead "$(pid_of telnetd)"
check "log says lockdown resumed" grep -q 'lockdown resumed' "$LR/tmp/akam_lockdown.log"
kill "$(cat "$LR/tmp/akam_lockdown.lock/pid")" 2>/dev/null
LD_ENV=(AKAM_FAST_ROUNDS=3 AKAM_FAST_PERIOD=0 AKAM_SLOW_ROUNDS=2 AKAM_SLOW_PERIOD=0)

echo "== L3b. slow loop relaunches akam_ssh.sh start, detached"
new_ld_root l3b
mkdir -p "$LR/usr/sbin"
cat >"$LR/usr/sbin/akam_ssh.sh" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/ssh_calls"
sleep 20
EOF
chmod 755 "$LR/usr/sbin/akam_ssh.sh"
start=$(date +%s)
check "lockdown exits 0 even with a slow helper" run_lockdown "$LR"
check "loop not blocked by helper" test $(($(date +%s) - start)) -lt 10
sleep 0.5
check "helper called with start" grep -q '^start$' "$LR/tmp/ssh_calls"
check "helper called once per slow round" test "$(wc -l <"$LR/tmp/ssh_calls")" = 2

echo "== L4. late telnetd killed during the loop, then bounded exit"
new_ld_root l4
LD_ENV=(AKAM_FAST_ROUNDS=6 AKAM_FAST_PERIOD=1 AKAM_SLOW_ROUNDS=0 AKAM_SLOW_PERIOD=1)
(run_lockdown "$LR" >/dev/null 2>&1 &)
sleep 1.5
spawn telnetd "telnetd -p 24"
sleep 3
check "late telnetd killed" dead "$(pid_of telnetd)"
sleep 3.5
check "loop ended by itself (lock released)" test ! -d "$LR/tmp/akam_lockdown.lock"
check "log says bounded loop finished" grep -q 'bounded loop finished' "$LR/tmp/akam_lockdown.log"
LD_ENV=(AKAM_FAST_ROUNDS=3 AKAM_FAST_PERIOD=0 AKAM_SLOW_ROUNDS=2 AKAM_SLOW_PERIOD=0)

echo "== L5. anti-duplicate lock and stale lock"
new_ld_root l5
spawn telnetd "telnetd -p 24"
spawn holder "sleep 600"
mkdir "$LR/tmp/akam_lockdown.lock"
pid_of holder >"$LR/tmp/akam_lockdown.lock/pid"
check "live lock: exits 0" run_lockdown "$LR"
check "live lock: second instance did nothing" alive "$(pid_of telnetd)"
check "live lock: lock kept for holder" test -d "$LR/tmp/akam_lockdown.lock"
echo 999999 >"$LR/tmp/akam_lockdown.lock/pid"
check "stale lock: exits 0" run_lockdown "$LR"
check "stale lock: taken over, telnetd killed" dead "$(pid_of telnetd)"

echo "== L6. robustness"
new_ld_root l6
rm -f "$STUBS/ps"
check "no ps available: exits 0" run_lockdown "$LR"

echo
echo "passed: $PASS  failed: $FAIL  (shell under test: $SH_UNDER_TEST)"
[ "$FAIL" -eq 0 ]
