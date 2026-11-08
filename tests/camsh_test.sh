#!/usr/bin/env bash
# Tests for tools/wizard/lib/camsh.sh against the fake camera (tools/wizard/fake_camera.sh):
# end markers and exit codes, IAC and CR filtering, timeouts, reconnect, manual fallback after
# 2 failures, empty stdin in manual mode, dry-run, prompt relay. Nothing leaves localhost.
# The transcripts the fake reproduces are reconstructed from docs/akam_v1_backup_sd.md (section 8),
# not recorded on a camera.
# shellcheck disable=SC2016  # commands for the camera shell are single-quoted on purpose
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/fixtures/fake_camera_env.sh
. "$ROOT/tests/fixtures/fake_camera_env.sh"
fc_require_busybox
# shellcheck source=tools/lib/host.sh
. "$ROOT/tools/lib/host.sh"
# shellcheck source=tools/wizard/lib/log.sh
. "$ROOT/tools/wizard/lib/log.sh"
# shellcheck source=tools/wizard/lib/ui.sh
. "$ROOT/tools/wizard/lib/ui.sh"
# shellcheck source=tools/wizard/lib/camsh.sh
. "$ROOT/tools/wizard/lib/camsh.sh"

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { # name, command...
    local n=$1
    shift
    if "$@"; then ok "$n"; else ko "$n"; fi
}
T=$(mktemp -d)
trap 'fc_cleanup; rm -rf "$T"' EXIT
fc_start "$@" || { echo "cannot start the fake camera"; exit 1; }
NOCOLOR=1
export NO_COLOR=$NOCOLOR

# Each test runs in a subshell: its camsh state (open fd, counters) dies with it.
t_basic() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_exec 'echo hello' || exit 1
    [ "$CAMSH_RC" = 0 ] && [ "$CAMSH_OUT" = $'hello\n' ]
)
t_iac_cr_filtered() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_exec 'printf "a\r\nb\r\n"' || exit 1
    # No IAC (0xff), no CR, no echo of the typed line and no marker in the output.
    [ "$CAMSH_OUT" = $'a\nb\n' ] && ! printf '%s' "$CAMSH_OUT" | LC_ALL=C grep -q $'\377' &&
        ! printf '%s' "$CAMSH_OUT" | grep -q -e '__AKAM_DONE' -e $'\r'
)
t_exit_code() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_exec 'false' || exit 1
    [ "$CAMSH_RC" = 1 ] || exit 1
    camsh_exec 'sh -c "exit 7"' || exit 1
    [ "$CAMSH_RC" = 7 ] || exit 1
    camsh_ok 'true' && ! camsh_ok 'false'
)
t_unique_markers() (
    # A command that prints something looking like a marker must not end the command early.
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_exec 'echo "__AKAM_DONE_1_1:0__"; echo after; false' || exit 1
    [ "$CAMSH_RC" = 1 ] && printf '%s' "$CAMSH_OUT" | grep -q '^after$'
)
t_two_commands_in_a_row() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_exec 'echo one' || exit 1
    camsh_exec 'echo two' || exit 1
    [ "$CAMSH_OUT" = $'two\n' ]
)
t_timeout_single() (
    # One command that does not finish: camsh_start + camsh_finish gives 1 after the timeout.
    local t0 rc=0
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_start 'sleep 6' || exit 1
    t0=$SECONDS
    camsh_finish 2 || rc=$?
    [ "$rc" = 1 ] && [ $((SECONDS - t0)) -le 4 ]
)
t_command_limits() (
    local long
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    long=$(printf 'x%.0s' $(seq 1 400))
    ! camsh_exec "echo $long" 2>/dev/null &&
        ! camsh_exec $'echo a\necho b' 2>/dev/null
)
t_reconnect_once() (
    # The camera closes the connection at the next command: camsh reopens and succeeds.
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl close 1
    camsh_exec 'echo again' || exit 1
    [ "$CAMSH_OUT" = $'again\n' ] && [ "$CAMSH_MANUAL" = 0 ] && [ "$CAMSH_FAILS" = 0 ]
)
t_manual_after_two_failures() (
    # The camera ignores every command: 2 failures, then manual mode with the pasted output.
    export AKAM_CAMSH_TIMEOUT=2
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl drop 100
    camsh_exec 'echo hi' 2 <<<$'pasted line\nEND\ny' >"$T/manual.out" 2>&1 || exit 1
    fc_ctl drop 0
    [ "$CAMSH_MANUAL" = 1 ] && [ "$CAMSH_OUT" = $'pasted line\n' ] && [ "$CAMSH_RC" = 0 ] &&
        grep -q 'failure 2 of 2' "$T/manual.out" && grep -q 'MANUAL' "$T/manual.out" &&
        grep -q 'echo hi' "$T/manual.out"
)
t_manual_stays_manual() (
    camsh_manual_on
    CAMSH_HOST=127.0.0.1
    camsh_exec 'echo a' <<<$'x\nEND\nn' >/dev/null 2>&1 || exit 1
    [ "$CAMSH_RC" = 1 ] && [ "$CAMSH_MANUAL" = 1 ]
)
t_manual_empty_stdin_fails() (
    camsh_manual_on
    CAMSH_HOST=127.0.0.1
    ! camsh_exec 'echo a' </dev/null >/dev/null 2>&1
)
t_manual_sends_nothing() (
    # In manual mode nothing is sent to the camera (the command log stays unchanged).
    local before after
    before=$(fc_cmds | wc -l)
    camsh_manual_on
    CAMSH_HOST=127.0.0.1
    camsh_exec 'echo never-sent' <<<$'\nEND\ny' >/dev/null 2>&1 || exit 1
    after=$(fc_cmds | wc -l)
    [ "$before" = "$after" ]
)
t_dry_run() (
    local before after out
    before=$(fc_cmds | wc -l)
    UI_DRY=1
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    out=$(camsh_exec 'echo x'; camsh_wait_prompt 'p: ')
    after=$(fc_cmds | wc -l)
    [ "$before" = "$after" ] && printf '%s' "$out" | grep -q 'WOULD run on the camera: echo x'
)
t_prompt_relay() (
    local rc=0
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_start 'printf "Name: "; read x; echo "got=$x"' || exit 1
    camsh_wait_prompt 'Name: ' 10 || exit 1
    camsh_send_line 'abc' || exit 1
    camsh_finish 10 || rc=$?
    [ "$rc" = 0 ] && printf '%s' "$CAMSH_OUT" | grep -q '^got=abc$'
)
t_prompt_command_ends_early() (
    local rc=0
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    camsh_start 'echo no prompt here; false' || exit 1
    camsh_wait_prompt 'Name: ' 5 || rc=$?
    [ "$rc" = 4 ] && [ "$CAMSH_RC" = 1 ]
)
t_connection_refused() (
    local port
    port=$(fc_free_port)
    ! camsh_open 127.0.0.1 "$port"
)
t_net_probe() (
    net_probe 127.0.0.1 "$FC_PORT" "$(fc_free_port)"
    [ "$PROBE_OPEN" = "$FC_PORT" ]
)

for t in t_basic t_iac_cr_filtered t_exit_code t_unique_markers t_two_commands_in_a_row t_timeout_single \
    t_command_limits t_reconnect_once t_manual_after_two_failures t_manual_stays_manual \
    t_manual_empty_stdin_fails t_manual_sends_nothing t_dry_run t_prompt_relay \
    t_prompt_command_ends_early t_connection_refused t_net_probe; do
    check "$t" "$t"
done

echo "camsh_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
