# shellcheck shell=bash
# Plain, friendly terminal output and prompts. Needs tools/lib/host.sh and log.sh.
# Colours only on a TTY (host_say). All text is English.

UI_DRY=0

ui_say() { host_say plain "$*"; log_line "SAY $*"; }
ui_bold() { host_say bold "$*"; }
ui_good() { host_say green "$*"; }
ui_warn() { host_say yellow "WARNING: $*"; log_line "WARN $*"; }
ui_err() { host_say red "ERROR: $*" >&2; log_line "ERROR $*"; }
ui_hint() { printf '  What to do: %s\n' "$*"; }
ui_blank() { echo; }

# ui_fail MESSAGE [HINT]: error + what to do, returns 1.
ui_fail() {
    ui_err "$1"
    [ -z "${2:-}" ] || ui_hint "$2"
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
    printf '%s ' "$1" >&2
    if [ -t 0 ]; then
        IFS= read -r -s SECRET || return 1
        echo >&2
    else
        IFS= read -r SECRET || return 1
    fi
    log_add_secret "$SECRET"
}

# ui_confirm PROMPT: y/N. A rehearsal only shows the question.
ui_confirm() {
    if [ "$UI_DRY" = 1 ]; then ui_say "WOULD ask: $1 [y/N]"; return 0; fi
    host_confirm "$1"
}

# ui_confirm_typed PROMPT EXPECTED: exact typed text, never skipped, never defaulted.
# A rehearsal only shows the question; it never passes a real confirmation.
ui_confirm_typed() {
    if [ "$UI_DRY" = 1 ]; then ui_say "WOULD ask: $1 (type: $2)"; return 0; fi
    host_confirm_typed "$1" "$2"
}

# ui_banner N TOTAL TITLE
ui_banner() {
    ui_blank
    ui_bold "Step $1/$2 - $3"
}
