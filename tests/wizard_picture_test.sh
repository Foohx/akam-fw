#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam picture (show, brightness, contrast, saturation, sharpness, wb, night, reset) against a fake
# ssh that runs the command line the wizard sends on a fake camera: a fake `nc` plays the 8091 control port
# (isp.*, img.wb, night.mode, LOG lines, an old firmware, a camera that cannot save). The real camera is never
# contacted.
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

export STUB_DIR=$T/stub CAM=$T/cam
mkdir -p "$STUB_DIR/bin" "$CAM"
reset_cam() {
    for k in isp.brightness isp.contrast isp.saturation isp.sharpness; do printf '0\n' >"$CAM/$k"; done
    printf 'conf\n' >"$CAM/img.wb"
    printf 'auto\n' >"$CAM/night.mode"
}
reset_cam
cat >"$STUB_DIR/bin/nc" <<'EOF'
#!/usr/bin/env bash
# fake nc: only `nc -w 2 127.0.0.1 8091`; speaks the control port for the picture keys.
[ "$*" = "-w 2 127.0.0.1 8091" ] || { echo "fake nc: unexpected arguments: $*" >&2; exit 1; }
mode=$(cat "$CAM/mode" 2>/dev/null || echo normal)
[ "$mode" = silent ] && { cat >/dev/null; exit 0; }
echo "LOG [control] something the port pushes by itself"
while IFS= read -r line; do
    set -- $line
    echo "LOG [ak_rtsp] noise before $1"
    key=${2:-}
    case $key in
        isp.brightness | isp.contrast | isp.saturation | isp.sharpness | img.wb | night.mode) ;;
        *) echo "ERR unknown param $key"; continue ;;
    esac
    note=""
    [ "$mode" = nosave ] && note=" (not saved: Read-only file system)"
    case $1 in
        GET) echo "$key=$(cat "$CAM/$key")" ;;
        SET)
            v=${3:-}
            case $key in
                isp.*)
                    case $v in
                        '' | *[!0-9-]*) echo "ERR $key range [-50,50]"; continue ;;
                    esac
                    if [ "$v" -lt -50 ] || [ "$v" -gt 50 ]; then echo "ERR $key range [-50,50]"; continue; fi
                    ;;
                img.wb) case $v in conf | manual) ;; *) echo "ERR invalid img.wb value $v (want conf|manual)"; continue ;; esac; note="" ;;
                night.mode) case $v in auto | day | night) ;; *) echo "ERR invalid night.mode value $v (want auto|day|night)"; continue ;; esac; note="" ;;
            esac
            printf '%s\n' "$v" >"$CAM/$key"
            echo "OK $key=$v$note"
            ;;
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
S=$(new_state p)

# ---- show --------------------------------------------------------------------------------
a "$S" picture </dev/null >"$T/show.out" 2>&1
check "picture: succeeds" test $? -eq 0
check "picture: the four numbers" has "$T/show.out" "Brightness 0, contrast 0, saturation 0, sharpness 0"
check "picture: white balance" has "$T/show.out" "White balance: conf"
check "picture: night" has "$T/show.out" "Day/night: auto"
check "picture: says wb is saved" has "$T/show.out" "own block). Saved on the camera."
check "picture: says night is saved" has "$T/show.out" "day or night). Saved on the camera."
check "picture: LOG lines are not shown" hasnt "$T/show.out" "noise before"
a "$S" picture show </dev/null >"$T/show2.out" 2>&1
check "picture show: same" has "$T/show2.out" "Brightness 0"
check "show only sent GET lines" bash -c '! grep -q SET "$1"' _ "$STUB_DIR/ssh.log"

# ---- set one --------------------------------------------------------------------------------
a "$S" picture brightness 10 </dev/null >"$T/b.out" 2>&1
check "brightness 10: ok and the camera got it" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = 10 ]' _ $? "$CAM/isp.brightness"
check "brightness 10: reported" has "$T/b.out" "Brightness 10"
check "brightness 10: done" has "$T/b.out" "Done."
check "brightness 10: others untouched" test "$(cam isp.contrast)" = 0
a "$S" picture contrast -20 </dev/null >/dev/null 2>&1
check "contrast -20" test "$(cam isp.contrast)" = -20
a "$S" picture saturation 50 </dev/null >/dev/null 2>&1
check "saturation 50" test "$(cam isp.saturation)" = 50
a "$S" picture sharpness -50 </dev/null >/dev/null 2>&1
check "sharpness -50" test "$(cam isp.sharpness)" = -50
a "$S" picture sharpness 007 </dev/null >/dev/null 2>&1
check "leading zeros removed" test "$(cam isp.sharpness)" = 7
a "$S" picture sharpness -0 </dev/null >/dev/null 2>&1
check "-0 sent as 0" test "$(cam isp.sharpness)" = 0
a "$S" picture wb manual </dev/null >"$T/w.out" 2>&1
check "wb manual" test "$(cam img.wb)" = manual
a "$S" picture wb conf </dev/null >/dev/null 2>&1
check "wb conf" test "$(cam img.wb)" = conf
a "$S" picture night night </dev/null >"$T/n.out" 2>&1
check "night night" test "$(cam night.mode)" = night
a "$S" picture night auto </dev/null >/dev/null 2>&1
check "night auto" test "$(cam night.mode)" = auto

# ---- several words, in any order -------------------------------------------------------------
a "$S" picture night day contrast 5 brightness -5 </dev/null >"$T/m.out" 2>&1
check "several words" test "$(cam night.mode)" = day -a "$(cam isp.contrast)" = 5 -a "$(cam isp.brightness)" = -5

# ---- reset -----------------------------------------------------------------------------------
a "$S" picture saturation 30 sharpness 4 </dev/null >/dev/null 2>&1
a "$S" picture reset </dev/null >"$T/r.out" 2>&1
check "reset: succeeds" test $? -eq 0
check "reset: the four numbers are 0" bash -c 'for k in isp.brightness isp.contrast isp.saturation isp.sharpness; do [ "$(cat "$1/$k")" = 0 ] || exit 1; done' _ "$CAM"
check "reset: leaves wb and night alone" test "$(cam img.wb)" = conf -a "$(cam night.mode)" = day
check "reset: says what it did" has "$T/r.out" "back to 0"

# ---- camera cases -----------------------------------------------------------------------------
setmode nosave
a "$S" picture brightness 8 </dev/null >"$T/ns.out" 2>&1
check "cannot save: still succeeds, warns" bash -c '[ "$1" -eq 0 ] && grep -q "lost at the next reboot" "$2"' _ $? "$T/ns.out"
setmode silent
a "$S" picture contrast 9 </dev/null >"$T/sil.out" 2>&1
check "no answer: fails, not reported as done" bash -c '[ "$1" -ne 0 ] && ! grep -q "Done." "$2"' _ $? "$T/sil.out"
a "$S" picture </dev/null >"$T/sil2.out" 2>&1
check "no answer: show fails" test $? -ne 0
setmode normal
reset_cam
STUB_SSH_RC=255 a "$S" picture brightness 3 </dev/null >"$T/down.out" 2>&1
check "camera unreachable: fails clearly, nothing changed" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2" && [ "$(cat "$3")" = 0 ]' _ $? "$T/down.out" "$CAM/isp.brightness"

# ---- refusals: nothing reaches the camera ---------------------------------------------------------
: >"$STUB_DIR/ssh.log"
for bad in "brightness" "brightness 51" "brightness -51" "brightness abc" "brightness 5x" "brightness 1.5" "brightness --5" "brightness -" 'brightness $(id)' "brightness 100" \
    "contrast" "saturation x" "sharpness 999" "wb" "wb auto" "wb Conf" "night" "night dusk" "night ON" "frob" "reset now" "reset brightness 1" "show reset" "show wb conf" \
    "brightness 1 brightness 2" "wb conf wb manual" "night day night auto" "brightness 5; reboot"; do
    # shellcheck disable=SC2086  # word list on purpose
    a "$S" picture $bad </dev/null >"$T/bad.out" 2>&1
    check "refused (exit 2): picture $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"
check "refusals: nothing changed" test "$(cam isp.brightness)" = 0 -a "$(cam img.wb)" = conf

# ---- dry run -------------------------------------------------------------------------------------
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run picture brightness -5 wb manual </dev/null >"$T/dry.out" 2>&1
check "dry-run: WOULD send both" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD send: SET isp.brightness -5" "$2" && grep -q "WOULD send: SET img.wb manual" "$2"' _ $? "$T/dry.out"
a "$S" --dry-run picture reset </dev/null >"$T/dry1.out" 2>&1
check "dry-run reset: four SETs" test "$(grep -c "WOULD send: SET isp\." "$T/dry1.out")" -eq 4
a "$S" --dry-run picture </dev/null >"$T/dry2.out" 2>&1
check "dry-run show: read-only WOULD" has "$T/dry2.out" "WOULD send (read-only)"
check "dry-run: the camera was not contacted" test ! -s "$STUB_DIR/ssh.log"

# ---- help ----------------------------------------------------------------------------------------
a "$S" help >"$T/help.out" 2>&1
check "help lists picture" has "$T/help.out" "picture [show | brightness N | contrast N | saturation N | sharpness N | wb conf|manual | night auto|day|night | reset]"

echo "wizard_picture_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
