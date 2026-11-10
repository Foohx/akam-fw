#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam talk and ./akam say against a fake ssh (the camera's RTSP password command), a
# fake akam-talk (AKAM_TALK: records its arguments, its environment and its input) and a fake
# microphone/ffmpeg. Checks the password reaches akam-talk only through its environment, never a
# command line, the output or the log; the exit codes of akam-talk become clear messages; no
# audio tool is needed for `talk test`. The real camera is never contacted.
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
unset AKAM_STATE_DIR AKAM_SSH AKAM_RTSP_PASSWORD AKAM_TALK_CAPTURE_CMD
export STUB_DIR=$T/stub
mkdir -p "$STUB_DIR/bin"

# fake ssh: the camera's akam_rtsp_auth.sh answers status/show; STUB_AUTH=on|off|invalid
cat >"$STUB_DIR/ssh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
case $cmd in
    true) exit 0 ;;
    "/usr/sbin/akam_rtsp_auth.sh status")
        case ${STUB_AUTH:-on} in on) echo "enabled (user viewer)" ;; off) echo "disabled" ;; invalid) echo "INVALID"; exit 3 ;; esac
        ;;
    "/usr/sbin/akam_rtsp_auth.sh show")
        case ${STUB_AUTH:-on} in on) echo "user: viewer" ;; *) echo "disabled" ;; esac
        ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh

# fake akam-talk: records argv, whether the password was in its environment, and its input size
cat >"$STUB_DIR/akam-talk" <<'EOF'
#!/usr/bin/env bash
{
    printf 'ARGS %s\n' "$*"
    printf 'PWENV %s\n' "${AKAM_RTSP_PASSWORD:-}"
} >>"$STUB_DIR/talk.log"
if [ -t 0 ]; then n=tty; else n=$(wc -c)  ; fi
printf 'STDIN %s\n' "$(echo $n)" >>"$STUB_DIR/talk.log"
exit "${STUB_TALK_RC:-0}"
EOF
chmod +x "$STUB_DIR/akam-talk"
export AKAM_TALK=$STUB_DIR/akam-talk

# fake ffmpeg (for say): writes 3200 bytes, records its arguments
cat >"$STUB_DIR/bin/ffmpeg" <<'EOF'
#!/usr/bin/env bash
printf 'FFMPEG %s\n' "$*" >>"$STUB_DIR/talk.log"
[ -z "${STUB_FFMPEG_FAIL:-}" ] || exit 1
head -c 3200 /dev/zero
EOF
chmod +x "$STUB_DIR/bin/ffmpeg"

# A PATH without any audio tool (ffmpeg, arecord, pactl): every other executable of the system, linked.
mkdir -p "$T/minbin"
for d in /usr/bin /bin; do
    for f in "$d"/*; do
        b=${f##*/}
        case $b in ffmpeg | ffplay | ffprobe | arecord | aplay | pactl | parecord) continue ;; esac
        [ -x "$f" ] && [ ! -e "$T/minbin/$b" ] && ln -s "$f" "$T/minbin/$b"
    done
done

printf 'fake key\n' >"$T/key"
new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
a() { local st=$1; shift; PATH="$STUB_DIR/bin:$PATH" bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
reset() { : >"$STUB_DIR/ssh.log"; : >"$STUB_DIR/talk.log"; }
PW='Tr0ub4dor&3-sentinel'

# ---- talk test: no audio tool needed -----------------------------------------------------
S=$(new_state test)
reset
AKAM_RTSP_PASSWORD=$PW a "$S" talk test </dev/null >"$T/t1.out" 2>&1
check "talk test: succeeds" test $? -eq 0
check "talk test: akam-talk gets the user and a 2 s tone" has "$STUB_DIR/talk.log" "ARGS --user viewer --tone 2 rtsp://127.0.0.1:554"
check "talk test: the password is in the environment of akam-talk" has "$STUB_DIR/talk.log" "PWENV $PW"
check "talk test: the password is not on any command line" bash -c '! grep -q "^ARGS.*$1" "$2"' _ "$PW" "$STUB_DIR/talk.log"
check "talk test: the password never reaches ssh" hasnt "$STUB_DIR/ssh.log" "$PW"
check "talk test: the password is not in the output" hasnt "$T/t1.out" "$PW"
check "talk test: the password is not in the log" bash -c '! grep -rqF -- "$1" "$2"' _ "$PW" "$S"
check "talk test: read the user name over ssh" has "$STUB_DIR/ssh.log" "akam_rtsp_auth.sh show"

# ---- the password is asked when the environment has none ---------------------------------
reset
printf '%s\n' "$PW" | a "$S" talk test >"$T/t2.out" 2>&1
check "talk test with a prompt: succeeds" test $? -eq 0
check "prompt: the typed password reaches akam-talk" has "$STUB_DIR/talk.log" "PWENV $PW"
check "prompt: not echoed in the output" hasnt "$T/t2.out" "$PW"
check "prompt: not in the log" bash -c '! grep -rqF -- "$1" "$2"' _ "$PW" "$S"

# ---- microphone --------------------------------------------------------------------------------
reset
AKAM_RTSP_PASSWORD=$PW AKAM_TALK_CAPTURE_CMD='head -c 16000 /dev/zero' a "$S" talk </dev/null >"$T/m1.out" 2>&1
check "talk: succeeds" test $? -eq 0
check "talk: akam-talk is told the input is raw s16le" has "$STUB_DIR/talk.log" "ARGS --user viewer --format s16le rtsp://127.0.0.1:554"
check "talk: the microphone audio is its stdin" has "$STUB_DIR/talk.log" "STDIN 16000"
check "talk: says how to stop" has "$T/m1.out" "Ctrl-C"
reset
AKAM_RTSP_PASSWORD=$PW AKAM_TALK_CAPTURE_CMD='head -c 100 /dev/zero' a "$S" talk 30 </dev/null >"$T/m2.out" 2>&1
check "talk 30: accepted" test $? -eq 0
check "talk 30: says the limit" has "$T/m2.out" "at most 30 seconds"

# ---- say ------------------------------------------------------------------------------------------
printf 'not really audio' >"$T/clip.mp3"
reset
AKAM_RTSP_PASSWORD=$PW a "$S" say "$T/clip.mp3" </dev/null >"$T/s1.out" 2>&1
check "say: succeeds" test $? -eq 0
check "say: ffmpeg converts to 8 kHz mono s16le" has "$STUB_DIR/talk.log" "-i $T/clip.mp3 -ar 8000 -ac 1 -f s16le -"
check "say: akam-talk gets the converted audio" has "$STUB_DIR/talk.log" "STDIN 3200"
reset
STUB_FFMPEG_FAIL=1 AKAM_RTSP_PASSWORD=$PW a "$S" say "$T/clip.mp3" </dev/null >"$T/s2.out" 2>&1
check "say: a failing conversion hands akam-talk an empty input (it then exits 6, see below)" has "$STUB_DIR/talk.log" "STDIN 0"
reset
a "$S" say "$T/missing.wav" </dev/null >"$T/s3.out" 2>&1
check "say: missing file fails" test $? -ne 0
check "say: missing file named" has "$T/s3.out" "cannot read"
check "say: missing file does not start akam-talk" test ! -s "$STUB_DIR/talk.log"
a "$S" say </dev/null >"$T/s4.out" 2>&1
check "say without a file: exit 2" test $? -eq 2

# a WAV needs no ffmpeg
printf 'RIFFxxxxWAVEfmt ' >"$T/clip.wav"
reset
PATH="$T/minbin" AKAM_RTSP_PASSWORD=$PW bash "$AKAM" --no-color --state-dir "$S" --ip 127.0.0.1 --identity "$T/key" say "$T/clip.wav" </dev/null >"$T/s5.out" 2>&1
check "say of a WAV without ffmpeg: akam-talk reads the file itself" has "$STUB_DIR/talk.log" "ARGS --user viewer --input $T/clip.wav"

# ---- no stream password, no talk --------------------------------------------------------------
reset
STUB_AUTH=off AKAM_RTSP_PASSWORD=$PW a "$S" talk test </dev/null >"$T/n1.out" 2>&1
check "password off: refused" test $? -ne 0
check "password off: says why" has "$T/n1.out" "never offers talk without one"
check "password off: points at rtsp-auth set" has "$T/n1.out" "./akam rtsp-auth set"
check "password off: akam-talk not started" test ! -s "$STUB_DIR/talk.log"
reset
STUB_AUTH=invalid AKAM_RTSP_PASSWORD=$PW a "$S" talk test </dev/null >"$T/n2.out" 2>&1
check "invalid credentials file: refused" test $? -ne 0

# ---- akam-talk exit codes become messages ----------------------------------------------------------
for pair in "3:does not offer talk" "4:refused the user name or password" "2:did not answer on port 554" "6:no audio was read"; do
    rc=${pair%%:*}; msg=${pair#*:}
    reset
    STUB_TALK_RC=$rc AKAM_RTSP_PASSWORD=$PW a "$S" talk test </dev/null >"$T/e$rc.out" 2>&1
    check "akam-talk exit $rc: the wizard fails" test $? -ne 0
    check "akam-talk exit $rc: message '$msg'" has "$T/e$rc.out" "$msg"
done

# ---- arguments -------------------------------------------------------------------------------------------
for bad in 0 abc 4000 -5 "1 2"; do
    # shellcheck disable=SC2086
    a "$S" talk $bad </dev/null >"$T/bad.out" 2>&1
    check "talk $bad: exit 2" test $? -eq 2
done

# ---- no microphone tool ----------------------------------------------------------------------------------
reset
PATH="$T/minbin" AKAM_RTSP_PASSWORD=$PW bash "$AKAM" --no-color --state-dir "$S" --ip 127.0.0.1 --identity "$T/key" talk </dev/null >"$T/nm.out" 2>&1
check "no ffmpeg or arecord: talk fails with a hint" bash -c '[ "$1" -ne 0 ] && grep -q "no way to read the microphone" "$2"' _ $? "$T/nm.out"
reset
PATH="$T/minbin" AKAM_RTSP_PASSWORD=$PW bash "$AKAM" --no-color --state-dir "$S" --ip 127.0.0.1 --identity "$T/key" talk test </dev/null >"$T/nm2.out" 2>&1
check "no audio tool: talk test still works" test $? -eq 0
reset
PATH="$T/minbin" AKAM_RTSP_PASSWORD=$PW bash "$AKAM" --no-color --state-dir "$S" --ip 127.0.0.1 --identity "$T/key" say "$T/clip.mp3" </dev/null >"$T/nm3.out" 2>&1
check "no ffmpeg: say of an mp3 fails with a hint" bash -c '[ "$1" -ne 0 ] && grep -q "ffmpeg is needed" "$2"' _ $? "$T/nm3.out"

# ---- dry run ----------------------------------------------------------------------------------------------
reset
a "$S" --dry-run talk </dev/null >"$T/d1.out" 2>&1
check "dry-run talk: WOULD lines, succeeds" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD run:" "$2"' _ $? "$T/d1.out"
a "$S" --dry-run say "$T/clip.mp3" </dev/null >"$T/d2.out" 2>&1
check "dry-run say: WOULD line" has "$T/d2.out" "WOULD convert"
check "dry-run: the camera was not contacted, akam-talk not run" test ! -s "$STUB_DIR/ssh.log" -a ! -s "$STUB_DIR/talk.log"

# ---- help ----------------------------------------------------------------------------------------------------
a "$S" help >"$T/help.out" 2>&1
check "help lists talk" has "$T/help.out" "talk [SECONDS | test]"
check "help lists say" has "$T/help.out" "say FILE"

echo "wizard_talk_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
