#!/usr/bin/env bash
# Test of tools/hw_tests/image_noise.py with synthetic videos (ffmpeg lavfi noise, three levels):
# the noise numbers and the bit rate must grow with the noise, bad arguments and sources must fail.
# Skipped (exit 0, "skip:") without ffmpeg / ffprobe / python3.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TOOL="$REPO/tools/hw_tests/image_noise.py"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for t in ffmpeg ffprobe python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "skip: $t not installed"; exit 0; }
done

for n in 0 8 24; do
    if ! ffmpeg -v error -y -f lavfi -i "color=c=gray:s=320x240:r=15,noise=alls=$n:allf=t+u" \
        -frames:v 24 -c:v mpeg4 -q:v 3 "$WORK/n$n.mkv" 2>/dev/null; then
        echo "skip: this ffmpeg cannot encode the synthetic test video"
        exit 0
    fi
done

fail=0
expect() { # description, command...
    local desc=$1
    shift
    if ! "$@"; then
        echo "FAIL: $desc"
        fail=1
    fi
}

out=$(python3 -I "$TOOL" --frames 16 --skip 2 --roi 20,20,200,120 \
    "clean=$WORK/n0.mkv" "low=$WORK/n8.mkv" "high=$WORK/n24.mkv")
echo "$out"

col() { # label, column number (1 = label)
    printf '%s\n' "$out" | awk -v l="$1" -v c="$2" '$1 == l { print $c }'
}
# shellcheck disable=SC2329  # called through expect
gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }
# shellcheck disable=SC2329  # called through expect
exit_is() { # expected code, command...
    local want=$1 rc=0
    shift
    "$@" >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq "$want" ]
}

expect "sigma_t: high noise above low noise" gt "$(col high 3)" "$(col low 3)"
expect "sigma_t: low noise above clean" gt "$(col low 3)" "$(col clean 3)"
expect "sigma_s: high noise above low noise" gt "$(col high 4)" "$(col low 4)"
expect "sigma_s: low noise above clean" gt "$(col low 4)" "$(col clean 4)"
expect "kbps: noisy stream above clean stream" gt "$(col high 5)" "$(col clean 5)"
expect "luma of the grey test pattern is about 128" gt "$(col clean 2)" 100

expect "no source: exit 2" exit_is 2 python3 -I "$TOOL"
expect "bad --roi: exit 2" exit_is 2 python3 -I "$TOOL" --roi 1,2,3 "$WORK/n0.mkv"
expect "unreadable source: exit 1" exit_is 1 python3 -I "$TOOL" "$WORK/missing.mkv"

if [ "$fail" -eq 0 ]; then echo "image_noise_test: OK"; fi
exit "$fail"
