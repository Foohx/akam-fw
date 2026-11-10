#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam led (show, status, ir) against a fake ssh that runs the command line the wizard
# sends on a fake camera: a fake `nc` plays the 8091 control port (led.status, led.state,
# night.ir_led, LOG lines pushed by the port, an old firmware without the keys, a camera that cannot
# save). The real camera is never contacted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"

T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { local n=$1; shift; if "$@"; then ok "$n"; else ko "$n"; fi; }
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

export HOME=$T/home NO_COLOR=1
mkdir -p "$HOME"
unset AKAM_STATE_DIR AKAM_SSH

# The fake camera: its 8091 state is three files; nc reads the lines on stdin and answers.
export STUB_DIR=$T/stub CAM=$T/cam
mkdir -p "$STUB_DIR/bin" "$CAM"
printf 'on\n' >"$CAM/status"
printf 'auto\n' >"$CAM/ir"
cat >"$STUB_DIR/bin/nc" <<'EOF'
#!/usr/bin/env bash
# fake nc: only `nc -w 2 127.0.0.1 8091`; speaks the control port for the LED keys.
[ "$*" = "-w 2 127.0.0.1 8091" ] || { echo "fake nc: unexpected arguments: $*" >&2; exit 1; }
mode=$(cat "$CAM/mode" 2>/dev/null || echo normal)
[ "$mode" = silent ] && { cat >/dev/null; exit 0; }
echo "LOG [leds] something the port pushes by itself"
while IFS= read -r line; do
    set -- $line
    echo "LOG [ak_rtsp] noise before $1"
    key=${2:-}
    case $1:$key in
        GET:led.status | GET:night.ir_led | GET:led.state)
            [ "$mode" = old ] && { echo "ERR unknown param $key"; continue; }
            case $key in
                led.status) echo "led.status=$(cat "$CAM/status")" ;;
                night.ir_led) echo "night.ir_led=$(cat "$CAM/ir")" ;;
                led.state) st=$(cat "$CAM/status"); [ "$st" = auto ] && echo "led.state=blink_wifi" || echo "led.state=$st" ;;
            esac
            ;;
        SET:led.status | SET:night.ir_led)
            [ "$mode" = old ] && { echo "ERR unknown param $key"; continue; }
            v=${3:-}
            case $key:$v in
                led.status:on | led.status:off | led.status:auto) f=status ;;
                night.ir_led:auto | night.ir_led:off) f=ir ;;
                *) echo "ERR invalid $key value $v"; continue ;;
            esac
            note=""
            if [ "$mode" = nosave ]; then note=" (not saved: Read-only file system)"; fi
            printf '%s\n' "$v" >"$CAM/$f"
            echo "OK $key=$v$note"
            ;;
        *) echo "ERR unknown param $key" ;;
    esac
done
EOF
chmod +x "$STUB_DIR/bin/nc"

cat >"$STUB_DIR/ssh" <<'EOF'
#!/usr/bin/env bash
# fake ssh: logs its argv, runs the remote command line in bash with the fake nc first in PATH
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
[ "${STUB_SSH_RC:-0}" = 0 ] || exit "$STUB_SSH_RC"
case $cmd in
    true) exit 0 ;;
    printf\ *) exec env PATH="$STUB_DIR/bin:$PATH" bash -c "$cmd" ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
cam() { cat "$CAM/$1"; }
setmode() { printf '%s\n' "$1" >"$CAM/mode"; }
setmode normal

# ---- show ------------------------------------------------------------------------
S=$(new_state show)
a "$S" led </dev/null >"$T/show.out" 2>&1
check "led: succeeds" test $? -eq 0
check "led: front LED on" has "$T/show.out" "Front LED: on"
check "led: IR auto" has "$T/show.out" "IR LEDs at night: auto"
check "led: says it is saved" has "$T/show.out" "saved on the camera"
check "led: LOG lines of the port are not shown" hasnt "$T/show.out" "noise before"
a "$S" led show </dev/null >"$T/show2.out" 2>&1
check "led show: same as led" has "$T/show2.out" "Front LED: on"
check "show changed nothing" test "$(cam status)" = on -a "$(cam ir)" = auto
check "show only sent GET lines" bash -c '! grep -q SET "$1"' _ "$STUB_DIR/ssh.log"

# ---- set status -------------------------------------------------------------------
: >"$STUB_DIR/ssh.log"
a "$S" led status off </dev/null >"$T/s1.out" 2>&1
check "led status off: succeeds" test $? -eq 0
check "led status off: the camera got it" test "$(cam status)" = off
check "led status off: IR untouched" test "$(cam ir)" = auto
check "led status off: reported" has "$T/s1.out" "Front LED: off"
check "led status off: done line" has "$T/s1.out" "Done."
check "led status off: ONE ssh call for the SET plus one for the show" test "$(grep -c 'nc -w 2' "$STUB_DIR/ssh.log")" -eq 2
a "$S" led status auto </dev/null >"$T/s2.out" 2>&1
check "led status auto: camera got it" test "$(cam status)" = auto
check "led status auto: explains what auto does" has "$T/s2.out" "blinks slowly while someone watches"
check "led status auto: shows the state" has "$T/s2.out" "now: blink_wifi"
a "$S" led status on </dev/null >"$T/s3.out" 2>&1
check "led status on: camera got it" test "$(cam status)" = on

# ---- set ir ----------------------------------------------------------------------------
a "$S" led ir off </dev/null >"$T/i1.out" 2>&1
check "led ir off: succeeds and the camera got it" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = off ]' _ $? "$CAM/ir"
check "led ir off: status untouched" test "$(cam status)" = on
check "led ir off: reported" has "$T/i1.out" "IR LEDs at night: off"
a "$S" led ir auto </dev/null >"$T/i2.out" 2>&1
check "led ir auto: camera got it" test "$(cam ir)" = auto

# ---- both, either order --------------------------------------------------------------
a "$S" led status off ir off </dev/null >"$T/b1.out" 2>&1
check "both: status off and ir off" test "$(cam status)" = off -a "$(cam ir)" = off
a "$S" led ir auto status auto </dev/null >"$T/b2.out" 2>&1
check "both, reversed: auto and auto" test "$(cam status)" = auto -a "$(cam ir)" = auto
check "both: one SET batch" has "$STUB_DIR/ssh.log" "SET led.status auto"
a "$S" led status on ir auto </dev/null >/dev/null 2>&1

# ---- refusals: nothing reaches the camera ----------------------------------------------
: >"$STUB_DIR/ssh.log"
for bad in "status" "status blink" "status ON" "status 1" "ir" "ir on" "ir night" "frob" "status on status off" "ir off ir auto" "show status on" "status on; reboot" 'status $(id)'; do
    # shellcheck disable=SC2086  # word list on purpose
    a "$S" led $bad </dev/null >"$T/bad.out" 2>&1
    rc=$?
    check "refused (exit 2): led $bad" test "$rc" -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"
check "refusals: nothing changed" test "$(cam status)" = on -a "$(cam ir)" = auto

# ---- camera problems ---------------------------------------------------------------------
setmode old
a "$S" led </dev/null >"$T/old1.out" 2>&1
check "old firmware: show fails" test $? -ne 0
check "old firmware: says to update" has "$T/old1.out" "./akam update"
a "$S" led status off </dev/null >"$T/old2.out" 2>&1
check "old firmware: set fails" test $? -ne 0
check "old firmware: set says the camera refused" has "$T/old2.out" "refused"
setmode silent
a "$S" led status off </dev/null >"$T/sil.out" 2>&1
check "no answer: set fails, not reported as done" bash -c '[ "$1" -ne 0 ] && ! grep -q "Done." "$2"' _ $? "$T/sil.out"
setmode nosave
a "$S" led status off </dev/null >"$T/ns.out" 2>&1
check "cannot save: still succeeds" test $? -eq 0
check "cannot save: warns that it is lost at reboot" has "$T/ns.out" "lost at the next reboot"
setmode normal
printf 'on\n' >"$CAM/status"
STUB_SSH_RC=255 a "$S" led status off </dev/null >"$T/down.out" 2>&1
check "camera unreachable: fails with a clear message" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2"' _ $? "$T/down.out"
check "camera unreachable: nothing changed" test "$(cam status)" = on

# ---- dry run ---------------------------------------------------------------------------------
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run led status off ir off </dev/null >"$T/dry.out" 2>&1
check "dry-run: succeeds and shows WOULD lines" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD send: SET led.status off" "$2" && grep -q "WOULD send: SET night.ir_led off" "$2"' _ $? "$T/dry.out"
a "$S" --dry-run led </dev/null >"$T/dry2.out" 2>&1
check "dry-run show: read-only WOULD" has "$T/dry2.out" "WOULD send (read-only)"
check "dry-run: the camera was not contacted" test ! -s "$STUB_DIR/ssh.log"
check "dry-run: nothing changed" test "$(cam status)" = on

# ---- help ----------------------------------------------------------------------------------------
a "$S" help >"$T/help.out" 2>&1
check "help lists led" has "$T/help.out" "led [show | status on|off|auto] [ir auto|off]"

echo "wizard_led_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
