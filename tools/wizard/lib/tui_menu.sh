# shellcheck shell=bash
# The full-screen main menu of the wizard (alternate screen, keyboard driven). Sourced by ./akam
# after tui.sh and the step files; used only when TUI_ON=1. Needs: step_status, step_title,
# step_why, step_what, step_deps, step_blockers, step_validate, step_run, wizard_walk, cmd_* and
# state_get from the wizard libraries.
#
#   Up/Down, j/k   move            Enter  run the selected step
#   1-9            jump to a step   ?      what / why / risk of the selected step
#   r              pick several steps to run (checkboxes)     a  run every step that is not done
#   q              quit (progress is kept; ./akam resumes)

MENU_NAMES=()
MENU_KIND=()
MENU_TITLE=()
MENU_ST=()
MENU_NOTE=()
MENU_MSG=""
MENU_MSG_KIND=""
MENU_TOP=0
MENU_SEL=0
MENU_TOOLS="rtsp-auth update restore wipe-sd"

tool_title() {
    case $1 in
        rtsp-auth) echo "Password on the video stream" ;;
        update) echo "Update the image over SSH (no card)" ;;
        restore) echo "Restore the original firmware from your backup" ;;
        wipe-sd) echo "Erase a microSD card (it holds your Wi-Fi password)" ;;
    esac
}

# tui_menu_load: the list of rows: the wizard steps first, then the maintenance tools.
tui_menu_load() {
    local s
    MENU_NAMES=() MENU_KIND=() MENU_TITLE=()
    for s in $WIZ_STEPS; do
        MENU_NAMES+=("$s")
        MENU_KIND+=(step)
        MENU_TITLE+=("$(step_title "$s")")
    done
    for s in $MENU_TOOLS; do
        MENU_NAMES+=("$s")
        MENU_KIND+=(tool)
        MENU_TITLE+=("$(tool_title "$s")")
    done
}

# tui_menu_refresh: recompute every status (done / todo / blocked / stale). Slow (it hashes the
# kit and the image), so it runs after a step, not on every key.
tui_menu_refresh() {
    local i n=${#MENU_NAMES[@]} st reason
    MENU_ST=() MENU_NOTE=()
    for ((i = 0; i < n; i++)); do
        if [ "${MENU_KIND[i]}" != step ]; then
            MENU_ST+=(tool)
            MENU_NOTE+=("")
            continue
        fi
        st=$(step_status "${MENU_NAMES[i]}")
        MENU_ST+=("$st")
        case $st in
            blocked) MENU_NOTE+=("Waiting for: $(step_blockers "${MENU_NAMES[i]}")") ;;
            stale)
                reason=$(step_validate "${MENU_NAMES[i]}" || true)
                MENU_NOTE+=("Inputs changed: ${reason:-yes}. Run this step again.")
                ;;
            *) MENU_NOTE+=("") ;;
        esac
    done
}

# tui_menu_first_todo: index of the first step that is todo or stale (0 when all are done).
tui_menu_first_todo() {
    local i
    for ((i = 0; i < ${#MENU_NAMES[@]}; i++)); do
        case ${MENU_ST[i]} in todo | stale) printf '%s\n' "$i"; return 0 ;; esac
    done
    printf '0\n'
}

# tui_menu_os: "Arch Linux x86_64" from os-release (falls back to the detected family).
tui_menu_os() {
    local f name="" line
    f=$(host_os_release_file)
    if [ -r "$f" ]; then
        while IFS= read -r line; do
            case $line in PRETTY_NAME=*) name=${line#PRETTY_NAME=} ;; esac
        done <"$f"
    fi
    name=${name//\"/}
    [ -n "$name" ] || name=$(host_os)
    printf '%s %s\n' "$name" "$(host_arch)"
}

tui_menu_kit() {
    local v p
    v=$(state_get kit_version)
    p=$(state_get kit_path)
    if [ -z "$p" ]; then printf 'not downloaded yet\n'; return 0; fi
    printf 'akam %s%s\n' "${v:-?}" "$([ "$(state_get kit_unsigned_ok)" = 1 ] && echo ' (unsigned)')"
}

tui_menu_image() {
    local p sha
    p=$(state_get image_path)
    sha=$(state_get image_sha)
    if [ -z "$p" ]; then printf 'not built yet\n'; return 0; fi
    printf '%s %s\n' "${p##*/}" "${sha:+(${sha:0:8})}"
}

# tui_menu_draw: paint the whole screen in one write (no flicker).
tui_menu_draw() {
    local out="" w inner lw rw n=${#MENU_NAMES[@]} i first=1 done_n=0 sel=$1
    local rows_avail vis_rows top_pos sel_pos pos entries=() e line plain mark icon ic stw stc tw
    local title_l title_r fill os cam kit img lab tc

    w=$((TUI_COLS - 1))
    if [ "$w" -gt 100 ]; then w=100; fi
    inner=$((w - 4))
    lw=$((inner / 2))
    rw=$((inner - lw))
    for ((i = 0; i < n; i++)); do
        if [ "${MENU_ST[i]}" = "done" ]; then done_n=$((done_n + 1)); fi
    done
    local steps_n=0
    for ((i = 0; i < n; i++)); do if [ "${MENU_KIND[i]}" = step ]; then steps_n=$((steps_n + 1)); fi; done

    # header box
    title_l="Akam $TUI_I_DOT guided setup"
    title_r="v$AKAM_WIZARD_VERSION  $done_n/$steps_n done"
    if [ "$UI_DRY" = 1 ]; then title_r="REHEARSAL (--dry-run)  $title_r"; fi
    tui_rep "$TUI_I_H" $((w - 5 - ${#title_l} - ${#title_r} - 2))
    fill=$REPLY
    out+="$TUI_C$TUI_I_TL$TUI_I_H $TUI_B$title_l$TUI_RST$TUI_C $fill ${TUI_RST}${TUI_D}${title_r}${TUI_RST}${TUI_C} $TUI_I_TR$TUI_RST"$'\e[K\n'
    os=$(tui_menu_os)
    cam=${OPT_IP:-$(state_get camera_ip)}
    [ -n "$cam" ] || cam="not set yet"
    kit=$(tui_menu_kit)
    img=$(tui_menu_image)
    local pair
    for pair in "Host|$os|Camera|$cam" "Kit|$kit|Image|$img"; do
        IFS='|' read -r lab os cam kit <<<"$pair"
        tui_fit "$os" $((lw - 8))
        line="$TUI_D$(printf '%-8s' "$lab")$TUI_RST$REPLY"
        tui_fit "$kit" $((rw - 8 - 1))
        line+=" $TUI_D$(printf '%-8s' "$cam")$TUI_RST$REPLY"
        out+="$TUI_C$TUI_I_V$TUI_RST $line $TUI_C$TUI_I_V$TUI_RST"$'\e[K\n'
    done
    tui_rep "$TUI_I_H" $((w - 2))
    out+="$TUI_C$TUI_I_BL$REPLY$TUI_I_BR$TUI_RST"$'\e[K\n'

    # list rows (steps, divider, tools) inside a scrolling window
    for ((i = 0; i < n; i++)); do
        if [ "${MENU_KIND[i]}" = tool ] && [ "$first" = 1 ]; then entries+=(-1); first=0; fi
        entries+=("$i")
    done
    rows_avail=$((TUI_ROWS - 9))
    if [ "$rows_avail" -lt 3 ]; then rows_avail=3; fi
    vis_rows=${#entries[@]}
    if [ "$vis_rows" -gt "$rows_avail" ]; then vis_rows=$rows_avail; fi
    sel_pos=0
    for ((pos = 0; pos < ${#entries[@]}; pos++)); do
        if [ "${entries[pos]}" = "$sel" ]; then sel_pos=$pos; fi
    done
    top_pos=$MENU_TOP
    if [ "$sel_pos" -lt "$top_pos" ]; then top_pos=$sel_pos; fi
    if [ "$sel_pos" -ge $((top_pos + vis_rows)) ]; then top_pos=$((sel_pos - vis_rows + 1)); fi
    MENU_TOP=$top_pos
    tw=$((w - 19))
    for ((pos = top_pos; pos < top_pos + vis_rows; pos++)); do
        e=${entries[pos]}
        if [ "$e" = -1 ]; then
            tui_rep "$TUI_I_H" $((w - 18))
            out+="  $TUI_D$TUI_I_H$TUI_I_H Maintenance $REPLY$TUI_RST"$'\e[K\n'
            continue
        fi
        case ${MENU_ST[e]} in
            done) icon=$TUI_I_DONE ic=$TUI_G stw="done" stc=$TUI_G ;;
            todo) icon=$TUI_I_TODO ic=$TUI_C stw="todo" stc=$TUI_C ;;
            blocked) icon=$TUI_I_BLOCK ic=$TUI_R stw="blocked" stc=$TUI_R ;;
            stale) icon=$TUI_I_STALE ic=$TUI_Y stw="stale" stc=$TUI_Y ;;
            *) icon=$TUI_I_TOOL ic=$TUI_BL stw="" stc=$TUI_D ;;
        esac
        mark="  "
        if [ "$e" = "$sel" ]; then mark="$TUI_I_SEL "; fi
        if [ "${MENU_KIND[e]}" = step ]; then printf -v lab '%2d' $((e + 1)); else lab="  "; fi
        tui_fit "${MENU_TITLE[e]}" "$tw"
        if [ "$e" = "$sel" ]; then
            printf -v plain ' %s%s %s  %s %-9s' "$mark" "$icon" "$lab" "$REPLY" "$stw"
            tui_fit "$plain" "$w"
            line="$TUI_REV$TUI_B$REPLY$TUI_RST"
        else
            tc=""
            if [ "${MENU_ST[e]}" = blocked ]; then tc=$TUI_D; fi
            line=" $mark$ic$icon$TUI_RST $TUI_D$lab$TUI_RST  $tc$REPLY$TUI_RST "
            printf -v line '%s%s%-9s%s' "$line" "$stc" "$stw" "$TUI_RST"
        fi
        out+="$line"$'\e[K\n'
    done

    # detail for the selected row, message line, footer
    tui_wrap $((w - 4)) "$(step_what "${MENU_NAMES[sel]}")"
    out+=$'\e[K\n'
    for ((i = 0; i < 2; i++)); do
        line=${TUI_WRAPPED[i]:-}
        if [ "$i" -eq 1 ] && [ "${#TUI_WRAPPED[@]}" -gt 2 ]; then line="${line%?}$TUI_I_ELL"; fi
        out+="  $TUI_D$line$TUI_RST"$'\e[K\n'
    done
    if [ -n "$MENU_MSG" ]; then
        case $MENU_MSG_KIND in
            err) stc=$TUI_R ;;
            ok) stc=$TUI_G ;;
            *) stc=$TUI_Y ;;
        esac
        tui_clip "$MENU_MSG" $((w - 4))
        out+="  $stc$REPLY$TUI_RST"$'\e[K\n'
    elif [ -n "${MENU_NOTE[sel]}" ]; then
        tui_clip "${MENU_NOTE[sel]}" $((w - 4))
        out+="  $TUI_Y$REPLY$TUI_RST"$'\e[K\n'
    else
        out+=$'\e[K\n'
    fi
    tui_clip "$TUI_I_UP$TUI_I_DN move  $TUI_I_ENTER run  ? help  r pick several  a run all  q quit" $((w - 2))
    out+=" $TUI_B$REPLY$TUI_RST"$'\e[K\e[J'
    printf '\e[H%s' "$out" >&2
}

# tui_menu_help INDEX: what / why / risk of one row, in a box, until a key is pressed.
tui_menu_help() {
    local i=$1 name why="" risk="" needs st l
    name=${MENU_NAMES[i]}
    if [ "${MENU_KIND[i]}" = step ]; then
        why=$(step_why "$name")
        case $why in
            *" Risk: "*) risk=${why#* Risk: }; why=${why%% Risk: *} ;;
        esac
        why=${why#Why: }
        needs=$(step_deps "$name")
        st=${MENU_ST[i]}
    fi
    TUI_BUF=()
    tui_box_build "$TUI_C" "${MENU_TITLE[i]}" "What it does" "  $(step_what "$name")" ""
    if [ -n "$why" ]; then tui_box_build "$TUI_G" "Why" "$why"; fi
    if [ -n "$risk" ]; then tui_box_build "$TUI_Y" "Risk" "$risk"; fi
    if [ "${MENU_KIND[i]}" = step ]; then
        tui_box_build "$TUI_D" "Status" "${st}${needs:+  $TUI_I_DOT  needs: $needs}"
    fi
    printf '\e[H\e[2J' >&2
    for l in "${TUI_BUF[@]}"; do printf '%s\n' "$l" >&2; done
    printf '\n %sPress any key to go back%s' "$TUI_D" "$TUI_RST" >&2
    tui_readkey
}

# tui_menu_run INDEX: leave the full screen, run the step or tool in the normal terminal (it
# talks through the same widgets), then come back to the menu.
tui_menu_run() {
    local i=$1 name rc=0 pick
    name=${MENU_NAMES[i]}
    tui_screen_leave
    printf '\n' >&2
    if [ "${MENU_KIND[i]}" = step ]; then
        step_run "$name" || rc=$?
    else
        case $name in
            rtsp-auth)
                if tui_choose "Video stream password" "Set or change the password" "Switch it off" "Show the current state"; then
                    pick=$TUI_INDEX
                    case $pick in
                        0) cmd_rtsp_auth set || rc=$? ;;
                        1) cmd_rtsp_auth disable || rc=$? ;;
                        *) cmd_rtsp_auth status || rc=$? ;;
                    esac
                fi
                ;;
            update) cmd_update || rc=$? ;;
            restore) cmd_restore || rc=$? ;;
            wipe-sd) cmd_wipe_sd || rc=$? ;;
        esac
    fi
    printf '\n' >&2
    if [ "$rc" -eq 0 ]; then
        printf '%s%s%s %s: finished\n' "$TUI_G" "$TUI_I_OK" "$TUI_RST" "${MENU_TITLE[i]}" >&2
        MENU_MSG="${MENU_TITLE[i]}: finished" MENU_MSG_KIND=ok
    else
        printf '%s%s%s %s: stopped (exit %s). Fix the problem above, then run it again.\n' "$TUI_R" "$TUI_I_ERR" "$TUI_RST" "${MENU_TITLE[i]}" "$rc" >&2
        MENU_MSG="${MENU_TITLE[i]}: stopped (exit $rc)" MENU_MSG_KIND=err
    fi
    tui_pause "Press any key to return to the menu..."
    tui_screen_enter
    tui_menu_refresh
    # after a success, the cursor goes to the next thing to do
    if [ "$rc" -eq 0 ] && [ "${MENU_KIND[i]}" = step ]; then MENU_SEL=$(tui_menu_first_todo); fi
    return 0
}

# tui_menu_pick: checkboxes over the steps that can run now; they run in order, stopping at the
# first failure.
tui_menu_pick() {
    local i items=() idx=() init="" k=0 rc=0 s
    for ((i = 0; i < ${#MENU_NAMES[@]}; i++)); do
        [ "${MENU_KIND[i]}" = step ] || continue
        [ "${MENU_ST[i]}" != blocked ] || continue
        items+=("${MENU_TITLE[i]}  [${MENU_ST[i]}]")
        idx+=("$i")
        case ${MENU_ST[i]} in todo | stale) init+="$k " ;; esac
        k=$((k + 1))
    done
    if [ "${#items[@]}" -eq 0 ]; then
        MENU_MSG="No step can run yet." MENU_MSG_KIND=warn
        return 0
    fi
    tui_screen_leave
    printf '\n' >&2
    if tui_multi -i "$init" "Which steps do you want to run now (in this order)?" "${items[@]}"; then
        for s in ${TUI_SELECTED[@]+"${TUI_SELECTED[@]}"}; do
            printf '\n' >&2
            step_run "${MENU_NAMES[${idx[s]}]}" || { rc=$?; break; }
        done
        if [ "$rc" -ne 0 ]; then
            MENU_MSG="Stopped at ${MENU_TITLE[${idx[s]}]} (exit $rc)." MENU_MSG_KIND=err
        else
            MENU_MSG="Selected steps finished." MENU_MSG_KIND=ok
        fi
        tui_pause "Press any key to return to the menu..."
    fi
    tui_screen_enter
    tui_menu_refresh
    return 0
}

# tui_main_menu: the event loop. Returns 0 when the user quits.
tui_main_menu() {
    local n k
    tui_menu_load
    tui_menu_refresh
    n=${#MENU_NAMES[@]}
    MENU_SEL=$(tui_menu_first_todo)
    MENU_MSG="" MENU_MSG_KIND="" MENU_TOP=0
    tui_size
    tui_screen_enter
    while true; do
        if [ "$TUI_RESIZED" = 1 ]; then
            tui_size
            printf '\e[2J' >&2
        fi
        if [ "$TUI_ROWS" -lt 12 ] || [ "$TUI_COLS" -lt 44 ]; then
            printf '\e[H\e[2JTerminal too small (%sx%s): need at least 44x12.\nResize it, or run ./akam --plain.\e[J' "$TUI_COLS" "$TUI_ROWS" >&2
            tui_readkey 1
            case $TUI_KEY in q | ctrl-c | eof) break ;; esac
            continue
        fi
        tui_menu_draw "$MENU_SEL"
        tui_readkey 1
        case $TUI_KEY in
            timeout | unknown) continue ;;
        esac
        MENU_MSG="" MENU_MSG_KIND=""
        case $TUI_KEY in
            up | k) MENU_SEL=$(((MENU_SEL + n - 1) % n)) ;;
            down | j | tab) MENU_SEL=$(((MENU_SEL + 1) % n)) ;;
            home | g) MENU_SEL=0 ;;
            end | G) MENU_SEL=$((n - 1)) ;;
            pgup) MENU_SEL=$((MENU_SEL > 5 ? MENU_SEL - 5 : 0)) ;;
            pgdn) MENU_SEL=$((MENU_SEL + 5 < n ? MENU_SEL + 5 : n - 1)) ;;
            [1-9])
                k=$((TUI_KEY - 1))
                if [ "$k" -lt "$n" ] && [ "${MENU_KIND[k]}" = step ]; then MENU_SEL=$k; fi
                ;;
            '?' | h) tui_menu_help "$MENU_SEL" ;;
            r) tui_menu_pick ;;
            a)
                tui_screen_leave
                printf '\n' >&2
                if wizard_walk; then MENU_MSG="Run finished." MENU_MSG_KIND=ok; else MENU_MSG="Run stopped." MENU_MSG_KIND=err; fi
                tui_pause "Press any key to return to the menu..."
                tui_screen_enter
                tui_menu_refresh
                ;;
            enter)
                if [ "${MENU_ST[MENU_SEL]}" = blocked ]; then
                    MENU_MSG="${MENU_NOTE[MENU_SEL]}  (run those steps first)" MENU_MSG_KIND=warn
                else
                    tui_menu_run "$MENU_SEL"
                fi
                ;;
            q | ctrl-c | eof | esc) break ;;
        esac
    done
    tui_screen_leave
    ui_say "Progress is saved ($STATE_FILE). Run ./akam to resume."
    return 0
}
