#!/usr/bin/env bash
# Host tests for akam_net.sh (camera Wi-Fi supervisor), run under the pinned BusyBox sh when available.
# ps, wpa_supplicant, udhcpc, ifconfig, kill, sleep and logger are stubs driven by files in a work dir:
# nothing touches a real process or network.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
NET=$REPO/tools/firmware_patch_templates/akam_net.sh

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

STUBS=$WORK/stubs
mkdir -p "$STUBS"
# State files in $FAKE: wpa_up, dhcp_up, ip_up. Calls are appended to $FAKE/calls.
cat >"$STUBS/ps" <<'EOF'
#!/bin/sh
echo "  PID USER       VSZ STAT COMMAND"
echo "    1 root      1000 S    init"
[ -e "$FAKE/wpa_up" ] && echo "  101 root      3000 S    wpa_supplicant -Dnl80211 -i wlan0 -c /etc/config/wpa_supplicant.conf -B"
[ -e "$FAKE/dhcp_up" ] && echo "  102 root      1000 S    udhcpc -i wlan0 -x hostname:akam"
echo "  103 root      1000 S    grep wpa_supplicant"
exit 0
EOF
cat >"$STUBS/wpa_supplicant" <<'EOF'
#!/bin/sh
echo "wpa_supplicant $*" >>"$FAKE/calls"
[ -n "${FAKE_WPA_FAIL:-}" ] || : >"$FAKE/wpa_up"
exit 0
EOF
cat >"$STUBS/udhcpc" <<'EOF'
#!/bin/sh
echo "udhcpc $*" >>"$FAKE/calls"
: >"$FAKE/dhcp_up"
[ -n "${FAKE_DHCP_NOIP:-}" ] || : >"$FAKE/ip_up"
exit 0
EOF
cat >"$STUBS/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >>"$FAKE/calls"
case "$1" in
    101) rm -f "$FAKE/wpa_up" ;;
    102) rm -f "$FAKE/dhcp_up" "$FAKE/ip_up" ;;
esac
exit 0
EOF
cat >"$STUBS/ifconfig" <<'EOF'
#!/bin/sh
echo "wlan0     Link encap:Ethernet  HWaddr 00:11:22:33:44:55"
[ -e "$FAKE/ip_up" ] && echo "          inet addr:192.168.1.50  Bcast:192.168.1.255  Mask:255.255.255.0"
exit 0
EOF
cat >"$STUBS/sleep" <<'EOF'
#!/bin/sh
echo "sleep $*" >>"$FAKE/calls"
exit 0
EOF
cat >"$STUBS/logger" <<'EOF'
#!/bin/sh
echo "logger $*" >>"$FAKE/calls"
exit 0
EOF
chmod +x "$STUBS"/*

# new_env NAME : fresh FAKE dir, tmp dir, conf present
new_env() {
    E=$WORK/$1
    mkdir -p "$E/fake" "$E/tmp" "$E/etc/config"
    echo 'ctrl_interface=/var/run' >"$E/etc/config/wpa_supplicant.conf"
    : >"$E/fake/calls"
}
# run_net CYCLES [env...]: run the supervisor for CYCLES checks
run_net() {
    _c=$1
    shift
    env FAKE="$E/fake" AKAM_ROOT="$E" AKAM_TMP_DIR="$E/tmp" AKAM_NET_MAX_CYCLES="$_c" AKAM_KILL="$STUBS/kill" \
        AKAM_LOGGER="${LOGGER_FOR_TEST:-$STUBS/logger}" PATH="$STUBS:$PATH" "$@" \
        "${SHCMD[@]}" "$NET" start 2>&1
}
count() { grep -c "$1" "$E/fake/calls" 2>/dev/null || true; }
lc() { grep -c "$1" "$E/tmp/akam_net.log" 2>/dev/null || true; }

# 1. no wpa_supplicant.conf: touches nothing
new_env noconf
rm -f "$E/etc/config/wpa_supplicant.conf"
run_net 3 >/dev/null
eq "no conf: wpa_supplicant never started" "$(count '^wpa_supplicant')" 0
eq "no conf: udhcpc never started" "$(count '^udhcpc')" 0
eq "no conf: nothing killed" "$(count '^kill')" 0
eq "no conf: only sleeps of 30 s" "$(grep -vc '^sleep 30$\|^logger' "$E/fake/calls")" 0

# 2. healthy: no action
new_env healthy
: >"$E/fake/wpa_up"; : >"$E/fake/dhcp_up"; : >"$E/fake/ip_up"
run_net 5 >/dev/null
eq "healthy: wpa not started" "$(count '^wpa_supplicant')" 0
eq "healthy: udhcpc not started" "$(count '^udhcpc')" 0
eq "healthy: 5 sleeps of 30 s (low CPU loop)" "$(count '^sleep 30$')" 5
eq "healthy: first call is the sleep (never races the wrapper bring-up)" "$(grep -v "^logger" "$E/fake/calls" | head -1)" "sleep 30"

# 3. everything down: both daemons started, with the hostname helper absent -> "akam"
new_env alldown
run_net 2 >/dev/null
eq "all down: wpa_supplicant started once" "$(count '^wpa_supplicant')" 1
case $(grep '^wpa_supplicant' "$E/fake/calls") in
    *"-Dnl80211 -i wlan0 -c $E/etc/config/wpa_supplicant.conf -B"*) ok "all down: wpa_supplicant command line" ;;
    *) ko "all down: wpa_supplicant command line ($(grep '^wpa_supplicant' "$E/fake/calls"))" ;;
esac
eq "all down: udhcpc started once" "$(count '^udhcpc')" 1
case $(grep '^udhcpc' "$E/fake/calls") in
    *"-i wlan0 -x hostname:akam"*) ok "all down: udhcpc with the default DHCP hostname" ;;
    *) ko "all down: udhcpc command line" ;;
esac
eq "all down then healthy on the next check: no second start" "$(count '^wpa_supplicant')" 1
eq "recovery is logged" "$(lc 'network back')" 1
eq "events go to syslog when logger exists" "$(( $(count '^logger') > 0 ))" 1

# 4. udhcpc dead only: only udhcpc is started
new_env dhcpdead
: >"$E/fake/wpa_up"
run_net 1 >/dev/null
eq "udhcpc dead: wpa_supplicant left alone" "$(count '^wpa_supplicant')" 0
eq "udhcpc dead: udhcpc restarted" "$(count '^udhcpc')" 1

# 5. wpa_supplicant dead only: restarted, udhcpc restarted too (fresh association)
new_env wpadead
: >"$E/fake/dhcp_up"
run_net 1 >/dev/null
eq "wpa dead: wpa_supplicant restarted" "$(count '^wpa_supplicant')" 1
eq "wpa dead: stale udhcpc killed" "$(count '^kill 102')" 1
eq "wpa dead: udhcpc restarted" "$(count '^udhcpc')" 1

# 6. both alive, no IPv4 (router gone): udhcpc restarts at checks 1, 3, then both from attempt 3 (check 6)
new_env noip
: >"$E/fake/wpa_up"; : >"$E/fake/dhcp_up"
run_net 6 FAKE_DHCP_NOIP=1 >/dev/null
eq "no IPv4: 3 repair attempts in 6 checks (gaps 1 and 2 checks)" "$(lc 'repair attempt')" 3
eq "no IPv4: udhcpc restarted on every attempt" "$(count '^udhcpc')" 3
eq "no IPv4: wpa_supplicant restarted only from the 3rd attempt" "$(count '^wpa_supplicant')" 1

# 7. backoff grows: wpa never comes up. Attempts at checks 1, 3, 6, 11, 20 (gaps 1, 2, 4, 8, then 10)
new_env backoff
run_net 21 FAKE_WPA_FAIL=1 >/dev/null
eq "backoff: attempts at checks 1,3,6,11,20 only" "$(lc 'repair attempt')" 5
eq "backoff: wpa_supplicant attempted 5 times, not 21" "$(count '^wpa_supplicant')" 5
last=$(grep 'repair attempt' "$E/tmp/akam_net.log" | tail -1)
case $last in *"check 20"*"attempt 5"*) ok "backoff: last attempt is at check 20" ;; *) ko "backoff: last attempt line: $last" ;; esac

# 8. cap: gap never exceeds 10 checks (5 minutes)
new_env cap
run_net 52 FAKE_WPA_FAIL=1 >/dev/null
# attempts at 1,3,6,11,20,31,42 (gaps 1,2,4,8,10,10), next would be 53
eq "cap: gap stays at 10 checks" "$(lc 'repair attempt')" 7

# 9. recovery resets the backoff
new_env reset
run_net 4 FAKE_WPA_FAIL=1 >/dev/null
rm -f "$E/fake/calls"; : >"$E/fake/calls"
: >"$E/fake/wpa_up"; : >"$E/fake/dhcp_up"; : >"$E/fake/ip_up"
run_net 2 >/dev/null
eq "healthy again: nothing started" "$(count '^wpa_supplicant')" 0

# 10. anti-duplicate: a live holder whose cmdline says akam_net stops a second supervisor
new_env dup
mkdir -p "$E/proc/$$" "$E/tmp/akam_net.lock"
printf 'sh\0/usr/sbin/akam_net.sh\0start\0' >"$E/proc/$$/cmdline"
echo $$ >"$E/tmp/akam_net.lock/pid"
out=$(run_net 3 AKAM_PROC_DIR="$E/proc")
eq "duplicate: no loop (no sleep)" "$(count '^sleep')" 0
case $out in *) ;; esac
eq "duplicate: refusal logged" "$(lc 'another instance')" 1

# 11. stale lock (dead pid, or a live pid that is not akam_net) is taken over
new_env stale
mkdir -p "$E/tmp/akam_net.lock" "$E/proc/$$"
echo 99999999 >"$E/tmp/akam_net.lock/pid"
printf 'bash\0' >"$E/proc/$$/cmdline"
run_net 2 AKAM_PROC_DIR="$E/proc" >/dev/null
eq "stale lock (dead pid): takes over" "$(count '^sleep 30$')" 2
new_env stale2
mkdir -p "$E/tmp/akam_net.lock" "$E/proc/$$"
echo $$ >"$E/tmp/akam_net.lock/pid"
printf 'bash\0' >"$E/proc/$$/cmdline"
run_net 2 AKAM_PROC_DIR="$E/proc" >/dev/null
eq "stale lock (pid reused by another program): takes over" "$(count '^sleep 30$')" 2
if [ ! -d "$E/tmp/akam_net.lock" ]; then ok "lock removed on exit"; else ko "lock removed on exit"; fi

# 12. log cap
new_env logcap
for i in $(seq 1 130); do echo "old line $i" >>"$E/tmp/akam_net.log"; done
run_net 1 >/dev/null
n=$(wc -l <"$E/tmp/akam_net.log")
if [ "$n" -le 101 ]; then ok "log capped ($n lines)"; else ko "log not capped ($n lines)"; fi

# 13. no logger: file only, nothing fails
new_env nologger
run_net 1 AKAM_LOGGER=/nonexistent/logger >/dev/null
eq "no logger: log file still written" "$(( $(lc 'start') > 0 ))" 1

# 14. status
new_env status
out=$(env AKAM_ROOT="$E" AKAM_TMP_DIR="$E/tmp" "${SHCMD[@]}" "$NET" status 2>&1); rc=$?
eq "status without a supervisor: exit 1" "$rc" 1
case $out in *"not running"*) ok "status text" ;; *) ko "status text ($out)" ;; esac
out=$(env AKAM_ROOT="$E" AKAM_TMP_DIR="$E/tmp" "${SHCMD[@]}" "$NET" bogus 2>&1); rc=$?
eq "bad usage: exit 2" "$rc" 2

# 15. syntax under the pinned shell, and BusyBox-only applets
if "${SHCMD[@]}" -n "$NET"; then ok "sh -n"; else ko "sh -n"; fi
if grep -nE '(^|[^a-z_])(pgrep|pkill|mktemp|tr )([^a-z_]|$)' "$NET" | grep -v '^[0-9]*:#' | grep -v 'no pgrep' >/dev/null; then
    ko "uses an applet missing on the camera"
else ok "no pgrep/pkill/mktemp/tr"; fi

echo "akam_net_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
