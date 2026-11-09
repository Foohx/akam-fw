#!/usr/bin/env bash
# shellcheck disable=SC2016
# ./akam on a real (pseudo) terminal: the full-screen menu, running a step from it, the card picker,
# red yes/no confirmations, the Wi-Fi password choice, Ctrl-C, resizing, NO_COLOR and the ASCII fallback.
# The pseudo-terminal is driven by tests/fixtures/pty_drive.py (Python 3 standard library only);
# without Python 3 the whole test is skipped with a "skip:" line. No camera, no card, no network:
# the SD tool and check_host are stubs.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
DRIVE="$ROOT/tests/fixtures/pty_drive.py"

if ! command -v python3 >/dev/null 2>&1 || ! python3 -I -c 'import pty, termios, fcntl' 2>/dev/null; then
    echo "skip: python3 with the pty module is not available (pseudo-terminal helper), terminal tests not run"
    exit 0
fi

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
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

UTF=""
for loc in C.UTF-8 C.utf8 en_US.UTF-8; do
    if [ "$(LC_ALL=$loc bash -c 'x=✔; echo ${#x}' 2>/dev/null)" = 1 ]; then UTF=$loc; break; fi
done

export STUB_LOG=$T/stub.log
: >"$STUB_LOG"
cat >"$T/check_host.sh" <<'EOF'
#!/usr/bin/env bash
echo "stub check_host $*"
exit 0
EOF
cat >"$T/sd_cli.sh" <<'EOF'
#!/usr/bin/env bash
case $1 in
    list-candidates)
        if [ "${2:-}" = --human ]; then
            echo "  /dev/sdb   3.9 GB   Card Reader   usb"
        else
            printf '/dev/sdb\t3965190144\tCard Reader\tusb\t\n/dev/sdc\t31914983424\tUltra microSD\tusb\t\n'
        fi
        ;;
    prepare | verify | wipe)
        echo "stub sd $1 $* pass=${AKAM_WIFI_PASS:+set}" >>"$STUB_LOG"
        echo "VERIFY: OK"
        ;;
esac
EOF
chmod +x "$T/check_host.sh" "$T/sd_cli.sh"
export AKAM_CHECK_HOST=$T/check_host.sh AKAM_SD_CLI=$T/sd_cli.sh
export TERM=xterm-256color
unset NO_COLOR AKAM_PLAIN AKAM_TUI_FORCE AKAM_FORCE_TTY AKAM_WIFI_PASS AKAM_WIFI_SSID AKAM_SD_PASS AKAM_SD_SSID
export LANG=${UTF:-C} LC_ALL=${UTF:-C}

# run NAME [pty options] STEP... -- ARGS...: run ./akam on a pty; $T/NAME.raw is the raw output,
# $T/NAME.res the helper's verdict (EXIT=, TTY=), $T/NAME.txt the output without escape sequences.
run() {
    local name=$1
    shift
    python3 -I "$DRIVE" --out "$T/$name.raw" "$@" >"$T/$name.res" 2>&1
    sed -E 's/\x1b\[[0-9;?]*[A-Za-z]//g' "$T/$name.raw" | tr -d '\r' >"$T/$name.txt"
}
new_state() { rm -rf "$T/st"; mkdir -p "$T/st"; printf "default\n" >"$T/st/current_camera"; : >"$STUB_LOG"; }
# last_frame NAME: the last full menu frame (text only).
last_frame() {
    python3 -I - "$T/$1.raw" <<'EOF'
import re, sys
d = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
frames = [f for f in d.split("\x1b[H") if "Maintenance" in f or "guided setup" in f]
f = (frames[-1] if frames else "").split("\x1b[J")[0]
f = f.replace("\x1b[K", "").replace("\r", "")
print(re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", f))
EOF
}
frame_width() { # FILE: widest line in characters
    python3 -I -c 'import sys; print(max(len(l.rstrip("\n")) for l in open(sys.argv[1], encoding="utf-8")))' "$1"
}
terminal_restored() { # NAME: alternate screen left as often as entered, cursor shown last, tty sane
    python3 -I - "$T/$1.raw" "$T/$1.res" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
res = open(sys.argv[2]).read()
enter, leave = d.count(b"\x1b[?1049h"), d.count(b"\x1b[?1049l")
hide_last = d.rfind(b"\x1b[?25l")
show_last = d.rfind(b"\x1b[?25h")
ok = enter >= 1 and enter == leave and show_last > hide_last and "TTY=sane" in res
sys.exit(0 if ok else 1)
EOF
}

# ---------------------------------------------------------------- the menu
new_state
run menu 'expect:Check this computer' 'send:j' 'send:j' 'send:?' 'expect:What it does' 'send:x' 'expect:guided setup' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "menu: header names the project and version" has "$T/menu.txt" "Akam"
check "menu: header shows the version" has "$T/menu.txt" "v1.0.0-dev"
check "menu: header shows the detected host" has "$T/menu.txt" "Host"
check "menu: header shows the camera line" has "$T/menu.txt" "Camera"
check "menu: header shows the kit and image summary" bash -c 'grep -q "Kit" "$1" && grep -q "Image" "$1"' _ "$T/menu.txt"
check "menu: lists every step" bash -c 'for s in "Check this computer" "Get the Akam kit" "Back up the camera" "Flash the image to the camera" "Verify the result"; do grep -qF "$s" "$1" || exit 1; done' _ "$T/menu.txt"
check "menu: has a maintenance section" has "$T/menu.txt" "Maintenance"
check "menu: key hints in the footer" has "$T/menu.txt" "? help"
check "menu: ? shows what / why / risk of the selected step" bash -c 'grep -q "What it does" "$1" && grep -q "Why" "$1" && grep -q "Risk" "$1"' _ "$T/menu.txt"
check "menu: q quits with exit 0" has "$T/menu.res" "EXIT=0"
check "menu: terminal restored on quit (alt screen, cursor, tty modes)" terminal_restored menu
check "menu: no sudo, no state change from just looking" bash -c '[ ! -s "$1/state" ] || ! grep -q "step_" "$1/state"' _ "$T/st"
if [ -n "$UTF" ]; then
    check "menu: UTF-8 status icons (done, todo, blocked) and the selection marker" bash -c 'grep -q "●" "$1" && grep -q "✖" "$1" && grep -q "▸" "$1"' _ "$T/menu.txt"
fi
last_frame menu >"$T/menu.frame"
check "menu: fits 80x24 (frame rows: $(grep -c "" "$T/menu.frame"), widest: $(frame_width "$T/menu.frame"))" bash -c '[ "$(grep -c "" "$1")" -le 24 ] && [ "$2" -le 80 ]' _ "$T/menu.frame" "$(frame_width "$T/menu.frame")"

# ---------------------------------------------------------------- blocked, then run a step from the menu
new_state
run blocked 'expect:Check this computer' 'send:j' 'send:\r' 'expect:Waiting for: doctor' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "menu: Enter on a blocked step explains and stays in the menu" has "$T/blocked.txt" "Waiting for: doctor"
check "menu: ... and does not start the step" hasnt "$T/blocked.txt" "Step 2/9"
new_state
run step 'expect:Check this computer' 'send:\r' 'expect:Start this step?' 'send:\r' 'expect:Press any key' 'send:x' 'expect:Get the Akam kit' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "menu: Enter runs the step (section rule shown)" has "$T/step.txt" "Step 1/9"
check "menu: entering a step shows what it does, what it touches and asks Start this step?" bash -c 'grep -q "What it does:" "$1" && grep -q "What it touches:" "$1" && grep -q "Start this step?" "$1"' _ "$T/step.txt"
check "menu: Enter takes the default (Yes) and the step runs" bash -c 'grep -q "Start this step? .*Yes" "$1"' _ "$T/step.txt"
check "menu: the command is shown before it runs" has "$T/step.txt" "Running (read-only): $T/check_host.sh --profile kit"
check "menu: the step's result is announced" has "$T/step.txt" "Check this computer: finished"
check "menu: the step is recorded as done" bash -c 'grep -q "^step_doctor_done=" "$1/state"' _ "$T/st"
check "menu: back in the menu, the next step is no longer blocked" bash -c '! grep -q "Waiting for" <(sed -n "/Press any key to return/,\$p" "$1")' _ "$T/step.txt"
check "menu: terminal restored after running a step and quitting" terminal_restored step
check "menu: the first todo step is preselected after a run (Get the Akam kit described)" has "$T/step.txt" "Downloads the prebuilt Akam kit"

# the start question: No / Esc go back to the menu and the step is not run
new_state
run startno 'expect:Check this computer' 'send:\r' 'expect:Start this step?' 'send:n' 'expect:not started' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "menu: answering n to 'Start this step?' returns to the menu without running it" bash -c 'grep -q "not started" "$1" && ! grep -q "Step 1/9" "$1" && ! grep -q "^step_doctor_done=" "$2/state"' _ "$T/startno.txt" "$T/st"
new_state
run startesc 'expect:Check this computer' 'send:\r' 'expect:Start this step?' 'send:\x1b' 'expect:not started' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "menu: Esc at 'Start this step?' means No" bash -c '! grep -q "Step 1/9" "$1" && ! grep -q "^step_doctor_done=" "$2/state"' _ "$T/startesc.txt" "$T/st"

# ---------------------------------------------------------------- Ctrl-C
new_state
run ctrlc 'expect:Check this computer' 'send:j' 'send:\x03' -- "$AKAM" --state-dir "$T/st"
check "Ctrl-C in the menu exits 130" has "$T/ctrlc.res" "EXIT=130"
check "Ctrl-C restores the terminal (alt screen, cursor, tty modes)" terminal_restored ctrlc
check "Ctrl-C says what happened" has "$T/ctrlc.txt" "Interrupted"
new_state
run sigterm 'expect:Check this computer' 'sigint' -- "$AKAM" --state-dir "$T/st"
check "SIGINT to the process group also restores the terminal" terminal_restored sigterm

# ---------------------------------------------------------------- the card picker and the red yes/no confirmation
new_state
run wipe_ok 'expect:Which disk is your microSD card?' 'send:j' 'send:k' 'send:\r' 'expect:Zero-fill /dev/sdb now?' 'send:y' 'expect:Card wiped' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "picker: lists the candidate disks with size and model" bash -c 'grep -q "/dev/sdb" "$1" && grep -q "3.9 GB" "$1" && grep -q "Ultra microSD" "$1" && grep -q "/dev/sdc" "$1"' _ "$T/wipe_ok.txt"
check "picker + yes: the SD tool is called for the picked disk, with --yes-i-confirmed for the same device" has "$STUB_LOG" "stub sd wipe wipe --device /dev/sdb --yes-i-confirmed /dev/sdb"
check "red box: names the device, its model and size and what happens" bash -c 'grep -q "Device: /dev/sdb" "$1" && grep -q "Model: Card Reader" "$1" && grep -q "Size: 4.0 GB\|Size: 3.9 GB" "$1" && grep -q "overwritten with zeros" "$1"' _ "$T/wipe_ok.txt"
check "red box: drawn in red" bash -c 'grep -qaP "\x1b\[3[01]m" "$1"' _ "$T/wipe_ok.raw"
check "red box: nothing to type" hasnt "$T/wipe_ok.txt" "Type exactly"
check "red box run ends cleanly" has "$T/wipe_ok.res" "EXIT=0"
new_state
run wipe_bad 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Zero-fill /dev/sdb now?' 'send:\r' 'expect:not confirmed' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "red box: Enter takes the default (No), nothing is wiped" bash -c '! grep -q "wipe --device" "$1"' _ "$STUB_LOG"
check "red box: No explains that nothing changed" has "$T/wipe_bad.txt" "nothing was changed"
check "red box: the refusal is an error panel" has "$T/wipe_bad.txt" "Something went wrong"
check "red box: the run reports failure (exit 1)" has "$T/wipe_bad.res" "EXIT=1"
new_state
run wipe_ahead 'expect:Which disk is your microSD card?' 'send:\ry' 'expect:Zero-fill /dev/sdb now?' 'wait:1' 'send:\x1b' 'expect:not confirmed' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "red box: a key typed ahead (before the box was on screen) is thrown away, it never answers yes" bash -c '! grep -q "wipe --device" "$1"' _ "$STUB_LOG"
new_state
run wipe_q 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Zero-fill /dev/sdb now?' 'send:q' 'expect:not confirmed' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "red box: q means No" bash -c '! grep -q "wipe --device" "$1"' _ "$STUB_LOG"
new_state
run wipe_escb 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Zero-fill /dev/sdb now?' 'send:\x1b' 'expect:not confirmed' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "red box: Esc means No" bash -c '! grep -q "wipe --device" "$1"' _ "$STUB_LOG"
new_state
run wipe_arrow 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Zero-fill /dev/sdb now?' 'send:\x1b[C' 'send:\r' 'expect:Card wiped' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "red box: arrow right then Enter selects Yes" has "$STUB_LOG" "wipe --device /dev/sdb --yes-i-confirmed /dev/sdb"
new_state
run wipe_esc 'expect:Which disk is your microSD card?' 'send:\x1b' 'expect:no card chosen' -- "$AKAM" --state-dir "$T/st" wipe-sd
check "picker: Esc cancels, nothing is wiped" bash -c '! grep -q "wipe --device" "$1"' _ "$STUB_LOG"
check "picker: cancelling leaves a clean terminal" has "$T/wipe_esc.res" "TTY=sane"

# ---------------------------------------------------------------- the Wi-Fi password choice
SEC="Wifi-S3cret-Pw"
new_state
printf 'step_doctor_done=2026-01-01T00:00:00Z\n' >"$T/st/state"
run wifi_show 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Wi-Fi network name' 'send:MyNet\r' 'expect:Show the password while typing?' 'send:\r' "send:$SEC" 'expect:Erase /dev/sdb now?' 'send:\x03' -- "$AKAM" --state-dir "$T/st" prepare-backup-sd
check "wifi: asks 'Show the password while typing?' with Y as the default" bash -c 'grep -q "Show the password while typing?" "$1" && grep -q "\[Y/n\]" "$1"' _ "$T/wifi_show.txt"
check "wifi: the password is visible while typing when the user said yes" has "$T/wifi_show.txt" "$SEC"
new_state
printf 'step_doctor_done=2026-01-01T00:00:00Z\n' >"$T/st/state"
run wifi_hide 'expect:Which disk is your microSD card?' 'send:\r' 'expect:Wi-Fi network name' 'send:MyNet\r' 'expect:Show the password while typing?' 'send:n' "send:$SEC\r" 'expect:Erase /dev/sdb now?' 'send:y' 'expect:Backup card ready' -- "$AKAM" --state-dir "$T/st" prepare-backup-sd
check "wifi: with 'n' the password never appears on the terminal" hasnt "$T/wifi_hide.raw" "$SEC"
check "wifi: the card tool got the password through its environment" has "$STUB_LOG" "stub sd prepare"
check "wifi: ... as pass=set, not on the command line" bash -c 'grep -q "pass=set" "$1" && ! grep -q "$2" "$1"' _ "$STUB_LOG" "$SEC"
check "wifi: no secret in the state file or the log" bash -c '! grep -rq "$2" "$1"' _ "$T/st" "$SEC"
check "wifi: the flow completes (the yes was accepted)" has "$T/wifi_hide.res" "EXIT=0"

# ---------------------------------------------------------------- resize, colours, ASCII
new_state
run resize --cols 80 --rows 24 'expect:Check this computer' 'resize:60x16' 'send:j' 'resize:100x30' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
python3 -I - "$T/resize.raw" <<'EOF' >"$T/resize.frames"
import re, sys
d = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
for f in d.split("\x1b[H"):
    if "guided setup" not in f:
        continue
    f = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", f.split("\x1b[J")[0].replace("\x1b[K", "")).replace("\r", "")
    print("%d %d" % (max(len(l) for l in f.split("\n")), f.count("\n") + 1))
EOF
check "resize: the menu redraws narrower after SIGWINCH (a frame fits 60 columns)" bash -c 'awk "\$1 <= 60 && \$2 <= 16 { n++ } END { exit !(n >= 1) }" "$1"' _ "$T/resize.frames"
check "resize: ... and wider again (a frame uses more than 80 columns)" bash -c 'awk "\$1 > 80 { n++ } END { exit !(n >= 1) }" "$1"' _ "$T/resize.frames"
check "resize: the terminal is restored at the end" terminal_restored resize
new_state
NO_COLOR=1 run nocolor 'expect:Check this computer' 'send:?' 'expect:What it does' 'send:x' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "NO_COLOR: no colour or attribute sequence at all" bash -c '! grep -qaP "\x1b\[[0-9;]*m" "$1"' _ "$T/nocolor.raw"
check "NO_COLOR: the selection is still visible (marker)" bash -c 'grep -q "▸\|>" "$1"' _ "$T/nocolor.txt"
new_state
run color 'expect:Check this computer' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "colours: on by default on a terminal" bash -c 'grep -qaP "\x1b\[3[0-9]m" "$1"' _ "$T/color.raw"
new_state
LANG=C LC_ALL=C run ascii 'expect:Check this computer' 'send:j' 'send:?' 'expect:What it does' 'send:x' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "ASCII fallback: a non-UTF-8 locale prints only ASCII (plus escape sequences)" bash -c '! LC_ALL=C grep -qaP "[^\x00-\x7f]" "$1"' _ "$T/ascii.raw"
check "ASCII fallback: the menu still shows the steps" has "$T/ascii.txt" "Get the Akam kit"
new_state
run small --cols 40 --rows 10 'expect:Terminal too small' 'send:q' -- "$AKAM" --state-dir "$T/st"
check "a terminal that is too small gets a message, not a broken screen" has "$T/small.txt" "Terminal too small"
check "... and q still quits cleanly" terminal_restored small

# ---------------------------------------------------------------- plain fallbacks on a terminal
new_state
run plain --cols 80 --rows 24 'expect:Akam setup progress' 'send:q\r' -- "$AKAM" --plain --state-dir "$T/st"
check "--plain on a terminal: the line-based menu (no alternate screen)" bash -c 'grep -q "Akam setup progress" "$1" && ! grep -qa "1049h" "$2"' _ "$T/plain.txt" "$T/plain.raw"
new_state
AKAM_PLAIN=1 run plainenv 'expect:Akam setup progress' 'send:q\r' -- "$AKAM" --state-dir "$T/st"
check "AKAM_PLAIN=1 on a terminal: the line-based menu" bash -c '! grep -qa "1049h" "$1"' _ "$T/plainenv.raw"
new_state
run dry 'expect:Check this computer' 'send:q' 'expect:Progress is saved' -- "$AKAM" --dry-run --state-dir "$T/st"
check "--dry-run on a terminal: menu with the rehearsal badge" has "$T/dry.txt" "REHEARSAL"

# ---------------------------------------------------------------- cameras: name asked on the first run, shown in the header, c switches
rm -rf "$T/st"
mkdir -p "$T/st"
run camname 'expect:Camera name' 'send:salon\r' 'expect:Check this computer' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "cameras: the first terminal run asks for the camera's name" has "$T/camname.txt" "Camera name"
check "cameras: the menu header shows the camera" has "$T/camname.txt" "salon"
check "cameras: its state lives in cameras/salon" bash -c '[ -f "$1/cameras/salon/state" ] && [ "$(cat "$1/current_camera")" = salon ]' _ "$T/st"
run camsw 'expect:Check this computer' 'send:c' 'expect:Which camera do you want to work on?' 'send:\r' 'expect:Check this computer' 'send:q' 'expect:Progress is saved' -- "$AKAM" --state-dir "$T/st"
check "cameras: the c key offers the known cameras and a new one" bash -c 'grep -q "salon" "$1" && grep -q "A new camera" "$1"' _ "$T/camsw.txt"

echo
echo "wizard_tui_pty_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
