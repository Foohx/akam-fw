#!/usr/bin/env bash
# Host test of tools/video_quality.sh: a synthetic noisy static scene encoded with x264 (GOP 30, no B frames) must give
# the GOP, frame-diff and static-region sections; bad arguments are refused. Skipped when ffmpeg or libx264 is missing.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
S=$HERE/../tools/video_quality.sh
for t in ffmpeg ffprobe python3; do command -v "$t" >/dev/null 2>&1 || { echo "video_quality_test: $t missing, skipped"; exit 0; }; done
ENC=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
case $ENC in *libx264*) ;; *) echo "video_quality_test: libx264 missing, skipped"; exit 0 ;; esac
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
PASS=0
FAIL=0
check() { local n=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "PASS  $n"; else FAIL=$((FAIL + 1)); echo "FAIL  $n"; fi; }
ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=15,noise=alls=12:allf=t,format=yuv420p" -t 12 -c:v libx264 -b:v 300k -g 30 -bf 0 -profile:v main "$W/a.mkv"
"$S" "$W/a.mkv" >"$W/o.txt" 2>&1 && rc=0 || rc=$?
check "runs" test "$rc" -eq 0
check "GOP line" grep -q '^GOP: I every' "$W/o.txt"
check "static blocks found" grep -qE '^static regions: [0-9]+ of [0-9]+ blocks static' "$W/o.txt"
check "sharpness drop printed" grep -qE '^sharpness: I frame [0-9.]+, last quarter of the GOP [0-9.]+, drop [+-][0-9.]+ %' "$W/o.txt"
check "I-boundary jump printed" grep -q '^I-boundary jump of the sharpness' "$W/o.txt"
"$S" "$W/a.mkv" 40 >"$W/s.txt" 2>&1 && rc=0 || rc=$?
check "MAX_FRAMES: runs" test "$rc" -eq 0
check "MAX_FRAMES: 40 frames" grep -q '^frames: 40 ' "$W/s.txt"
"$S" >/dev/null 2>&1 && rc=0 || rc=$?
check "no argument: exit 2" test "$rc" -eq 2
"$S" "$W/none.mkv" >/dev/null 2>&1 && rc=0 || rc=$?
check "missing file: exit 2" test "$rc" -eq 2
echo "video_quality_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
