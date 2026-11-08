# shellcheck shell=bash
# Resumable state: a plain KEY=VALUE file, never sourced, never holds a secret.
# Real runs use STATE_DIR/state; --dry-run uses STATE_DIR/state.dry so that a rehearsal never
# marks a real step as done. Keys: [a-z][a-z0-9_]*, values: one line.
# Needs log.sh (secret check).

STATE_DIR=""
STATE_FILE=""
STATE_DRY=0

# state_default_dir: XDG state directory for Akam.
state_default_dir() {
    printf '%s\n' "${AKAM_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/akam}"
}

# state_init DIR DRY: select the state file (DRY=1 selects state.dry).
state_init() {
    STATE_DIR=$1
    STATE_DRY=$2
    mkdir -p "$STATE_DIR"
    chmod 700 "$STATE_DIR" 2>/dev/null || true
    if [ "$STATE_DRY" = 1 ]; then STATE_FILE="$STATE_DIR/state.dry"; else STATE_FILE="$STATE_DIR/state"; fi
    [ -f "$STATE_FILE" ] || (umask 077 && : >"$STATE_FILE")
    chmod 600 "$STATE_FILE" 2>/dev/null || true
}

# state_reset_dry: forget a previous rehearsal.
state_reset_dry() {
    (umask 077 && : >"$STATE_DIR/state.dry")
    chmod 600 "$STATE_DIR/state.dry" 2>/dev/null || true
}

# state_get KEY: print the value (empty when unset).
state_get() {
    { grep -m1 "^$1=" "$STATE_FILE" 2>/dev/null || true; } | cut -d= -f2-
}

# state_set KEY VALUE: atomic update. Refuses a bad key, a multi-line value or a secret.
state_set() {
    local key=$1 val=$2 tmp
    case $key in
        [a-z]*) ;;
        *) echo "state_set: bad key '$key'" >&2; return 1 ;;
    esac
    case $key in *[!a-z0-9_]*) echo "state_set: bad key '$key'" >&2; return 1 ;; esac
    case $val in *$'\n'*) echo "state_set: multi-line value for '$key'" >&2; return 1 ;; esac
    if log_has_secret "$val"; then
        echo "state_set: refusing to store a secret in '$key'" >&2
        return 1
    fi
    tmp="$STATE_FILE.tmp.$$"
    (umask 077 && { grep -v "^$key=" "$STATE_FILE" 2>/dev/null || true; printf '%s=%s\n' "$key" "$val"; } >"$tmp")
    mv "$tmp" "$STATE_FILE"
}

# state_unset KEY
state_unset() {
    local tmp="$STATE_FILE.tmp.$$"
    (umask 077 && { grep -v "^$1=" "$STATE_FILE" 2>/dev/null || true; } >"$tmp")
    mv "$tmp" "$STATE_FILE"
}

# state_key_of STEP: step name -> state key prefix (get-kit -> get_kit).
state_key_of() {
    printf '%s\n' "$1" | tr '-' '_'
}

# state_mark_done STEP / state_clear_done STEP / state_is_marked STEP
state_mark_done() { state_set "step_$(state_key_of "$1")_done" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"; }
state_clear_done() { state_unset "step_$(state_key_of "$1")_done"; }
state_is_marked() { [ -n "$(state_get "step_$(state_key_of "$1")_done")" ]; }
