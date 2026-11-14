#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam audio (show, volume, beep, on, off, talk) against a fake ssh that runs the command line the
# wizard sends on a fake camera: a fake `nc` plays the 8091 control port (audio.* keys, LOG lines pushed by the
# port, an old firmware, a camera that cannot save, a speaker that needs a restart, a speaker that is not
# running). The real camera, microphone, speaker, ffmpeg and arecord are never touched.
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
printf '50\n' >"$CAM/volume"
printf '1\n' >"$CAM/enable"
printf '1\n' >"$CAM/talk"
printf '1\n' >"$CAM/mic"
printf '0\n' >"$CAM/beeps"
cat >"$STUB_DIR/bin/nc" <<'EOF'
#!/usr/bin/env bash
# fake nc: only `nc -w 2 127.0.0.1 8091`; speaks the control port for the audio keys.
[ "$*" = "-w 2 127.0.0.1 8091" ] || { echo "fake nc: unexpected arguments: $*" >&2; exit 1; }
mode=$(cat "$CAM/mode" 2>/dev/null || echo normal)
[ "$mode" = silent ] && { cat >/dev/null; exit 0; }
echo "LOG [audio_out] something the port pushes by itself"
while IFS= read -r line; do
    set -- $line
    echo "LOG [ak_rtsp] noise before $1"
    key=${2:-}
    case $key in
        audio.*) ;;
        *) echo "ERR unknown param $key"; continue ;;
    esac
    [ "$mode" = old ] && { echo "ERR unknown param $key"; continue; }
    note=""
    [ "$mode" = nosave ] && note=" (not saved: Read-only file system)"
    case $1:$key in
        GET:audio.out_volume) echo "audio.out_volume=$(cat "$CAM/volume")" ;;
        GET:audio.out_enable) echo "audio.out_enable=$(cat "$CAM/enable")" ;;
        GET:audio.talk_enable) echo "audio.talk_enable=$(cat "$CAM/talk")" ;;
        GET:audio.mic_enable) echo "audio.mic_enable=$(cat "$CAM/mic")" ;;
        GET:audio.out_state) [ "$(cat "$CAM/enable")" = 1 ] && echo "audio.out_state=idle" || echo "audio.out_state=disabled" ;;
        GET:audio.out_underruns) echo "audio.out_underruns=3" ;;
        GET:audio.out_drops) echo "audio.out_drops=0" ;;
        SET:audio.out_volume)
            case ${3:-} in
                '' | *[!0-9]*) echo "ERR audio.out_volume range [0,100]" ;;
                *) if [ "$3" -gt 100 ]; then echo "ERR audio.out_volume range [0,100]"; else printf '%s\n' "$3" >"$CAM/volume"; echo "OK audio.out_volume=$3$note"; fi ;;
            esac
            ;;
        SET:audio.mic_enable)
            case ${3:-} in 0 | 1) printf '%s\n' "$3" >"$CAM/mic"; echo "OK audio.mic_enable=$3 (restart to apply)" ;; *) echo "ERR audio.mic_enable range [0,1]" ;; esac
            ;;
        SET:audio.talk_enable)
            case ${3:-} in 0 | 1) printf '%s\n' "$3" >"$CAM/talk"; echo "OK audio.talk_enable=$3$note" ;; *) echo "ERR audio.talk_enable range [0,1]" ;; esac
            ;;
        SET:audio.out_enable)
            case ${3:-} in
                0 | 1)
                    printf '%s\n' "$3" >"$CAM/enable"
                    [ "$mode" = restart ] && [ "$3" = 1 ] && note=" (restart to apply)"
                    echo "OK audio.out_enable=$3$note"
                    ;;
                *) echo "ERR audio.out_enable range [0,1]" ;;
            esac
            ;;
        SET:audio.out_test)
            if [ "$mode" = notrunning ]; then echo "ERR audio out is not running (audio.out_state=disabled)"
            elif [ "${3:-}" = 1 ]; then echo $(( $(cat "$CAM/beeps") + 1 )) >"$CAM/beeps"; echo "OK audio.out_test=1"
            else echo "ERR audio.out_test want 1 (play the beep)"; fi
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
S=$(new_state a)

# ---- show --------------------------------------------------------------------------------
a "$S" audio </dev/null >"$T/show.out" 2>&1
check "audio: succeeds" test $? -eq 0
check "audio: speaker on" has "$T/show.out" "Speaker output: on (state: idle)"
check "audio: volume" has "$T/show.out" "Volume: 50"
check "audio: talk on" has "$T/show.out" "Talk (RTSP audio backchannel): on"
check "audio: counters" has "$T/show.out" "underruns 3, drops 0"
check "audio: says it is saved" has "$T/show.out" "saved on the camera"
check "audio: LOG lines are not shown" hasnt "$T/show.out" "noise before"
a "$S" audio show </dev/null >"$T/show2.out" 2>&1
check "audio show: same as audio" has "$T/show2.out" "Volume: 50"
check "show only sent GET lines" bash -c '! grep -q SET "$1"' _ "$STUB_DIR/ssh.log"

# ---- volume --------------------------------------------------------------------------------
a "$S" audio volume 30 </dev/null >"$T/v.out" 2>&1
check "volume 30: succeeds and the camera got it" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = 30 ]' _ $? "$CAM/volume"
check "volume 30: reported" has "$T/v.out" "Volume: 30"
check "volume 30: done" has "$T/v.out" "Done."
a "$S" audio volume 007 </dev/null >/dev/null 2>&1
check "volume 007: sent as 7" test "$(cam volume)" = 7
a "$S" audio volume 100 </dev/null >/dev/null 2>&1
check "volume 100 accepted" test "$(cam volume)" = 100
a "$S" audio volume 0 </dev/null >/dev/null 2>&1
check "volume 0 accepted" test "$(cam volume)" = 0
printf '50\n' >"$CAM/volume"

# ---- beep, on, off, talk -----------------------------------------------------------------------
a "$S" audio beep </dev/null >"$T/b.out" 2>&1
check "beep: succeeds, one beep" bash -c '[ "$1" -eq 0 ] && [ "$(cat "$2")" = 1 ]' _ $? "$CAM/beeps"
check "beep: says a beep plays" has "$T/b.out" "beep plays"
a "$S" audio off </dev/null >"$T/off.out" 2>&1
check "off: the camera got it" test "$(cam enable)" = 0
check "off: says it stays off after a reboot" has "$T/off.out" "stays off after a reboot"
a "$S" audio on </dev/null >"$T/on.out" 2>&1
check "on: the camera got it" test "$(cam enable)" = 1
check "on: no restart needed here" hasnt "$T/on.out" "reboot the camera"
a "$S" audio talk off </dev/null >"$T/t.out" 2>&1
check "talk off: the camera got it" test "$(cam talk)" = 0
check "talk off: reported" has "$T/t.out" "Talk (RTSP audio backchannel): off"
a "$S" audio talk on </dev/null >/dev/null 2>&1
check "talk on: the camera got it" test "$(cam talk)" = 1

a "$S" audio mic off </dev/null >"$T/m.out" 2>&1
check "mic off: the camera got it" test "$(cam mic)" = 0
check "mic off: tells to reboot and that the track goes" bash -c 'grep -q "reboot the camera" "$1" && grep -q "no audio track" "$1"' _ "$T/m.out"
check "mic off: reported" has "$T/m.out" "Microphone: off"
a "$S" audio mic on </dev/null >/dev/null 2>&1
check "mic on: the camera got it" test "$(cam mic)" = 1
a "$S" audio show </dev/null >"$T/show2.out" 2>&1
check "show: microphone line" has "$T/show2.out" "Microphone: on"

# ---- camera cases -------------------------------------------------------------------------------
setmode restart
a "$S" audio on </dev/null >"$T/r.out" 2>&1
check "on, restart to apply: succeeds" test $? -eq 0
check "on, restart to apply: tells to reboot" has "$T/r.out" "reboot the camera"
setmode notrunning
a "$S" audio beep </dev/null >"$T/nr.out" 2>&1
check "beep, speaker not running: fails" test $? -ne 0
check "beep, speaker not running: says why" has "$T/nr.out" "audio out is not running"
setmode old
a "$S" audio </dev/null >"$T/old1.out" 2>&1
check "old firmware: show fails and says to update" bash -c '[ "$1" -ne 0 ] && grep -q "./akam update" "$2"' _ $? "$T/old1.out"
a "$S" audio volume 20 </dev/null >"$T/old2.out" 2>&1
check "old firmware: set says the camera refused" has "$T/old2.out" "refused"
setmode silent
a "$S" audio volume 20 </dev/null >"$T/sil.out" 2>&1
check "no answer: fails, not reported as done" bash -c '[ "$1" -ne 0 ] && ! grep -q "Done." "$2"' _ $? "$T/sil.out"
setmode nosave
a "$S" audio volume 20 </dev/null >"$T/ns.out" 2>&1
check "cannot save: still succeeds, warns" bash -c '[ "$1" -eq 0 ] && grep -q "lost at the next reboot" "$2"' _ $? "$T/ns.out"
setmode normal
printf '50\n' >"$CAM/volume"
STUB_SSH_RC=255 a "$S" audio volume 10 </dev/null >"$T/down.out" 2>&1
check "camera unreachable: fails clearly, nothing changed" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2" && [ "$(cat "$3")" = 50 ]' _ $? "$T/down.out" "$CAM/volume"

# ---- refusals: nothing reaches the camera ---------------------------------------------------------
: >"$STUB_DIR/ssh.log"
for bad in "volume" "volume 101" "volume -1" "volume abc" "volume 5x" "volume 1.5" 'volume $(id)' "volume 10 20" "volume 1000" "beep now" "on now" "off 1" "show on" "talk" "talk maybe" "talk on off" "mic" "mic maybe" "mic on off" "frob" "VOLUME 10" "on; reboot"; do
    # shellcheck disable=SC2086  # word list on purpose
    a "$S" audio $bad </dev/null >"$T/bad.out" 2>&1
    check "refused (exit 2): audio $bad" test $? -eq 2
done
check "refusals: the camera was never contacted" test ! -s "$STUB_DIR/ssh.log"
check "refusals: nothing changed" test "$(cam volume)" = 50

# ---- dry run -------------------------------------------------------------------------------------
: >"$STUB_DIR/ssh.log"
a "$S" --dry-run audio volume 20 </dev/null >"$T/dry.out" 2>&1
check "dry-run: WOULD send" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD send: SET audio.out_volume 20" "$2"' _ $? "$T/dry.out"
a "$S" --dry-run audio mic off </dev/null >"$T/dry3.out" 2>&1
check "dry-run mic: WOULD send" has "$T/dry3.out" "WOULD send: SET audio.mic_enable 0"
a "$S" --dry-run audio beep </dev/null >"$T/dry1.out" 2>&1
check "dry-run beep: WOULD send" has "$T/dry1.out" "WOULD send: SET audio.out_test 1"
a "$S" --dry-run audio </dev/null >"$T/dry2.out" 2>&1
check "dry-run show: read-only WOULD" has "$T/dry2.out" "WOULD send (read-only)"
check "dry-run: the camera was not contacted, nothing changed" bash -c '[ ! -s "$1" ] && [ "$(cat "$2")" = 50 ]' _ "$STUB_DIR/ssh.log" "$CAM/volume"

# ---- help and talk message -----------------------------------------------------------------------
a "$S" help >"$T/help.out" 2>&1
check "help lists audio" has "$T/help.out" "audio [show | volume 0-100 | beep | on | off | talk on|off | mic on|off]"
check "the talk message points to a command that exists" bash -c 'grep -q "cmd_audio()" "$1/tools/wizard/lib/steps_camera.sh" && grep -q "Heard it? If not: ./akam audio" "$1/tools/wizard/lib/steps_camera.sh"' _ "$ROOT"

echo "wizard_audio_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
