# shellcheck shell=bash
# Several cameras, one state each. Sourced by ./akam after state.sh and ui.sh.
#
# Layout under BASE_DIR (default ~/.local/state/akam, or --state-dir):
#   BASE_DIR/state, akam.log, ...     the camera named "default": the state kept by earlier versions of
#                                     the wizard, which is simply that camera (nothing is moved)
#   BASE_DIR/cameras/NAME/state ...   every other camera: its own state file, log and reflash logs
#   BASE_DIR/kit, BASE_DIR/downloads  shared by all cameras (the kit does not depend on the camera)
#   BASE_DIR/current_camera           the name chosen last (./akam --camera NAME, or the menu)
# A camera name is a short label such as "salon": lowercase letters, digits and "-", at most 20
# characters, not starting or ending with "-". It is used in the folder names (~/akam-backup-salon-<date>)
# and as the camera's network host name.
# Expected globals: BASE_DIR, DRY (0/1), CAM_NAME, STATE_DIR (state.sh).

BASE_DIR=""
CAM_NAME="default"
CAMS_FRESH=0
CAM_ASKED=""
CAM_NAME_MAX=20

# cams_name_valid NAME
cams_name_valid() {
    case $1 in
        '' | -* | *- | *[!a-z0-9-]*) return 1 ;;
    esac
    [ "${#1}" -le "$CAM_NAME_MAX" ]
}

# cams_dir NAME: the state directory of that camera.
cams_dir() {
    if [ "$1" = default ]; then printf '%s\n' "$BASE_DIR"; else printf '%s\n' "$BASE_DIR/cameras/$1"; fi
}

# cams_slug: "salon-" for a named camera, empty for "default" (folder names keep their old form).
cams_slug() {
    if [ "$CAM_NAME" = default ]; then return 0; fi
    printf '%s-' "$CAM_NAME"
}

# cams_known: the names of the cameras that have a state, one per line, "default" first.
cams_known() {
    local d n
    if [ -f "$BASE_DIR/state" ] || [ -f "$BASE_DIR/state.dry" ] || [ "$CAM_NAME" = default ]; then echo default; fi
    for d in "$BASE_DIR"/cameras/*; do
        [ -d "$d" ] || continue
        n=${d##*/}
        if cams_name_valid "$n"; then echo "$n"; fi
    done
}

# cams_summary NAME: "3/9 steps done  192.168.1.50" read from the camera's state file.
cams_summary() {
    local f n ip
    f=$(cams_dir "$1")/state
    n=0
    ip=""
    if [ -f "$f" ]; then
        n=$(grep -c '^step_[a-z_]*_done=' "$f" || true)
        ip=$({ grep -m1 '^camera_ip=' "$f" || true; } | cut -d= -f2-)
    fi
    printf '%s/%s steps done%s\n' "$n" "$(step_count)" "${ip:+  $ip}"
}

# cams_current_read: the name stored by the last run ("" when none).
cams_current_read() {
    local n=""
    if [ -f "$BASE_DIR/current_camera" ]; then
        IFS= read -r n <"$BASE_DIR/current_camera" || true
    fi
    if cams_name_valid "$n"; then printf '%s\n' "$n"; fi
}

# cams_remember NAME: write current_camera (never in a rehearsal).
cams_remember() {
    [ "$DRY" = 1 ] && return 0
    (umask 077 && printf '%s\n' "$1" >"$BASE_DIR/current_camera.tmp.$$" && mv "$BASE_DIR/current_camera.tmp.$$" "$BASE_DIR/current_camera")
    return 0
}

# cams_init BASE WANTED: pick the camera. WANTED = --camera NAME ("" when not given). Prints nothing;
# sets BASE_DIR, CAM_NAME and CAMS_FRESH (1 on a first run: no camera has any state yet).
# Order: --camera, the one used last, then "default".
cams_init() {
    BASE_DIR=$1
    CAMS_FRESH=0
    mkdir -p "$BASE_DIR"
    chmod 700 "$BASE_DIR" 2>/dev/null || true
    if [ ! -f "$BASE_DIR/current_camera" ] && [ ! -f "$BASE_DIR/state" ] && [ ! -d "$BASE_DIR/cameras" ]; then CAMS_FRESH=1; fi
    if [ -n "$2" ]; then
        CAM_NAME=$2
    else
        CAM_NAME=$(cams_current_read)
        CAM_NAME=${CAM_NAME:-default}
    fi
}

# cams_open: select CAM_NAME for real: state file, log. Called once after cams_init and again after a switch.
cams_open() {
    local d
    d=$(cams_dir "$CAM_NAME")
    state_init "$d" "$DRY"
    log_init "$d"
    log_line "CAMERA $CAM_NAME"
}

# cams_switch NAME: change camera in the middle of a run (menu key c, new camera).
cams_switch() {
    if ! cams_name_valid "$1"; then return 1; fi
    CAM_NAME=$1
    cams_open
    cams_remember "$CAM_NAME"
}

# cams_ask_name: ask a short name; sets ANSWER-like result in CAM_ASKED. Empty answer = "default".
cams_ask_name() {
    local a
    ui_say "  Give this camera a short name, for example 'salon' (lowercase letters, digits and '-', at most $CAM_NAME_MAX)."
    ui_say "  It is used for the backup and image folders and as the camera's network name. Several cameras can be set up, each with its own progress."
    while :; do
        ui_prompt "  Camera name [default]:" "default" || return 1
        a=$ANSWER
        if cams_name_valid "$a"; then
            CAM_ASKED=$a
            return 0
        fi
        ui_say "  '$a' is not valid: use lowercase letters, digits and '-' (at most $CAM_NAME_MAX, not starting or ending with '-')."
    done
}

# cams_first_run_name: on the very first run of the interactive menu, ask for the camera's name (before
# any state is created). Nothing is asked in a rehearsal, with --camera or without a terminal.
# Returns 0 when CAM_NAME was set from the answer.
cams_first_run_name() {
    if [ "$CAMS_FRESH" != 1 ] || [ "$DRY" = 1 ] || [ -n "$OPT_CAMERA" ] || ! host_is_tty; then return 1; fi
    cams_ask_name || return 1
    CAM_NAME=$CAM_ASKED
}

# cmd_cameras: list the known cameras.
cmd_cameras() {
    local n mark
    ui_bold "Cameras (state in $BASE_DIR)"
    while IFS= read -r n; do
        mark=" "
        if [ "$n" = "$CAM_NAME" ]; then mark="*"; fi
        printf '  %s %-20s %s\n' "$mark" "$n" "$(cams_summary "$n")"
    done < <(cams_known)
    ui_say "  Switch with: ./akam --camera NAME   (a new name starts a new camera)"
}
