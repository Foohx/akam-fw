#!/usr/bin/env bash
# shellcheck disable=SC2012,SC2016  # ls for modes and inodes, single-quoted commands for other shells
# Host tests for akam_ntp.sh (camera time sync helper, G08), run under the pinned BusyBox sh when available.
# ntpd, date, timeout, nice, sleep, kill and logger are stubs: nothing touches the network or the host clock.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
NTP=$REPO/tools/firmware_patch_templates/akam_ntp.sh
WRAP=$REPO/tools/firmware_patch_templates/ak_rtsp_wrapper.sh

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
# $FAKE/calls records every call. ntpd succeeds unless $FAKE/ntpd_fail exists; the fake clock year is in
# $FAKE/year and becomes 2025 after a successful ntpd (like a real step).
cat >"$STUBS/ntpd" <<'EOF'
#!/bin/sh
echo "ntpd $*" >>"$FAKE/calls"
if [ -e "$FAKE/ntpd_fail" ]; then
    [ -e "$FAKE/ntpd_fail_n" ] && { n=$(cat "$FAKE/ntpd_fail_n"); n=$((n - 1)); if [ "$n" -le 0 ]; then rm -f "$FAKE/ntpd_fail" "$FAKE/ntpd_fail_n"; else echo "$n" >"$FAKE/ntpd_fail_n"; fi; }
    exit 1
fi
echo 2025 >"$FAKE/year"
exit 0
EOF
cat >"$STUBS/date" <<'EOF'
#!/bin/sh
case "$*" in
    "-u +%Y") cat "$FAKE/year" ;;
    *) echo "$(cat "$FAKE/year")-06-01 12:00:00" ;;
esac
EOF
cat >"$STUBS/timeout" <<'EOF'
#!/bin/sh
echo "timeout $1" >>"$FAKE/calls"
shift
exec "$@"
EOF
cat >"$STUBS/nice" <<'EOF'
#!/bin/sh
echo "nice $1 $2" >>"$FAKE/calls"
shift 2
exec "$@"
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
cat >"$STUBS/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >>"$FAKE/calls"
exit 0
EOF
chmod +x "$STUBS"/*

new_env() {
    E=$WORK/$1
    rm -rf "$E"
    mkdir -p "$E/fake" "$E/tmp" "$E/etc/config"
    echo 1970 >"$E/fake/year"
    : >"$E/fake/calls"
}
# run_ntp ARGS... : the helper with stubs first in PATH
run_ntp() {
    env FAKE="$E/fake" AKAM_ROOT="$E" AKAM_TMP_DIR="$E/tmp" AKAM_NTPD="$STUBS/ntpd" AKAM_KILL="$STUBS/kill" \
        AKAM_LOGGER="$STUBS/logger" AKAM_NTP_NO_START=1 PATH="$STUBS:$PATH" "${EXTRA[@]}" \
        "${SHCMD[@]}" "$NTP" "$@" 2>&1
}
EXTRA=()
count() { grep -c "$1" "$E/fake/calls" 2>/dev/null || true; }
CONF() { echo "$E/etc/config/akam/ntp"; }

# 1. off by default: run does nothing, no traffic
new_env off
run_ntp run >/dev/null
eq "no file: ntpd never called" "$(count '^ntpd')" 0
eq "no file: no sleep either (the helper exits)" "$(count '^sleep')" 0
out=$(run_ntp status)
case $out in *"time sync: off (default)"*) ok "status: off by default" ;; *) ko "status off: $out" ;; esac
case $out in *"sys.time_set: 0"*) ok "status: clock before 2024 reported as not set" ;; *) ko "status clock: $out" ;; esac

# 2. set: validates, writes 0600 in a 0700 directory, lines are exact
new_env set
out=$(run_ntp set pool.ntp.org 192.168.1.1)
case $out in *"Time sync on, servers: pool.ntp.org 192.168.1.1"*) ok "set: confirms" ;; *) ko "set: $out" ;; esac
eq "set: file content" "$(cat "$(CONF)")" "$(printf 'server=pool.ntp.org\nserver=192.168.1.1')"
eq "set: file mode 600" "$(ls -l "$(CONF)" | cut -c1-10)" "-rw-------"
eq "set: directory mode 700" "$(ls -ld "$E/etc/config/akam" | cut -c1-10)" "drwx------"
eq "set: no temporary file left" "$(ls "$E/etc/config/akam" | wc -l | tr -d ' ')" 1
eq "set: ntpd not run by set itself (the helper does, at boot or when started)" "$(count '^ntpd')" 0
run_ntp set time.example.net >/dev/null
eq "set again: replaces the list" "$(cat "$(CONF)")" "server=time.example.net"

# 3. refusals: nothing changes, exit 2
new_env refuse
run_ntp set pool.ntp.org >/dev/null
before=$(cat "$(CONF)")
for bad in "" "-x" "--version" "-p" "a b" 'a;b' '$(id)' 'a`id`' "a/b" ".a" "a." "a..b" "-a.example" "a-.example" "pool.ntp.org:123" "[::1]" "no_underscore" "ex ample" "$(printf 'a%.0s' $(seq 1 64)).org"; do
    run_ntp set "$bad" >/dev/null
    rc=$?
    if [ "$rc" -eq 2 ] && [ "$(cat "$(CONF)")" = "$before" ]; then ok "refused (exit 2, file untouched): '$bad'"; else ko "not refused: '$bad' (rc=$rc)"; fi
done
run_ntp set a.org b.org c.org d.org >/dev/null
eq "4 servers refused (max 3)" "$?" 2
run_ntp set >/dev/null
eq "no server refused" "$?" 2
run_ntp set ok.org 'bad name' >/dev/null
eq "one bad server among good ones: refused as a whole" "$?" 2
eq "refusals left the file as it was" "$(cat "$(CONF)")" "$before"
run_ntp frob >/dev/null
eq "unknown word: usage, exit 2" "$?" 2
run_ntp off now >/dev/null
eq "off takes no argument" "$?" 2

# 4. run: one-shot ntpd -q with every server, low priority, bounded by timeout; clock set -> slow resync
new_env run
run_ntp set pool.ntp.org 10.0.0.1 >/dev/null
EXTRA=(AKAM_NTP_MAX_CYCLES=1)
run_ntp run >/dev/null
EXTRA=()
eq "run: ntpd called once" "$(count '^ntpd')" 1
eq "run: -n -q and every server as -p" "$(grep '^ntpd' "$E/fake/calls")" "ntpd -n -q -p pool.ntp.org -p 10.0.0.1"
eq "run: under timeout 60" "$(grep '^timeout' "$E/fake/calls")" "timeout 60"
eq "run: low priority (nice -n 10)" "$(grep '^nice' "$E/fake/calls")" "nice -n 10"
eq "run: success logged" "$(grep -c 'sync ok' "$E/tmp/akam_ntp.log")" 1
eq "run: lock removed at exit" "$([ -e "$E/tmp/akam_ntp.lock" ] && echo held || echo free)" free
out=$(run_ntp status)
case $out in *"sys.time_set: 1"*) ok "status: clock 2025 reported as set" ;; *) ko "status after sync: $out" ;; esac

# 4b. no `timeout` applet: ntpd -q still runs, and a hung ntpd is killed by the shell watchdog
new_env notimeout
run_ntp set pool.ntp.org >/dev/null
EXTRA=(AKAM_NTP_MAX_CYCLES=1 AKAM_TIMEOUT_BIN=no-such-timeout-applet)
run_ntp run >/dev/null
eq "no timeout applet: ntpd called once, no timeout call" "$(count '^ntpd')$(count '^timeout')" 10
eq "no timeout applet: success logged" "$(grep -c 'sync ok' "$E/tmp/akam_ntp.log")" 1
cp "$STUBS/ntpd" "$STUBS/ntpd.real"
printf '#!/bin/sh\necho "ntpd $*" >>"$FAKE/calls"\nexec tail -f /dev/null\n' >"$STUBS/ntpd"
new_env hang
run_ntp set pool.ntp.org >/dev/null
EXTRA=(AKAM_NTP_MAX_CYCLES=1 AKAM_TIMEOUT_BIN=no-such-timeout-applet AKAM_NTP_TIMEOUT=3)
run_ntp run >/dev/null
eq "hung ntpd: killed by the watchdog, reported as a failed pass" "$(grep -c 'sync failed' "$E/tmp/akam_ntp.log")" 1
mv "$STUBS/ntpd.real" "$STUBS/ntpd"
EXTRA=()

# 4c. TERM during a pass: the ntpd child must not be left behind (it would wait for ever on a dead server)
cp "$STUBS/ntpd" "$STUBS/ntpd.real"
printf '#!/bin/sh\necho $$ >"$FAKE/ntpd.pid"\nexec tail -f /dev/null\n' >"$STUBS/ntpd"
new_env orphan
run_ntp set pool.ntp.org >/dev/null
EXTRA=(AKAM_NTP_MAX_CYCLES=1 AKAM_TIMEOUT_BIN=no-such-timeout-applet AKAM_NTP_TIMEOUT=100)
run_ntp run >/dev/null &
hp=$!
for _i in $(seq 1 20); do [ -s "$E/fake/ntpd.pid" ] && break; sleep 0.25; done
np=$(cat "$E/fake/ntpd.pid" 2>/dev/null)
kill -TERM "$hp" 2>/dev/null
wait "$hp" 2>/dev/null
gone=no
for _i in $(seq 1 12); do kill -0 "$np" 2>/dev/null || { gone=yes; break; }; sleep 0.25; done
[ "$gone" = yes ] || kill -KILL "$np" 2>/dev/null
eq "TERM during a pass: the ntpd child is gone" "$gone" yes
mv "$STUBS/ntpd.real" "$STUBS/ntpd"
EXTRA=()

# 5. failures: retries 15, 30, 60, 120, 300, 600, 600; success switches to the 6 h rhythm
new_env retry
run_ntp set pool.ntp.org >/dev/null
: >"$E/fake/ntpd_fail"
EXTRA=(AKAM_NTP_MAX_CYCLES=7)
run_ntp run >/dev/null
EXTRA=()
eq "retry: sleeps 15 30 60 120 300 600" "$(grep '^sleep' "$E/fake/calls" | tr '\n' ' ' | sed 's/sleep //g')" "15 30 60 120 300 600 "
eq "retry: seven passes" "$(count '^ntpd')" 7
new_env recover
run_ntp set pool.ntp.org >/dev/null
: >"$E/fake/ntpd_fail"
echo 2 >"$E/fake/ntpd_fail_n"
EXTRA=(AKAM_NTP_MAX_CYCLES=4)
run_ntp run >/dev/null
EXTRA=()
eq "recover: 2 failures then success then the 6 h wait" "$(grep '^sleep' "$E/fake/calls" | tr '\n' ' ' | sed 's/sleep //g')" "15 30 21600 "
eq "recover: recovery logged" "$(grep -c 'sync ok after 2 failed' "$E/tmp/akam_ntp.log")" 1

# 6. a clock set by hand and an unreachable server: slow rhythm, not a retry storm
new_env hand
run_ntp set pool.ntp.org >/dev/null
echo 2025 >"$E/fake/year"
: >"$E/fake/ntpd_fail"
EXTRA=(AKAM_NTP_MAX_CYCLES=2)
run_ntp run >/dev/null
EXTRA=()
eq "clock already set: failure waits the long interval" "$(grep '^sleep' "$E/fake/calls" | head -1)" "sleep 21600"

# 7. invalid or extra lines in the file are ignored, at most 3 servers are used, CRLF tolerated
new_env messy
mkdir -p "$E/etc/config/akam"
printf 'server=a.org\r\nserver=-evil\nserver=b.org\nserver=bad name\nrubbish\nserver=c.org\nserver=d.org\n' >"$(CONF)"
EXTRA=(AKAM_NTP_MAX_CYCLES=1)
run_ntp run >/dev/null
EXTRA=()
eq "messy file: only valid servers, max 3" "$(grep '^ntpd' "$E/fake/calls")" "ntpd -n -q -p a.org -p b.org -p c.org"
new_env novalid
mkdir -p "$E/etc/config/akam"
printf 'server=-x\nfoo\n' >"$(CONF)"
run_ntp run >/dev/null
eq "file without a valid server: ntpd never called" "$(count '^ntpd')" 0
out=$(run_ntp status)
case $out in *"no valid server"*) ok "status: says no valid server" ;; *) ko "status novalid: $out" ;; esac

# 8. only one helper at a time (lock), a stale lock is taken over
new_env lock
run_ntp set pool.ntp.org >/dev/null
mkdir -p "$E/tmp/akam_ntp.lock"
echo $$ >"$E/tmp/akam_ntp.lock/pid"       # a live pid ...
mkdir -p "$E/proc/$$"
echo "sh /usr/sbin/akam_ntp.sh run" >"$E/proc/$$/cmdline"   # ... that really is a helper
EXTRA=(AKAM_NTP_MAX_CYCLES=1 AKAM_PROC_DIR="$E/proc")
run_ntp run >/dev/null
eq "live helper holds the lock: second run exits at once" "$(count '^ntpd')" 0
echo 999999 >"$E/tmp/akam_ntp.lock/pid"
run_ntp run >/dev/null
EXTRA=()
eq "stale lock: taken over" "$(count '^ntpd')" 1

# 9. off: removes the file, stops the helper, idempotent
new_env offcmd
run_ntp set pool.ntp.org >/dev/null
mkdir -p "$E/tmp/akam_ntp.lock" "$E/proc/$$"
echo $$ >"$E/tmp/akam_ntp.lock/pid"
echo "sh /usr/sbin/akam_ntp.sh run" >"$E/proc/$$/cmdline"
EXTRA=(AKAM_PROC_DIR="$E/proc")
out=$(run_ntp off)
EXTRA=()
case $out in *"Time sync off"*) ok "off: confirms" ;; *) ko "off: $out" ;; esac
eq "off: file removed" "$([ -e "$(CONF)" ] && echo there || echo gone)" gone
eq "off: the running helper was killed" "$(count "^kill $$")" 1
out=$(run_ntp off)
case $out in *"already off"*) ok "off twice: says already off" ;; *) ko "off twice: $out" ;; esac

# 10. log is capped
new_env logcap
run_ntp set pool.ntp.org >/dev/null
: >"$E/fake/ntpd_fail"
EXTRA=(AKAM_NTP_MAX_CYCLES=150)
run_ntp run >/dev/null
EXTRA=()
n=$(wc -l <"$E/tmp/akam_ntp.log" | tr -d ' ')
if [ "$n" -le 101 ] && [ "$n" -ge 20 ]; then ok "log capped ($n lines after 150 failures)"; else ko "log lines: $n"; fi

# 11. the wrapper starts the helper only when the file exists
if grep -Fq '[ -f "$AKAM_ROOT/etc/config/akam/ntp" ] && [ -x /usr/sbin/akam_ntp.sh ]' "$WRAP" &&
    grep -Fq '( /usr/sbin/akam_ntp.sh run </dev/null >/dev/null 2>&1 & )' "$WRAP"; then ok "wrapper: helper started detached, only when the file exists"; else ko "wrapper does not guard the ntp helper"; fi

# 12. no hostname or option can reach ntpd as an option
if ! grep -n 'ntpd.*\$1\b' "$NTP" | grep -v '^#' >/dev/null; then ok "ntpd never gets a raw argument"; else ko "ntpd gets a raw argument"; fi

echo "akam_ntp_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
