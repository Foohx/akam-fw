# shellcheck shell=bash
# Host-side wizard steps. Sourced by ./akam after tools/lib/host.sh, log.sh, state.sh, ui.sh.
# Expected globals (set by ./akam): ROOT, OPT_* (command-line values), UI_DRY.
# Camera-side steps (find-camera, flash, verify, and the telnet part of backup) are placeholders
# until the camera wave: they print the manual instructions from the docs.
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
        sd_cli) v=${AKAM_SD_CLI:-$ROOT/tools/sd/akam_sd.sh} ;;
        hw_list) v=${AKAM_HW_LIST:-$ROOT/tools/release/supported_hardware.txt} ;;
        compat) v=${AKAM_KIT_COMPAT:-$ROOT/tools/wizard/kit_compat.txt} ;;
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
    ui_bold "Akam setup progress ($mode; state: $STATE_FILE)"
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

cmd_get_kit() {
    local kit=$OPT_KIT ver=$OPT_VERSION base dl f rc ext fmt sha vargs
    step_need get-kit || return 1
    step_banner get-kit
    if [ -z "$kit" ]; then
        if [ -z "$ver" ]; then
            case $AKAM_WIZARD_VERSION in
                *dev*)
                    if [ "$UI_DRY" = 1 ]; then
                        ver="X.Y.Z"
                        ui_say "  (development checkout: using the placeholder version X.Y.Z)"
                    else
                        ui_fail "this development checkout has no release to download." \
                    "pass --version X.Y.Z to download that release, or --kit FILE for a kit you already have, or run ./akam build."
                        return 1
                    fi
                    ;;
                *) ver=$AKAM_WIZARD_VERSION ;;
            esac
        fi
        base="https://github.com/$KIT_REPO/releases/download/v$ver"
        dl="$STATE_DIR/downloads/$ver"
        kit="$dl/akam-kit-$ver.tar.xz"
        ui_say "  Downloading kit $ver from $base"
        run_cmd mkdir -p "$dl" || return 1
        for f in "akam-kit-$ver.tar.xz" SHA256SUMS; do
            run_cmd curl --proto '=https' --tlsv1.2 -fL --retry 3 -o "$dl/$f" "$base/$f" ||
                { ui_fail "download of $f failed." "check your network and the version number, then retry."; return 1; }
        done
        if ! run_cmd curl --proto '=https' --tlsv1.2 -fL --retry 3 -o "$dl/SHA256SUMS.sig" "$base/SHA256SUMS.sig"; then
            ui_warn "no signature file in this release: the kit would be checked by checksum only."
            rm -f "$dl/SHA256SUMS.sig"
        fi
    fi
    if [ -e "$kit" ]; then
        kit="$(cd "$(dirname "$kit")" && pwd)/$(basename "$kit")"
    elif [ "$UI_DRY" != 1 ]; then
        { ui_fail "kit not found: $kit" "check the path given to --kit."; return 1; }
    fi

    ext="$STATE_DIR/kit"
    if [ "$UI_DRY" = 1 ]; then ext="$STATE_DIR/kit.dry"; fi
    vargs=(--extract-to "$ext")
    if [ -n "$OPT_ALLOW_UNSIGNED" ]; then vargs+=(--allow-unsigned); fi
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
        if ! ui_confirm_typed "Use this kit without a signature?" "USE UNSIGNED KIT"; then
            { ui_fail "unsigned kit refused." "get the signed release, or pass --allow-unsigned if you really mean it."; return 1; }
        fi
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
    state_set kit_version "$(_kit_manifest_value "$ext" akam_version)"
    state_mark_done get-kit
    ui_good "  Kit verified (format $fmt, sha256 ${sha:0:12}...)."
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
# Contract with tools/sd (parallel work), called through bash:
#   list-candidates                         one removable disk per line
#   prepare --mode backup|first-boot|flash-files --device DEV [--pubkey F] [--image F]
#   verify  --mode MODE --device DEV
# Wi-Fi settings reach it ONLY through the environment (AKAM_SD_SSID, AKAM_SD_PASS).
sd_available() { [ -f "$(wiz_tool sd_cli)" ]; }

# sd_call ARGS...: run the SD CLI with the Wi-Fi environment; rehearsal only shows it.
sd_call() {
    local rc=0
    export AKAM_SD_SSID="${SD_SSID:-}" AKAM_SD_PASS="${SD_PASS:-}"
    run_cmd bash "$(wiz_tool sd_cli)" "$@" || rc=$?
    unset AKAM_SD_SSID AKAM_SD_PASS
    return "$rc"
}

# sd_choose_device: set SD_DEV (asks, with the candidate list when the SD CLI exists).
sd_choose_device() {
    SD_DEV=$OPT_DEVICE
    if [ -z "$SD_DEV" ]; then
        if sd_available; then
            ui_say "  Removable disks found:"
            run_ro bash "$(wiz_tool sd_cli)" list-candidates || true
        fi
        if [ "$UI_DRY" = 1 ]; then SD_DEV=/dev/sdX; else
            ui_prompt "  Device of your microSD card (for example /dev/sdb), or empty to cancel:" "" || return 1
            SD_DEV=$ANSWER
        fi
    fi
    case $SD_DEV in
        /dev/?*) ;;
        *) { ui_fail "no valid device given." "pass --device /dev/sdX (whole disk, not a partition)."; return 1; } ;;
    esac
    case $SD_DEV in
        *" "*) { ui_fail "invalid device name." ""; return 1; } ;;
    esac
}

# sd_confirm_erase: typed ERASE <disk>; needs a TTY.
sd_confirm_erase() {
    local name=${SD_DEV##*/}
    ui_require_tty "erasing the microSD card" || return 1
    ui_warn "everything on $SD_DEV will be ERASED. Check the model and size above: this must be your microSD card."
    ui_confirm_typed "Erase $SD_DEV?" "ERASE $name" ||
        { ui_fail "erase not confirmed." "nothing was changed."; return 1; }
}

cmd_prepare_backup_sd() {
    step_need prepare-backup-sd || return 1
    step_banner prepare-backup-sd
    ui_say "  This card boots the camera into a temporary rescue shell (Telnet on port 24) so that"
    ui_say "  you can copy the original firmware. It holds your Wi-Fi password IN CLEAR TEXT:"
    ui_say "  treat the card as secret and delete wifi.conf from it when you are done."
    if [ "$UI_DRY" != 1 ] && ! sd_available; then
        ui_fail "the SD card tool ($(wiz_tool sd_cli)) is not available in this checkout." \
            "follow docs/akam_v1_backup_sd.md section 5 by hand, then run: ./akam step find-camera"
        return 1
    fi
    sd_choose_device || return 1
    SD_SSID=${OPT_SSID:-${AKAM_SD_SSID:-}}
    SD_PASS=${AKAM_SD_PASS:-}
    log_add_secret "$SD_PASS"
    if [ "$UI_DRY" != 1 ]; then
        if [ -z "$SD_SSID" ]; then
            ui_prompt "  Wi-Fi network name (SSID):" "" || return 1
            SD_SSID=$ANSWER
        fi
        if [ -z "$SD_PASS" ]; then
            ui_secret "  Wi-Fi password (typed without echo, never saved by the wizard):" || return 1
            SD_PASS=$SECRET
        fi
        if [ -z "$SD_SSID" ] || [ -z "$SD_PASS" ]; then
            { ui_fail "Wi-Fi name and password are required." ""; return 1; }
        fi
    fi
    ui_say "  Wi-Fi settings are passed to the SD tool through the environment (AKAM_SD_PASS=***)."
    sd_confirm_erase || return 1
    sd_call prepare --mode backup --device "$SD_DEV" || { ui_fail "preparing the card failed." "nothing else was changed; fix the problem above and retry."; return 1; }
    sd_call verify --mode backup --device "$SD_DEV" || { ui_fail "the card did not verify." "prepare it again."; return 1; }
    state_set backup_sd_device "$SD_DEV"
    state_mark_done prepare-backup-sd
    ui_good "  Backup card ready. Put it in the camera, power the camera on, then run: ./akam"
}

# ------------------------------------------------------------------ camera placeholders (next wave)
placeholder_step() { # name, doc, text...
    local name=$1 doc=$2
    shift 2
    step_banner "$name"
    ui_say "  Automatic handling of this step comes in the next release of the wizard."
    ui_say "  For now do it by hand: $doc"
    local l
    for l in "$@"; do ui_say "    $l"; done
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask: have you completed this step by hand? [y/N]"
        state_mark_done "$name"
        return 0
    fi
    if ui_confirm "  Have you completed this step by hand?"; then
        state_mark_done "$name"
        return 0
    fi
    ui_say "  Not marked as done. Run ./akam step $name when you have finished it."
    return 1
}

cmd_find_camera() {
    step_need find-camera || return 1
    placeholder_step find-camera "docs/akam_v1_backup_sd.md section 7" \
        "Insert the backup card, power the camera, wait about a minute." \
        "Find its IP address in your router's DHCP list (maker name 'AltoBeam')." \
        "Check it answers: ping -c 3 <ip>; the rescue shell listens on port 24."
}

cmd_flash() {
    step_need flash || return 1
    placeholder_step flash "docs/akam_v1_flash_and_restore.md" \
        "Insert the flash card, open the Telnet shell on port 24 and run install_with_flash_tool.sh." \
        "You must type YES yourself when it asks. Do NOT cut the power while it writes." \
        "Never paste several lines at once: one command at a time."
}

cmd_verify() {
    step_need verify || return 1
    placeholder_step verify "docs/akam_v1_ssh_and_lockdown.md and docs/akam_v1_test_plan.md" \
        "Check SSH with your key: ssh -i ~/.ssh/akam root@<ip>, then run akam_hwcheck.sh." \
        "Remove the card, reboot, and confirm only ports 22, 554 and 8091 are open."
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

cmd_backup() {
    local d=$OPT_BACKUP_DIR
    # Importing a backup you already have (--backup-dir) skips the camera steps before it.
    if [ -z "$d" ]; then step_need backup || return 1; fi
    step_banner backup
    ui_say "  Camera-side backup (double dd + copy to the PC) arrives with the camera steps."
    ui_say "  Until then do it by hand: docs/akam_v1_backup_sd.md sections 8 and 9,"
    ui_say "  then give the wizard the folder: ./akam backup --backup-dir DIR"
    if [ -z "$d" ]; then
        if [ "$UI_DRY" = 1 ]; then
            ui_say "  WOULD check your backup folder (8 files, APP size, SHA256SUMS) once --backup-dir is given."
            state_set backup_dir "<backup-dir>"
            state_set backup_sha dry-run
            state_mark_done backup
            return 0
        fi
        { ui_fail "no backup folder given." "run: ./akam backup --backup-dir DIR"; return 1; }
    fi
    [ -d "$d" ] || { ui_fail "not a folder: $d" ""; return 1; }
    d="$(cd "$d" && pwd)"
    backup_check "$d" || return 1
    ui_warn "COPY THIS FOLDER TO A SECOND MEDIUM (another disk or USB stick) BEFORE continuing."
    ui_warn "It is the only way back to the original firmware. Never publish it: it holds proprietary firmware."
    ui_confirm "  Have you made a second copy on a different medium?" ||
        { ui_fail "second copy not confirmed." "make the copy, then run this step again."; return 1; }
    state_set backup_dir "$d"
    state_set backup_sha "$(host_sha256 "$d/mtdblock7.bin")"
    state_mark_done backup
    ui_good "  Backup folder accepted: $d"
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
    out=${OPT_OUT:-$HOME/akam-image-$(date +%Y%m%d-%H%M%S)}
    pass=()
    if [ -n "$OPT_COMP" ]; then pass+=(--comp "$OPT_COMP"); fi
    if [ -n "$OPT_ALLOW_UNSIGNED" ]; then pass+=(--allow-unsigned); fi

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
                ui_confirm_typed "Build with unverified compression '${STOCK_COMP:-unknown}'?" "I HAVE CHECKED THE KERNEL" ||
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
            ui_warn "this firmware is NOT on the validated list. Akam may not work and the camera may need a restore."
            ui_require_tty "confirming an untested firmware" || return 1
            ui_confirm_typed "Continue with untested firmware?" "I UNDERSTAND THIS IS UNTESTED" ||
                { ui_fail "untested firmware not confirmed." "nothing was built."; return 1; }
            pass+=(--i-understand-untested)
        fi
    elif [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD check the backup (8 files, APP readable with unsquashfs, bin/sbin/local present),"
        ui_say "  show the stock compression and hardware verdict, and ask for"
        ui_say "  'I UNDERSTAND THIS IS UNTESTED' unless the firmware is validated."
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
        run_cmd bash "$(wiz_tool assemble)" --backup-dir "$bdir" --kit "${kit:-<kit>}" --out "$out" ${pass[@]+"${pass[@]}"} || rc=$?
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
    if [ "$(grep -c '' "$1")" -ne 1 ]; then ui_err "$1 must contain exactly one line"; return 1; fi
    case $(cut -d' ' -f1 "$1") in
        ssh-ed25519) ;;
        *) ui_err "$1 is not an ssh-ed25519 public key"; return 1 ;;
    esac
}

cmd_prepare_flash_sd() {
    local pub=${OPT_PUBKEY:-$HOME/.ssh/akam.pub}
    step_need prepare-flash-sd || return 1
    step_banner prepare-flash-sd
    if [ ! -f "$pub" ]; then
        ui_say "  No SSH key at $pub. Akam uses a dedicated key. The command to create it:"
        if [ "$UI_DRY" = 1 ]; then
            run_cmd ssh-keygen -t ed25519 -f "${pub%.pub}" -C akam
        else
            ui_confirm "  Create it now (you will be asked for a passphrase)?" || { ui_fail "no SSH key." "create one, then pass --pubkey FILE."; return 1; }
            run_cmd ssh-keygen -t ed25519 -f "${pub%.pub}" -C akam || return 1
        fi
    fi
    if [ -f "$pub" ] || [ "$UI_DRY" != 1 ]; then pubkey_check "$pub" || return 1; fi
    if [ "$UI_DRY" != 1 ] && ! sd_available; then
        ui_fail "the SD card tool ($(wiz_tool sd_cli)) is not available in this checkout." \
            "prepare the card by hand: see docs/akam_v1_ssh_and_lockdown.md and tools/sd/prepare_first_boot_sd.sh"
        return 1
    fi
    sd_choose_device || return 1
    SD_SSID=${OPT_SSID:-${AKAM_SD_SSID:-}}
    SD_PASS=${AKAM_SD_PASS:-}
    log_add_secret "$SD_PASS"
    if [ "$UI_DRY" != 1 ]; then
        if [ -z "$SD_SSID" ]; then
            ui_prompt "  Wi-Fi network name (SSID):" "" || return 1
            SD_SSID=$ANSWER
        fi
        if [ -z "$SD_PASS" ]; then
            ui_secret "  Wi-Fi password (typed without echo, never saved by the wizard):" || return 1
            SD_PASS=$SECRET
        fi
    fi
    sd_confirm_erase || return 1
    sd_call prepare --mode first-boot --device "$SD_DEV" --pubkey "$pub" --image "$(state_get image_path)" ||
        { ui_fail "preparing the card failed." "the camera was not touched; fix the problem above and retry."; return 1; }
    sd_call verify --mode first-boot --device "$SD_DEV" ||
        { ui_fail "the card did not verify (no *.bin at its root, authorized_keys must be your public key)." "prepare it again."; return 1; }
    state_set flash_sd_image_sha "$(state_get image_sha)"
    state_mark_done prepare-flash-sd
    ui_good "  Flash card ready. Remember: wifi.conf on the card holds your Wi-Fi password in clear text."
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
            "") step_run "$next" || true ;;
            [1-9])
                n=0
                for s in $WIZ_STEPS; do n=$((n + 1)); if [ "$n" = "$ANSWER" ]; then step_run "$s" || true; fi; done
                ;;
            *) ui_say "Please type a step number, a, or q." ;;
        esac
    done
}
