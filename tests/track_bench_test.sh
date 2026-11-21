#!/usr/bin/env bash
# Tracker bench regression test (host, no hardware): runs the closed-loop synthetic scenarios of tools/dev/track_sim.c through the CURRENT tracker
# (track_logic.c + track.c) and compares every number with tests/golden/track_bench.txt, the numbers of the tracker when the bench was created. A metric that got WORSE than
# the baseline fails; one that got better is reported ("improved"), and then the baseline is updated on purpose:
#     TRACK_BENCH_UPDATE=1 tests/track_bench_test.sh      (rewrites tests/golden/track_bench.txt: review the diff, it is the proof of the change)
# Direction per key: lock / frame: higher is better; err_*, rev_lat_*, still_travel, flips, cmds, starts, reacq_*, lost_ms, tilt_travel, err_drift: lower is better; pan_range / tilt_range are
# informational (not compared). A tolerance (0.5 % points, 1 px, 50 ms, 2 counts, 20 steps, or 2 % of the baseline, whichever is larger) only absorbs libm / compiler differences
# between machines: the simulation is deterministic (fixed seeds). A -1 (no such event in the scenario) must stay -1. Also runs the open-loop replay on the recorded fixtures.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
GOLD=$REPO/tests/golden/track_bench.txt
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export TRACK_BENCH_BUILD=$WORK/build

fail=0
ok() { echo "ok   - $1"; }
bad() { echo "FAIL - $1"; fail=$((fail + 1)); }

"$REPO/tools/dev/track_score" sim --raw > "$WORK/now.txt" || { echo "FAIL - track_sim did not run"; exit 1; }
n=$(wc -l < "$WORK/now.txt")
if [ "$n" -ge 10 ]; then ok "the sim produced $n scenarios"; else bad "the sim produced only $n scenarios"; fi
"$REPO/tools/dev/track_score" sim --raw > "$WORK/now2.txt"
if cmp -s "$WORK/now.txt" "$WORK/now2.txt"; then ok "the sim is deterministic (two runs byte for byte equal)"; else bad "the sim is not deterministic"; fi

if [ "${TRACK_BENCH_UPDATE:-0}" = 1 ]; then
    cp "$WORK/now.txt" "$GOLD"
    echo "baseline rewritten: $GOLD"
fi

awk -v gold="$GOLD" '
function tol(k, b,   t) {
    t = (k == "lock" || k == "frame" || k == "err_drift") ? 0.5 : (k ~ /^err_/ ? 1 : (k ~ /^(rev_lat|reacq)/ ? 50 : (k ~ /^(still_travel|tilt_travel)$/ ? 20 : (k == "lost_ms" ? 50 : 2))))
    if (k == "err_drift") t = 1
    if (b < 0) b = -b
    return (b * 0.02 > t) ? b * 0.02 : t
}
BEGIN {
    while ((getline l < gold) > 0) {
        n = split(l, a, " "); for (i = 2; i <= n; i++) { split(a[i], kv, "="); base[a[1], kv[1]] = kv[2]; has[a[1]] = 1; keys[kv[1]] = 1 }
    }
}
{
    n = split($0, a, " "); name = a[1]; seen[name] = 1
    if (!(name in has)) { printf "FAIL - %s: not in the baseline\n", name; bad++; next }
    for (i = 2; i <= n; i++) {
        split(a[i], kv, "="); k = kv[1]; v = kv[2] + 0; b = base[name, k] + 0
        if (k == "pan_range" || k == "tilt_range") continue
        if (base[name, k] == "") { printf "FAIL - %s %s: not in the baseline\n", name, k; bad++; continue }
        if (b < 0 || v < 0) { if ((b < 0) != (v < 0)) { printf "FAIL - %s %s: %s, baseline %s (event appeared or vanished)\n", name, k, kv[2], base[name, k]; bad++ } continue }
        hi = (k == "lock" || k == "frame")
        d = hi ? b - v : v - b          # > 0 = worse
        if (d > tol(k, b)) { printf "FAIL - %s %s: %s, baseline %s (worse)\n", name, k, kv[2], base[name, k]; bad++ }
        else if (-d > tol(k, b)) { printf "note - %s %s: %s, baseline %s (improved: update the baseline)\n", name, k, kv[2], base[name, k]; imp++ }
    }
}
END {
    for (nm in has) if (!(nm in seen)) { printf "FAIL - %s: in the baseline, not produced\n", nm; bad++ }
    printf "%s - %d metric(s) worse than the baseline, %d improved\n", bad ? "FAIL" : "ok  ", bad + 0, imp + 0
    exit bad ? 1 : 0
}' "$WORK/now.txt" > "$WORK/cmp.txt" && cmpst=0 || cmpst=1
cat "$WORK/cmp.txt"
[ "$cmpst" = 0 ] || fail=$((fail + 1))

# the open-loop replay on the two recorded sessions (tests/fixtures/track): runs, counts the inputs
out=$("$REPO/tools/dev/track_score" replay --raw "$REPO/tests/fixtures/track/torso_down.raw" "$REPO/tests/fixtures/track/fills_frame.raw")
if echo "$out" | grep -q '^torso_down.raw ticks=22 '; then ok "replay: torso_down.raw has 22 ticks"; else bad "replay: torso_down.raw ticks"; fi
if echo "$out" | grep -q '^fills_frame.raw ticks=10 '; then ok "replay: fills_frame.raw has 10 ticks"; else bad "replay: fills_frame.raw ticks"; fi
if "$REPO/tools/dev/track_score" replay --emit "$REPO/tests/fixtures/track/torso_down.raw" | grep -q '^V [0-9]* '; then ok "replay --emit prints the commands"; else bad "replay --emit prints no command"; fi
# absences (found in the real session 20261120-session2): 440 s with nobody in view are not a reacquisition (reacq_mean was 154 s) and not tracking time (lock was 13.5 %). Synthetic: person 10 s, nobody 120 s, person 10 s.
{
    echo "C deadband_pct=6 tband_pct=10 gain_pan=640 gain_tilt=384 tilt_enable=1 lost_grace_s=3 return_home=1 return_delay_s=30 lost_n=-1"
    now=1000
    for _ in $(seq 1 50); do echo "I $now $((now - 100)) 1275 1300 1 1275 1300 0 0 0 1300 300 3650 300 2100 1 {0,80,140,40,180,150} me=0 cr=0 ob=0 rb=0 ct=1"; now=$((now + 200)); done
    for _ in $(seq 1 120); do echo "I $now $((now - 100)) 1275 1300 1 1275 1300 0 0 0 1300 300 3650 300 2100 0 me=0 cr=0 ob=0 rb=0 ct=1"; now=$((now + 1000)); done
    for _ in $(seq 1 50); do echo "I $now $((now - 100)) 1275 1300 1 1275 1300 0 0 0 1300 300 3650 300 2100 1 {0,80,140,40,180,150} me=0 cr=0 ob=0 rb=0 ct=1"; now=$((now + 200)); done
} > "$WORK/absence.raw"
ab=$("$REPO/tools/dev/track_score" replay --raw "$WORK/absence.raw")
if echo "$ab" | grep -q ' absent_n=1 absent_s=119$' && echo "$ab" | grep -q ' lock=100.0 ' && echo "$ab" | grep -q ' lock_all=45.5 ' && echo "$ab" | grep -q ' reacq_mean=-1 '; then ok "replay: a 2 min absence is not a reacquisition and not tracking time (lock 100, lock_all 45.5)"; else bad "replay absence scoring: $ab"; fi
# the motor replay: dry run only (never touches a camera)
printf 'V 1000 1 4 0 0\nV 1200 -1 4 0 0\nV 1400 0 0 0 0\n' > "$WORK/v.txt"
dry=$(bash "$REPO/tools/dev/motor_replay.sh" --dry-run --matrix "$WORK/v.txt")
if [ "$(echo "$dry" | grep -c '^== cap')" = 8 ]; then ok "motor_replay dry run: 8 variants"; else bad "motor_replay dry run variants"; fi
if CAM=192.0.2.7 bash "$REPO/tools/dev/track_fetch.sh" --dry-run --out "$WORK/o" | grep -q 'root@192.0.2.7 cat /tmp/akam2k_track.raw'; then ok "track_fetch dry run"; else bad "track_fetch dry run"; fi
if [ ! -e "$WORK/o" ]; then ok "track_fetch dry run writes nothing"; else bad "track_fetch dry run wrote"; fi

# a real hardware session (git-ignored, not committed): TRACK_SESSION=<file.raw>, default the first session of .local/logs/track-sessions; skipped when absent
SESS=${TRACK_SESSION:-}
if [ -z "$SESS" ]; then
    for d in "$REPO/.local/logs/track-sessions" "$(cd "$REPO" && cd "$(git rev-parse --git-common-dir)/.." 2>/dev/null && pwd)/.local/logs/track-sessions"; do
        SESS=""
        for f in "$d"/*/track.raw "$d"/*/akam2k_track.raw; do
            if [ -r "$f" ]; then SESS=$f; break; fi
        done
        if [ -n "$SESS" ]; then break; fi
    done
fi
if [ -n "$SESS" ] && [ -r "$SESS" ]; then
    r=$("$REPO/tools/dev/track_score" replay --raw "$SESS")
    if echo "$r" | grep -Eq ' ticks=[1-9][0-9]* .* cmds=[0-9]+ '; then ok "replay of the hardware session $(basename "$(dirname "$SESS")") scores: $(echo "$r" | cut -d' ' -f2-6)"; else bad "replay of the hardware session did not score"; fi
else
    echo "note: hardware session not present, replay check not run (.local/logs/track-sessions/*/track.raw)"
fi

if [ "$fail" -ne 0 ]; then
    echo "track bench: $fail failed"
    exit 1
fi
echo "track bench: 0 failed"
