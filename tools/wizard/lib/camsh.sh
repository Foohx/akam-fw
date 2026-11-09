# shellcheck shell=bash
# camsh: a scripted telnet session with the camera's BusyBox shell (maintenance telnet, port 24),
# written in pure bash over /dev/tcp. No telnet, expect, nc or python on the PC. bash 3.2 compatible.
#
# The PC always connects OUT to the camera (the PC firewall blocks inbound connections).
#
# How a command is run
#   - one command per send, never longer than 350 bytes, no newline in it;
#   - every command is wrapped as: echo "__AKAM_BEG_<nonce>_<n>_"x; CMD; echo "__AKAM_DONE_<nonce>_<n>:"$?"__".
#     Both markers are built by the shell, so the pty echo of the typed line (which holds the quotes)
#     never matches them. Everything received before the BEG marker is dropped: the BusyBox line
#     editor echoes the typed line, wraps it with newlines every 80 columns and emits ESC[6n, none of
#     which may leak into the output of the command;
#   - the answer is read byte by byte with a read timeout; telnet IAC negotiation bytes and
#     carriage returns are dropped; the timeout is an inactivity timeout (reset on every byte).
#
# Failure policy (spec D4): a command that gets no answer (timeout, closed session, refused
# connection) counts as a failure. After 2 consecutive failures, or with ./akam --manual-telnet,
# the session switches to MANUAL MODE for the rest of the run: camsh prints the exact command, the
# user pastes it in their own telnet window and pastes the output back (or confirms). The
# wizard never invents the output of a command.
#
# The "YES" typed to the flash script is NEVER sent from here: the callers relay the prompt and
# only send what a human typed (camsh_send_line is a plain transport).
#
# Globals set by the API
#   CAMSH_OUT   output of the last command (echo and marker removed, CR removed)
#   CAMSH_RC    exit code of the last command (manual mode: 0 or 1 from the user's answer)
#   CAMSH_MANUAL 1 once manual mode is active
# Needs ui.sh and log.sh. Test hooks: AKAM_PORT_TELNET (default 24), AKAM_CAMSH_TIMEOUT (default 30).

CAMSH_HOST=""
CAMSH_PORT=24
CAMSH_OPEN=0
CAMSH_MANUAL=${CAMSH_MANUAL:-0}
CAMSH_FAILS=0
CAMSH_SEQ=0
CAMSH_NONCE=""
CAMSH_RE=""
CAMSH_BEG=""
CAMSH_BEG_SEEN=""
CAMSH_PART=""
CAMSH_OUT=""
CAMSH_RC=""
CAMSH_STREAM=0
CAMSH_STREAM_SKIP=""
CAMSH_ECHO_SKIP=""
CAMSH_MAX_OUT=1048576
CAMSH_MAX_CMD=350
# A progress bar of flash_tool: "Writing: [====>   ]  45% (n/total)". These segments end with a
# carriage return (no newline): they never go to CAMSH_OUT, they only feed ui_progress.
CAMSH_PROG_RE='^[[:space:]]*([A-Za-z][A-Za-z +/-]*): \[[=> ]*\][[:space:]]*([0-9]{1,3})%'
CAMSH_ESC=$'\033'

# ------------------------------------------------------------------ plain network helpers
# net_port_open HOST PORT [SECS]: true when a TCP connection succeeds within SECS (default 3).
# The connection attempt runs in the background so that a silent host cannot block the wizard.
# NEVER use it on a port where a one-shot listener waits (the probe would consume the accept).
net_port_open() {
    local h=$1 p=$2 t=${3:-3} pid i=0 max
    max=$((t * 5))
    ( exec 9<>"/dev/tcp/$h/$p" ) 2>/dev/null &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$i" -ge "$max" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 1
        fi
        sleep 0.2
        i=$((i + 1))
    done
    wait "$pid" 2>/dev/null
}

# net_probe HOST PORT...: sets PROBE_OPEN to the list of open ports (parallel, 3 s).
net_probe() {
    local h=$1 p i=0 d
    shift
    PROBE_OPEN=""
    d=$(mktemp -d)
    for p in "$@"; do
        ( if net_port_open "$h" "$p" 3; then : >"$d/$p"; fi ) &
    done
    wait
    for p in "$@"; do
        if [ -e "$d/$p" ]; then PROBE_OPEN="$PROBE_OPEN$p "; fi
        i=$((i + 1))
    done
    rm -rf "$d"
    PROBE_OPEN=${PROBE_OPEN% }
}

# ------------------------------------------------------------------ session
camsh_dry() { [ "${UI_DRY:-0}" = 1 ]; }

# camsh_open HOST [PORT]: connect. Manual mode: nothing to open. Returns 1 when refused.
camsh_open() {
    CAMSH_HOST=$1
    CAMSH_PORT=${2:-${AKAM_PORT_TELNET:-24}}
    camsh_close
    CAMSH_PART=""
    CAMSH_NONCE="$$$((RANDOM % 1000))"
    [ "$CAMSH_MANUAL" = 1 ] && return 0
    camsh_dry && return 0
    log_line "CAM open $CAMSH_HOST:$CAMSH_PORT"
    if ! net_port_open "$CAMSH_HOST" "$CAMSH_PORT" 5; then
        log_line "CAM open failed"
        return 1
    fi
    if ! { exec 7<>"/dev/tcp/$CAMSH_HOST/$CAMSH_PORT"; } 2>/dev/null; then
        log_line "CAM open failed (exec)"
        return 1
    fi
    CAMSH_OPEN=1
}

camsh_close() {
    if [ "$CAMSH_OPEN" = 1 ]; then
        exec 7<&- 7>&- || true
        log_line "CAM close"
    fi
    CAMSH_OPEN=0
}

# camsh_manual_on: switch to manual mode (--manual-telnet or after 2 failures).
camsh_manual_on() {
    camsh_close
    CAMSH_MANUAL=1
}

# ------------------------------------------------------------------ reading
# camsh_stream_set ON [SKIP_REGEX]: show (ON=1) the output of commands as it arrives, minus lines
# matching SKIP_REGEX (progress noise).
camsh_stream_set() {
    CAMSH_STREAM=$1
    CAMSH_STREAM_SKIP=${2:-}
}

camsh_show_line() { # line: shown to the user when streaming
    [ "$CAMSH_STREAM" = 1 ] || return 0
    if [ -n "$CAMSH_STREAM_SKIP" ] && [[ $1 =~ $CAMSH_STREAM_SKIP ]]; then return 0; fi
    ui_progress_close
    printf '    | %s\n' "$1"
}

# camsh_strip_ansi TEXT: set REPLY to TEXT without terminal escape sequences. BusyBox's ls colours
# its names even over telnet (ESC[1;32m name ESC[m), which broke every comparison of a name.
camsh_strip_ansi() {
    REPLY=$1
    case $1 in
        *"$CAMSH_ESC"*)
            REPLY=$(printf '%s' "$1" | LC_ALL=C sed -E "s#${CAMSH_ESC}\\[[0-9;?]*[ -/]*[@-~]##g; s#${CAMSH_ESC}[()][A-Za-z0-9]##g")
            ;;
    esac
}

# _camsh_progress LINE: true (and shown as a progress bar when streaming) when LINE is one.
_camsh_progress() {
    [[ $1 =~ $CAMSH_PROG_RE ]] || return 1
    if [ "$CAMSH_STREAM" = 1 ]; then ui_progress "${BASH_REMATCH[1]}" "$((10#${BASH_REMATCH[2]}))"; fi
    return 0
}

# camsh_out_has TEXT: true when the output of the last command contains TEXT. A case pattern, not
# "printf | grep -q": with pipefail the printf of a big output dies of SIGPIPE when grep -q exits at
# the first match, and the pipeline then reports failure (this hid a real flash success).
camsh_out_has() {
    case $CAMSH_OUT in
        *"$1"*) return 0 ;;
    esac
    return 1
}

# _camsh_line LINE: handle one completed line. Returns 0 when it is the end marker.
# Lines before the BEG marker (echo of the typed command, prompt, terminal queries) are dropped.
_camsh_line() {
    local line=$1 pre
    camsh_strip_ansi "$line"
    line=$REPLY
    if [ -n "$CAMSH_RE" ] && [[ $line =~ $CAMSH_RE ]]; then
        CAMSH_RC=${BASH_REMATCH[1]}
        CAMSH_RE=""
        pre=${line%%__AKAM_DONE_*}
        # an output without a final newline ends right before the marker
        if [ -n "$CAMSH_BEG_SEEN" ] && [ -n "$pre" ] && [ "${#CAMSH_OUT}" -lt "$CAMSH_MAX_OUT" ]; then
            CAMSH_OUT="$CAMSH_OUT$pre"$'\n'
            camsh_show_line "$pre"
        fi
        return 0
    fi
    if [ -n "$CAMSH_BEG" ] && [ -z "$CAMSH_BEG_SEEN" ]; then
        case $line in
            *"$CAMSH_BEG"*) CAMSH_BEG_SEEN=1 ;;
        esac
        return 1
    fi
    if _camsh_progress "$line"; then return 1; fi
    if [ -n "$CAMSH_ECHO_SKIP" ]; then
        local e=$CAMSH_ECHO_SKIP
        CAMSH_ECHO_SKIP=""
        [ "$line" = "$e" ] && return 1
    fi
    if [ "${#CAMSH_OUT}" -lt "$CAMSH_MAX_OUT" ]; then CAMSH_OUT="$CAMSH_OUT$line"$'\n'; fi
    camsh_show_line "$line"
    return 1
}

# _camsh_read TIMEOUT [PROMPT]: read until the end marker of the pending command (return 0), until
# the partial last line ends with PROMPT (return 3), the inactivity TIMEOUT (return 1) or the end
# of the session (return 2).
_camsh_read() {
    local tmo=$1 prompt=${2:-} line c rc iac=0 deadline
    local LC_ALL=C
    line=$CAMSH_PART
    CAMSH_PART=""
    deadline=$((SECONDS + tmo))
    while :; do
        rc=0
        IFS= read -r -t 1 -n 1 c <&7 || rc=$?
        if [ "$rc" -eq 0 ]; then
            deadline=$((SECONDS + tmo))
            if [ -z "$c" ]; then
                if _camsh_line "$line"; then return 0; fi
                line=""
                continue
            fi
            case $iac in
                0)
                    case $c in
                        $'\377') iac=1 ;;
                        $'\r')
                            # flash_tool redraws its bar with \r: a segment that is a bar is consumed now
                            if [ -n "$CAMSH_BEG_SEEN" ] && [[ $line == *"%"* ]] && _camsh_progress "$line"; then line=""; fi
                            ;;
                        *) line="$line$c" ;;
                    esac
                    ;;
                1)
                    case $c in
                        $'\373' | $'\374' | $'\375' | $'\376') iac=2 ;; # WILL WONT DO DONT + option
                        $'\372') iac=3 ;;                               # SB ... IAC SE
                        *) iac=0 ;;                                     # 2-byte command (or IAC IAC)
                    esac
                    ;;
                2) iac=0 ;;
                3) if [ "$c" = $'\377' ]; then iac=4; fi ;;
                4) if [ "$c" = $'\360' ]; then iac=0; else iac=3; fi ;;
            esac
        elif [ "$rc" -gt 128 ]; then
            # Idle for one second: look for the prompt in the unfinished line.
            if [ -n "$prompt" ] && [ -n "$CAMSH_BEG_SEEN" ] && [ -n "$line" ] && [[ $line == *"$prompt" || $line == *"${prompt% }" ]]; then
                camsh_show_line "$line"
                CAMSH_PART=""
                return 3
            fi
            if [ "$SECONDS" -ge "$deadline" ]; then
                CAMSH_PART=$line
                return 1
            fi
        else
            CAMSH_PART=$line
            CAMSH_OPEN=0
            return 2
        fi
    done
}

# ------------------------------------------------------------------ one command
# camsh_start CMD: send CMD followed by the end marker; do not wait. Returns 1 when the session is
# unusable. Manual mode: show the command and wait until the user says it is running.
camsh_start() {
    local cmd=$1
    case $cmd in
        *$'\n'*) ui_err "camsh: a command must be a single line"; return 2 ;;
    esac
    if [ "${#cmd}" -gt "$CAMSH_MAX_CMD" ]; then
        ui_err "camsh: command too long (${#cmd} bytes, max $CAMSH_MAX_CMD)"
        return 2
    fi
    CAMSH_OUT=""
    CAMSH_RC=""
    log_line "CAM> $cmd"
    if camsh_dry; then
        ui_say "  WOULD run on the camera: $cmd"
        return 0
    fi
    if [ "$CAMSH_MANUAL" = 1 ]; then
        _camsh_manual_show "$cmd"
        return 0
    fi
    [ "$CAMSH_OPEN" = 1 ] || return 1
    CAMSH_SEQ=$((CAMSH_SEQ + 1))
    CAMSH_RE="__AKAM_DONE_${CAMSH_NONCE}_${CAMSH_SEQ}:([0-9]+)__"
    CAMSH_BEG="__AKAM_BEG_${CAMSH_NONCE}_${CAMSH_SEQ}_x"
    CAMSH_BEG_SEEN=""
    if ! printf 'echo "__AKAM_BEG_%s_%s_"x; %s; echo "__AKAM_DONE_%s_%s:"$?"__"\n' \
        "$CAMSH_NONCE" "$CAMSH_SEQ" "$cmd" "$CAMSH_NONCE" "$CAMSH_SEQ" >&7 2>/dev/null; then
        CAMSH_OPEN=0
        return 1
    fi
}

# camsh_finish [TIMEOUT]: wait for the end of the command started by camsh_start. 0 when the
# marker came (CAMSH_RC set, the command may still have failed), 1 timeout, 2 session closed.
camsh_finish() {
    local tmo=${1:-${AKAM_CAMSH_TIMEOUT:-30}} rc=0
    if camsh_dry; then CAMSH_RC=0; return 0; fi
    if [ "$CAMSH_MANUAL" = 1 ]; then
        _camsh_manual_collect
        return $?
    fi
    [ "$CAMSH_OPEN" = 1 ] || return 2
    _camsh_read "$tmo" "" || rc=$?
    if [ "$rc" -eq 0 ]; then
        log_line "CAM rc=$CAMSH_RC"
        return 0
    fi
    log_line "CAM no answer (read rc=$rc)"
    return "$rc"
}

# camsh_wait_prompt PROMPT [TIMEOUT]: stream the output of the started command until its last
# unfinished line ends with PROMPT. 0 prompt seen, 4 the command ended instead (CAMSH_RC set),
# 1 timeout, 2 session closed. Manual mode returns 5: the caller must guide the user.
camsh_wait_prompt() {
    local prompt=$1 tmo=${2:-${AKAM_CAMSH_TIMEOUT:-30}} rc=0
    if camsh_dry; then return 0; fi
    [ "$CAMSH_MANUAL" = 1 ] && return 5
    [ "$CAMSH_OPEN" = 1 ] || return 2
    _camsh_read "$tmo" "$prompt" || rc=$?
    case $rc in
        3) return 0 ;;
        0) return 4 ;;
        *) return "$rc" ;;
    esac
}

# camsh_send_line TEXT: transport only. Callers must pass what a human typed.
camsh_send_line() {
    camsh_dry && return 0
    [ "$CAMSH_OPEN" = 1 ] || return 1
    CAMSH_ECHO_SKIP=$1
    printf '%s\n' "$1" >&7 2>/dev/null || { CAMSH_OPEN=0; return 1; }
}

# camsh_interrupt: send Ctrl-C (ends a foreground command such as a stuck nc).
camsh_interrupt() {
    camsh_dry && return 0
    [ "$CAMSH_OPEN" = 1 ] || return 1
    printf '\003' >&7 2>/dev/null || { CAMSH_OPEN=0; return 1; }
}

# camsh_exec CMD [TIMEOUT]: run one command and wait. Reopens the session once; after 2 failures
# in a row it falls back to manual mode (and stays there). Returns 0 when the command ran
# (look at CAMSH_RC for its exit code), 1 when nothing could be obtained.
camsh_exec() {
    local cmd=$1 tmo=${2:-${AKAM_CAMSH_TIMEOUT:-30}} rc
    camsh_dry && { camsh_start "$cmd"; return 0; }
    while [ "$CAMSH_MANUAL" != 1 ]; do
        rc=0
        if [ "$CAMSH_OPEN" != 1 ]; then camsh_open "$CAMSH_HOST" "$CAMSH_PORT" || rc=1; fi
        if [ "$rc" -eq 0 ]; then
            camsh_start "$cmd" || rc=$?
            [ "$rc" -eq 2 ] && return 1 # a malformed command is a bug of the caller, not a failure
        fi
        if [ "$rc" -eq 0 ]; then camsh_finish "$tmo" || rc=$?; fi
        if [ "$rc" -eq 0 ]; then CAMSH_FAILS=0; return 0; fi
        CAMSH_FAILS=$((CAMSH_FAILS + 1))
        camsh_close
        ui_warn "the camera did not answer (failure $CAMSH_FAILS of 2)."
        if [ "$CAMSH_FAILS" -ge 2 ]; then
            ui_warn "switching to MANUAL mode: you will paste each command in your own telnet window."
            CAMSH_MANUAL=1
        fi
    done
    camsh_start "$cmd" || return 1
    camsh_finish "$tmo"
}

# camsh_ok CMD [TIMEOUT]: camsh_exec, and the command must also exit with 0.
camsh_ok() {
    camsh_exec "$@" || return 1
    [ "$CAMSH_RC" = 0 ]
}

# camsh_line N: line N (1-based) of CAMSH_OUT.
camsh_line() { printf '%s\n' "$CAMSH_OUT" | sed -n "${1}p"; }

# ------------------------------------------------------------------ manual mode
_camsh_manual_show() {
    ui_blank
    ui_bold "MANUAL STEP - in your own telnet session to the camera ($CAMSH_HOST port $CAMSH_PORT)"
    ui_say "  Run exactly this ONE command (copy it, paste it, press Enter):"
    ui_say ""
    ui_say "      $1"
    ui_say ""
}

# _camsh_manual_collect: ask for the pasted output and for the result. Empty stdin = failure.
_camsh_manual_collect() {
    local l out="" ans
    ui_say "  Paste the output here (nothing to paste? skip this), then type END on a line of its own."
    while :; do
        printf '  > ' >&2
        if ! IFS= read -r l; then
            ui_err "no input: cannot continue in manual mode."
            return 1
        fi
        l=${l%$'\r'}
        [ "$l" = END ] && break
        out="$out$l"$'\n'
    done
    CAMSH_OUT=$out
    printf '  Did the command succeed (no error message)? [y/N] ' >&2
    if ! IFS= read -r ans; then return 1; fi
    case $ans in y | Y | yes | YES | Yes) CAMSH_RC=0 ;; *) CAMSH_RC=1 ;; esac
    log_line "CAM manual rc=$CAMSH_RC"
    return 0
}

# camsh_manual_wait TEXT: manual mode only; ask the user to press Enter when TEXT is done.
camsh_manual_wait() {
    local ans
    ui_say "  $1"
    printf '  Press Enter when done (or type q to stop): ' >&2
    IFS= read -r ans || return 1
    [ "$ans" != q ]
}
