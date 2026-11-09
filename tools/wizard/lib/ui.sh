# shellcheck shell=bash
# Friendly terminal output and prompts. Needs tools/lib/host.sh and log.sh (and tui.sh, optional).
# Two modes: plain line-based prompts (pipes, --plain, no TTY: the behaviour the tests rely on)
# and, on a real terminal, the styled widgets of tui.sh (TUI_ON=1). Same questions, same safety
# rules, same return codes in both. Colours only on a TTY (host_say). All text is English.

UI_DRY=0
UI_LAST_CMD=""
UI_LAST_RC=0

ui_say() { host_say plain "$*"; log_line "SAY $*"; }
ui_bold() { host_say bold "$*"; }
ui_good() { host_say green "$*"; }
ui_warn() { host_say yellow "WARNING: $*"; log_line "WARN $*"; }
ui_err() { host_say red "ERROR: $*" >&2; log_line "ERROR $*"; }
ui_hint() { printf '  What to do: %s\n' "$*"; }
ui_blank() { echo; }

# ui_note_cmd SHOWN RC: remember the last command that ran (the error panel shows a failed one).
ui_note_cmd() {
    UI_LAST_CMD=""
    UI_LAST_RC=0
    if [ "$2" -ne 0 ]; then
        UI_LAST_CMD=$1
        UI_LAST_RC=$2
    fi
    return 0
}

# ui_fail MESSAGE [HINT]: error + what to do, returns 1. On a terminal: a red panel that also
# shows the exact command that failed (when there is one) and the next step.
ui_fail() {
    if [ "${TUI_ON:-0}" = 1 ]; then
        log_line "ERROR $1"
        tui_error_panel "$1" "$UI_LAST_CMD" "$UI_LAST_RC" "${2:-}"
    else
        ui_err "$1"
        [ -z "${2:-}" ] || ui_hint "$2"
    fi
    UI_LAST_CMD=""
    UI_LAST_RC=0
    return 1
}

# ui_require_tty ACTION: destructive or interactive actions need a terminal (rehearsals do not).
ui_require_tty() {
    [ "$UI_DRY" = 1 ] && return 0
    host_is_tty && return 0
    ui_err "$1 needs an interactive terminal and refuses to run without one."
    ui_hint "run ./akam from a terminal, or use --dry-run to rehearse."
    return 2
}

# ui_prompt PROMPT DEFAULT: read a line into ANSWER. In a rehearsal an empty stdin gives DEFAULT.
ui_prompt() {
    ANSWER=""
    if [ "${TUI_ON:-0}" = 1 ] && [ "$UI_DRY" != 1 ]; then
        tui_input "$1" "$2" || return 1
        ANSWER=$TUI_RESULT
        return 0
    fi
    printf '%s ' "$1" >&2
    if ! IFS= read -r ANSWER; then
        if [ "$UI_DRY" = 1 ]; then ANSWER=$2; echo >&2; return 0; fi
        return 1
    fi
    [ -n "$ANSWER" ] || ANSWER=$2
    return 0
}

# ui_secret PROMPT: read a secret without echo into SECRET (never logged).
ui_secret() {
    SECRET=""
    if [ "${TUI_ON:-0}" = 1 ]; then
        tui_input -s "$1" || return 1
        SECRET=$TUI_RESULT
        log_add_secret "$SECRET"
        return 0
    fi
    printf '%s ' "$1" >&2
    if [ -t 0 ]; then
        IFS= read -r -s SECRET || return 1
        echo >&2
    else
        IFS= read -r SECRET || return 1
    fi
    log_add_secret "$SECRET"
}

# ui_secret_ask PROMPT: a secret the user may prefer to see while typing (the Wi-Fi password).
# On a terminal it first asks "Show the password while typing? [Y/n]" (Enter = show) and hides
# the input on n. Without a terminal on stdin (tests, pipes) it reads one visible line, as ui_secret.
# SECRET is never logged.
ui_secret_ask() {
    local show=1 a=""
    SECRET=""
    if [ "${TUI_ON:-0}" = 1 ]; then
        tui_yesno "Show the password while typing?" y || show=0
        if [ "$show" = 1 ]; then
            tui_input -s -v "$1" || return 1
        else
            tui_input -s "$1" || return 1
        fi
        SECRET=$TUI_RESULT
    elif [ -t 0 ] && [ "$UI_DRY" != 1 ]; then
        printf 'Show the password while typing? [Y/n] ' >&2
        IFS= read -r a || return 1
        case $a in n | N | no | No | NO) show=0 ;; esac
        if [ "$show" = 1 ]; then
            printf '%s ' "$1" >&2
            IFS= read -r SECRET || return 1
        else
            ui_secret "$1" || return 1
        fi
    else
        ui_secret "$1" || return 1
    fi
    log_add_secret "$SECRET"
}

# ui_confirm PROMPT: y/N. A rehearsal only shows the question.
ui_confirm() {
    if [ "$UI_DRY" = 1 ]; then ui_say "WOULD ask: $1 [y/N]"; return 0; fi
    if [ "${TUI_ON:-0}" = 1 ]; then tui_yesno "$1" n; return; fi
    host_confirm "$1"
}

# ui_ask_yn PROMPT: like ui_confirm (default No) but a rehearsal is not special-cased: the caller
# already handled it. Refuses without a terminal, like host_confirm.
ui_ask_yn() {
    if [ "${TUI_ON:-0}" = 1 ]; then tui_yesno "$1" n; return; fi
    host_confirm "$1"
}

# ui_pick_route PROMPT LABEL_A LABEL_B TITLE: choose between two ways. Returns 0 for A, 1 for B.
# Plain mode (and rehearsals) ask the same y/N question as before, "yes" meaning A.
ui_pick_route() {
    if [ "${TUI_ON:-0}" != 1 ] || [ "$UI_DRY" = 1 ]; then ui_confirm "$1"; return; fi
    tui_choose "${4:-Which way?}" "$2" "$3" || return 1
    [ "$TUI_INDEX" -eq 0 ]
}

# ui_choose TITLE ITEM...: pick one item. Sets CHOICE_INDEX (0-based). Returns 1 on cancel or EOF.
# Terminal: the arrow-key list. Plain mode: a numbered list and a prompt (Enter = the first item).
# shellcheck disable=SC2034  # CHOICE_INDEX is read by the callers
ui_choose() {
    local title=$1 i=0 it
    shift
    CHOICE_INDEX=-1
    if [ "${TUI_ON:-0}" = 1 ] && [ "$UI_DRY" != 1 ]; then
        tui_choose "$title" "$@" || return 1
        CHOICE_INDEX=$TUI_INDEX
        return 0
    fi
    ui_say "  $title"
    for it in "$@"; do
        i=$((i + 1))
        ui_say "    $i) $it"
    done
    while :; do
        printf '  Number [1]: ' >&2
        IFS= read -r ANSWER || return 1
        ANSWER=${ANSWER:-1}
        case $ANSWER in
            '' | *[!0-9]*) ;;
            *) if [ "$ANSWER" -ge 1 ] && [ "$ANSWER" -le "$#" ]; then CHOICE_INDEX=$((ANSWER - 1)); return 0; fi ;;
        esac
        ui_say "  Please type a number between 1 and $#."
    done
}

# ui_drain_input: throw away what was typed ahead on the real terminal (a key pressed during a long
# command or a sudo prompt must never answer a question that was not on screen yet). Piped input
# (tests, AKAM_FORCE_TTY) is not a terminal and is left alone.
ui_drain_input() {
    local saved k
    [ -t 0 ] || return 0
    saved=$(stty -g 2>/dev/null) || return 0
    stty -icanon -echo min 0 time 1 2>/dev/null || return 0
    while IFS= read -r -s -n 1 -t 0.1 k; do :; done
    stty "$saved" 2>/dev/null || true
}

# ui_danger TITLE QUESTION LINE...: the confirmation of an action that cannot be undone (erasing a card,
# writing the camera flash, untested hardware). A red warning box states exactly what will happen, then ONE
# yes/no choice whose default is No (arrow keys or y/N; Esc, q, an empty answer or the end of the input mean
# No). It needs a real terminal (returns 2 and refuses without one). A rehearsal only shows the question and
# never asks. Returns 0 only for an explicit yes. Nothing is typed: after a yes the wizard itself passes the
# answers on to the tool that asks again (the SD tool, the camera's flash script).
ui_danger() {
    local title=$1 question=$2 l
    shift 2
    if [ "$UI_DRY" = 1 ]; then ui_say "WOULD ask: $question [y/N] (default: No) -- $title: $*"; return 0; fi
    ui_require_tty "$title" || return 2
    if [ "${TUI_ON:-0}" = 1 ]; then
        TUI_BUF=()
        tui_box_build "$TUI_R" "$TUI_I_WARN $title" "$@"
        for l in "${TUI_BUF[@]}"; do printf '%s\n' "$l" >&2; done
        ui_drain_input
        tui_yesno "$question" n
        return
    fi
    ui_blank
    ui_warn "$title"
    for l in "$@"; do ui_say "  $l"; done
    ui_drain_input
    host_confirm "  $question"
}

# ui_banner N TOTAL TITLE
ui_banner() {
    ui_blank
    UI_LAST_CMD=""
    if [ "${TUI_ON:-0}" = 1 ]; then
        tui_rule "Step $1/$2 $TUI_I_DOT $3"
    else
        ui_bold "Step $1/$2 - $3"
    fi
}

# ui_progress LABEL PERCENT: one progress bar for a long camera command (flash_tool prints about
# 200 KB of carriage-return bars). On a terminal widget screen the bar is redrawn in place; in
# plain mode only one line per 10 % is printed, so a log or a pipe never floods.
UI_PROG_LABEL=""
UI_PROG_LAST=-1
UI_PROG_OPEN=0
ui_progress() {
    local label=$1 pct=$2 bar
    if [ "$label" != "$UI_PROG_LABEL" ]; then
        ui_progress_close
        UI_PROG_LABEL=$label
        UI_PROG_LAST=-1
    fi
    if [ "$pct" -gt 100 ]; then pct=100; fi
    if [ "${TUI_ON:-0}" = 1 ]; then
        if [ "$pct" -eq "$UI_PROG_LAST" ]; then return 0; fi
        UI_PROG_LAST=$pct
        tui_bar "$pct" 24
        bar=$REPLY
        printf '\r\e[K    | %s  %s' "$label" "$bar" >&2
        UI_PROG_OPEN=1
        if [ "$pct" -ge 100 ]; then ui_progress_close; fi
        return 0
    fi
    # plain mode: the first value, then every 10 %, then 100 %
    if [ "$UI_PROG_LAST" -lt 0 ] || [ "$pct" -ge $((UI_PROG_LAST + 10)) ] || { [ "$pct" -ge 100 ] && [ "$UI_PROG_LAST" -lt 100 ]; }; then
        UI_PROG_LAST=$pct
        printf '    | %s: %s%%\n' "$label" "$pct"
    fi
}

# ui_progress_close: end the bar line (a bar redrawn in place has no newline yet) and forget a
# finished bar so that the next phase starts from 0.
ui_progress_close() {
    if [ "$UI_PROG_OPEN" = 1 ]; then printf '\n' >&2; fi
    UI_PROG_OPEN=0
    if [ "$UI_PROG_LAST" -ge 100 ]; then
        UI_PROG_LABEL=""
        UI_PROG_LAST=-1
    fi
}
