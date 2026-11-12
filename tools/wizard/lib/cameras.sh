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

# cams_summary NAME: "3/9 steps done  192.0.2.50" read from the camera's state file.
cams_summary() {
    local f n ip last st
    f=$(cams_dir "$1")/state
    n=0
    ip=""
    if [ -f "$f" ]; then
        n=$(grep -c '^step_[a-z_]*_done=' "$f" || true)
        ip=$({ grep -m1 '^camera_ip=' "$f" || true; } | cut -d= -f2-)
    fi
    last=""
    if [ -f "$f" ]; then
        for st in $WIZ_STEPS; do
            if grep -q "^step_${st//-/_}_done=" "$f"; then last=$st; fi
        done
    fi
    printf '%s/%s steps done%s%s\n' "$n" "$(step_count)" "${ip:+  $ip}" "${last:+  last: $last}"
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
    # What was given on the command line belonged to the previous camera: an address, a backup folder
    # or an image must never be applied to another camera (the SSH key is the user's, it stays).
    # shellcheck disable=SC2034  # read by the steps, set by ./akam
    OPT_IP="" OPT_BACKUP_DIR="" OPT_IMAGE="" OPT_OUT=""
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

# cams_has_legacy: progress from before cameras had names sits in BASE/state (that camera is "default").
cams_has_legacy() {
    [ ! -f "$BASE_DIR/current_camera" ] && [ ! -d "$BASE_DIR/cameras" ] && [ -f "$BASE_DIR/state" ] &&
        grep -q '^step_[a-z_]*_done=' "$BASE_DIR/state"
}

# cams_move_files OLD NEW: move the state, log and reflash logs of OLD into the directory of NEW (never
# overwrites). The kit and the downloads are shared and stay where they are.
cams_move_files() {
    local od nd f
    od=$(cams_dir "$1")
    nd=$(cams_dir "$2")
    if [ -e "$nd/state" ]; then return 1; fi
    mkdir -p "$nd"
    chmod 700 "$nd" 2>/dev/null || true
    if [ "$1" = default ] && [ -f "$od/state" ] && [ ! -e "$od/state.before-rename.bak" ]; then
        # the progress of earlier versions is the user's: keep an untouched copy before anything moves
        (umask 077 && cp -p "$od/state" "$od/state.before-rename.bak") ||
            { ui_warn "could not keep a backup copy of $od/state: not moving anything."; return 1; }
    fi
    if [ "$1" = default ]; then
        for f in state state.dry akam.log reflash.log reflash-precheck.log rtsp_auth.err; do
            if [ -e "$od/$f" ]; then mv "$od/$f" "$nd/$f"; fi
        done
    else
        for f in "$od"/* "$od"/.[!.]*; do
            if [ -e "$f" ]; then mv "$f" "$nd/"; fi
        done
        rmdir "$od" 2>/dev/null || true
    fi
}

# cams_first_run_legacy: the first menu run after an update: the progress already made belongs to one
# camera. Ask what to call it (Enter keeps "default") and move its files. Sets CAM_NAME. Returns 1 when
# nothing was asked.
cams_first_run_legacy() {
    local a
    if [ "$DRY" = 1 ] || [ -n "$OPT_CAMERA" ] || ! host_is_tty || ! cams_has_legacy; then return 1; fi
    ui_say "  The progress saved by an earlier version belongs to one camera. What is its name?"
    ui_say "  (Enter keeps 'default'. Choose the name you want to see in the menu and in the folder names.)"
    cams_ask_name || return 1
    a=$CAM_ASKED
    if [ "$a" != default ]; then
        cams_move_files default "$a" || { ui_warn "a camera called '$a' already has progress: keeping the name 'default'."; return 1; }
    fi
    CAM_NAME=$a
}

# cmd_rename_camera NEW: give the current camera another name (its progress moves with it).
cmd_rename_camera() {
    local new=$1 old=$CAM_NAME
    if ! cams_name_valid "$new"; then
        ui_fail "'$new' is not a valid camera name." "use lowercase letters, digits and '-' (at most $CAM_NAME_MAX, not starting or ending with '-')."
        return 1
    fi
    if [ "$new" = "$old" ]; then ui_say "  This camera is already called $new."; return 0; fi
    if [ "$DRY" = 1 ]; then ui_say "  WOULD rename the camera '$old' to '$new' (its state and logs move to $(cams_dir "$new"))."; return 0; fi
    if [ -e "$(cams_dir "$new")/state" ]; then
        ui_fail "a camera called '$new' already exists." "pick another name (./akam cameras lists them)."
        return 1
    fi
    cams_move_files "$old" "$new" || return 1
    CAM_NAME=$new
    cams_open
    cams_remember "$new"
    ui_good "  Camera '$old' is now called '$new'. Backup and image folders already made keep their names."
}

# cmd_register: ./akam --camera NAME --ip IP register [--backup-dir DIR]. A camera that already runs Akam
# (flashed before, or by hand): no backup, image or flash is redone. It probes the camera, records its
# address, and marks the install steps done so that update, rtsp-auth, hostname and restore are usable.
# Nothing is recorded unless SSH works and /dev/mtd7 reads back over SSH (its md5 is kept as flash_md5).
cmd_register() {
    local sp rp cp tp p reg_out reg_md5 reg_host
    step_banner_free "Register a camera that already runs Akam"
    ui_say "  Why: use update, the video password and the network name on a camera that is already installed. Risk: none (only this PC's notes change)."
    if [ -z "$OPT_IP" ]; then
        ui_fail "register needs the camera's address." "run: ./akam --camera NAME --ip ADDRESS register [--backup-dir DIR]"
        return 1
    fi
    CAM_IP=$OPT_IP
    if [ "$UI_DRY" != 1 ]; then cam_valid_host "$CAM_IP" || { ui_fail "'$CAM_IP' is not a valid address." "use the camera's IPv4 address."; return 1; }; fi
    sp=$(cam_port ssh) rp=$(cam_port rtsp) cp=$(cam_port ctl) tp=$(cam_port telnet)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD probe $CAM_IP (ports $sp, $rp, $cp open and $tp closed = Akam), check the backup folder given with --backup-dir, log in over SSH and read /dev/mtd7 back,"
        ui_say "  then record the address and mark find-camera, flash and verify (and backup, with a backup folder) as done for camera '$CAM_NAME'."
        return 0
    fi
    net_probe "$CAM_IP" "$sp" "$rp" "$cp" "$tp"
    ui_say "  Open ports: ${PROBE_OPEN:-none}"
    for p in "$sp" "$rp" "$cp"; do
        case " $PROBE_OPEN " in
            *" $p "*) ;;
            *)
                ui_fail "$CAM_IP does not look like a camera running Akam (SSH, video and control ports must answer; port $p does not)." \
                    "check the address in your router, and that the camera has finished booting."
                return 1
                ;;
        esac
    done
    case " $PROBE_OPEN " in
        *" $tp "*) ui_fail "port $tp (maintenance telnet) is open: this camera still runs the toolkit card." "finish with ./akam step verify instead, or take the card out and restart it."; return 1 ;;
    esac
    if [ -n "$OPT_BACKUP_DIR" ]; then
        if [ ! -d "$OPT_BACKUP_DIR" ]; then ui_fail "not a folder: $OPT_BACKUP_DIR" ""; return 1; fi
        backup_check "$(cd "$OPT_BACKUP_DIR" && pwd)" || return 1
    else
        ui_warn "no --backup-dir: restore and the hardware check need your backup folder. Register it later with: ./akam --camera $CAM_NAME backup --backup-dir DIR"
    fi
    cam_key
    # Never record flash and verify as done for a camera that was not really looked at: SSH must work and
    # the APP partition must read back (its md5 is kept as the proof of what the camera runs).
    if ! cam_ssh_ok; then
        ui_fail "SSH to root@$CAM_IP with the key $CAM_KEY does not work, so this camera cannot be checked: nothing was recorded." \
            "pass the private key you put on the camera with --identity FILE (and check the address)."
        return 1
    fi
    ui_good "  SSH with $CAM_KEY works. Reading the camera's name and its APP partition (/dev/mtd7) ..."
    # shellcheck disable=SC2016  # expanded by the camera's shell
    reg_out=$("${CAM_SSH[@]}" 'echo "REG_HOSTNAME=$(hostname)"; echo "REG_MTD7_MD5=$(dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum | cut -d" " -f1)"' 2>/dev/null) || reg_out=""
    reg_md5=$(printf '%s\n' "$reg_out" | sed -n 's/^REG_MTD7_MD5=//p' | tail -n 1 | tr -d '[:space:]')
    reg_host=$(printf '%s\n' "$reg_out" | sed -n 's/^REG_HOSTNAME=//p' | tail -n 1 | tr -cd 'A-Za-z0-9._-')
    case $reg_md5 in
        *[!0-9a-f]* | '') reg_md5="" ;;
    esac
    if [ "${#reg_md5}" != 32 ] || [ "$reg_md5" = d41d8cd98f00b204e9800998ecf8427e ]; then
        ui_fail "could not read /dev/mtd7 back from the camera, so it cannot be told that it runs Akam: nothing was recorded." \
            "check that the camera has finished booting and that SSH gives a root shell, then try again."
        return 1
    fi
    ui_say "  Camera network name: ${reg_host:-unknown}. APP partition (mtd7) md5: $reg_md5"
    state_set camera_ip "$CAM_IP"
    state_set flash_md5 "$reg_md5"
    state_set camera_hostname "${reg_host:-unknown}"
    state_set camera_kind akam
    state_set camera_telnet 0
    state_set camera_registered 1
    state_mark_done find-camera
    if [ -n "$OPT_BACKUP_DIR" ]; then
        OPT_BACKUP_DIR="$(cd "$OPT_BACKUP_DIR" && pwd)"
        state_set backup_dir "$OPT_BACKUP_DIR"
        state_set backup_sha "$(host_sha256 "$OPT_BACKUP_DIR/mtdblock7.bin")"
        state_mark_done backup
    fi
    state_mark_done flash
    state_mark_done verify
    ui_good "  Camera '$CAM_NAME' registered at $CAM_IP as already installed. Use the maintenance tools of the menu: update, video password, network name."
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
