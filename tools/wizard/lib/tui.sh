# shellcheck shell=bash
# shellcheck disable=SC2034,SC2119,SC2120  # theme/result variables are read by other files; tui_readkey's timeout is optional
# Terminal UI toolkit for the wizard: pure bash, no dependency (no gum, dialog, whiptail, fzf).
# Sourced by ./akam after ui.sh. Everything here is inert until tui_init turns TUI_ON on, which
# happens only on a real terminal (stdin, stdout and stderr are TTYs, TERM is not dumb) and
# without --plain / AKAM_PLAIN. Otherwise ui.sh keeps its line-based behaviour unchanged.
#
# Widgets (inline, drawn on stderr, redrawn in place, collapsed to one line when done):
#   tui_choose [-d N] TITLE ITEM...        single choice   -> TUI_INDEX, TUI_RESULT
#   tui_multi [-i "I J"] TITLE ITEM...     checkboxes      -> TUI_SELECTED (indexes)
#   tui_yesno PROMPT [y|n]                 yes/no          -> return 0 (yes) / 1 (no, Esc, EOF)
#   tui_input [-s] [-v] PROMPT [DEFAULT]   text / secret   -> TUI_RESULT
#   tui_typed PROMPT EXPECTED              typed phrase    -> return 0 only on an exact match
#   tui_task LABEL CMD...                  spinner + elapsed time (+ progress bar when known)
#   tui_box COLOR TITLE LINE...            coloured section box
#   tui_error_panel MSG CMD RC HINT        error panel with the exact failed command
# Keys come from file descriptor TUI_FD (0 = the terminal). Tests feed a key file instead.
# Test hooks: AKAM_TUI_FORCE=1 (enable without a TTY), AKAM_TUI_SIZE=COLSxROWS,
# AKAM_TUI_COLOR=1 (force colour), AKAM_TUI_ASCII=1 (force the ASCII icon set).

TUI_ON=0
TUI_FD=0
TUI_KEY=""
TUI_RESULT=""
TUI_INDEX=-1
TUI_SELECTED=()
TUI_COLS=80
TUI_ROWS=24
TUI_STTY=""
TUI_RAW=0
TUI_ALT=0
TUI_CURSOR_HIDDEN=0
TUI_RESIZED=0
TUI_TASK_PID=""
TUI_PENDING_SIG=""
TUI_REGION=0
TUI_BUF=()
TUI_ABOVE=()
TUI_WRAPPED=()
TUI_CH=""
TUI_UTF8=0
TUI_SECRET_SHOW=0

# ------------------------------------------------------------------ mode, theme, size
# tui_utf8_ok: the locale is UTF-8 AND bash really counts characters (not bytes).
tui_utf8_ok() {
    local loc=${LC_ALL:-${LC_CTYPE:-${LANG:-}}} probe='✔'
    [ "${AKAM_TUI_ASCII:-}" != 1 ] || return 1
    case $loc in
        *[Uu][Tt][Ff]-8* | *[Uu][Tt][Ff]8*) ;;
        *) return 1 ;;
    esac
    [ "${#probe}" -eq 1 ]
}

# tui_color_ok: colours allowed (NO_COLOR, dumb terminals and non-TTY output turn them off).
tui_color_ok() {
    [ -z "${NO_COLOR:-}" ] || return 1
    case ${TERM:-dumb} in dumb | "") return 1 ;; esac
    [ "${AKAM_TUI_COLOR:-}" = 1 ] || [ -t 2 ]
}

# tui_theme: set icons and colour tokens (empty strings when colours are off).
tui_theme() {
    if tui_utf8_ok; then
        TUI_UTF8=1
        TUI_I_DONE='✔' TUI_I_TODO='●' TUI_I_BLOCK='✖' TUI_I_STALE='⟳' TUI_I_SEL='▸' TUI_I_TOOL='◆'
        TUI_I_OK='✔' TUI_I_ERR='✖' TUI_I_WARN='▲' TUI_I_ELL='…' TUI_I_DOT='·' TUI_I_MASK='•'
        TUI_I_TL='╭' TUI_I_TR='╮' TUI_I_BL='╰' TUI_I_BR='╯' TUI_I_H='─' TUI_I_V='│' TUI_I_HH='━'
        TUI_I_FULL='█' TUI_I_EMPTY='░' TUI_I_UP='↑' TUI_I_DN='↓' TUI_I_ENTER='⏎' TUI_I_ARROW='›'
        TUI_SPIN=('⠋' '⠙' '⠹' '⠸' '⠼' '⠴' '⠦' '⠧' '⠇' '⠏')
    else
        TUI_UTF8=0
        TUI_I_DONE='+' TUI_I_TODO='o' TUI_I_BLOCK='x' TUI_I_STALE='~' TUI_I_SEL='>' TUI_I_TOOL='*'
        TUI_I_OK='+' TUI_I_ERR='x' TUI_I_WARN='!' TUI_I_ELL='~' TUI_I_DOT='-' TUI_I_MASK='*'
        TUI_I_TL='+' TUI_I_TR='+' TUI_I_BL='+' TUI_I_BR='+' TUI_I_H='-' TUI_I_V='|' TUI_I_HH='='
        TUI_I_FULL='#' TUI_I_EMPTY='-' TUI_I_UP='^' TUI_I_DN='v' TUI_I_ENTER='Enter' TUI_I_ARROW='>'
        TUI_SPIN=('|' '/' '-' $'\\')
    fi
    if tui_color_ok; then
        TUI_RST=$'\e[0m' TUI_B=$'\e[1m' TUI_D=$'\e[90m' TUI_REV=$'\e[7m'
        TUI_R=$'\e[31m' TUI_G=$'\e[32m' TUI_Y=$'\e[33m' TUI_BL=$'\e[34m' TUI_M=$'\e[35m' TUI_C=$'\e[36m'
    else
        TUI_RST="" TUI_B="" TUI_D="" TUI_REV="" TUI_R="" TUI_G="" TUI_Y="" TUI_BL="" TUI_M="" TUI_C=""
    fi
}

# tui_size: set TUI_COLS / TUI_ROWS (AKAM_TUI_SIZE=COLSxROWS overrides, for tests).
tui_size() {
    local r="" c="" sz=${AKAM_TUI_SIZE:-}
    TUI_RESIZED=0
    if [ -n "$sz" ]; then
        c=${sz%x*}
        r=${sz#*x}
    elif [ -t 2 ]; then
        { read -r r c; } < <(stty size <&2 2>/dev/null) || true
    fi
    case $r$c in '' | *[!0-9]*) r=24 c=80 ;; esac
    [ "$r" -gt 0 ] || r=24
    [ "$c" -gt 0 ] || c=80
    TUI_ROWS=$r
    TUI_COLS=$c
}

# tui_init PLAIN: decide the mode once. PLAIN=1 (--plain) or AKAM_PLAIN forces the line-based UI.
tui_init() {
    TUI_ON=0
    if [ "${1:-0}" = 1 ] || [ -n "${AKAM_PLAIN:-}" ]; then return 0; fi
    if [ "${AKAM_TUI_FORCE:-}" != 1 ]; then
        { [ -t 0 ] && [ -t 1 ] && [ -t 2 ]; } || return 0
    fi
    case ${TERM:-dumb} in dumb | "") return 0 ;; esac
    TUI_ON=1
    tui_theme
    tui_size
    tui_install_traps
}

# ------------------------------------------------------------------ terminal state and traps
tui_raw_on() {
    [ "$TUI_RAW" = 1 ] && return 0
    if [ -t "$TUI_FD" ]; then
        TUI_STTY=$(stty -g <&"$TUI_FD" 2>/dev/null) || TUI_STTY=""
        stty -echo -icanon min 1 time 0 <&"$TUI_FD" 2>/dev/null || true
    fi
    TUI_RAW=1
    return 0
}

tui_raw_off() {
    if [ "$TUI_RAW" = 1 ] && [ -n "$TUI_STTY" ] && [ -t "$TUI_FD" ]; then
        stty "$TUI_STTY" <&"$TUI_FD" 2>/dev/null || stty sane <&"$TUI_FD" 2>/dev/null || true
    fi
    TUI_RAW=0
    return 0
}

tui_cursor_hide() {
    printf '\e[?25l' >&2
    TUI_CURSOR_HIDDEN=1
}

tui_cursor_show() {
    if [ "$TUI_CURSOR_HIDDEN" = 1 ]; then printf '\e[?25h' >&2; fi
    TUI_CURSOR_HIDDEN=0
}

# tui_screen_enter / tui_screen_leave: alternate screen + raw keys for the full-screen menu.
tui_screen_enter() {
    tui_raw_on
    printf '\e[?1049h\e[2J\e[H' >&2
    TUI_ALT=1
    tui_cursor_hide
}

tui_screen_leave() {
    tui_cursor_show
    if [ "$TUI_ALT" = 1 ]; then printf '\e[?1049l' >&2; fi
    TUI_ALT=0
    tui_raw_off
}

# tui_cleanup: put the terminal back (idempotent; runs on EXIT, Ctrl-C, hangup, kill).
tui_cleanup() {
    [ "$TUI_ON" = 1 ] || return 0
    if [ -n "$TUI_TASK_PID" ]; then
        kill "$TUI_TASK_PID" 2>/dev/null || true
        TUI_TASK_PID=""
    fi
    tui_cursor_show
    if [ "$TUI_ALT" = 1 ]; then printf '\e[?1049l' >&2; fi
    TUI_ALT=0
    tui_raw_off
    if [ -n "$TUI_RST" ]; then printf '%s' "$TUI_RST" >&2; fi
    return 0
}

tui_on_signal() { # NAME NUMBER
    tui_cleanup
    printf '\n%s\n' "Interrupted ($1). Nothing was changed after this point; run ./akam to resume." >&2
    exit $((128 + $2))
}

tui_on_winch() { TUI_RESIZED=1; }

# tui_sig_handler NAME NUMBER: while the keyboard is being read (raw mode), only note the signal:
# exiting from inside bash's read would let read put its own saved (raw) terminal modes back after
# our cleanup. tui_readkey notices it as soon as read returns (at most half a second later).
# Outside a read (a step is running, a spinner is drawn) it ends the program at once.
tui_sig_handler() {
    if [ "$TUI_RAW" = 1 ]; then
        TUI_PENDING_SIG="$1 $2"
    else
        tui_on_signal "$1" "$2"
    fi
}

tui_install_traps() {
    trap tui_cleanup EXIT
    trap 'tui_sig_handler INT 2' INT
    trap 'tui_sig_handler HUP 1' HUP
    trap 'tui_sig_handler TERM 15' TERM
    trap tui_on_winch WINCH
}

# tui_rearm_traps: call after a section that ran with "trap - INT HUP TERM" (the flash write).
tui_rearm_traps() {
    if [ "$TUI_ON" = 1 ]; then tui_install_traps; fi
    return 0
}

# ------------------------------------------------------------------ keys
# tui_readkey [TIMEOUT]: wait for one key; TUI_KEY is one of
#   up down left right home end pgup pgdn enter esc tab space backspace delete
#   ctrl-a ctrl-c ctrl-e ctrl-k ctrl-u ctrl-w eof timeout resize unknown, or the typed character.
tui_readkey() {
    local k="" k2="" c="" seq="" rc=0 t=${1:-0.5}
    TUI_KEY=""
    while true; do
        rc=0
        IFS= read -rsn1 -t "$t" k <&"$TUI_FD" || rc=$?
        if [ -n "$TUI_PENDING_SIG" ]; then
            # shellcheck disable=SC2086  # "NAME NUMBER", two words on purpose
            tui_on_signal $TUI_PENDING_SIG
        fi
        if [ "$rc" -eq 0 ]; then break; fi
        if [ "$rc" -gt 128 ]; then
            if [ -n "${1:-}" ]; then TUI_KEY=timeout; return 0; fi
            # a resize while an inline widget waits: re-measure and let the caller redraw
            if [ "$TUI_RESIZED" = 1 ]; then tui_size; TUI_KEY=resize; return 0; fi
            continue
        fi
        TUI_KEY=eof
        return 0
    done
    case $k in
        "" | $'\r') TUI_KEY=enter ;;
        " ") TUI_KEY=space ;;
        $'\t') TUI_KEY=tab ;;
        $'\x7f' | $'\b') TUI_KEY=backspace ;;
        $'\x01') TUI_KEY=ctrl-a ;;
        $'\x03') TUI_KEY=ctrl-c ;;
        $'\x04') TUI_KEY=eof ;;
        $'\x05') TUI_KEY=ctrl-e ;;
        $'\x0b') TUI_KEY=ctrl-k ;;
        $'\x15') TUI_KEY=ctrl-u ;;
        $'\x17') TUI_KEY=ctrl-w ;;
        $'\e')
            if ! IFS= read -rsn1 -t 0.05 k2 <&"$TUI_FD"; then
                TUI_KEY=esc
                return 0
            fi
            case $k2 in
                '[' | O)
                    while IFS= read -rsn1 -t 0.05 c <&"$TUI_FD"; do
                        seq+=$c
                        case $c in [A-Za-z~]) break ;; esac
                    done
                    case ${seq: -1} in
                        A) TUI_KEY=up ;;
                        B) TUI_KEY=down ;;
                        C) TUI_KEY=right ;;
                        D) TUI_KEY=left ;;
                        H) TUI_KEY=home ;;
                        F) TUI_KEY=end ;;
                        Z) TUI_KEY=shift-tab ;;
                        '~')
                            case ${seq%?} in
                                1 | 7) TUI_KEY=home ;;
                                4 | 8) TUI_KEY=end ;;
                                3) TUI_KEY=delete ;;
                                5) TUI_KEY=pgup ;;
                                6) TUI_KEY=pgdn ;;
                                *) TUI_KEY=unknown ;;
                            esac
                            ;;
                        *) TUI_KEY=unknown ;;
                    esac
                    ;;
                *) TUI_KEY=unknown ;;
            esac
            ;;
        *)
            case $k in
                [[:cntrl:]]) TUI_KEY=unknown ;;
                *) TUI_KEY=$k ;;
            esac
            ;;
    esac
    return 0
}

# tui_is_char: the last key is one printable character (TUI_CH holds it; space included).
tui_is_char() {
    TUI_CH=""
    if [ "$TUI_KEY" = space ]; then TUI_CH=" "; return 0; fi
    if [ "${#TUI_KEY}" -eq 1 ]; then TUI_CH=$TUI_KEY; return 0; fi
    return 1
}

# ------------------------------------------------------------------ small helpers
# tui_rep CHAR N: set REPLY to CHAR repeated N times.
tui_rep() {
    local n=$2 s
    if [ "$n" -le 0 ]; then REPLY=""; return 0; fi
    printf -v s '%*s' "$n" ""
    REPLY=${s// /$1}
}

# tui_fit TEXT WIDTH: set REPLY to TEXT cut with an ellipsis and padded to exactly WIDTH.
tui_fit() {
    local t=$1 w=$2 pad
    if [ "${#t}" -gt "$w" ]; then
        if [ "$w" -gt 1 ]; then t="${t:0:$((w - 1))}$TUI_I_ELL"; else t=${t:0:$w}; fi
    fi
    pad=$((w - ${#t}))
    printf -v REPLY '%s%*s' "$t" "$pad" ""
}

# tui_clip TEXT WIDTH: like tui_fit without the padding.
tui_clip() {
    REPLY=$1
    if [ "${#REPLY}" -gt "$2" ]; then
        if [ "$2" -gt 1 ]; then REPLY="${REPLY:0:$(($2 - 1))}$TUI_I_ELL"; else REPLY=${REPLY:0:$2}; fi
    fi
}

# tui_wrap WIDTH TEXT: fill the array TUI_WRAPPED with TEXT wrapped at word boundaries.
tui_wrap() {
    local l
    TUI_WRAPPED=()
    while IFS= read -r l; do
        TUI_WRAPPED+=("$l")
    done < <(printf '%s\n' "$2" | fold -s -w "$1" | sed 's/[[:space:]]*$//')
    if [ "${#TUI_WRAPPED[@]}" -eq 0 ]; then TUI_WRAPPED=(""); fi
}

# tui_elapsed SECONDS: set REPLY to m:ss.
tui_elapsed() {
    printf -v REPLY '%d:%02d' $(($1 / 60)) $(($1 % 60))
}

# tui_bar PERCENT WIDTH: set REPLY to a progress bar followed by the percentage.
tui_bar() {
    local p=$1 w=$2 f e fs es
    [ "$p" -ge 0 ] || p=0
    [ "$p" -le 100 ] || p=100
    f=$((p * w / 100))
    e=$((w - f))
    tui_rep "$TUI_I_FULL" "$f"
    fs=$REPLY
    tui_rep "$TUI_I_EMPTY" "$e"
    es=$REPLY
    printf -v REPLY '%s%s%s%s%s %3d%%' "$TUI_G" "$fs" "$TUI_D" "$es" "$TUI_RST" "$p"
}

# tui_parse_progress TEXT [TOTAL_BYTES]: print an integer 0-100 found in TEXT, or nothing.
# Understands "45%" / "45.3%" (curl, rsync, pv) and, with TOTAL_BYTES, dd's "N bytes".
tui_parse_progress() {
    local t=$1 total=${2:-} p="" n
    t=${t//$'\r'/$'\n'}
    p=$(printf '%s\n' "$t" | grep -Eo '[0-9]{1,3}(\.[0-9]+)?%' | tail -n1 || true)
    if [ -n "$p" ]; then
        p=${p%%[.%]*}
        if [ "$p" -le 100 ]; then printf '%s\n' "$p"; return 0; fi
        return 0
    fi
    if [ -n "$total" ] && [ "$total" -gt 0 ] 2>/dev/null; then
        n=$(printf '%s\n' "$t" | grep -Eo '^[0-9]+ bytes' | tail -n1 | cut -d' ' -f1 || true)
        if [ -n "$n" ]; then
            p=$((n * 100 / total))
            if [ "$p" -gt 100 ]; then p=100; fi
            printf '%s\n' "$p"
        fi
    fi
    return 0
}

# ------------------------------------------------------------------ inline region
# TUI_BUF holds the lines of the widget, already fitted to the width. The cursor always rests
# on the last line, so a redraw is: go up TUI_REGION-1 lines, clear to the end, print again.
tui_region_draw() { # [CURSOR_COLUMN]
    local out="" i n=${#TUI_BUF[@]}
    if [ "$TUI_REGION" -gt 1 ]; then out=$'\e['$((TUI_REGION - 1))'A'; fi
    out+=$'\r\e[J'
    for ((i = 0; i < n; i++)); do
        out+=${TUI_BUF[i]}
        if [ "$i" -lt $((n - 1)) ]; then out+=$'\n'; fi
    done
    if [ -n "${1:-}" ]; then
        out+=$'\r'
        if [ "$1" -gt 0 ]; then out+=$'\e['$1'C'; fi
    fi
    printf '%s' "$out" >&2
    TUI_REGION=$n
}

# tui_region_done SUMMARY: replace the widget by one summary line and move on.
tui_region_done() {
    local out=""
    if [ "$TUI_REGION" -gt 1 ]; then out=$'\e['$((TUI_REGION - 1))'A'; fi
    out+=$'\r\e[J'"$1"$'\n'
    printf '%s' "$out" >&2
    TUI_REGION=0
}

tui_widget_begin() { # [hide-cursor]
    tui_size
    TUI_REGION=0
    tui_raw_on
    if [ "${1:-}" = hide ]; then tui_cursor_hide; fi
}

tui_widget_end() {
    tui_cursor_show
    tui_raw_off
}

# tui_label TEXT: strip leading blanks and a trailing colon from a prompt.
tui_label() {
    local t=$1
    t=${t#"${t%%[![:space:]]*}"}
    t=${t%"${t##*[![:space:]]}"}
    t=${t%:}
    REPLY=$t
}

# tui_hint TEXT: dim help line fitted to the width.
tui_hint() {
    tui_clip "  $1" $((TUI_COLS - 1))
    REPLY="$TUI_D$REPLY$TUI_RST"
}

# ------------------------------------------------------------------ widget: single choice
tui_choose() {
    local def=0 title n sel=0 top=0 vis i line txt lab
    if [ "${1:-}" = -d ]; then def=$2; shift 2; fi
    title=$1
    shift
    local items=("$@")
    n=${#items[@]}
    TUI_RESULT="" TUI_INDEX=-1
    [ "$n" -gt 0 ] || return 1
    sel=$def
    if [ "$sel" -ge "$n" ] || [ "$sel" -lt 0 ]; then sel=0; fi
    tui_label "$title"
    lab=$REPLY
    tui_widget_begin hide
    while true; do
        vis=$n
        if [ "$vis" -gt $((TUI_ROWS - 4)) ]; then vis=$((TUI_ROWS - 4)); fi
        if [ "$vis" -lt 1 ]; then vis=1; fi
        if [ "$sel" -lt "$top" ]; then top=$sel; fi
        if [ "$sel" -ge $((top + vis)) ]; then top=$((sel - vis + 1)); fi
        TUI_BUF=()
        tui_clip "$lab" $((TUI_COLS - 4))
        TUI_BUF+=("$TUI_C$TUI_B?$TUI_RST $TUI_B$REPLY$TUI_RST")
        for ((i = top; i < top + vis; i++)); do
            txt=${items[i]}
            tui_clip "$txt" $((TUI_COLS - 8))
            if [ "$i" -eq "$sel" ]; then
                line="  $TUI_C$TUI_B$TUI_I_SEL $REPLY$TUI_RST"
            else
                line="    $REPLY"
            fi
            TUI_BUF+=("$line")
        done
        tui_hint "$TUI_I_UP$TUI_I_DN move  $TUI_I_DOT  1-9 jump  $TUI_I_DOT  Enter select  $TUI_I_DOT  Esc cancel"
        TUI_BUF+=("$REPLY")
        tui_region_draw
        tui_readkey
        case $TUI_KEY in
            up | k) sel=$(((sel + n - 1) % n)) ;;
            down | j | tab) sel=$(((sel + 1) % n)) ;;
            home | g) sel=0 ;;
            end | G) sel=$((n - 1)) ;;
            pgup) sel=$((sel > vis ? sel - vis : 0)) ;;
            pgdn) sel=$((sel + vis < n ? sel + vis : n - 1)) ;;
            [1-9]) if [ "$TUI_KEY" -le "$n" ]; then sel=$((TUI_KEY - 1)); fi ;;
            enter)
                TUI_INDEX=$sel
                TUI_RESULT=${items[sel]}
                tui_clip "$TUI_RESULT" $((TUI_COLS - ${#lab} - 8))
                tui_region_done "$TUI_G$TUI_I_OK$TUI_RST $lab $TUI_I_ARROW $TUI_B$REPLY$TUI_RST"
                tui_widget_end
                return 0
                ;;
            esc | q | eof | ctrl-c)
                tui_region_done "$TUI_D$TUI_I_DOT $lab $TUI_I_ARROW (cancelled)$TUI_RST"
                tui_widget_end
                return 1
                ;;
        esac
    done
}

# ------------------------------------------------------------------ widget: checkboxes
tui_multi() {
    local init="" title n sel=0 top=0 vis i line lab count k
    if [ "${1:-}" = -i ]; then init=$2; shift 2; fi
    title=$1
    shift
    local items=("$@") on=()
    n=${#items[@]}
    TUI_SELECTED=()
    TUI_RESULT=""
    [ "$n" -gt 0 ] || return 1
    for ((i = 0; i < n; i++)); do on[i]=0; done
    for k in $init; do
        if [ "$k" -ge 0 ] && [ "$k" -lt "$n" ]; then on[k]=1; fi
    done
    tui_label "$title"
    lab=$REPLY
    tui_widget_begin hide
    while true; do
        vis=$n
        if [ "$vis" -gt $((TUI_ROWS - 4)) ]; then vis=$((TUI_ROWS - 4)); fi
        if [ "$vis" -lt 1 ]; then vis=1; fi
        if [ "$sel" -lt "$top" ]; then top=$sel; fi
        if [ "$sel" -ge $((top + vis)) ]; then top=$((sel - vis + 1)); fi
        count=0
        for ((i = 0; i < n; i++)); do if [ "${on[i]}" = 1 ]; then count=$((count + 1)); fi; done
        TUI_BUF=()
        tui_clip "$lab" $((TUI_COLS - 16))
        TUI_BUF+=("$TUI_C$TUI_B?$TUI_RST $TUI_B$REPLY$TUI_RST $TUI_D($count/$n selected)$TUI_RST")
        for ((i = top; i < top + vis; i++)); do
            tui_clip "${items[i]}" $((TUI_COLS - 10))
            if [ "${on[i]}" = 1 ]; then k="[$TUI_I_OK]"; else k="[ ]"; fi
            if [ "$i" -eq "$sel" ]; then
                line="  $TUI_C$TUI_B$TUI_I_SEL $k $REPLY$TUI_RST"
            else
                line="    $k $REPLY"
            fi
            TUI_BUF+=("$line")
        done
        tui_hint "$TUI_I_UP$TUI_I_DN move  $TUI_I_DOT  Space toggle  $TUI_I_DOT  a all/none  $TUI_I_DOT  Enter ok  $TUI_I_DOT  Esc cancel"
        TUI_BUF+=("$REPLY")
        tui_region_draw
        tui_readkey
        case $TUI_KEY in
            up | k) sel=$(((sel + n - 1) % n)) ;;
            down | j | tab) sel=$(((sel + 1) % n)) ;;
            home | g) sel=0 ;;
            end | G) sel=$((n - 1)) ;;
            space) on[sel]=$((1 - on[sel])) ;;
            a | A)
                k=1
                if [ "$count" -eq "$n" ]; then k=0; fi
                for ((i = 0; i < n; i++)); do on[i]=$k; done
                ;;
            enter)
                TUI_SELECTED=()
                for ((i = 0; i < n; i++)); do
                    if [ "${on[i]}" = 1 ]; then TUI_SELECTED+=("$i"); fi
                done
                tui_region_done "$TUI_G$TUI_I_OK$TUI_RST $lab $TUI_I_ARROW $TUI_B${#TUI_SELECTED[@]} of $n selected$TUI_RST"
                tui_widget_end
                return 0
                ;;
            esc | q | eof | ctrl-c)
                TUI_SELECTED=()
                tui_region_done "$TUI_D$TUI_I_DOT $lab $TUI_I_ARROW (cancelled)$TUI_RST"
                tui_widget_end
                return 1
                ;;
        esac
    done
}

# ------------------------------------------------------------------ widget: yes / no
# tui_yesno PROMPT [y|n]: default answer highlighted; y/n answer at once, Enter takes the
# highlighted one, Esc / EOF / Ctrl-C mean No. Returns 0 for yes.
tui_yesno() {
    local def=${2:-n} yes=0 lab ys ns hint
    [ "$def" = y ] && yes=1
    tui_label "$1"
    lab=$REPLY
    tui_widget_begin hide
    while true; do
        if [ "$yes" = 1 ]; then
            ys="$TUI_G$TUI_B$TUI_I_SEL Yes$TUI_RST" ns="$TUI_D  No$TUI_RST"
        else
            ys="$TUI_D  Yes$TUI_RST" ns="$TUI_R$TUI_B$TUI_I_SEL No$TUI_RST"
        fi
        if [ "$def" = y ]; then hint="[Y/n]"; else hint="[y/N]"; fi
        TUI_BUF=()
        tui_clip "$lab" $((TUI_COLS - 24))
        TUI_BUF+=("$TUI_C$TUI_B?$TUI_RST $TUI_B$REPLY$TUI_RST $TUI_D$hint$TUI_RST  $ys  $ns")
        tui_region_draw
        tui_readkey
        case $TUI_KEY in
            y | Y) yes=1; break ;;
            n | N) yes=0; break ;;
            left | right | tab | h | l | space) yes=$((1 - yes)) ;;
            enter) break ;;
            esc | q | eof | ctrl-c) yes=0; break ;;
        esac
    done
    if [ "$yes" = 1 ]; then
        tui_region_done "$TUI_G$TUI_I_OK$TUI_RST $lab $TUI_I_ARROW ${TUI_B}Yes$TUI_RST"
    else
        tui_region_done "$TUI_D$TUI_I_DOT$TUI_RST $lab $TUI_I_ARROW ${TUI_B}No$TUI_RST"
    fi
    tui_widget_end
    [ "$yes" = 1 ]
}

# ------------------------------------------------------------------ line editor
# tui_line_edit LABEL LABEL_WIDTH MODE DEFAULT: edit one line under the lines already in TUI_ABOVE.
# LABEL may hold colour codes; LABEL_WIDTH is its visible width.
# MODE: text | secret (masked) | show (a secret typed in clear). Result in TUI_RESULT, return 0
# on Enter, 1 on Esc / EOF / Ctrl-C. Every printable key, including q ? y n and space, is typed.
tui_line_edit() {
    local label=$1 lw=$2 mode=$3 def=${4:-} buf="" pos=0 avail start view col shown i
    tui_raw_on
    while true; do
        if [ "$TUI_RESIZED" = 1 ]; then tui_size; fi
        avail=$((TUI_COLS - lw - 3))
        if [ "$avail" -lt 8 ]; then avail=8; fi
        start=0
        if [ "$pos" -ge "$avail" ]; then start=$((pos - avail + 1)); fi
        view=${buf:start:avail}
        if [ "$mode" = secret ]; then
            tui_rep "$TUI_I_MASK" "${#view}"
            shown=$REPLY
        else
            shown=$view
        fi
        col=$((lw + 1 + pos - start))
        TUI_BUF=()
        for ((i = 0; i < ${#TUI_ABOVE[@]}; i++)); do TUI_BUF+=("${TUI_ABOVE[i]}"); done
        if [ -z "$buf" ] && [ -n "$def" ]; then
            tui_clip "$def" $((avail - 1))
            TUI_BUF+=("$label $TUI_D$REPLY$TUI_RST")
        else
            TUI_BUF+=("$label $shown")
        fi
        tui_region_draw "$col"
        printf '\e[?25h' >&2
        tui_readkey
        case $TUI_KEY in
            enter)
                if [ -z "$buf" ]; then buf=$def; fi
                TUI_RESULT=$buf
                return 0
                ;;
            esc | eof | ctrl-c) TUI_RESULT=""; return 1 ;;
            backspace) if [ "$pos" -gt 0 ]; then buf=${buf:0:pos-1}${buf:pos}; pos=$((pos - 1)); fi ;;
            delete) buf=${buf:0:pos}${buf:pos+1} ;;
            left) if [ "$pos" -gt 0 ]; then pos=$((pos - 1)); fi ;;
            right) if [ "$pos" -lt "${#buf}" ]; then pos=$((pos + 1)); fi ;;
            home | ctrl-a) pos=0 ;;
            end | ctrl-e) pos=${#buf} ;;
            ctrl-u) buf=${buf:pos}; pos=0 ;;
            ctrl-k) buf=${buf:0:pos} ;;
            ctrl-w)
                i=$pos
                while [ "$i" -gt 0 ] && [ "${buf:i-1:1}" = " " ]; do i=$((i - 1)); done
                while [ "$i" -gt 0 ] && [ "${buf:i-1:1}" != " " ]; do i=$((i - 1)); done
                buf=${buf:0:i}${buf:pos}
                pos=$i
                ;;
            *)
                if tui_is_char; then
                    buf=${buf:0:pos}$TUI_CH${buf:pos}
                    pos=$((pos + 1))
                fi
                ;;
        esac
    done
}

# tui_input [-s] [-v] PROMPT [DEFAULT]: -s secret (masked), -v with -s: type it in clear.
tui_input() {
    local secret=0 visible=0 lab def rc=0 mode=text
    while [ $# -gt 0 ]; do
        case $1 in
            -s) secret=1; shift ;;
            -v) visible=1; shift ;;
            *) break ;;
        esac
    done
    tui_label "$1"
    lab=$REPLY
    def=${2:-}
    if [ "$secret" = 1 ] && [ "$visible" = 0 ] && [ "$TUI_SECRET_SHOW" != 1 ]; then mode=secret; fi
    if [ "$secret" = 1 ] && [ "$mode" = text ]; then mode=show; fi
    tui_clip "$lab" $((TUI_COLS - 6))
    TUI_ABOVE=()
    tui_widget_begin
    tui_line_edit "$TUI_C$TUI_B?$TUI_RST $TUI_B$REPLY$TUI_RST $TUI_I_ARROW" $((${#REPLY} + 4)) "$mode" "$def" || rc=$?
    TUI_ABOVE=()
    tui_cursor_hide
    if [ "$rc" -ne 0 ]; then
        tui_region_done "$TUI_D$TUI_I_DOT $lab $TUI_I_ARROW (cancelled)$TUI_RST"
    elif [ "$secret" = 1 ]; then
        tui_region_done "$TUI_G$TUI_I_OK$TUI_RST $lab $TUI_I_ARROW ${TUI_D}(entered, ${#TUI_RESULT} characters, hidden)$TUI_RST"
    else
        tui_clip "$TUI_RESULT" $((TUI_COLS - ${#lab} - 8))
        tui_region_done "$TUI_G$TUI_I_OK$TUI_RST $lab $TUI_I_ARROW $TUI_B$REPLY$TUI_RST"
    fi
    tui_widget_end
    return "$rc"
}

# ------------------------------------------------------------------ widget: typed confirmation
# tui_typed PROMPT EXPECTED: a red box and a plain typing line. Nothing but the exact phrase passes;
# there is no default, no shortcut key and no timeout. Esc, EOF or a different text return 1.
tui_typed() {
    local prompt=$1 want=$2 rc=0 ok=0 inner w
    tui_widget_begin
    w=$((TUI_COLS - 2))
    if [ "$w" -gt 78 ]; then w=78; fi
    inner=$((w - 4))
    TUI_BUF=()
    tui_box_build "$TUI_R" "$TUI_I_WARN Confirmation required" "$prompt" "" "Type exactly:  $want" "(Enter to confirm, Esc to cancel. Nothing happens unless it matches.)"
    TUI_ABOVE=("${TUI_BUF[@]}")
    tui_line_edit "$TUI_R$TUI_B$TUI_I_ARROW$TUI_RST" 1 text "" || rc=$?
    if [ "$rc" -eq 0 ] && [ "$TUI_RESULT" = "$want" ]; then ok=1; fi
    TUI_ABOVE=()
    tui_cursor_hide
    tui_clip "$want" $((inner - 14))
    if [ "$ok" = 1 ]; then
        tui_region_done "$TUI_G$TUI_I_OK$TUI_RST confirmed: $TUI_B$REPLY$TUI_RST"
    else
        tui_region_done "$TUI_R$TUI_I_ERR$TUI_RST not confirmed (nothing was changed)"
    fi
    tui_widget_end
    [ "$ok" = 1 ]
}

# tui_pause [TEXT]: wait for one key.
tui_pause() {
    printf '%s' "${TUI_D}${1:-Press any key to continue...}${TUI_RST}" >&2
    tui_raw_on
    tui_readkey
    tui_raw_off
    printf '\r\e[K' >&2
}

# ------------------------------------------------------------------ boxes and panels
# tui_box_build COLOR TITLE LINE...: append a box (wrapped to the width) to TUI_BUF.
tui_box_build() {
    local color=$1 title=$2 w inner l fill top line
    shift 2
    w=$((TUI_COLS - 2))
    if [ "$w" -gt 78 ]; then w=78; fi
    if [ "$w" -lt 20 ]; then w=20; fi
    inner=$((w - 4))
    tui_clip "$title" $((w - 6))
    top=$REPLY
    tui_rep "$TUI_I_H" $((w - 5 - ${#top}))
    fill=$REPLY
    TUI_BUF+=("$color$TUI_I_TL$TUI_I_H $TUI_B$top$TUI_RST$color $fill$TUI_I_TR$TUI_RST")
    for line in "$@"; do
        tui_wrap "$inner" "$line"
        for l in "${TUI_WRAPPED[@]}"; do
            tui_fit "$l" "$inner"
            TUI_BUF+=("$color$TUI_I_V$TUI_RST $REPLY $color$TUI_I_V$TUI_RST")
        done
    done
    tui_rep "$TUI_I_H" $((w - 2))
    TUI_BUF+=("$color$TUI_I_BL$REPLY$TUI_I_BR$TUI_RST")
}

# tui_box COLOR TITLE LINE...: print a coloured section box on stdout.
tui_box() {
    local l
    TUI_BUF=()
    tui_box_build "$@"
    for l in "${TUI_BUF[@]}"; do printf '%s\n' "$l"; done
}

# tui_error_panel MESSAGE COMMAND RC HINT: a red panel on stderr. COMMAND is the exact (redacted)
# command line that failed, HINT the next step.
tui_error_panel() {
    local l lines=("$1")
    if [ -n "${2:-}" ]; then lines+=("" "Command:  $2" "Exit code: ${3:-?}"); fi
    if [ -n "${4:-}" ]; then lines+=("" "Next step:  $4"); fi
    TUI_BUF=()
    tui_box_build "$TUI_R" "$TUI_I_ERR Something went wrong" "${lines[@]}"
    for l in "${TUI_BUF[@]}"; do printf '%s\n' "$l" >&2; done
}

# tui_rule TITLE: a heavy section rule across the width (step banners).
tui_rule() {
    local t=$1 w fill
    w=$((TUI_COLS - 1))
    if [ "$w" -gt 78 ]; then w=78; fi
    tui_clip "$t" $((w - 6))
    t=$REPLY
    tui_rep "$TUI_I_HH" $((w - 4 - ${#t}))
    fill=$REPLY
    printf '%s%s%s %s%s%s %s%s%s\n' "$TUI_BL" "$TUI_I_HH$TUI_I_HH" "$TUI_RST" "$TUI_B" "$t" "$TUI_RST" "$TUI_BL" "$fill" "$TUI_RST"
}

# ------------------------------------------------------------------ spinner / progress
# tui_task LABEL CMD...: run CMD quietly in the background with a spinner and the elapsed time;
# a percentage found in its output (see tui_parse_progress) turns the spinner into a bar. The
# output goes to CAPTURED (and the last lines are shown when the command fails). Returns CMD's
# status. CMD must be an external command or a function that needs no terminal input.
tui_task() {
    local label=$1 out start frame=0 rc=0 pct="" line="" total=${TUI_TASK_TOTAL:-} tick=0 el bar w lab
    shift
    out=$(mktemp) || return 1
    "$@" >"$out" 2>&1 </dev/null &
    TUI_TASK_PID=$!
    start=$SECONDS
    tui_cursor_hide
    while kill -0 "$TUI_TASK_PID" 2>/dev/null; do
        if [ $((tick % 5)) -eq 0 ]; then
            line=$(tail -c 2000 "$out" 2>/dev/null | tr '\r' '\n' | tr -d '\000-\010\013-\037' | grep -v '^[[:space:]]*$' | tail -n1 || true)
            line=$(log_redact "$line")
            pct=$(tui_parse_progress "$(tail -c 2000 "$out" 2>/dev/null || true)" "$total")
        fi
        if [ "$TUI_RESIZED" = 1 ]; then tui_size; fi
        tui_elapsed $((SECONDS - start))
        el=$REPLY
        w=$((TUI_COLS - 1))
        if [ -n "$pct" ]; then
            tui_bar "$pct" 20
            bar=$REPLY
            tui_clip "$label" $((w - 36))
            printf '\r\e[K  %s%s%s %s  %s%s%s  %s' "$TUI_C" "${TUI_SPIN[frame]}" "$TUI_RST" "$REPLY" "$TUI_D" "$el" "$TUI_RST" "$bar" >&2
        else
            tui_clip "$label" $((w - 14 - ${#line}))
            lab=$REPLY
            tui_clip "$line" $((w - 12 - ${#lab}))
            printf '\r\e[K  %s%s%s %s  %s%s  %s%s' "$TUI_C" "${TUI_SPIN[frame]}" "$TUI_RST" "$lab" "$TUI_D" "$el" "$REPLY" "$TUI_RST" >&2
        fi
        frame=$(((frame + 1) % ${#TUI_SPIN[@]}))
        tick=$((tick + 1))
        sleep 0.1
    done
    wait "$TUI_TASK_PID" || rc=$?
    TUI_TASK_PID=""
    tui_elapsed $((SECONDS - start))
    el=$REPLY
    CAPTURED=$(cat "$out" 2>/dev/null || true)
    rm -f "$out"
    tui_cursor_show
    if [ "$rc" -eq 0 ]; then
        printf '\r\e[K  %s%s%s %s  %s%s%s\n' "$TUI_G" "$TUI_I_OK" "$TUI_RST" "$label" "$TUI_D" "$el" "$TUI_RST" >&2
    else
        printf '\r\e[K  %s%s%s %s  %sfailed (exit %s, %s)%s\n' "$TUI_R" "$TUI_I_ERR" "$TUI_RST" "$label" "$TUI_R" "$rc" "$el" "$TUI_RST" >&2
        log_redact "$(printf '%s\n' "$CAPTURED" | tr '\r' '\n' | tail -n 12)" | sed "s/^/    $TUI_D|$TUI_RST /" >&2
    fi
    return "$rc"
}
