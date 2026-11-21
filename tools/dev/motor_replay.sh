#!/usr/bin/env bash
# Motor replay (2K): play the velocity commands of a recorded tracking session back to the camera through the signed API, with NOBODY in front and tracking OFF, and measure the
# step loss as the shift of the home view: grab a home frame, play the stream, ptz.home, grab again, tools/dev/drift_shift (src/features/track/drift_match.c) prints the displacement.
# A loss-free stream ends at the same view (0,0). Variants find the motion patterns that lose steps (speed caps, with or without direction reversals).
#
# Usage: tools/dev/motor_replay.sh [options] STREAM
#   STREAM            a raw recording (akam2k_track.raw: the commands of its D lines, i.e. what the camera was told), or a "V <ms> pan_dir pan_div tilt_dir tilt_div" file
#                     (tools/dev/track_score replay --emit FILE: what the CURRENT tracker would command on the recorded boxes)
#   --cap N           0 = as recorded (default). 1 / 2 / 3 = the slowest-to-fastest speed caps: no axis faster than divisor 8 / 5 / 3 (125 / 200 / 333 steps/s; 500 steps/s is divisor 2)
#   --no-reversals    a pan or tilt command opposite to the previous one within 1.5 s is turned into a stop of that axis (the same stream without direction reversals)
#   --matrix          run the stream in 8 variants: caps 0,1,2,3 x with / without reversals (home + a fresh home frame before each; each ends with ptz.home and a drift reading)
#   --repeat N        play the stream N times in a row before the final home (accumulates the loss; default 1)
#   --max-s S         stop the stream after S seconds of recording (default: all)
#   --recal           ptz.recalibrate before the run (and between matrix variants)
#   --out DIR         results directory (default <repo>/.local/logs/motor-replay/<timestamp>)
#   --dry-run         print the plan (the schedules) and touch nothing: no network, no camera, no ffmpeg, no results directory
# Needs: python3, ffmpeg, cc; the API helper .local/bin/api.sh (the signed API: POST /akam/v1/actions/ptz.vel {"pan":"..","tilt":"..","lease_ms":".."} strings, -1..1, tilt up +);
# the camera address in CAM or .local/secrets/rot2k_ip (RTSP stream0; api.sh talks to the same camera). Calls are at least 150 ms apart. The motion is the real one: run it only when the head may move freely, nobody in
# the room, `track.enabled` off (the script refuses when the state says tracking is on). Not run by any test.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LOCAL=${AKAM_LOCAL_DIR:-$REPO/.local}
if [ ! -d "$LOCAL/bin" ] && MAIN=$(git -C "$REPO" rev-parse --git-common-dir 2>/dev/null) && [ -d "$MAIN/../.local/bin" ]; then
    LOCAL=$(cd "$MAIN/../.local" && pwd)   # a git worktree: the git-ignored .local of the main checkout
fi
API=${API_SH:-$LOCAL/bin/api.sh}
CAM=${CAM:-}
[ -n "$CAM" ] || { [ -r "$LOCAL/secrets/rot2k_ip" ] && CAM=$(head -1 "$LOCAL/secrets/rot2k_ip"); }
[ -n "$CAM" ] || CAM="<camera-ip>"   # the address is never written in a tracked file: env CAM or .local/secrets/rot2k_ip
URL=rtsp://$CAM:554/stream0
W=320 H=180
CAP=0 NOREV=0 MATRIX=0 REPEAT=1 MAXS=0 RECAL=0 DRY=0 OUT="" STREAM=""
while [ $# -gt 0 ]; do
    case $1 in
        --cap) CAP=${2:?}; shift ;;
        --no-reversals) NOREV=1 ;;
        --matrix) MATRIX=1 ;;
        --repeat) REPEAT=${2:?}; shift ;;
        --max-s) MAXS=${2:?}; shift ;;
        --recal) RECAL=1 ;;
        --out) OUT=${2:?}; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n 2,23p "$0"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) STREAM=$1 ;;
    esac
    shift
done
[ -n "$STREAM" ] && [ -r "$STREAM" ] || { echo "usage: motor_replay.sh [options] STREAM (see the header)" >&2; exit 2; }
case $CAP in 0|1|2|3) ;; *) echo "--cap 0..3" >&2; exit 2 ;; esac
[ -n "$OUT" ] || OUT=$LOCAL/logs/motor-replay/$(date +%Y%m%d_%H%M%S)

# schedule STREAM CAP NOREV -> lines "t_ms pan tilt lease_ms" (API strings, pan right +, tilt up +), >= 150 ms between lines, a final stop line
schedule() {
    python3 -I - "$STREAM" "$1" "$2" "$MAXS" <<'PY'
import re, sys
path, cap, norev, maxs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
MIN_DIV = {0: 2, 1: 8, 2: 5, 3: 3}[cap]
D = re.compile(r'^D (\d+) .*? cmd=([+\-0-])(-?\d+)/(\d+),([du0\-])(-?\d+)/(\d+) lease=(-?\d+)')
V = re.compile(r'^V (\d+) (-?\d+) (\d+) (-?\d+) (\d+)')
cmds = []   # (t_ms, pan_dir, pan_div, tilt_dir, tilt_div)  tilt_dir in IMAGE sign (+ = below)
for line in open(path, errors='replace'):
    m = D.match(line)
    if m:
        t, pd, pv, td, tv = int(m[1]), int(m[3]), int(m[4]), int(m[6]), int(m[7])
        cmds.append((t, pd, pv, td, tv))
        continue
    m = V.match(line)
    if m:
        cmds.append(tuple(int(m[i]) for i in range(1, 6)))
if not cmds:
    sys.exit("no velocity command found in %s (a D line needs track_debug=2 and the continuous mode)" % path)
t0 = cmds[0][0]
def vel(d, div):   # divisor -> API velocity, the inverse of src/features/ptz/ptz_vel.c: rate = 50 + 450 (|v| - 0.05) / 0.95 steps/s, divisor = round(1000 / rate)
    if not d or not div:
        return 0.0
    div = max(div, MIN_DIV)
    rate = 1000.0 / div
    v = 0.05 + 0.95 * (rate - 50.0) / 450.0
    return d * min(1.0, max(0.06, v))
out, last_t, last_dir, last_dir_t = [], -10**9, [0, 0], [-10**9, -10**9]
for (t, pd, pv, td, tv) in cmds:
    t -= t0
    if maxs and t > maxs * 1000:
        break
    dirs = [pd, -td]   # API tilt: up +
    if norev:
        for i in range(2):
            if dirs[i] and last_dir[i] and dirs[i] != last_dir[i] and t - last_dir_t[i] < 1500:
                dirs[i] = 0
            if dirs[i]:
                last_dir[i], last_dir_t[i] = dirs[i], t
    pan = vel(dirs[0], pv)
    tilt = vel(dirs[1], tv) if dirs[1] else 0.0
    if t - last_t < 150:   # rate limit: the newer command replaces the one not yet sent
        if out:
            out[-1] = (out[-1][0], pan, tilt)
        continue
    out.append((t, pan, tilt))
    last_t = t
last = out[-1][0] if out else 0
out.append((last + 600, 0.0, 0.0))
for t, p, q in out:
    print("%d %.3f %.3f 450" % (t, p, q))
PY
}

act() { # act NAME JSON
    printf '%s' "$2" > "$OUT/a.json"
    "$API" POST "/akam/v1/actions/$1" "" "$OUT/a.json" >/dev/null
}
state() { "$API" GET /akam/v1/state | tr ',' '\n' | grep -E "\"$1\"" | head -1 | cut -d'"' -f4; }
grab() { # grab NAME: one gray 320x180 frame as a raw file
    ffmpeg -loglevel error -y -rtsp_transport tcp -i "$URL" -frames:v 1 -vf "scale=$W:$H,format=gray" -f rawvideo "$OUT/$1.gray"
}
wait_home() { local _; for _ in $(seq 1 40); do [ "$(state ptz.state)" = home ] && return 0; sleep 1; done; return 1; }
play() { # play SCHEDULE_FILE: send each line at its time
    local t0 now t pan tilt lease
    t0=$(date +%s%3N)
    while read -r t pan tilt lease; do
        now=$(( $(date +%s%3N) - t0 ))
        [ "$t" -gt "$now" ] && sleep "$(awk -v d=$(( t - now )) 'BEGIN{printf "%.3f", d/1000}')"
        act ptz.vel "{\"pan\":\"$pan\",\"tilt\":\"$tilt\",\"lease_ms\":\"$lease\"}"
    done < "$1"
}

run_variant() { # run_variant NAME CAP NOREV
    local name=$1 cap=$2 norev=$3 sched="$OUT/$1.sched" n
    schedule "$cap" "$norev" > "$sched"
    n=$(wc -l < "$sched")
    if [ "$DRY" = 1 ]; then
        echo "== $name (cap $cap, no-reversals $norev): $n commands over $(tail -1 "$sched" | cut -d' ' -f1) ms, x$REPEAT"
        awk '{c[($2>0)-($2<0)]++; if($2<0)p=-$2; else p=$2; if(p>mx)mx=p} END{printf "   pan dirs  -:%d 0:%d +:%d  max |pan| %.2f\n", c[-1],c[0],c[1],mx}' "$sched"
        head -4 "$sched" | sed 's/^/   /'
        return 0
    fi
    [ "$RECAL" = 1 ] && { act ptz.recalibrate '{}'; sleep 5; }
    act ptz.home '{"ptz.home":"1"}'
    wait_home || { echo "home not reached" >&2; return 1; }
    sleep 2
    grab "$name.before"
    for _ in $(seq 1 "$REPEAT"); do play "$sched"; sleep 1; done
    act ptz.home '{"ptz.home":"1"}'
    wait_home || echo "warning: home state not reported after the run" >&2
    sleep 2
    grab "$name.after"
    printf '%s ' "$name" | tee -a "$OUT/results.txt"
    "$OUT/drift_shift" "$W" "$H" "$OUT/$name.before.gray" "$OUT/$name.after.gray" | tee -a "$OUT/results.txt" || true
}

if [ "$DRY" = 1 ]; then
    echo "dry-run: stream $STREAM, results would go to $OUT, camera $CAM, api helper $API"
    OUT=${TMPDIR:-$HOME/.cache/akam-tmp}/motor-replay-dry.$$   # schedules are computed in a scratch dir, removed at the end
    mkdir -p "$OUT"
else
    mkdir -p "$OUT"
    cc -O2 -Wall -Wextra -I"$REPO/src/features/track" -o "$OUT/drift_shift" "$HERE/drift_shift.c" "$REPO/src/features/track/drift_match.c"
    [ "$(state track.enabled)" = "0" ] || [ "$(state track.enabled)" = "false" ] || { echo "tracking is on (or the state is unreadable): turn it off first" >&2; exit 3; }
fi
if [ "$MATRIX" = 1 ]; then
    for c in 0 1 2 3; do
        run_variant "cap${c}_rev" "$c" 0
        run_variant "cap${c}_norev" "$c" 1
    done
else
    run_variant "cap${CAP}_$([ "$NOREV" = 1 ] && echo norev || echo rev)" "$CAP" "$NOREV"
fi
[ "$DRY" = 1 ] && rm -rf "$OUT"
exit 0
