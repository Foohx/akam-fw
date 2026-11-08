# shellcheck shell=bash
# Wizard log: akam.log in the state directory. Every command is logged BEFORE it runs.
# Secrets are masked: values registered with log_add_secret, plus "AKAM_SD_PASS=", "pass=",
# "password=" and "psk=" assignments. The state file and the log never receive a secret.

LOG_FILE=""
LOG_SECRETS=()

# log_init DIR: create DIR (mode 700) and select DIR/akam.log.
log_init() {
    mkdir -p "$1"
    chmod 700 "$1" 2>/dev/null || true
    LOG_FILE="$1/akam.log"
    (umask 077 && : >>"$LOG_FILE")
    chmod 600 "$LOG_FILE" 2>/dev/null || true
}

# log_add_secret VALUE: from now on VALUE is masked everywhere (ignored when empty).
log_add_secret() {
    [ -n "${1:-}" ] || return 0
    LOG_SECRETS+=("$1")
}

# log_has_secret TEXT: true when TEXT contains a registered secret.
log_has_secret() {
    local s
    for s in ${LOG_SECRETS[@]+"${LOG_SECRETS[@]}"}; do
        case $1 in *"$s"*) return 0 ;; esac
    done
    return 1
}

# log_redact TEXT: print TEXT with secrets replaced by ***.
log_redact() {
    local t=$1 s
    for s in ${LOG_SECRETS[@]+"${LOG_SECRETS[@]}"}; do
        t=${t//"$s"/***}
    done
    printf '%s\n' "$t" | sed -E 's/(AKAM_SD_PASS=)[^ ]*/\1***/g; s/((pass(word)?|psk)=)[^ ]*/\1***/Ig'
}

# log_line TEXT...: append a time-stamped, redacted line (no-op before log_init).
log_line() {
    [ -n "$LOG_FILE" ] || return 0
    local msg
    msg=$(log_redact "$*")
    printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$msg" >>"$LOG_FILE"
}
