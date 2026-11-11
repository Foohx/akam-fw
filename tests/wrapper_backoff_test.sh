#!/usr/bin/env bash
# Host tests for the respawn backoff and crash counter of ak_rtsp_wrapper.sh (G04), run under the pinned
# BusyBox sh when available. ak_rtsp is a stub that advances a fake /proc/uptime and exits with a chosen
# code; sleep and logger are stubs. Nothing touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
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
# fake ak_rtsp: "runs" FAKE_RUN seconds (advances the fake uptime) and exits with FAKE_RC
cat >"$STUBS/ak_rtsp" <<'EOF'
#!/bin/sh
echo "ak_rtsp started" >>"$FAKE/calls"
read -r up _ <"$FAKE/proc/uptime"
up=${up%%.*}
echo "$((up + FAKE_RUN)).00 0.00" >"$FAKE/proc/uptime"
exit "${FAKE_RC:-1}"
EOF
chmod +x "$STUBS"/*

new_env() {
    E=$WORK/$1
    mkdir -p "$E/fake/proc" "$E/tmp"
    echo "1000.00 900.00" >"$E/fake/proc/uptime"
    : >"$E/fake/calls"
}
# run_wrap [env...]: one wrapper run (what the stock watchdog does at each respawn)
run_wrap() {
    env FAKE="$E/fake" AKAM_ROOT="$E" AKAM_TMP_DIR="$E/tmp" AKAM_PROC_DIR="$E/fake/proc" \
        AKAM_RTSP_BIN="$STUBS/ak_rtsp" AKAM_LOGGER="${LOGGER_FOR_TEST:-$STUBS/logger}" PATH="$STUBS:$PATH" "$@" \
        "${SHCMD[@]}" "$WRAP" >"$E/out" 2>&1
}
sleeps() { grep '^sleep' "$E/fake/calls" | sed 's/^sleep //' | while read -r s; do printf '%s ' "$s"; done; }
state() { cat "$E/tmp/akam_respawn" 2>/dev/null; }

# 1. first start: no wait, no helper fork; counter written after the exit
new_env first
run_wrap FAKE_RUN=3 FAKE_RC=1; rc=$?
eq "exit code of ak_rtsp is propagated" "$rc" 1
eq "first start never sleeps" "$(sleeps)" ""
eq "ak_rtsp started once" "$(grep -c '^ak_rtsp' "$E/fake/calls")" 1
case $(state) in "count=1 total=1 last_rc=1 last_run=3 up=1003") ok "state after the first short run" ;; *) ko "state after the first short run ($(state))" ;; esac

# 2. a crash loop: delays 0, 2, 5, 10, 30, 30 before starts 2..7
new_env loop
for i in 1 2 3 4 5 6 7; do run_wrap FAKE_RUN=2 FAKE_RC=3; done
eq "backoff sequence before restarts (none before the 1st and 2nd)" "$(sleeps)" "2 5 10 30 30 "
case $(state) in "count=7 total=7 last_rc=3"*) ok "counter after 7 short runs" ;; *) ko "counter after 7 short runs ($(state))" ;; esac
case $(cat "$E/out") in *"waiting 30s"*) ok "the wait is announced" ;; *) ko "the wait is announced" ;; esac

# 3. a stable run resets the counter; the next crash restarts at once
run_wrap FAKE_RUN=600 FAKE_RC=0
case $(state) in "count=0 total=8 last_rc=0 last_run=600"*) ok "10 min run: counter reset, total kept" ;; *) ko "stable run ($(state))" ;; esac
: >"$E/fake/calls"
run_wrap FAKE_RUN=1 FAKE_RC=9
eq "after a stable run the next start is immediate" "$(sleeps)" ""
run_wrap FAKE_RUN=1 FAKE_RC=9
eq "the crash right after waits nothing either (count 1 -> 0 s)" "$(sleeps)" ""
run_wrap FAKE_RUN=1 FAKE_RC=9
eq "then the backoff starts (count 2 -> 2 s)" "$(sleeps)" "2 "

# 4. 599 s is still a short run
new_env edge
run_wrap FAKE_RUN=599 FAKE_RC=1
case $(state) in "count=1 "*) ok "599 s counts as a short run" ;; *) ko "599 s ($(state))" ;; esac

# 5. software watchdog exit code is named in the log
new_env swdog
run_wrap FAKE_RUN=100 FAKE_RC=70
case $(cat "$E/tmp/akam_wrapper.log") in *"code 70 (software stall watchdog) after 100s"*) ok "exit 70 logged as the software watchdog" ;; *) ko "exit 70 log ($(cat "$E/tmp/akam_wrapper.log"))" ;; esac

# 6. syslog when logger exists, file always, capped; works without logger
eq "syslog line sent via logger" "$(grep -c '^logger -t akam_wrapper' "$E/fake/calls")" 1
new_env nologger
run_wrap FAKE_RUN=1 FAKE_RC=1 AKAM_LOGGER=/nonexistent/logger; rc=$?
eq "no logger: wrapper still exits with ak_rtsp's code" "$rc" 1
eq "no logger: file log written" "$(grep -c 'ak_rtsp exited with code 1' "$E/tmp/akam_wrapper.log")" 1
new_env cap
for i in $(seq 1 130); do echo "old $i" >>"$E/tmp/akam_wrapper.log"; done
run_wrap FAKE_RUN=1 FAKE_RC=1
n=$(wc -l <"$E/tmp/akam_wrapper.log")
if [ "$n" -le 101 ]; then ok "wrapper log capped ($n lines)"; else ko "wrapper log not capped ($n lines)"; fi

# 7. corrupted state is treated as zero, never breaks the start
new_env corrupt
echo "count=abc total=;rm -rf" >"$E/tmp/akam_respawn"
run_wrap FAKE_RUN=1 FAKE_RC=1; rc=$?
eq "corrupted state: starts, exit code kept" "$rc" 1
eq "corrupted state: no wait" "$(sleeps)" ""
case $(state) in "count=1 total=1 "*) ok "corrupted state restarted from zero" ;; *) ko "corrupted state ($(state))" ;; esac

# 8. unreadable /proc/uptime: no crash, run counted as 0 s
new_env nouptime
rm -f "$E/fake/proc/uptime"
cat >"$STUBS/ak_rtsp_nouptime" <<'EOF'
#!/bin/sh
exit 5
EOF
chmod +x "$STUBS/ak_rtsp_nouptime"
run_wrap AKAM_RTSP_BIN="$STUBS/ak_rtsp_nouptime"; rc=$?
eq "missing uptime: exit code kept" "$rc" 5
case $(state) in "count=1 total=1 last_rc=5 last_run=0 "*) ok "missing uptime: run counted as 0 s" ;; *) ko "missing uptime ($(state))" ;; esac

# 9. the first start stays fast: the block before ak_rtsp forks nothing when count is 0
if "${SHCMD[@]}" -n "$WRAP"; then ok "sh -n"; else ko "sh -n"; fi
before=$(awk '/^CRASH_N=0/{f=1} /^# --- Network hostname/{f=0} f' "$WRAP")
# shellcheck disable=SC2016 # the patterns are literal on purpose
case $before in *'$('* | *'`'*) ko "backoff block forks a subshell" ;; *) ok "backoff block has no command substitution (state read with builtins)" ;; esac
if grep -q 'akam_net.sh start' "$WRAP"; then ok "wrapper launches akam_net.sh detached"; else ko "wrapper launches akam_net.sh"; fi
if grep -Fq '( /usr/sbin/akam_net.sh start </dev/null >/dev/null 2>&1 & )' "$WRAP"; then ok "akam_net.sh detached like akam_ssh.sh"; else ko "akam_net.sh not detached the same way"; fi

echo "wrapper_backoff_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
