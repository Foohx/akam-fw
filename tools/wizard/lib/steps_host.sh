# shellcheck shell=bash
# Host-side wizard steps. Sourced by ./akam after tools/lib/host.sh, log.sh, state.sh, ui.sh.
# Expected globals (set by ./akam): ROOT, OPT_* (command-line values), UI_DRY.
# The camera-side steps (find-camera, backup from the camera, flash, verify, update, restore) are in
# steps_camera.sh; this file keeps the shared parts (backup_accept, hw_verdict, step metadata).
# Test hooks: AKAM_CHECK_HOST, AKAM_VERIFY_KIT, AKAM_ASSEMBLE, AKAM_SD_CLI, AKAM_HW_LIST.

WIZ_STEPS="doctor get-kit prepare-backup-sd find-camera backup image prepare-flash-sd flash verify"
APP_PARTITION_BYTES=5242880
KIT_REPO=Foohx/akam-fw

wiz_tool() { # name default
    local v
    case $1 in
        check_host) v=${AKAM_CHECK_HOST:-$ROOT/tools/build/check_host.sh} ;;
        verify_kit) v=${AKAM_VERIFY_KIT:-$ROOT/tools/release/verify_kit.sh} ;;
        assemble) v=${AKAM_ASSEMBLE:-$ROOT/tools/image/assemble_from_kit.sh} ;;
        sd_cli) v=${AKAM_SD_CLI:-$ROOT/tools/sd/sd_card.sh} ;;
        hw_list) v=${AKAM_HW_LIST:-$ROOT/tools/release/supported_hardware.txt} ;;
        compat) v=${AKAM_KIT_COMPAT:-$ROOT/tools/wizard/kit_compat.txt} ;;
        reflash) v=${AKAM_REFLASH:-$ROOT/tools/flash_scripts/reflash_over_ssh.sh} ;;
        hwcheck) v=${AKAM_HWCHECK:-$ROOT/tools/hw_tests/akam_hwcheck.sh} ;;
    esac
    printf '%s\n' "$v"
}

# ------------------------------------------------------------------ running commands
_q() {
    case $1 in
        '' | *[!A-Za-z0-9_./:=@%+,-]*) printf "'%s'" "$1" ;;
        *) printf '%s' "$1" ;;
    esac
}

# _show_cmd CMD...: SHOWN = the redacted, shell-quoted command line.
_show_cmd() {
    local a r out=""
    for a in "$@"; do
        r=$(log_redact "$a")
        out="$out$(_q "$r") "
    done
    SHOWN=${out% }
}

_refuse_sudo() {
    case ${1:-} in
        sudo | */sudo)
            ui_err "the wizard never runs sudo. Run this command yourself, then come back:"
            ui_say "    $SHOWN"
            return 0
            ;;
    esac
    return 1
}

# run_cmd CMD...: show, log, then run (rehearsal: only show). For anything that changes something.
run_cmd() {
    local rc=0
    _show_cmd "$@"
    if _refuse_sudo "$@"; then return 1; fi
    log_line "CMD $SHOWN"
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run: $SHOWN"
        return 0
    fi
    ui_say "  Running: $SHOWN"
    "$@" || rc=$?
    log_line "RC $rc"
    ui_note_cmd "$SHOWN" "$rc"
    return "$rc"
}

# run_ro CMD...: for read-only commands; they also run in a rehearsal.
run_ro() {
    local rc=0
    _show_cmd "$@"
    if _refuse_sudo "$@"; then return 1; fi
    log_line "CMD(ro) $SHOWN"
    ui_say "  Running (read-only): $SHOWN"
    "$@" || rc=$?
    log_line "RC $rc"
    ui_note_cmd "$SHOWN" "$rc"
    return "$rc"
}

# run_task LABEL CMD...: like run_cmd for a non-interactive command that may take a while (the
# image build). On a terminal: spinner with elapsed time (and a progress bar when the output shows
# a percentage); the output is kept in CAPTURED and shown only when the command fails. Otherwise
# exactly run_cmd (same lines, output streamed, rehearsals only show it). The command is shown and
# logged first.
run_task() {
    local label=$1 rc=0
    shift
    if [ "${TUI_ON:-0}" != 1 ] || [ "$UI_DRY" = 1 ]; then run_cmd "$@"; return; fi
    _show_cmd "$@"
    if _refuse_sudo "$@"; then return 1; fi
    log_line "CMD $SHOWN"
    ui_say "  Running: $SHOWN"
    tui_task "$label" "$@" || rc=$?
    log_line "RC $rc"
    ui_note_cmd "$SHOWN" "$rc"
    return "$rc"
}

# run_ro_cap CMD...: like run_ro, output kept in CAPTURED (and shown).
run_ro_cap() {
    local rc=0
    _show_cmd "$@"
    log_line "CMD(ro) $SHOWN"
    ui_say "  Running (read-only): $SHOWN"
    CAPTURED=$("$@" 2>&1) || rc=$?
    log_line "RC $rc"
    ui_note_cmd "$SHOWN" "$rc"
    return "$rc"
}

# ------------------------------------------------------------------ step metadata
step_num() {
    local i=0 s
    for s in $WIZ_STEPS; do
        i=$((i + 1))
        if [ "$s" = "$1" ]; then echo "$i"; return 0; fi
    done
    return 1
}

step_count() { local n=0 s; for s in $WIZ_STEPS; do n=$((n + 1)); done; echo "$n"; }

step_valid_name() {
    step_num "$1" >/dev/null 2>&1
}

step_title() {
    case $1 in
        doctor) echo "Check this computer" ;;
        get-kit) echo "Get the Akam kit" ;;
        prepare-backup-sd) echo "Prepare the backup microSD card" ;;
        find-camera) echo "Find the camera on your network" ;;
        backup) echo "Back up the camera (all 8 partitions)" ;;
        image) echo "Build the Akam image from your backup" ;;
        prepare-flash-sd) echo "Prepare the flash microSD card" ;;
        flash) echo "Flash the image to the camera" ;;
        verify) echo "Verify the result" ;;
    esac
}

# step_why NAME: one-line why and risk.
step_why() {
    case $1 in
        doctor) echo "Why: tools must be present before anything else. Risk: none (read-only)." ;;
        get-kit) echo "Why: the prebuilt kit means no compiling. Risk: none (download + signature check)." ;;
        prepare-backup-sd) echo "Why: boots the camera into a rescue shell. Risk: ERASES the microSD card." ;;
        find-camera) echo "Why: later steps need the camera address. Risk: none." ;;
        backup) echo "Why: the only way back to the original firmware. Risk: none for the camera; keep TWO copies." ;;
        image) echo "Why: builds your Akam image from YOUR dump. Risk: none (files on this PC only)." ;;
        prepare-flash-sd) echo "Why: carries the image and your SSH key. Risk: ERASES the microSD card." ;;
        flash) echo "Why: installs Akam. Risk: WRITES the camera flash; a power cut can brick it." ;;
        verify) echo "Why: proves SSH and video work before you remove the card. Risk: none." ;;
    esac
}

# step_what NAME: what the step does, in plain words (the ? help of the menu).
step_what() {
    case $1 in
        doctor) echo "Checks that the tools this wizard needs are installed (read-only) and tells you the exact install command for your system if one is missing." ;;
        get-kit) echo "Downloads the prebuilt Akam kit from the GitHub release and checks its checksum and signature. Nothing is compiled on your PC." ;;
        prepare-backup-sd) echo "Writes a small rescue payload on a microSD card. Booted in the camera, it opens a temporary maintenance shell so the original firmware can be copied." ;;
        find-camera) echo "Asks the camera's IP address (see your router's client list, maker 'AltoBeam') and checks which ports answer." ;;
        backup) echo "Reads the 8 flash partitions of the camera twice through the rescue shell, compares the two reads, and copies them to a folder on this PC." ;;
        image) echo "Builds your own Akam image from the kit and YOUR backup (the stock APP partition). Files stay on this PC; the image is exactly 5 MiB." ;;
        prepare-flash-sd) echo "Writes the image, the flash tool and your SSH public key on a microSD card that the camera will read." ;;
        flash) echo "Writes the image to the camera's flash over SSH (when Akam already runs) or through the maintenance shell, then reads it back and compares." ;;
        verify) echo "Checks SSH, the open ports and the hardware, powers nothing off, then repeats the check without the card." ;;
        rtsp-auth) echo "Adds or removes a user name and password (Digest login) on the video stream. Off by default." ;;
        hostname) echo "Gives the camera the network name you chose for it (the name of the camera), so your router lists it by name. Applied at the next power cycle." ;;
        update) echo "Flashes a newer image over SSH. No card and no maintenance shell needed once Akam runs." ;;
        restore) echo "Puts the original APP partition from your backup back on the camera." ;;
        wipe-sd) echo "Zero-fills a microSD card that holds your Wi-Fi password in clear text (or a dump). You get a red box naming the card and a yes/no (default No)." ;;
    esac
}

# step_touches NAME: what the step changes (shown before it starts).
step_touches() {
    case $1 in
        doctor | find-camera) echo "Nothing is changed (read-only checks)." ;;
        get-kit) echo "Files in the wizard's folder on this PC (the kit)." ;;
        prepare-backup-sd | prepare-flash-sd) echo "The microSD card you choose: it is ERASED. Files on this PC." ;;
        backup) echo "The camera is only read. A new backup folder on this PC." ;;
        image) echo "Files on this PC only (the image folder)." ;;
        flash) echo "The camera's flash (APP partition), and its saved Wi-Fi file if it is missing." ;;
        verify) echo "The camera's saved Wi-Fi file if it is missing, and its network name. Nothing else; you unplug the power yourself." ;;
        rtsp-auth) echo "One small file on the camera (the stream password), over SSH." ;;
        hostname) echo "One small file on the camera (its network name), over SSH." ;;
        update | restore) echo "The camera's flash (APP partition)." ;;
        wipe-sd) echo "The microSD card you choose: it is zero-filled." ;;
    esac
}

# step_is_risky NAME: the step writes a flash or erases a card (the start box is red).
step_is_risky() {
    case $1 in prepare-backup-sd | prepare-flash-sd | flash | update | restore | wipe-sd) return 0 ;; esac
    return 1
}

# step_confirm_start NAME TITLE: before a step or tool starts from the menu: what it does, what it
# touches, the risk, then "Start this step? [Y/n]" (Enter = yes). No, Esc or q = back to the menu.
# The red yes/no confirmations of the step itself come on top of this. Not used in a
# rehearsal. Returns 0 to start.
step_confirm_start() {
    local name=$1 title=$2 why risk what touches a color
    what=$(step_what "$name")
    touches=$(step_touches "$name")
    why=$(step_why "$name")
    risk=""
    case $why in *" Risk: "*) risk=${why#* Risk: } ;; esac
    if [ "${TUI_ON:-0}" = 1 ]; then
        color=$TUI_C
        if step_is_risky "$name"; then color=$TUI_R; fi
        TUI_BUF=()
        tui_box_build "$color" "$title" "What it does:  $what" "What it touches:  $touches" ${risk:+"Risk:  $risk"}
        for a in "${TUI_BUF[@]}"; do printf '%s\n' "$a" >&2; done
        tui_yesno "Start this step?" y
        return
    fi
    ui_bold "$title"
    ui_say "  What it does: $what"
    ui_say "  What it touches: $touches"
    if [ -n "$risk" ]; then ui_say "  Risk: $risk"; fi
    printf '  Start this step? [Y/n] ' >&2
    IFS= read -r a || return 1
    case $a in n | N | no | No | NO | q | Q) return 1 ;; esac
    return 0
}

step_deps() {
    case $1 in
        get-kit | prepare-backup-sd) echo "doctor" ;;
        find-camera) echo "prepare-backup-sd" ;;
        backup) echo "find-camera" ;;
        image) echo "get-kit backup" ;;
        prepare-flash-sd) echo "image" ;;
        flash) echo "prepare-flash-sd" ;;
        verify) echo "flash" ;;
        *) echo "" ;;
    esac
}

step_banner() {
    ui_banner "$(step_num "$1")" "$(step_count)" "$(step_title "$1")"
    ui_say "  $(step_why "$1")"
}

# ------------------------------------------------------------------ status and staleness
_file_sha() { # file: sha256 or empty
    [ -f "$1" ] || return 0
    host_sha256 "$1"
}

# _kit_current_sha: sha of the kit recorded in the state, as it is on disk now.
_kit_current_sha() {
    local p
    p=$(state_get kit_path)
    if [ "$(state_get mode)" = source ]; then echo source; return 0; fi
    if [ -d "$p" ]; then _file_sha "$p/MANIFEST.txt"; return 0; fi
    _file_sha "$p"
}

_backup_current_sha() {
    _file_sha "$(state_get backup_dir)/mtdblock7.bin"
}

# step_validate NAME: print the reason and return 1 when the recorded result no longer matches
# the inputs on disk (stale). In a rehearsal a missing file is tolerated.
step_validate() {
    local now
    case $1 in
        get-kit)
            [ "$(state_get mode)" = source ] && return 0
            now=$(_kit_current_sha)
            if [ -z "$now" ]; then
                [ "$STATE_DRY" = 1 ] && return 0
                echo "the kit file is gone: $(state_get kit_path)"
                return 1
            fi
            if [ "$now" != "$(state_get kit_sha)" ]; then
                [ "$STATE_DRY" = 1 ] && [ "$(state_get kit_sha)" = dry-run ] && return 0
                echo "the kit file changed since it was verified"
                return 1
            fi
            ;;
        backup)
            now=$(_backup_current_sha)
            if [ -z "$now" ]; then
                [ "$STATE_DRY" = 1 ] && return 0
                echo "the backup folder or its APP dump is gone: $(state_get backup_dir)"
                return 1
            fi
            if [ "$now" != "$(state_get backup_sha)" ]; then
                echo "the APP dump (mtdblock7.bin) changed since it was checked"
                return 1
            fi
            ;;
        image)
            now=$(_file_sha "$(state_get image_path)")
            if [ -z "$now" ]; then
                [ "$STATE_DRY" = 1 ] && return 0
                echo "the image file is gone: $(state_get image_path)"
                return 1
            fi
            if [ "$now" != "$(state_get image_sha)" ]; then
                echo "the image file changed after it was built"
                return 1
            fi
            if [ "$(_kit_current_sha)" != "$(state_get image_kit_sha)" ]; then
                echo "the kit changed since the image was built"
                return 1
            fi
            if [ "$(_backup_current_sha)" != "$(state_get image_backup_sha)" ]; then
                echo "the backup changed since the image was built"
                return 1
            fi
            ;;
        prepare-flash-sd)
            if [ "$(state_get flash_sd_image_sha)" != "$(state_get image_sha)" ]; then
                echo "the image changed after the flash card was prepared"
                return 1
            fi
            # The card holds a copy of the image: if the image is stale, so is the card.
            if ! now=$(step_validate image); then
                echo "the card was made from an image that is out of date ($now)"
                return 1
            fi
            ;;
    esac
    return 0
}

# step_status NAME: done | stale | blocked | todo
step_status() {
    local d
    if state_is_marked "$1"; then
        if step_validate "$1" >/dev/null; then echo "done"; else echo stale; fi
        return 0
    fi
    for d in $(step_deps "$1"); do
        if [ "$(step_status "$d")" != "done" ]; then echo blocked; return 0; fi
    done
    echo todo
}

# step_blockers NAME: names of unfinished dependencies.
step_blockers() {
    local d out=""
    for d in $(step_deps "$1"); do
        if [ "$(step_status "$d")" != "done" ]; then out="$out$d "; fi
    done
    printf '%s\n' "${out% }"
}

# step_need NAME: return 0 when NAME may run now; otherwise explain.
step_need() {
    local b st
    st=$(step_status "$1")
    if [ "$st" = stale ]; then
        ui_warn "this step was done before, but $(step_validate "$1" || true). It will run again."
        state_clear_done "$1"
    fi
    b=$(step_blockers "$1")
    if [ -n "$b" ]; then
        ui_fail "step '$1' needs these steps first: $b" "run: ./akam step ${b%% *}"
        return 1
    fi
    return 0
}

# status_table: numbered steps with status and one-line why/risk.
status_table() {
    local s st n=0 label reason mode=real
    if [ "$UI_DRY" = 1 ]; then mode=rehearsal; fi
    ui_bold "Akam setup progress ($mode; camera: $CAM_NAME; state: $STATE_FILE)"
    for s in $WIZ_STEPS; do
        n=$((n + 1))
        st=$(step_status "$s")
        case $st in
            done) label="done   " ;;
            stale) label="todo   " ;;
            blocked) label="blocked" ;;
            *) label="todo   " ;;
        esac
        printf '  %d. [%s] %-18s %s\n' "$n" "$label" "$s" "$(step_title "$s")"
        printf '       %s\n' "$(step_why "$s")"
        if [ "$st" = stale ]; then
            reason=$(step_validate "$s" || true)
            printf '       Inputs changed: %s. Run this step again.\n' "$reason"
        elif [ "$st" = blocked ]; then
            printf '       Waiting for: %s\n' "$(step_blockers "$s")"
        fi
    done
}

# ------------------------------------------------------------------ doctor
cmd_doctor() {
    local profile os rc=0
    step_banner doctor
    profile=kit
    if [ "$(state_get mode)" = source ]; then profile=build; fi
    if [ -n "$OPT_PROFILE" ]; then profile=$OPT_PROFILE; fi
    os=$(host_os)
    ui_say "  Host: $os $(host_arch)"
    if [ "$os" = unknown ]; then
        ui_warn "only Arch Linux and Debian/Ubuntu are supported (macOS is planned for later). Tools are still checked."
    fi
    run_ro "$(wiz_tool check_host)" --profile "$profile" || rc=$?
    if [ "$rc" -ne 0 ]; then
        ui_fail "some required tools are missing." \
            "run the install command printed above yourself (the wizard never runs sudo), then run ./akam doctor again."
        return 1
    fi
    state_set doctor_profile "$profile"
    state_mark_done doctor
    ui_good "  Host check passed (profile: $profile)."
}

# ------------------------------------------------------------------ get-kit / build
_kit_manifest_value() { # dir key
    local f
    for f in "$1/MANIFEST.txt" "$1"/*/MANIFEST.txt; do
        if [ -f "$f" ]; then
            { grep -m1 "^$2=" "$f" || true; } | cut -d= -f2-
            return 0
        fi
    done
}

# kit_format_ok FORMAT: gate 1 (wizard side). Uses tools/wizard/kit_compat.txt.
kit_format_ok() {
    grep -v '^[[:space:]]*#' "$(wiz_tool compat)" | grep -qxF "accept_kit_format|$1"
}

# download_file URL OUT: HTTPS only (also for redirects). Returns curl's exit code (22 = HTTP error).
download_file() {
    if [ "${TUI_ON:-0}" = 1 ] && [ "$UI_DRY" != 1 ]; then
        # -# makes curl print a percentage, which the task line turns into a progress bar.
        _show_cmd curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fL --retry 3 -# -o "$2" "$1"
        log_line "CMD $SHOWN"
        local rc=0
        tui_task "Downloading ${1##*/}" curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fL --retry 3 -# -o "$2" "$1" || rc=$?
        log_line "RC $rc"
        ui_note_cmd "$SHOWN" "$rc"
        return "$rc"
    fi
    run_cmd curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fL --retry 3 -o "$2" "$1"
}

cmd_get_kit() {
    local kit=$OPT_KIT ver=$OPT_VERSION base dl f rc ext fmt sha vargs unsigned=0
    step_need get-kit || return 1
    step_banner get-kit
    if [ -z "$kit" ]; then
        ver=${ver#v}
        if [ -z "$ver" ]; then
            case $AKAM_WIZARD_VERSION in
                *dev*)
                    if [ "$UI_DRY" = 1 ]; then
                        ver="X.Y.Z"
                        ui_say "  (development checkout: using the placeholder version X.Y.Z)"
                    else
                        ui_fail "this development checkout has no release to download." \
                            "pass --kit FILE for a kit you already have, or --version X.Y.Z to download that release, or run ./akam build."
                        return 1
                    fi
                    ;;
                *) ver=$AKAM_WIZARD_VERSION ;;
            esac
        fi
        case $ver in
            '' | *[!0-9A-Za-z._-]* | *..*)
                ui_fail "'$ver' is not a valid version." "use a number such as 1.0.0 (see https://github.com/$KIT_REPO/releases)."
                return 1
                ;;
        esac
        base="https://github.com/$KIT_REPO/releases/download/v$ver"
        dl="$BASE_DIR/downloads/$ver"
        kit="$dl/akam-kit-$ver.tar.xz"
        ui_say "  Downloading kit $ver from $base (HTTPS only; nothing from it is run before it is verified)."
        run_cmd mkdir -p "$dl" || return 1
        for f in "akam-kit-$ver.tar.xz" SHA256SUMS; do
            rc=0
            download_file "$base/$f" "$dl/$f" || rc=$?
            if [ "$rc" -ne 0 ]; then
                if [ "$UI_DRY" != 1 ]; then rm -f "$dl/$f"; fi
                if [ "$rc" -eq 22 ]; then
                    ui_fail "release v$ver was not found ($f). Akam may not have published a release yet." \
                        "see https://github.com/$KIT_REPO/releases for the versions that exist, or use a kit you already have: ./akam get-kit --kit FILE (or build from source: ./akam build)."
                else
                    ui_fail "download of $f failed (curl exit $rc)." \
                        "check your network and retry, or use a kit you already have: ./akam get-kit --kit FILE."
                fi
                return 1
            fi
        done
        rc=0
        download_file "$base/SHA256SUMS.sig" "$dl/SHA256SUMS.sig" || rc=$?
        if [ "$rc" -eq 22 ]; then
            ui_warn "no signature file in this release: the kit would be checked by checksum only."
            if [ "$UI_DRY" != 1 ]; then rm -f "$dl/SHA256SUMS.sig"; fi
        elif [ "$rc" -ne 0 ]; then
            if [ "$UI_DRY" != 1 ]; then rm -f "$dl/SHA256SUMS.sig"; fi
            ui_fail "download of SHA256SUMS.sig failed (curl exit $rc)." "check your network and retry."
            return 1
        fi
    fi
    if [ -e "$kit" ]; then
        kit="$(cd "$(dirname "$kit")" && pwd)/$(basename "$kit")"
    elif [ "$UI_DRY" != 1 ]; then
        { ui_fail "kit not found: $kit" "check the path given to --kit."; return 1; }
    fi

    ext="$BASE_DIR/kit"
    if [ "$UI_DRY" = 1 ]; then ext="$BASE_DIR/kit.dry"; fi
    vargs=(--extract-to "$ext")
    if [ -n "$OPT_ALLOW_UNSIGNED" ]; then vargs+=(--allow-unsigned); unsigned=1; fi
    if [ -n "$OPT_EXPECT_SHA256" ]; then vargs+=(--expect-sha256 "$OPT_EXPECT_SHA256"); fi
    if [ ! -e "$kit" ]; then
        ui_say "  WOULD run: $(wiz_tool verify_kit) ${vargs[*]} $kit   (kit not downloaded in a rehearsal)"
        state_set mode kit
        state_set kit_path "$kit"
        state_set kit_sha dry-run
        state_set kit_format 1
        state_mark_done get-kit
        return 0
    fi
    run_ro rm -rf "$ext"
    rc=0
    run_ro bash "$(wiz_tool verify_kit)" "${vargs[@]}" "$kit" || rc=$?
    if [ "$rc" -eq 3 ]; then
        ui_warn "this kit is NOT signed: only its checksum can be verified, so you must trust where you got it."
        if ! ui_danger "Unsigned kit" "Use this kit without a signature?" \
            "Only the checksum of this kit can be verified: nothing proves who made it." \
            "Kit: $kit" "Use it only if you got it from a source you trust. If in doubt, answer No."; then
            { ui_fail "unsigned kit refused." "get the signed release, or pass --allow-unsigned if you really mean it."; return 1; }
        fi
        unsigned=1
        run_ro rm -rf "$ext"
        rc=0
        run_ro bash "$(wiz_tool verify_kit)" "${vargs[@]}" --allow-unsigned "$kit" || rc=$?
    fi
    if [ "$rc" -ne 0 ]; then
        { ui_fail "the kit failed verification (see the message above)." \
            "do not use it. Download it again or get it from the official release page."; return 1; }
    fi
    fmt=$(_kit_manifest_value "$ext" kit_format)
    if ! kit_format_ok "${fmt:-?}"; then
        { ui_fail "this kit has format '${fmt:-?}', which this wizard does not support." \
            "use the wizard release that ships with this kit, or build from source (./akam build)."; return 1; }
    fi
    if [ -d "$kit" ]; then sha=$(_file_sha "$kit/MANIFEST.txt"); else sha=$(host_sha256 "$kit"); fi
    state_set mode kit
    state_set kit_path "$kit"
    state_set kit_sha "$sha"
    state_set kit_format "$fmt"
    state_set kit_unsigned_ok "$unsigned"
    state_set kit_version "$(_kit_manifest_value "$ext" akam_version)"
    state_mark_done get-kit
    ui_good "  Kit verified (format $fmt, sha256 ${sha:0:12}...)."
    if [ -z "$OPT_EXPECT_SHA256" ]; then
        ui_say "  Tip: compare that sha256 with the one in the release notes, or pass --expect-sha256 HEX."
    fi
}

cmd_build() {
    local rc=0
    step_need get-kit || return 1
    step_banner get-kit
    ui_say "  Building from source (optional path; needs more tools, takes longer)."
    run_ro "$(wiz_tool check_host)" --profile build || rc=$?
    if [ "$rc" -ne 0 ]; then
        { ui_fail "build tools are missing." "run the install command printed above yourself, then retry."; return 1; }
    fi
    run_cmd bash "$ROOT/tools/build/bootstrap.sh" || return 1
    run_cmd bash -c "cd '$ROOT' && . tools/build/env.sh && make -C src/ak_rtsp ak_rtsp flash_tool" || return 1
    run_cmd bash "$ROOT/tools/dropbear/build_dropbear.sh" || return 1
    state_set mode source
    state_set kit_path ""
    state_set kit_sha source
    state_mark_done get-kit
    ui_good "  Binaries built from source. The image step will use them."
}

# ------------------------------------------------------------------ SD card adapter
# Real CLI: tools/sd/sd_card.sh, always called through bash (exit 0 ok, 1 refused/failed, 2 usage):
#   list-candidates [--human]                  eligible whole disks; TSV: dev, bytes, model, transport, mounts
#   prepare --mode backup|first-boot --device DEV [--pubkey F] [--toolkit DIR]   ERASES the card
#   prepare --mode flash-files --device DEV --image F --flash-tool F --backup-dir DIR   adds files, no erase
#   verify  --mode MODE (--device DEV | --mount DIR | --stage-dir DIR) [--toolkit DIR]
#   wipe    --device DEV                       zero-fills the card
# Wi-Fi settings reach it ONLY through the environment (AKAM_WIFI_SSID, AKAM_WIFI_PASS).
# The SD tool asks for its own typed "ERASE <device>" when run by hand; the wizard asks one yes/no
# (default No) and then passes --yes-i-confirmed <device> (accepted only with the same --device, still on a
# real terminal, and the disk is re-checked before any write). The tool calls sudo itself; this wizard never runs sudo.
sd_available() { [ -f "$(wiz_tool sd_cli)" ]; }

sd_toolkit_dir() { printf '%s\n' "${AKAM_TOOLKIT_DIR:-$HOME/akam-sdprep/toolkit}"; }

# sd_call ARGS...: run the SD CLI; the Wi-Fi environment is given to "prepare" only.
sd_call() {
    local rc=0
    if [ "$1" = prepare ] && [ -n "${SD_SSID:-}" ] && [ -n "${SD_PASS:-}" ]; then
        export AKAM_WIFI_SSID="$SD_SSID" AKAM_WIFI_PASS="$SD_PASS"
    fi
    run_cmd bash "$(wiz_tool sd_cli)" "$@" || rc=$?
    unset AKAM_WIFI_SSID AKAM_WIFI_PASS
    return "$rc"
}

# sd_need_cli HINT: fail with a manual fallback when the SD tool is missing.
sd_need_cli() {
    if [ "$UI_DRY" != 1 ] && ! sd_available; then
        ui_fail "the SD card tool ($(wiz_tool sd_cli)) is not available in this checkout." "$1"
        return 1
    fi
}

# sd_pick_device TSV: terminal only. A list to pick the card from (the TSV of list-candidates);
# sets SD_DEV. The red yes/no confirmation still follows, whatever is picked here.
sd_pick_device() {
    local tsv=$1 dev size model tr mounts items=() devs=() gb
    while IFS=$'\t' read -r dev size model tr mounts; do
        [ -n "$dev" ] || continue
        gb="?"
        case $size in '' | *[!0-9]*) ;; *) gb="$((size / 1000000000)).$((size / 100000000 % 10)) GB" ;; esac
        devs+=("$dev")
        items+=("$dev  $gb  ${model:-unknown model}  (${tr:-?})${mounts:+  mounted: $mounts}")
    done <<<"$tsv"
    [ "${#devs[@]}" -gt 0 ] || return 1
    tui_choose "Which disk is your microSD card?" "${items[@]}" || return 1
    SD_DEV=${devs[TUI_INDEX]}
}

# sd_choose_device: set SD_DEV (whole disk). Lists the eligible cards, then asks.
sd_choose_device() {
    local cand="" rc=0 line found=0 size model tr mounts gb
    SD_DEV=$OPT_DEVICE
    SD_INFO=""
    if sd_available; then
        ui_say "  Removable disks that can be used as a microSD card (system disks are never listed):"
        run_ro bash "$(wiz_tool sd_cli)" list-candidates --human || true
        run_ro_cap bash "$(wiz_tool sd_cli)" list-candidates || rc=$?
        if [ "$rc" -eq 0 ]; then cand=$CAPTURED; fi
        if [ -z "$cand" ] && [ "$UI_DRY" != 1 ]; then
            { ui_fail "no microSD card found." "insert the card (USB reader or built-in SD slot), wait a few seconds, then run this step again."; return 1; }
        fi
    fi
    if [ -z "$SD_DEV" ] && [ "${TUI_ON:-0}" = 1 ] && [ "$UI_DRY" != 1 ] && [ -n "$cand" ]; then
        sd_pick_device "$cand" || { ui_fail "no card chosen." "run this step again and pick your microSD card, or pass --device /dev/sdX."; return 1; }
    fi
    if [ -z "$SD_DEV" ]; then
        if [ "$UI_DRY" = 1 ]; then
            SD_DEV=/dev/sdX
        else
            ui_prompt "  Device of your microSD card from the list above (for example /dev/sdb), or empty to cancel:" "" || return 1
            SD_DEV=$ANSWER
        fi
    fi
    case $SD_DEV in
        /dev/?*) ;;
        *) { ui_fail "no valid device given." "pass --device /dev/sdX (the whole disk, not a partition)."; return 1; } ;;
    esac
    case $SD_DEV in
        *" "*) { ui_fail "invalid device name." "pass --device /dev/sdX."; return 1; } ;;
    esac
    # A /dev/disk/by-id link is resolved here so that the confirmation names the real disk.
    if [ -L "$SD_DEV" ]; then SD_DEV=$(readlink -f -- "$SD_DEV"); fi
    if [ -n "$cand" ] && [ "$UI_DRY" != 1 ]; then
        while IFS=$'\t' read -r line size model tr mounts; do
            if [ "$line" = "$SD_DEV" ]; then
                found=1
                gb="?"
                case $size in '' | *[!0-9]*) ;; *) gb="$((size / 1000000000)).$((size / 100000000 % 10)) GB" ;; esac
                SD_INFO="Model: ${model:-unknown}  |  Size: $gb  |  Connected by: ${tr:-?}  |  Mounted: ${mounts:-nothing}"
            fi
        done <<<"$cand"
        if [ "$found" != 1 ]; then
            { ui_fail "$SD_DEV is not in the list of usable cards." \
                "pick a device from the list above; system disks, partitions and non-removable disks are refused on purpose."; return 1; }
        fi
    fi
}

# sd_get_wifi: SD_SSID / SD_PASS from --ssid, the environment (read once by ./akam) or a prompt.
sd_get_wifi() {
    SD_SSID=${OPT_SSID:-${ENV_WIFI_SSID:-}}
    SD_PASS=${ENV_WIFI_PASS:-}
    log_add_secret "$SD_PASS"
    if [ "$UI_DRY" = 1 ]; then return 0; fi
    if [ -z "$SD_SSID" ]; then
        ui_prompt "  Wi-Fi network name (SSID, 2.4 GHz):" "" || return 1
        SD_SSID=$ANSWER
    fi
    if [ -z "$SD_PASS" ]; then
        ui_secret_ask "  Wi-Fi password (never saved by the wizard):" || return 1
        SD_PASS=$SECRET
    fi
    if [ -z "$SD_SSID" ] || [ -z "$SD_PASS" ]; then
        { ui_fail "Wi-Fi name and password are required." "the camera needs them to join your network."; return 1; }
    fi
    ui_say "  Wi-Fi settings go to the SD tool through its environment (AKAM_WIFI_PASS=***), not the command line."
}

# sd_confirm_erase WHAT: one yes/no choice (default No) in a red box that names the device, its model and size
# and what will happen. Needs a real terminal. After a yes the wizard passes --yes-i-confirmed DEVICE to the SD
# tool (which still re-checks the disk before it writes). WHAT: "erase" or "wipe".
sd_confirm_erase() {
    local what=${1:-erase} title q lines=()
    ui_require_tty "erasing the microSD card" || return 1
    if [ "$what" = wipe ]; then
        title="ZERO-FILL $SD_DEV"
        q="Zero-fill $SD_DEV now?"
        lines+=("Every byte of $SD_DEV will be overwritten with zeros (it takes a few minutes). Nothing can be recovered.")
    else
        title="ERASE $SD_DEV"
        q="Erase $SD_DEV now?"
        lines+=("Everything on $SD_DEV will be ERASED: all partitions and files. This cannot be undone.")
    fi
    lines+=("Device: $SD_DEV" "${SD_INFO:-Model and size: see the list above}" "This must be your microSD card, not another disk. If in doubt, answer No.")
    lines+=("The SD tool asks for your sudo password itself (disk access needs root), and checks the disk again right before it writes.")
    ui_danger "$title" "$q" "${lines[@]}" || { ui_fail "not confirmed." "nothing was changed."; return 1; }
}

cmd_prepare_backup_sd() {
    local tk
    step_need prepare-backup-sd || return 1
    step_banner prepare-backup-sd
    ui_say "  This card boots the camera into a temporary rescue shell (Telnet on port 24) so that"
    ui_say "  you can copy the original firmware. It holds your Wi-Fi password IN CLEAR TEXT:"
    ui_say "  treat the card as secret and wipe it when you are done (./akam wipe-sd)."
    sd_need_cli "follow docs/manual/backup-sd.md section 5 by hand, then run: ./akam step find-camera" || return 1
    ui_require_tty "preparing the backup card" || return 1
    tk=$(sd_toolkit_dir)
    ui_say "  The SD tool uses the pinned LSC toolkit in $tk (downloaded once if missing, then"
    ui_say "  checked against a fixed commit and file hashes)."
    sd_choose_device || return 1
    sd_get_wifi || return 1
    sd_confirm_erase || return 1
    sd_call prepare --mode backup --device "$SD_DEV" --yes-i-confirmed "$SD_DEV" --toolkit "$tk" ||
        { ui_fail "preparing the card failed." "nothing else was changed; fix the problem above and retry."; return 1; }
    sd_call verify --mode backup --device "$SD_DEV" --toolkit "$tk" ||
        { ui_fail "the card did not verify." "prepare it again."; return 1; }
    state_set backup_sd_device "$SD_DEV"
    state_mark_done prepare-backup-sd
    ui_good "  Backup card ready. Put it in the camera, power the camera on, then run: ./akam"
}

# cmd_wipe_sd: zero-fill a card that holds the Wi-Fi password (or a dump).
cmd_wipe_sd() {
    local rc=0 d
    ui_bold "Erase a microSD card"
    ui_say "  Why: the cards made by this wizard hold your Wi-Fi password in clear text."
    ui_say "  Risk: ERASES the whole card (it takes a few minutes). Nothing else is touched."
    sd_need_cli "wipe the card with your own tools (for example: sudo dd if=/dev/zero of=/dev/sdX bs=4M)" || return 1
    ui_require_tty "erasing the microSD card" || return 1
    sd_choose_device || return 1
    sd_confirm_erase wipe || return 1
    run_cmd bash "$(wiz_tool sd_cli)" wipe --device "$SD_DEV" --yes-i-confirmed "$SD_DEV" || rc=$?
    if [ "$rc" -ne 0 ]; then
        { ui_fail "wiping the card failed." "nothing else was changed; fix the problem above and retry."; return 1; }
    fi
    for d in backup_sd_device flash_sd_device; do
        if [ "$(state_get "$d")" = "$SD_DEV" ]; then state_unset "$d"; fi
    done
    ui_good "  Card wiped: it no longer holds your Wi-Fi password."
}

# wipe_offer: after the last step, offer to wipe the cards recorded as holding a secret.
wipe_offer() {
    if [ -z "$(state_get backup_sd_device)$(state_get flash_sd_device)" ]; then return 0; fi
    ui_blank
    ui_warn "your microSD card(s) still hold your Wi-Fi password in clear text (wifi.conf)."
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD offer: wipe them now (a yes/no, default No), or later with ./akam wipe-sd"
        return 0
    fi
    if ! host_is_tty; then
        ui_say "  Wipe them with: ./akam wipe-sd"
        return 0
    fi
    if ui_confirm "  Wipe a card now?"; then
        cmd_wipe_sd || ui_say "  You can wipe later with: ./akam wipe-sd"
    else
        ui_say "  You can wipe later with: ./akam wipe-sd"
    fi
}

# ------------------------------------------------------------------ backup (import + checks)
BACKUP_FILES="mtdblock0.bin mtdblock1.bin mtdblock2.bin mtdblock3.bin mtdblock4.bin mtdblock5.bin mtdblock6.bin mtdblock7.bin"

# backup_check DIR: layout checks. Prints findings; returns 1 when the layout is wrong.
backup_check() {
    local d=$1 f extra
    for f in $BACKUP_FILES; do
        [ -f "$d/$f" ] || { ui_err "missing $f in $d"; return 1; }
    done
    for extra in "$d"/mtdblock[0-9]*.bin; do
        case ${extra##*/} in
            mtdblock[0-7].bin) ;;
            *) ui_err "unexpected file ${extra##*/}: this backup has more than 8 partitions (different MTD layout)."; return 1 ;;
        esac
    done
    if [ "$(wc -c <"$d/mtdblock7.bin" | tr -d ' ')" != "$APP_PARTITION_BYTES" ]; then
        ui_err "mtdblock7.bin is not $APP_PARTITION_BYTES bytes: the APP partition layout differs from the supported one."
        return 1
    fi
    if [ -f "$d/SHA256SUMS" ]; then
        if ! (cd "$d" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
            ui_err "SHA256SUMS does not match the files in $d"
            return 1
        fi
        ui_say "  SHA256SUMS verified."
    else
        ui_warn "no SHA256SUMS in $d: integrity of the copy from the camera was not checked."
    fi
}

# backup_secure DIR: the CONFIG partition holds the Wi-Fi password (wpa_supplicant.conf), the ONVIF password
# in clear text and the RTSP digest hash (G19): the folder is private to this user (0700, files 0600).
backup_secure() {
    local d=$1 f
    [ "$UI_DRY" = 1 ] && return 0
    if [ "${2:-}" != files-only ]; then
        chmod 700 "$d" 2>/dev/null || ui_warn "  could not restrict $d to your user (chmod 700): do it by hand."
    fi
    # only the backup's own files: the folder may be an existing one (USB stick root) holding other things
    for f in "$d"/mtdblock[0-7].bin "$d"/SHA256SUMS "$d"/README.txt "$d"/camera_info.txt; do
        if [ -f "$f" ]; then chmod 600 "$f" 2>/dev/null || true; fi
    done
    return 0
}

# backup_accept DIR: final checks and the second-copy gate for a backup folder, then record it.
# Used for a folder made by the camera steps and for an imported one (--backup-dir).
backup_accept() {
    local d=$1
    [ -d "$d" ] || { ui_fail "not a folder: $d" ""; return 1; }
    d="$(cd "$d" && pwd)"
    backup_check "$d" || return 1
    backup_secure "$d"
    ui_warn "THIS BACKUP CONTAINS SECRETS: your Wi-Fi password, the ONVIF password (clear text) and the RTSP hash are"
    ui_warn "inside the CONFIG partition. The folder is now private to your user (0700); keep the copy private too,"
    ui_warn "never upload it or put it in a shared or cloud-synced folder."
    ui_warn "COPY THIS FOLDER TO A SECOND MEDIUM (another disk or USB stick) BEFORE continuing."
    ui_warn "It is the only way back to the original firmware. Never publish it: it holds proprietary firmware."
    if [ -n "${OPT_BACKUP_COPY:-}" ]; then
        backup_copy_to "$d" "$OPT_BACKUP_COPY" || return 1
        ui_confirm "  Is $OPT_BACKUP_COPY on a different medium (another disk or USB stick)?" ||
            { ui_fail "second medium not confirmed." "copy the folder to another disk, then run this step again."; return 1; }
    else
        ui_confirm "  Have you made a second copy on a different medium?" ||
            { ui_fail "second copy not confirmed." "make the copy, then run this step again."; return 1; }
    fi
    state_set backup_dir "$d"
    state_set backup_sha "$(host_sha256 "$d/mtdblock7.bin")"
    state_mark_done backup
    ui_good "  Backup folder accepted: $d"
}

# backup_copy_to SRC DEST: copy the backup folder and check the copy with SHA256SUMS.
backup_copy_to() {
    local src=$1 dest=$2
    case $dest in "$src" | "$src"/*) { ui_fail "the copy must be outside $src" ""; return 1; } ;; esac
    local pre=0
    if [ -d "$dest" ]; then pre=1; fi
    (umask 077 && run_cmd mkdir -p "$dest") || return 1
    run_cmd cp -p "$src"/mtdblock[0-7].bin "$src"/SHA256SUMS "$dest"/ || { ui_fail "copying the backup failed." ""; return 1; }
    if [ "$UI_DRY" = 1 ]; then return 0; fi
    if ! (cd "$dest" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
        ui_fail "the copy in $dest does not match SHA256SUMS." "copy it again to a different medium."
        return 1
    fi
    if [ "$pre" = 1 ]; then backup_secure "$dest" files-only; else backup_secure "$dest"; fi
    ui_good "  Copy checked: $dest matches SHA256SUMS."
}

# backup_import: --backup-dir DIR, a backup you made by hand (docs/manual/backup-sd.md).
backup_import() {
    local d=$OPT_BACKUP_DIR
    step_banner backup
    backup_accept "$d"
}

# ------------------------------------------------------------------ image
# hw_verdict APPSHA: prints "status|device" from supported_hardware.txt (unknown -> untested).
hw_verdict() {
    local st
    st=$(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$(wiz_tool hw_list)" | awk -F'|' -v h="$1" '$7 == h { print $1 "|" $2; exit }')
    if [ -z "$st" ]; then st="untested|not in the supported hardware list (unknown stock APP)"; fi
    printf '%s\n' "$st"
}

# stock_info FILE: sets STOCK_COMP, STOCK_BLOCK; prints the lines to show.
stock_info() {
    STOCK_COMP=$(printf '%s\n' "$CAPTURED" | sed -n 's/^Compression \([a-z0-9]*\).*/\1/p' | head -1)
    STOCK_BLOCK=$(printf '%s\n' "$CAPTURED" | sed -n 's/^Block size \([0-9]*\).*/\1/p' | head -1)
    ui_say "  Stock APP compression: ${STOCK_COMP:-unknown}, block size ${STOCK_BLOCK:-unknown}"
    printf '%s\n' "$CAPTURED" | grep -i -e bcj -e filter | sed 's/^/    /' || true
}

cmd_image() {
    local bdir out kit sha verdict vstat vdev top missing a rc=0 img isz asha pass
    step_need image || return 1
    step_banner image
    bdir=${OPT_BACKUP_DIR:-$(state_get backup_dir)}
    kit=$(state_get kit_path)
    [ -z "$bdir" ] || [ ! -d "$bdir" ] || bdir="$(cd "$bdir" && pwd)"
    out=${OPT_OUT:-$HOME/akam-image-$(cams_slug)$(date +%Y%m%d-%H%M%S)}
    pass=()
    if [ -n "$OPT_COMP" ]; then pass+=(--comp "$OPT_COMP"); fi
    if [ -n "$OPT_ALLOW_UNSIGNED" ] || [ "$(state_get kit_unsigned_ok)" = 1 ]; then pass+=(--allow-unsigned); fi

    if [ -f "$bdir/mtdblock7.bin" ]; then
        backup_check "$bdir" || return 1
        # Gate 3: the APP dump must be a readable squashfs with bin, sbin, local.
        run_ro_cap unsquashfs -s "$bdir/mtdblock7.bin" ||
            { ui_fail "mtdblock7.bin is not a readable squashfs." "check that this is the APP partition of YOUR backup."; return 1; }
        stock_info
        run_ro_cap unsquashfs -l "$bdir/mtdblock7.bin" ||
            { ui_fail "cannot list the APP dump." ""; return 1; }
        top=$(printf '%s\n' "$CAPTURED" | sed -n 's#^squashfs-root/\([^/]*\)$#\1#p')
        missing=""
        for a in bin sbin local; do
            case " $(echo "$top" | tr "\n" " ") " in *" $a "*) ;; *) missing="$missing $a" ;; esac
        done
        if [ -n "$missing" ]; then
            { ui_fail "the APP dump lacks these top-level directories:$missing" "this does not look like a stock APP partition."; return 1; }
        fi
        ui_say "  APP dump readable: bin, sbin and local present."
        case ${STOCK_COMP:-unknown} in
            xz | gzip) ;;
            *)
                if [ -z "$OPT_ALLOW_UNKNOWN_COMP" ]; then
                    { ui_fail "the stock APP uses '${STOCK_COMP:-unknown}' compression, which Akam does not support." \
                        "only xz and gzip are supported. If you checked that your kernel can read it, add --allow-unknown-stock-comp."; return 1; }
                fi
                ui_danger "Unverified compression" "Build with '${STOCK_COMP:-unknown}' compression?" \
                    "Your stock APP uses '${STOCK_COMP:-unknown}' compression, which Akam has not been verified to read." \
                    "If the camera's kernel cannot read the image it will not start and only a restore (or a UART cable) can recover it." \
                    "Answer Yes only if you checked that your kernel supports it." ||
                    { ui_fail "not confirmed." ""; return 1; }
                pass+=(--allow-unknown-stock-comp)
                ;;
        esac
        if [ -n "$OPT_COMP" ] && [ "$OPT_COMP" != "$STOCK_COMP" ]; then
            ui_warn "you asked for --comp $OPT_COMP but your stock APP uses ${STOCK_COMP:-unknown}. The stock setting is the one proven to boot."
        fi
        asha=$(host_sha256 "$bdir/mtdblock7.bin")
        verdict=$(hw_verdict "$asha")
        vstat=${verdict%%|*}
        vdev=${verdict#*|}
        ui_say "  Hardware verdict: $vstat - $vdev"
        ui_say "  Stock APP sha256: $asha"
        if [ "$vstat" != validated ]; then
            ui_danger "Untested firmware" "Continue with this untested firmware?" \
                "This stock firmware is NOT on the validated list: Akam may not work and the camera may need a restore." \
                "Hardware verdict: $vstat - $vdev" "Stock APP sha256: $asha" ||
                { ui_fail "untested firmware not confirmed." "nothing was built."; return 1; }
            pass+=(--i-understand-untested)
        fi
    elif [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD check the backup (8 files, APP readable with unsquashfs, bin/sbin/local present),"
        ui_say "  show the stock compression and hardware verdict, and ask a yes/no (default No) in a red box"
        ui_say "  unless the firmware is validated."
        bdir=${bdir:-"<backup-dir>"}
        asha=dry-run
    else
        { ui_fail "no backup folder." "run: ./akam backup --backup-dir DIR"; return 1; }
    fi

    ui_warn "reminder: your backup must exist on TWO different media. Never publish dumps or images."
    if [ "$(state_get mode)" = source ]; then
        run_cmd bash -c "d=\$(mktemp -d) && unsquashfs -d \"\$d/app\" '$bdir/mtdblock7.bin' && APP_EXTRACTED=\"\$d/app\" FW_DIR='$out' bash '$ROOT/src/ak_rtsp/build_firmware.sh' --backup-dir '$bdir'" || rc=$?
    else
        [ "$UI_DRY" = 1 ] || [ -e "$kit" ] || { ui_fail "kit not found: $kit" "run ./akam get-kit again."; return 1; }
        run_task "Building the image" bash "$(wiz_tool assemble)" --backup-dir "$bdir" --kit "${kit:-<kit>}" --out "$out" ${pass[@]+"${pass[@]}"} || rc=$?
    fi
    [ "$rc" -eq 0 ] || { ui_fail "building the image failed (see the message above)." "nothing was written to the camera."; return 1; }

    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD verify (gate 4): image is exactly $APP_PARTITION_BYTES bytes, sha256 recorded, build_report.txt present."
        state_set image_path "$out/<image>"
        state_set image_sha dry-run
    else
        img=$(find "$out" -maxdepth 1 -name 'ak_rtsp_firmware_*.squashfs' | LC_ALL=C sort | tail -1)
        [ -n "$img" ] || { ui_fail "no image found in $out" ""; return 1; }
        isz=$(wc -c <"$img" | tr -d ' ')
        if [ "$isz" != "$APP_PARTITION_BYTES" ]; then
            { ui_fail "the image is $isz bytes, expected exactly $APP_PARTITION_BYTES." "do not flash it."; return 1; }
        fi
        [ -f "$out/build_report.txt" ] || { ui_fail "build_report.txt is missing in $out" ""; return 1; }
        sha=$(host_sha256 "$img")
        ui_say "  Image: $img"
        ui_say "  Size: $isz bytes (APP partition size). sha256: $sha"
        sed -n -e 's/^compression: */  Compression: /p' -e 's/^compression vs stock: */  Compared with stock: /p' \
            -e 's/^hardware verdict: */  Hardware verdict: /p' "$out/build_report.txt"
        state_set image_path "$img"
        state_set image_sha "$sha"
        state_set image_out "$out"
    fi
    state_set image_kit_sha "$(_kit_current_sha)"
    state_set image_backup_sha "$(_backup_current_sha)"
    if [ "$UI_DRY" = 1 ]; then state_set image_backup_sha "$(_backup_current_sha)"; fi
    state_mark_done image
    ui_good "  Image ready. Keep it private: it contains the vendor's proprietary files."
}

# ------------------------------------------------------------------ prepare-flash-sd
# pubkey_check FILE: gate 5 for the key: one ed25519 public key line, never a private key.
pubkey_check() {
    [ -f "$1" ] || { ui_err "public key not found: $1"; return 1; }
    if grep -q 'PRIVATE KEY' "$1"; then
        ui_err "$1 is a PRIVATE key. Never put it on the card; use the .pub file."
        return 1
    fi
    case $1 in *.pub) ;; *) ui_err "$1 does not end in .pub: give the PUBLIC key file."; return 1 ;; esac
    if [ "$(grep -c '' "$1")" -ne 1 ]; then ui_err "$1 must contain exactly one line"; return 1; fi
    case $(cut -d' ' -f1 "$1") in
        ssh-ed25519) ;;
        *) ui_err "$1 is not an ssh-ed25519 public key"; return 1 ;;
    esac
}

# _ssh_tilde PATH: a path under $HOME written as ~/... (what the menus show).
_ssh_tilde() {
    case $1 in
        "$HOME"/*) printf '~%s' "/${1#"$HOME"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

# _ssh_make_key PUB COMMENT: PUB exists afterwards: it is used when it is there, else the key pair is
# created with ssh-keygen (asked first; ssh-keygen asks the passphrase). Never overwrites a file.
_ssh_make_key() {
    local pub=$1 comment=$2 priv=${1%.pub}
    if [ -f "$pub" ]; then
        ui_say "  Using $(_ssh_tilde "$pub") (its private key stays on this PC; only the .pub goes on the card)."
        return 0
    fi
    if [ -e "$priv" ]; then
        ui_fail "$(_ssh_tilde "$priv") exists but $(_ssh_tilde "$pub") does not." "recreate the public key (ssh-keygen -y -f $priv > $pub), or choose another key."
        return 1
    fi
    ui_say "  There is no $(_ssh_tilde "$priv"). Akam creates an ed25519 key pair for it:"
    ui_confirm "  Create it now (you will be asked for a passphrase)?" || { ui_fail "no SSH key." "create one, then pass --pubkey FILE."; return 1; }
    mkdir -p "$(dirname "$priv")" && chmod 700 "$(dirname "$priv")" 2>/dev/null
    run_cmd ssh-keygen -t ed25519 -f "$priv" -C "$comment" || return 1
}

# _ssh_own_key: SSH_PUB is the key of this camera only, ~/.ssh/akam-NAME. An existing file of that
# name is never taken silently: say when it was made and ask (default No); else offer another name.
_ssh_own_key() {
    local name=$CAM_NAME pub priv try=0 when n2
    while :; do
        pub=$HOME/.ssh/akam-$name.pub
        priv=${pub%.pub}
        SSH_PUB=$pub
        if [ ! -e "$pub" ] && [ ! -e "$priv" ]; then _ssh_make_key "$pub" "akam-$name"; return; fi
        when=$(date -r "$priv" +%Y-%m-%d 2>/dev/null || echo "an unknown date")
        ui_say "  A key for the name $name already exists: $(_ssh_tilde "$priv") (created $when)."
        if ui_confirm "  Is it the key of this camera? Use it?"; then _ssh_make_key "$pub" "akam-$name"; return; fi
        try=$((try + 1))
        if [ "$try" -ge 3 ]; then ui_fail "no key chosen." "choose another key, or remove the old file yourself if it is not needed."; return 1; fi
        printf '  Another name for the key file (a-z, 0-9, -; empty cancels): ' >&2
        IFS= read -r n2 || n2=
        if [ -z "$n2" ]; then ui_fail "no key chosen." "run this step again."; return 1; fi
        case $n2 in
            *[!a-z0-9-]* | -* | *-) ui_say "  That is not a valid name." ;;
            *) name=$n2 ;;
        esac
    done
}

# ssh_choose_pubkey: sets SSH_PUB, the public key for the flash card, and remembers it in the state.
# Order: --pubkey, the key chosen before (state), else the question "Which SSH key should open this
# camera?": the shared key ~/.ssh/akam (the default, created when missing), a key for this camera only
# (~/.ssh/akam-NAME, created now) or an existing ed25519 key of ~/.ssh. Returns 1 when no key can be had.
ssh_choose_pubkey() {
    local shared=$HOME/.ssh/akam.pub own=$HOME/.ssh/akam-$CAM_NAME.pub cur f kind cands=() items=() n
    SSH_PUB=""
    if [ -n "$OPT_PUBKEY" ]; then
        SSH_PUB=$OPT_PUBKEY
        # ~ expanded and the path made absolute: it is saved in the state and must mean the same from any folder
        # shellcheck disable=SC2088
        case $SSH_PUB in
            "~") SSH_PUB=$HOME ;;
            "~/"*) SSH_PUB=$HOME/${SSH_PUB#"~/"} ;;
            /*) ;;
            *) SSH_PUB=$PWD/$SSH_PUB ;;
        esac
        return 0
    fi
    cur=$(state_get ssh_pubkey)
    if [ -n "$cur" ] && [ -f "$cur" ]; then
        SSH_PUB=$cur
        ui_say "  SSH key (chosen before): $cur"
        return 0
    fi
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask: Which SSH key should open this camera? (the shared key $(_ssh_tilde "${shared%.pub}"), a key for this camera only $(_ssh_tilde "${own%.pub}"), or a key of ~/.ssh)"
        SSH_PUB=$shared
        if [ ! -f "$shared" ]; then run_cmd ssh-keygen -t ed25519 -f "${shared%.pub}" -C akam; fi
        return 0
    fi
    for f in "$HOME"/.ssh/*.pub; do
        [ -f "$f" ] || continue
        if [ "$f" = "$shared" ] || [ "$f" = "$own" ]; then continue; fi
        kind=$(cut -d' ' -f1 "$f" 2>/dev/null | head -1)
        if [ "$kind" = ssh-ed25519 ] && [ "$(grep -c '' "$f")" = 1 ]; then cands+=("$f"); fi
    done
    items=("Shared key $(_ssh_tilde "${shared%.pub}") (all your cameras)" "A key for this camera only ($(_ssh_tilde "${own%.pub}"))")
    if [ "${#cands[@]}" -gt 0 ]; then items+=("${cands[@]}"); fi
    ui_choose "Which SSH key should open this camera?" "${items[@]}" || { ui_fail "no key chosen." "run this step again, or pass --pubkey FILE."; return 1; }
    n=$CHOICE_INDEX
    case $n in
        0) SSH_PUB=$shared; _ssh_make_key "$shared" akam ;;
        1) _ssh_own_key ;;
        *)
            SSH_PUB=${cands[n - 2]}
            ui_say "  Using $SSH_PUB (its private key stays on this PC; only the .pub goes on the card)."
            ;;
    esac
}

cmd_prepare_flash_sd() {
    local pub ft img bk tk
    step_need prepare-flash-sd || return 1
    step_banner prepare-flash-sd
    ssh_choose_pubkey || return 1
    pub=$SSH_PUB
    if [ -f "$pub" ] || [ "$UI_DRY" != 1 ]; then pubkey_check "$pub" || return 1; fi
    if [ "$UI_DRY" != 1 ]; then state_set ssh_pubkey "$pub"; fi
    sd_need_cli "prepare the card by hand: see docs/manual/ssh-and-lockdown.md and tools/sd/prepare_first_boot_sd.sh" || return 1
    ui_require_tty "preparing the flash card" || return 1
    if [ "$(state_get mode)" = source ]; then ft=$ROOT/src/ak_rtsp/flash_tool; else ft=$BASE_DIR/kit/bin/flash_tool; fi
    if [ "$UI_DRY" != 1 ] && [ ! -s "$ft" ]; then
        ui_fail "flash_tool not found: $ft" "run ./akam get-kit again (or ./akam build)."
        return 1
    fi
    img=$(state_get image_path)
    bk=$(state_get backup_dir)
    tk=$(sd_toolkit_dir)
    sd_choose_device || return 1
    sd_get_wifi || return 1
    sd_confirm_erase || return 1
    sd_call prepare --mode first-boot --device "$SD_DEV" --yes-i-confirmed "$SD_DEV" --pubkey "$pub" --toolkit "$tk" ||
        { ui_fail "preparing the card failed." "the camera was not touched; fix the problem above and retry."; return 1; }
    sd_call prepare --mode flash-files --device "$SD_DEV" --image "$img" --flash-tool "$ft" --backup-dir "$bk" ||
        { ui_fail "adding the flash files to the card failed." "the card is already erased: run this step again."; return 1; }
    sd_call verify --mode first-boot --device "$SD_DEV" --toolkit "$tk" ||
        { ui_fail "the card did not verify (no *.bin at its root, authorized_keys must be your public key)." "prepare it again."; return 1; }
    sd_call verify --mode flash-files --device "$SD_DEV" ||
        { ui_fail "the flash files on the card did not verify." "prepare it again."; return 1; }
    state_set flash_sd_device "$SD_DEV"
    state_set flash_sd_image_sha "$(state_get image_sha)"
    state_mark_done prepare-flash-sd
    ui_good "  Flash card ready. Remember: wifi.conf on the card holds your Wi-Fi password in clear text (./akam wipe-sd erases it)."
}

# ------------------------------------------------------------------ dispatcher
step_run() { # name
    case $1 in
        doctor) cmd_doctor ;;
        get-kit) if [ "$OPT_BUILD" = 1 ]; then cmd_build; else cmd_get_kit; fi ;;
        prepare-backup-sd) cmd_prepare_backup_sd ;;
        find-camera) cmd_find_camera ;;
        backup) cmd_backup ;;
        image) cmd_image ;;
        prepare-flash-sd) cmd_prepare_flash_sd ;;
        flash) cmd_flash ;;
        verify) cmd_verify ;;
        *) ui_fail "unknown step '$1'" "run ./akam help" ;;
    esac
}

# wizard_walk: run every step that is not done, in order (rehearsal: no prompts).
wizard_walk() {
    local s st
    for s in $WIZ_STEPS; do
        st=$(step_status "$s")
        if [ "$st" = "done" ]; then continue; fi
        if [ "$UI_DRY" != 1 ]; then
            ui_prompt "Next: $(step_title "$s")  [Enter] continue / s skip / q quit:" "" || return 1
            case $ANSWER in
                s | S) continue ;;
                q | Q) return 0 ;;
            esac
        fi
        step_run "$s" || { ui_say "Stopped at step '$s'. Fix the problem and run ./akam to resume."; return 1; }
    done
    ui_good "All steps are done."
}

# menu_start NAME: from the plain menu: summary and "Start this step? [Y/n]", then the step.
menu_start() {
    if step_confirm_start "$1" "$(step_title "$1")"; then
        step_run "$1" || true
    else
        ui_say "  Not started."
    fi
}

# wizard_menu: numbered menu; Enter runs the next step.
wizard_menu() {
    local s next n
    while true; do
        status_table
        next=""
        for s in $WIZ_STEPS; do
            if [ "$(step_status "$s")" = todo ] || [ "$(step_status "$s")" = stale ]; then next=$s; break; fi
        done
        ui_blank
        if [ -z "$next" ]; then ui_good "All steps are done."; return 0; fi
        ui_prompt "[Enter] run '$next' / number = run that step / a = run all remaining / q = quit:" "" || return 0
        case $ANSWER in
            q | Q) return 0 ;;
            a | A) wizard_walk || true ;;
            "") menu_start "$next" ;;
            [1-9])
                n=0
                for s in $WIZ_STEPS; do n=$((n + 1)); if [ "$n" = "$ANSWER" ]; then menu_start "$s"; fi; done
                ;;
            *) ui_say "Please type a step number, a, or q." ;;
        esac
    done
}
