#!/usr/bin/env bash
# shellcheck disable=SC2016
# Unit tests for the wizard terminal UI (tools/wizard/lib/tui.sh and its use in ui.sh): the widgets
# are driven with key sequences read from a file (TUI_FD), so no terminal is needed. The real
# full-screen menu on a pseudo-terminal is tested in tests/wizard_tui_pty_test.sh.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { # name, command...
    local n=$1
    shift
    if "$@"; then ok "$n"; else ko "$n"; fi
}
eq() { # name, expected, actual
    if [ "$2" = "$3" ]; then ok "$1"; else ko "$1 (expected '$2', got '$3')"; fi
}
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

# shellcheck source=tools/lib/host.sh
. "$ROOT/tools/lib/host.sh"
# shellcheck source=tools/wizard/lib/log.sh
. "$ROOT/tools/wizard/lib/log.sh"
# shellcheck source=tools/wizard/lib/ui.sh
. "$ROOT/tools/wizard/lib/ui.sh"
# shellcheck source=tools/wizard/lib/tui.sh
. "$ROOT/tools/wizard/lib/tui.sh"
log_init "$T/log"

export TERM=xterm AKAM_TUI_FORCE=1 AKAM_TUI_SIZE=80x24
unset NO_COLOR
# UTF-8 only when this machine really has such a locale (bash must count characters).
UTF=""
for loc in C.UTF-8 C.utf8 en_US.UTF-8; do
    if [ "$(LC_ALL=$loc bash -c 'x=✔; echo ${#x}' 2>/dev/null)" = 1 ]; then UTF=$loc; break; fi
done
export LANG=${UTF:-C} LC_ALL=${UTF:-C}
TUI_ON=1
tui_theme

ERR=$T/err
# K KEYS: the next widget reads these keys (printf %b escapes: \e[B = Down, \r = Enter).
K() { printf '%b' "$1" >"$T/keys"; exec 8<"$T/keys"; TUI_FD=8; : >"$ERR"; }
# last_line FILE: the text after the last clear-to-end, escape sequences removed.
last_line() { sed -E 's/\x1b\[[0-9;?]*[A-Za-z]//g' "$1" | tr '\r' '\n' | grep -v '^$' | tail -n1; }
plain() { sed -E 's/\x1b\[[0-9;?]*[A-Za-z]//g' "$1" | tr -d '\r'; }
# frame_rows FILE: number of rows of the last full frame drawn before the final summary.
frame_rows() {
    sed -E 's/\x1b\[J/\x01/g' "$1" | tr '\n\001' '\002\n' | tail -n2 | head -n1 | tr -cd '\002' | wc -c | tr -d ' '
}
# line_widths FILE: the visible width of every line (escape sequences removed, split at carriage
# returns), counted by bash itself (mawk and old awks count bytes, not characters).
line_widths() {
    local l
    while IFS= read -r l; do printf '%s\n' "${#l}"; done < <(sed -E 's/\x1b\[[0-9;?]*[A-Za-z]//g' "$1" | tr '\r' '\n')
}
max_width() { line_widths "$1" | sort -n | tail -n1; }
# distinct_widths FILE: how many different line widths there are (1 = a clean rectangle).
distinct_widths() { line_widths "$1" | sort -u | wc -l | tr -d ' '; }

# ---------------------------------------------------------------- key reader
rk() { # KEYS -> the key name of the first key
    K "$1"
    tui_readkey
    printf '%s' "$TUI_KEY"
}
eq "key: Down" down "$(rk '\e[B')"
eq "key: Up" up "$(rk '\e[A')"
eq "key: Right" right "$(rk '\e[C')"
eq "key: Left" left "$(rk '\e[D')"
eq "key: application-mode Up (ESC O A)" up "$(rk '\eOA')"
eq "key: Home" home "$(rk '\e[H')"
eq "key: End" end "$(rk '\e[F')"
eq "key: Home (ESC [ 1 ~)" home "$(rk '\e[1~')"
eq "key: Delete" delete "$(rk '\e[3~')"
eq "key: PageUp" pgup "$(rk '\e[5~')"
eq "key: PageDown" pgdn "$(rk '\e[6~')"
eq "key: Ctrl-Up (modifier ignored)" up "$(rk '\e[1;5A')"
eq "key: Enter as CR" enter "$(rk '\r')"
eq "key: Enter as LF" enter "$(rk '\n')"
eq "key: lone Esc" esc "$(rk '\e')"
eq "key: Space" space "$(rk ' ')"
eq "key: Tab" tab "$(rk '\t')"
eq "key: Backspace (DEL)" backspace "$(rk '\x7f')"
eq "key: Ctrl-C" ctrl-c "$(rk '\x03')"
eq "key: Ctrl-D is end of input" eof "$(rk '\x04')"
eq "key: end of file" eof "$(rk '')"
eq "key: letter" q "$(rk 'q')"
eq "key: unknown escape sequence" unknown "$(rk '\e[99X')"
eq "key: UTF-8 character" "é" "$(rk 'é')"

# ---------------------------------------------------------------- choose
K 'jj\r'
tui_choose "Pick one" alpha beta gamma delta 2>"$ERR"
eq "choose: j j Enter selects the third" "2:gamma" "$TUI_INDEX:$TUI_RESULT"
K '\e[B\e[B\e[A\r'
tui_choose "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: arrows move" 1 "$TUI_INDEX"
K '\e[A\r'
tui_choose "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: Up from the first wraps to the last" 2 "$TUI_INDEX"
K '3\r'
tui_choose "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: a digit jumps" 2 "$TUI_INDEX"
K '9\r'
tui_choose "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: a digit past the end is ignored" 0 "$TUI_INDEX"
K 'G\r'
tui_choose "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: G goes to the end" 2 "$TUI_INDEX"
K '\r'
tui_choose -d 1 "Pick one" alpha beta gamma 2>"$ERR"
eq "choose: -d sets the default" 1 "$TUI_INDEX"
K 'j\e'
tui_choose "Pick one" alpha beta 2>"$ERR"
rc=$?
eq "choose: Esc cancels (rc 1, no result)" "1:-1:" "$rc:$TUI_INDEX:$TUI_RESULT"
K 'q'
tui_choose "Pick one" alpha beta 2>"$ERR"
eq "choose: q cancels" 1 $?
K ''
tui_choose "Pick one" alpha beta 2>"$ERR"
eq "choose: end of input cancels (never picks)" 1 $?
K 'jj\r'
tui_choose "Pick one" alpha 2>"$ERR"
eq "choose: one item wraps in place" 0 "$TUI_INDEX"
# a long list in a short terminal: the widget stays inside the window and still reaches the end
items=()
for i in $(seq 1 30); do items+=("disk $i"); done
export AKAM_TUI_SIZE=60x10
K 'G\r'
tui_choose "Pick one" "${items[@]}" 2>"$ERR"
eq "choose: 30 items in 10 rows, G reaches the last" 29 "$TUI_INDEX"
rows=$(frame_rows "$ERR")
check "choose: a frame is shorter than the terminal (rows: $rows of 10)" test "$rows" -lt 10
export AKAM_TUI_SIZE=80x24
K '\r'
tui_choose "Which disk?" "/dev/sdb  3.7 GB  Reader  (usb)" 2>"$ERR"
check "choose: the summary line names the answer" has "$ERR" "/dev/sdb"
check "choose: the title is shown" has "$ERR" "Which disk?"

# ---------------------------------------------------------------- multi
K ' \e[B\e[B \r'
tui_multi "Steps" a b c d 2>"$ERR"
eq "multi: Space toggles, result is the indexes" "0 2" "${TUI_SELECTED[*]}"
K 'a\r'
tui_multi "Steps" a b c 2>"$ERR"
eq "multi: a selects all" "0 1 2" "${TUI_SELECTED[*]}"
K 'aa\r'
tui_multi "Steps" a b c 2>"$ERR"
eq "multi: a twice selects none" "" "${TUI_SELECTED[*]}"
K ' a\r'
tui_multi "Steps" a b c 2>"$ERR"
eq "multi: a with a partial selection selects all" "0 1 2" "${TUI_SELECTED[*]}"
K '\r'
tui_multi -i "1 2" "Steps" a b c 2>"$ERR"
eq "multi: -i presets the selection" "1 2" "${TUI_SELECTED[*]}"
K ' \e[B  \r'
tui_multi "Steps" a b c 2>"$ERR"
eq "multi: Space twice on one line toggles back (only the first line stays)" "0" "${TUI_SELECTED[*]}"
K 'a\e'
tui_multi "Steps" a b c 2>"$ERR"
rc=$?
eq "multi: Esc cancels and returns nothing" "1:" "$rc:${TUI_SELECTED[*]}"
K 'jj '
tui_multi "Steps" a b c 2>"$ERR"
eq "multi: end of input cancels" 1 $?
K ' \r'
tui_multi "Steps" a b c 2>"$ERR"
check "multi: the counter is drawn" has "$ERR" "(1/3 selected)"
check "multi: the hint lists the keys" has "$ERR" "a all/none"

# ---------------------------------------------------------------- yes / no
K '\r'
tui_yesno "Go on?" y 2>"$ERR"
eq "yesno: Enter takes the default yes" 0 $?
K '\r'
tui_yesno "Go on?" n 2>"$ERR"
eq "yesno: Enter takes the default no" 1 $?
K '\r'
tui_yesno "Go on?" 2>"$ERR"
eq "yesno: the default default is no" 1 $?
K 'y'
tui_yesno "Go on?" n 2>"$ERR"
eq "yesno: y says yes at once" 0 $?
K 'n'
tui_yesno "Go on?" y 2>"$ERR"
eq "yesno: n says no at once" 1 $?
K '\e[C\r'
tui_yesno "Go on?" n 2>"$ERR"
eq "yesno: an arrow switches the highlighted answer" 0 $?
K '\e'
tui_yesno "Go on?" y 2>"$ERR"
eq "yesno: Esc means no, whatever the default" 1 $?
K ''
tui_yesno "Go on?" y 2>"$ERR"
eq "yesno: end of input means no, whatever the default" 1 $?
K '\x03'
tui_yesno "Go on?" y 2>"$ERR"
eq "yesno: Ctrl-C key means no" 1 $?
K 'y'
tui_yesno "  Replace the image?" n 2>"$ERR"
check "yesno: the label is cleaned and the hint shows the default" has "$ERR" "[y/N]"

# ---------------------------------------------------------------- input
K 'hello\r'
tui_input "Name:" 2>"$ERR"
eq "input: typed text" hello "$TUI_RESULT"
K 'helxx\x7f\x7flo\r'
tui_input "Name" 2>"$ERR"
eq "input: Backspace edits" hello "$TUI_RESULT"
K 'hllo\e[D\e[D\e[De\r'
tui_input "Name" 2>"$ERR"
eq "input: Left then insert in the middle" hello "$TUI_RESULT"
K 'abc\e[H>\e[F<\r'
tui_input "Name" 2>"$ERR"
eq "input: Home and End" ">abc<" "$TUI_RESULT"
K 'abcd\e[D\e[D\e[3~\r'
tui_input "Name" 2>"$ERR"
eq "input: Delete removes the character under the cursor" abd "$TUI_RESULT"
K 'abc def\x15xyz\r'
tui_input "Name" 2>"$ERR"
eq "input: Ctrl-U clears before the cursor" xyz "$TUI_RESULT"
K 'abc def\x17X\r'
tui_input "Name" 2>"$ERR"
eq "input: Ctrl-W deletes the last word" "abc X" "$TUI_RESULT"
K '\r'
tui_input "User name [viewer]:" viewer 2>"$ERR"
eq "input: Enter on an empty line takes the default" viewer "$TUI_RESULT"
K 'bob\r'
tui_input "User name" viewer 2>"$ERR"
eq "input: typing beats the default" bob "$TUI_RESULT"
K 'q?yn j k\r'
tui_input "Free text" 2>"$ERR"
eq "input: every printable key, shortcuts and space included, is typed" "q?yn j k" "$TUI_RESULT"
K 'été\r'
tui_input "Name" 2>"$ERR"
eq "input: UTF-8 text" "été" "$TUI_RESULT"
K 'abc\e'
tui_input "Name" 2>"$ERR"
eq "input: Esc cancels (rc 1, empty result)" "1:" "$?:$TUI_RESULT"
K 'abc'
tui_input "Name" 2>"$ERR"
eq "input: end of input cancels" 1 $?
long=$(printf 'x%.0s' $(seq 1 150))
K "$long\r"
tui_input "A long value" 2>"$ERR"
eq "input: text longer than the terminal is kept whole" 150 "${#TUI_RESULT}"
check "input: the long text never overflows the row (widest: $(max_width "$ERR"))" test "$(max_width "$ERR")" -le 79

# ---------------------------------------------------------------- secret input
SEC="Sup3r-S3cret-pw"
K "$SEC\r"
tui_input -s "Password" 2>"$ERR"
eq "secret: the value is returned" "$SEC" "$TUI_RESULT"
check "secret: hidden mode never prints it" hasnt "$ERR" "$SEC"
check "secret: the final line says it was entered" has "$ERR" "(entered, ${#SEC} characters, hidden)"
K "$SEC\r"
tui_input -s -v "Password" 2>"$ERR"
check "secret: visible mode shows it while typing" has "$ERR" "$SEC"
check "secret: ... but the collapsed final line hides it" bash -c '! grep -qF -- "$2" <<<"$1"' _ "$(last_line "$ERR")" "$SEC"
eq "secret: visible mode returns it too" "$SEC" "$TUI_RESULT"

# ---------------------------------------------------------------- typed confirmation
tconf() { # KEYS PHRASE -> rc
    K "$1"
    tui_typed "Erase $2?" "$2" 2>"$ERR"
}
tconf 'ERASE /dev/sdb\r' "ERASE /dev/sdb"
eq "typed: the exact phrase passes" 0 $?
tconf 'erase /dev/sdb\r' "ERASE /dev/sdb"
eq "typed: case matters" 1 $?
tconf 'ERASE /dev/sdb \r' "ERASE /dev/sdb"
eq "typed: a trailing space fails" 1 $?
tconf 'ERASE /dev/sda\r' "ERASE /dev/sdb"
eq "typed: another device fails" 1 $?
tconf '\r' YES
eq "typed: Enter alone fails (no default)" 1 $?
tconf 'y\r' YES
eq "typed: y is not YES" 1 $?
tconf 'Y\r' YES
eq "typed: Y is not YES" 1 $?
tconf 'yes\r' YES
eq "typed: yes is not YES" 1 $?
tconf 'YES\r' YES
eq "typed: YES passes" 0 $?
tconf 'YES\e' YES
eq "typed: Esc cancels even after the right text" 1 $?
tconf 'YES' YES
eq "typed: end of input without Enter fails" 1 $?
tconf '' YES
eq "typed: empty input fails" 1 $?
tconf 'q\r' YES
eq "typed: q is typed, not a quit shortcut" 1 $?
check "typed: ... and the q shows in the buffer" has "$ERR" "q"
tconf 'jjj\r?\r' YES
eq "typed: menu keys are plain characters" 1 $?
tconf '\x03' YES
eq "typed: the Ctrl-C key fails" 1 $?
tconf 'YES\x7f\x7f\x7fYES\r' YES
eq "typed: Backspace edits before Enter" 0 $?
tconf 'ERASE /dev/sdb\r' "ERASE /dev/sdb"
check "typed: the red box names the phrase" has "$ERR" "Type exactly:  ERASE /dev/sdb"
check "typed: the box says nothing happens unless it matches" has "$ERR" "Nothing happens unless it matches"

# ---------------------------------------------------------------- ui.sh in terminal mode
UI_DRY=0
K 'ERASE /dev/sdb\r'
ui_confirm_typed "Erase /dev/sdb?" "ERASE /dev/sdb" 2>"$ERR"
eq "ui_confirm_typed (terminal mode): exact phrase" 0 $?
K 'nope\r'
ui_confirm_typed "Erase /dev/sdb?" "ERASE /dev/sdb" 2>"$ERR"
eq "ui_confirm_typed (terminal mode): wrong phrase" 1 $?
K 'ERASE /dev/sdb\r'
UI_DRY=1
out=$(ui_confirm_typed "Erase /dev/sdb?" "ERASE /dev/sdb" 2>"$ERR")
rc=$?
UI_DRY=0
check "ui_confirm_typed in a rehearsal only shows the question (and reads no key)" bash -c '[ "$1" = 0 ] && [[ "$2" == *"WOULD ask"*"type: ERASE /dev/sdb"* ]]' _ "$rc" "$out"
K 'y'
UI_DRY=1
out=$(ui_confirm "Go on?" 2>"$ERR")
UI_DRY=0
check "ui_confirm in a rehearsal never prompts" bash -c '[[ "$1" == *"WOULD ask: Go on? [y/N]"* ]]' _ "$out"
K '\r'
ui_confirm "Go on?" 2>"$ERR"
eq "ui_confirm (terminal mode): Enter is No" 1 $?
K 'y'
ui_confirm "Go on?" 2>"$ERR"
eq "ui_confirm (terminal mode): y is Yes" 0 $?
K 'viewer2\r'
ui_prompt "  User name [viewer]:" viewer 2>"$ERR"
eq "ui_prompt (terminal mode)" "0:viewer2" "$?:$ANSWER"
K '\r'
ui_prompt "  User name:" viewer 2>"$ERR"
eq "ui_prompt (terminal mode): default" viewer "$ANSWER"
K '\e'
ui_prompt "  User name:" viewer 2>"$ERR"
eq "ui_prompt (terminal mode): Esc returns 1" 1 $?
K "$SEC\r"
ui_secret "  Password:" 2>"$ERR"
check "ui_secret (terminal mode) returns the secret and never prints it" bash -c '[ "$1" = "$2" ] && ! grep -qF -- "$2" "$3"' _ "$SECRET" "$SEC" "$ERR"
check "... and registers it for log masking" log_has_secret "$SEC"
LOG_SECRETS=()
K '\rWifiPass-123\r'
ui_secret_ask "  Wi-Fi password:" 2>"$ERR"
eq "ui_secret_ask: Enter on 'Show the password?' (default Yes) then typing" WifiPass-123 "$SECRET"
check "ui_secret_ask: asks the question with [Y/n]" has "$ERR" "Show the password while typing?"
check "ui_secret_ask: [Y/n] default shown" has "$ERR" "[Y/n]"
check "ui_secret_ask: the password is shown while typing when asked" has "$ERR" "WifiPass-123"
check "ui_secret_ask: the secret is registered for masking" log_has_secret "WifiPass-123"
K 'nHidden-456\r'
ui_secret_ask "  Wi-Fi password:" 2>"$ERR"
eq "ui_secret_ask: n hides the input" Hidden-456 "$SECRET"
check "ui_secret_ask: nothing printed in hidden mode" hasnt "$ERR" "Hidden-456"
TUI_ON=0
printf 'Plain-789\n' >"$T/stdin"
ui_secret_ask "  Wi-Fi password:" <"$T/stdin" 2>"$ERR"
eq "ui_secret_ask (plain, stdin not a terminal): one visible line, no question" Plain-789 "$SECRET"
check "ui_secret_ask (plain): no question asked" hasnt "$ERR" "Show the password"
TUI_ON=1
LOG_SECRETS=()

# ---------------------------------------------------------------- ui_fail: error panel
ui_note_cmd "bash tools/sd/sd_card.sh prepare --device /dev/sdb" 3
ui_fail "preparing the card failed." "fix the problem above and retry." 2>"$ERR"
eq "ui_fail (terminal mode) returns 1" 1 $?
check "error panel: message" has "$ERR" "preparing the card failed."
check "error panel: the exact failed command" has "$ERR" "bash tools/sd/sd_card.sh prepare --device /dev/sdb"
check "error panel: exit code" has "$ERR" "Exit code: 3"
check "error panel: next step" has "$ERR" "Next step:  fix the problem above and retry."
ui_fail "second error without a command." "try again." 2>"$ERR"
check "error panel: the command is shown once, then forgotten" hasnt "$ERR" "sd_card.sh"
ui_note_cmd "true" 0
ui_fail "x" "y" 2>"$ERR"
check "error panel: a command that succeeded is not listed" hasnt "$ERR" "Command:"
TUI_ON=0
ui_note_cmd "bash x" 3
ui_fail "plain failure" "plain hint" >"$T/o" 2>&1
check "ui_fail (plain): the usual two lines, no box" bash -c 'grep -q "^ERROR: plain failure" "$1" && grep -q "What to do: plain hint" "$1" && ! grep -q "╭" "$1"' _ "$T/o"
TUI_ON=1

# ---------------------------------------------------------------- boxes and rules
tui_box "$TUI_G" "Title" "first line" "a second, much longer line that has to be wrapped inside the box because it is long" >"$T/box" 2>&1
eq "box: every row has the same visible width" 1 "$(distinct_widths "$T/box")"
check "box: long text is wrapped (more than 3 rows)" bash -c '[ "$(wc -l <"$1")" -gt 4 ]' _ "$T/box"
tui_rule "Step 1/9 - Check this computer" >"$T/rule"
eq "rule: one line" 1 "$(wc -l <"$T/rule" | tr -d ' ')"
check "rule: ... that fits the terminal (width $(max_width "$T/rule"))" test "$(max_width "$T/rule")" -le 79

# ---------------------------------------------------------------- helpers: fit, bar, progress
tui_fit "abc" 6
eq "fit: pads" "abc   " "$REPLY"
tui_fit "abcdefghij" 6
eq "fit: cuts with an ellipsis, exactly the width" 6 "${#REPLY}"
tui_bar 50 10
eq "bar: half of 10" "50%" "$(printf '%s' "$REPLY" | sed -E 's/\x1b\[[0-9;]*m//g; s/[^0-9%]//g')"
tui_bar 250 10
check "bar: clamped to 100%" bash -c '[[ "$1" == *"100%" ]]' _ "$REPLY"
eq "progress: percentage" 45 "$(tui_parse_progress 'curl: ######## 45.3%')"
eq "progress: the last of several (carriage returns)" 55 "$(tui_parse_progress $'  10%\r  30%\r  55%')"
eq "progress: nothing found" "" "$(tui_parse_progress 'no number here')"
eq "progress: above 100 is ignored" "" "$(tui_parse_progress 'x 150%')"
eq "progress: dd bytes with a total" 50 "$(tui_parse_progress $'1048576 bytes (1.0 MB, 1.0 MiB) copied, 1 s, 1.0 MB/s' 2097152)"
eq "progress: dd bytes without a total" "" "$(tui_parse_progress '1048576 bytes copied')"
tui_elapsed 75
eq "elapsed: m:ss" "1:15" "$REPLY"

# ---------------------------------------------------------------- spinner and progress task
export AKAM_TUI_SIZE=80x24
tui_task "Quick job" bash -c 'echo hello; exit 0' 2>"$ERR"
eq "task: success returns 0" 0 $?
eq "task: the output is kept in CAPTURED" hello "$CAPTURED"
check "task: success line with the label and elapsed time" bash -c 'sed -E "s/\x1b\[[0-9;?]*[A-Za-z]//g" "$1" | tr "\r" "\n" | grep -q "Quick job  0:0"' _ "$ERR"
tui_task "Failing job" bash -c 'echo "boom line"; exit 7' 2>"$ERR"
eq "task: failure returns the command status" 7 $?
check "task: failure line shows the exit code" has "$ERR" "failed (exit 7"
check "task: failure shows the last output lines" has "$ERR" "boom line"
log_add_secret "TopSecretValue"
tui_task "Leaky job" bash -c 'echo "token=TopSecretValue"; exit 1' 2>"$ERR"
check "task: a registered secret in the output is masked" hasnt "$ERR" "TopSecretValue"
LOG_SECRETS=()
tui_task "Download" bash -c 'echo " 40.0%"; sleep 1.3' 2>"$ERR"
check "task: a percentage in the output becomes a progress bar" has "$ERR" " 40%"
tui_task "Dots" bash -c 'sleep 0.5' 2>"$ERR"
check "task: spinner frames are drawn while it runs" bash -c '[ "$(tr "\r" "\n" <"$1" | grep -c "Dots")" -ge 3 ]' _ "$ERR"

# ---------------------------------------------------------------- theme: UTF-8, ASCII, colours
# probe ENV... -- EXPR: print EXPR after tui_theme, run with a clean environment plus ENV.
probe() {
    local envs=()
    while [ "$1" != -- ]; do envs+=("$1"); shift; done
    shift
    env -i PATH="$PATH" HOME="$T" "${envs[@]}" bash -c '. "'"$ROOT"'/tools/wizard/lib/tui.sh"; tui_theme; printf "%s" "'"$1"'"' 2>/dev/null
}
ICONS='$TUI_I_DONE$TUI_I_TODO$TUI_I_BLOCK$TUI_I_STALE$TUI_I_TL$TUI_I_H$TUI_I_V${TUI_SPIN[*]}$TUI_I_FULL$TUI_I_SEL$TUI_I_ELL'
probe TERM=xterm LANG=C LC_ALL=C -- "$ICONS" >"$T/ascii"
check "theme: a non-UTF-8 locale gives pure ASCII icons" bash -c '[ -s "$1" ] && ! LC_ALL=C grep -q "[^ -~]" "$1"' _ "$T/ascii"
probe TERM=xterm LANG=C.UTF-8 LC_ALL=C.UTF-8 AKAM_TUI_ASCII=1 -- "$ICONS" >"$T/ascii2"
check "theme: AKAM_TUI_ASCII=1 forces ASCII" bash -c '[ -s "$1" ] && ! LC_ALL=C grep -q "[^ -~]" "$1"' _ "$T/ascii2"
if [ -n "$UTF" ]; then
    eq "theme: UTF-8 icons for done / todo / blocked / stale" "✔●✖⟳" "$(probe TERM=xterm LANG=$UTF LC_ALL=$UTF -- '$TUI_I_DONE$TUI_I_TODO$TUI_I_BLOCK$TUI_I_STALE')"
else
    echo "note: no UTF-8 locale on this machine, the UTF-8 icon check is not run"
fi
eq "theme: NO_COLOR removes every colour and attribute" "" "$(probe TERM=xterm NO_COLOR=1 AKAM_TUI_COLOR=1 -- '$TUI_RST$TUI_B$TUI_G$TUI_R$TUI_C$TUI_D$TUI_REV')"
eq "theme: colours are on when allowed" $'\e[32m' "$(probe TERM=xterm AKAM_TUI_COLOR=1 -- '$TUI_G')"
eq "theme: TERM=dumb has no colours" "" "$(probe TERM=dumb AKAM_TUI_COLOR=1 -- '$TUI_G')"
AKAM_TUI_COLOR=1 tui_theme
K 'j\r'
tui_choose "Colour pick" a b c 2>"$ERR"
check "choose: the selected row is coloured (bold cyan)" has "$ERR" $'\e[36m'
tui_theme

# ---------------------------------------------------------------- mode decision
mode() { # ENV... -> TUI_ON after tui_init ARG
    env -i PATH="$PATH" HOME="$T" "$@" bash -c '. "'"$ROOT"'/tools/wizard/lib/tui.sh"; tui_init "${PLAIN:-0}"; printf %s "$TUI_ON"' </dev/null 2>/dev/null
}
eq "mode: stdin is not a terminal -> plain" 0 "$(mode TERM=xterm)"
eq "mode: forced (test hook) -> terminal mode" 1 "$(mode TERM=xterm AKAM_TUI_FORCE=1)"
eq "mode: --plain wins over everything" 0 "$(mode TERM=xterm AKAM_TUI_FORCE=1 PLAIN=1)"
eq "mode: AKAM_PLAIN wins over everything" 0 "$(mode TERM=xterm AKAM_TUI_FORCE=1 AKAM_PLAIN=1)"
eq "mode: TERM=dumb -> plain" 0 "$(mode TERM=dumb AKAM_TUI_FORCE=1)"
eq "mode: no TERM -> plain" 0 "$(mode AKAM_TUI_FORCE=1)"

# ---------------------------------------------------------------- terminal hygiene
: >"$ERR"
tui_screen_enter 2>"$ERR"
tui_cleanup 2>>"$ERR"
tui_cleanup 2>>"$ERR"
check "cleanup: leaves the alternate screen" has "$ERR" $'\e[?1049l'
check "cleanup: shows the cursor again" has "$ERR" $'\e[?25h'
check "cleanup: runs twice without harm (one alt-screen exit)" bash -c '[ "$(grep -o "1049l" "$1" | wc -l)" = 1 ]' _ "$ERR"
check "cleanup: the flags are back to zero" bash -c '[ "$1$2$3" = 000 ]' _ "$TUI_ALT" "$TUI_RAW" "$TUI_CURSOR_HIDDEN"
# a background task is stopped by cleanup (Ctrl-C while a spinner runs)
sleep 30 &
TUI_TASK_PID=$!
tui_cleanup 2>/dev/null
sleep 0.2
tpid=$!
wait "$tpid" 2>/dev/null
check "cleanup: stops a running task" bash -c '! kill -0 "$1" 2>/dev/null' _ "$tpid"
check "traps: EXIT, INT and WINCH are armed by tui_rearm_traps" bash -c '. "$1/tools/wizard/lib/tui.sh"; TUI_ON=1; tui_rearm_traps; t=$(trap -p); [[ "$t" == *"tui_cleanup"*EXIT* || "$t" == *EXIT* ]] && [[ "$t" == *INT* ]] && [[ "$t" == *WINCH* ]] && [[ "$t" == *TERM* ]] && [[ "$t" == *HUP* ]]' _ "$ROOT" 2>/dev/null

# ---------------------------------------------------------------- plain mode regressions (no terminal)
STATE=$T/state
out=$(env -u AKAM_TUI_FORCE bash "$ROOT/akam" --state-dir "$STATE" </dev/null 2>&1)
noesc() { case $1 in *$'\e'*) return 1 ;; esac; }
check "no terminal: ./akam prints the status table" bash -c '[[ "$1" == *"No terminal attached"* ]]' _ "$out"
check "no terminal: ... without any escape sequence" noesc "$out"
out=$(AKAM_FORCE_TTY=1 env -u AKAM_TUI_FORCE bash "$ROOT/akam" --plain --state-dir "$STATE" status </dev/null 2>&1)
check "--plain status: the usual table" bash -c '[[ "$1" == *"Akam setup progress"* ]]' _ "$out"
out=$(env -u AKAM_TUI_FORCE bash "$ROOT/akam" --plain --dry-run --state-dir "$STATE" </dev/null 2>&1)
check "--plain --dry-run: the rehearsal walk" bash -c '[[ "$1" == *REHEARSAL* ]]' _ "$out"
check "--plain --dry-run: ... without any escape sequence" noesc "$out"
out=$(bash "$ROOT/akam" help </dev/null)
check "help documents --plain" bash -c '[[ "$1" == *"--plain"* ]]' _ "$out"

echo
echo "wizard_tui_widgets_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
