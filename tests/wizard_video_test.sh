#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam video (show, bitrate, fps, gop, the refused flip/mirror/resolution, the restart question)
# against a fake ssh that runs the command line the wizard sends on a fake camera: a fake `nc` plays the 8091
# control port (video.*_cfg keys, LOG lines, an old firmware) and the fake ssh records `killall ak_rtsp`.
# The real camera is never contacted.
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
    printf '1536\n' >"$CAM/video.bitrate_cfg"
    printf '15\n' >"$CAM/video.fps_cfg"
    printf '2\n' >"$CAM/video.gop_cfg"
    printf '0\n' >"$CAM/video.cfg_pending"
    printf '1480\n' >"$CAM/video.bitrate"
    printf '147\n' >"$CAM/video.fps_actual"
    : >"$CAM/killed"
}
reset_cam
cat >"$STUB_DIR/bin/nc" <<'EOF'
#!/usr/bin/env bash
# fake nc: only `nc -w 2 127.0.0.1 8091`; speaks the control port for the video keys.
[ "$*" = "-w 2 127.0.0.1 8091" ] || { echo "fake nc: unexpected arguments: $*" >&2; exit 1; }
mode=$(cat "$CAM/mode" 2>/dev/null || echo normal)
[ "$mode" = silent ] && { cat >/dev/null; exit 0; }
echo "LOG [control] something the port pushes by itself"
while IFS= read -r line; do
    set -- $line
    echo "LOG [ak_rtsp] noise before $1"
    key=${2:-}
    case $key in
        video.bitrate_cfg | video.fps_cfg | video.gop_cfg | video.cfg_pending | video.bitrate | video.fps_actual) ;;
        *) echo "ERR unknown param $key"; continue ;;
    esac
    case $1 in
        GET) echo "$key=$(cat "$CAM/$key")" ;;
        SET)
            v=${3:-}
            case $key in
                video.bitrate_cfg) { case $v in '' | *[!0-9]*) false ;; *) [ "$v" -ge 500 ] && [ "$v" -le 3000 ] ;; esac; } || { echo "ERR invalid $key value (want 500..3000 kbit/s)"; continue; } ;;
                video.fps_cfg) case $v in 10 | 15) ;; *) echo "ERR invalid $key value (want 10 or 15)"; continue ;; esac ;;
                video.gop_cfg) case $v in 1 | 2 | 3 | 4) ;; *) echo "ERR invalid $key value (want 1..4 seconds)"; continue ;; esac ;;
                *) echo "ERR $key is read-only"; continue ;;
            esac
            [ "$mode" = nosave ] && { echo "ERR $key not saved: Read-only file system"; continue; }
            printf '%s\n' "$v" >"$CAM/$key"
            if [ "$(cat "$CAM/video.bitrate_cfg")" = 1536 ] && [ "$(cat "$CAM/video.fps_cfg")" = 15 ] && [ "$(cat "$CAM/video.gop_cfg")" = 2 ]; then
                printf '0\n' >"$CAM/video.cfg_pending"
                echo "OK $key=$v"
            else
                printf '1\n' >"$CAM/video.cfg_pending"
                if [ "$mode" = old ]; then echo "OK $key=$v"; else echo "OK $key=$v (restart to apply)"; fi
            fi
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
    "killall ak_rtsp") echo killed >>"$CAM/killed"; exit 0 ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
a_tty() { local st=$1; shift; AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
cam() { cat "$CAM/$1"; }
killed() { grep -c killed "$CAM/killed" || true; }
setmode() { printf '%s\n' "$1" >"$CAM/mode"; }
setmode normal
S=$(new_state v)

# ---- show --------------------------------------------------------------------------------
a "$S" video </dev/null >"$T/show.out" 2>&1
check "video: succeeds" test $? -eq 0
check "video: bitrate" has "$T/show.out" "Bitrate cap: 1536 kbit/s"
check "video: fps" has "$T/show.out" "Frame rate: 15 per second"
check "video: gop" has "$T/show.out" "Key frame every 2 second(s)"
check "video: measured values shown" has "$T/show.out" "Measured now: 1480 kbit/s, 147"
check "video: says resolution and flip are not available" has "$T/show.out" "Resolution and flip/mirror cannot be changed"
check "video: nothing pending, no warning" hasnt "$T/show.out" "not running yet"
check "video: LOG lines are not shown" hasnt "$T/show.out" "noise before"
a "$S" video show </dev/null >"$T/show2.out" 2>&1
check "video show: same" has "$T/show2.out" "Bitrate cap: 1536"
check "show only sent GET lines" bash -c '! grep -q SET "$1"' _ "$STUB_DIR/ssh.log"
check "show never restarts" test "$(killed)" -eq 0

# ---- set, no terminal: saved, restart not done -------------------------------------------------
a "$S" video bitrate 900 </dev/null >"$T/b.out" 2>&1
check "bitrate 900: ok, the camera has it" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = 900 ]' _ $? "$CAM/video.bitrate_cfg"
check "bitrate 900: says saved" has "$T/b.out" "Saved on the camera."
check "bitrate 900: says when it applies" has "$T/b.out" "applies at the next restart of the video service"
check "bitrate 900: the question is refused without a terminal and the service is not touched" test "$(killed)" -eq 0
check "bitrate 900: ends with the show and the pending warning" bash -c 'grep -q "Bitrate cap: 900" "$1" && grep -q "not running yet" "$1"' _ "$T/b.out"
check "bitrate 900: other settings untouched" test "$(cam video.fps_cfg)" = 15 -a "$(cam video.gop_cfg)" = 2

# ---- answer no (default) and yes ----------------------------------------------------------------
printf '\n' | a_tty "$S" video fps 10 >"$T/n.out" 2>&1
check "fps 10 + Enter: saved, not restarted" bash -c '[ "$(cat "$1")" = 10 ] && [ "$2" -eq 0 ]' _ "$CAM/video.fps_cfg" "$(killed)"
check "fps 10 + Enter: asks with a default of No" has "$T/n.out" "Restart the video service now?"
printf 'n\n' | a_tty "$S" video gop 3 >"$T/n2.out" 2>&1
check "gop 3 + n: saved, not restarted" bash -c '[ "$(cat "$1")" = 3 ] && [ "$2" -eq 0 ]' _ "$CAM/video.gop_cfg" "$(killed)"
printf 'y\n' | a_tty "$S" video bitrate 1200 gop 4 >"$T/y.out" 2>&1
check "yes: both values saved" test "$(cam video.bitrate_cfg)" = 1200 -a "$(cam video.gop_cfg)" = 4
check "yes: ak_rtsp was killed once (the wrapper respawns it)" test "$(killed)" -eq 1
check "yes: says it comes back" has "$T/y.out" "back in about 20 seconds"
check "yes: only killall ak_rtsp, never the watchdog or the wrapper" bash -c '! grep -E "watchdog|wrapper|reboot" "$1"' _ "$STUB_DIR/ssh.log"

# ---- back to the defaults: nothing pending, no question ----------------------------------------------
: >"$CAM/killed"
printf 'y\n' | a_tty "$S" video bitrate 1536 fps 15 gop 2 >"$T/d.out" 2>&1
check "defaults: restored" test "$(cam video.bitrate_cfg)" = 1536 -a "$(cam video.fps_cfg)" = 15 -a "$(cam video.gop_cfg)" = 2
check "defaults: no restart question, no kill" bash -c '! grep -q "Restart the video service" "$1" && [ "$2" -eq 0 ]' _ "$T/d.out" "$(killed)"

# ---- an old firmware that does not say "restart to apply" ------------------------------------------
setmode old
printf 'y\n' | a_tty "$S" video bitrate 800 >"$T/o.out" 2>&1
check "no restart note from the camera: saved, no question, no kill" bash -c '[ "$(cat "$1")" = 800 ] && ! grep -q "Restart the video service" "$2" && [ "$3" -eq 0 ]' _ "$CAM/video.bitrate_cfg" "$T/o.out" "$(killed)"
setmode normal
reset_cam

# ---- camera cases -----------------------------------------------------------------------------
setmode nosave
a "$S" video bitrate 900 </dev/null >"$T/ns.out" 2>&1
check "cannot save: fails and says why" bash -c '[ "$1" -ne 0 ] && grep -q "Read-only file system" "$2" && ! grep -q "Saved on the camera" "$2"' _ $? "$T/ns.out"
setmode silent
a "$S" video bitrate 900 </dev/null >"$T/sil.out" 2>&1
check "no answer: fails, not reported as saved" bash -c '[ "$1" -ne 0 ] && ! grep -q "Saved on the camera" "$2"' _ $? "$T/sil.out"
a "$S" video </dev/null >"$T/sil2.out" 2>&1
check "no answer: show fails" test $? -ne 0
setmode normal
reset_cam
STUB_SSH_RC=255 a "$S" video bitrate 900 </dev/null >"$T/down.out" 2>&1
check "camera unreachable: fails clearly, nothing changed" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2" && [ "$(cat "$3")" = 1536 ]' _ $? "$T/down.out" "$CAM/video.bitrate_cfg"

# ---- refusals: nothing reaches the camera ---------------------------------------------------------
: >"$STUB_DIR/ssh.log"
for bad in "bitrate" "bitrate 499" "bitrate 3001" "bitrate abc" "bitrate 5x" "bitrate -5" "bitrate 1.5" "bitrate 99999999" 'bitrate $(id)' "bitrate 1000; reboot" \
    "fps" "fps 12" "fps 5" "fps 30" "fps 0" "fps 010" "gop" "gop 0" "gop 5" "gop x" \
    "flip" "flip h" "flip none" "mirror" "resolution 1280x720" "frob" "show bitrate 900" "bitrate 900 bitrate 1000" "fps 10 fps 15" "gop 1 gop 2"; do
    # shellcheck disable=SC2086  # word list on purpose
    a "$S" video $bad </dev/null >"$T/bad.out" 2>&1
    check "refused (exit 2): video $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"
check "refusals: nothing changed" test "$(cam video.bitrate_cfg)" = 1536
a "$S" video flip h </dev/null >"$T/flip.out" 2>&1
check "flip: explains it is not available" has "$T/flip.out" "is not available"

# ---- dry run -------------------------------------------------------------------------------------
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run video bitrate 900 fps 10 </dev/null >"$T/dry.out" 2>&1
check "dry-run: WOULD send both and ask" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD send: SET video.bitrate_cfg 900" "$2" && grep -q "WOULD send: SET video.fps_cfg 10" "$2" && grep -q "WOULD ask: Restart the video service now?" "$2"' _ $? "$T/dry.out"
a "$S" --dry-run video </dev/null >"$T/dry2.out" 2>&1
check "dry-run show: read-only WOULD" has "$T/dry2.out" "WOULD send (read-only)"
check "dry-run: the camera was not contacted" test ! -s "$STUB_DIR/ssh.log"

# ---- help ----------------------------------------------------------------------------------------
a "$S" help >"$T/help.out" 2>&1
check "help lists video" has "$T/help.out" "video [show | bitrate N | fps N | gop N]"

echo "wizard_video_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
