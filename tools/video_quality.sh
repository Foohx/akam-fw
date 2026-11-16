#!/usr/bin/env bash
# video_quality.sh CAPTURE.mkv [MAX_FRAMES]
# Objective numbers for comparing two captures of the same scene (needs ffmpeg, ffprobe and awk on the PC, nothing on
# the camera). Prints:
#   - GOP structure: number of I/P/B frames, the distance between I frames (frames and seconds)
#   - average size of I and P frames (bytes) and the I/P ratio; a high ratio (> 10) with small P frames is the
#     "keyframe pumping" symptom: the picture smears over the GOP and snaps back at each I frame
#   - frame-diff pulse: mean luma difference to the previous frame (ffmpeg tblend difference + signalstats YAVG) on
#     the frames that are not I, on the I frames, and their ratio. Without pumping the ratio stays near 1; with it
#     the diff jumps at every I frame (measured 0.7 -> 3.4 on the 1080p, ratio about 5).
#   - the bitrate of the capture
# Use the same scene, the same lighting and the same length for both captures. Example (20 s of the stream):
#   ffmpeg -rtsp_transport tcp -i rtsp://CAMERA_IP:554/ -t 20 -c copy before.mkv
# The camera sends no B frames, so decode order is display order and the diff of frame N is matched to frame N.
# (tblend yields no output for the first frame, so the diff list is shifted by one.)
set -euo pipefail

usage() { echo "usage: $0 CAPTURE.mkv [MAX_FRAMES]" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage
IN=$1
MAXF=${2:-0}
case "$MAXF" in '' | *[!0-9]*) usage ;; esac
[ -r "$IN" ] || { echo "video_quality: cannot read $IN" >&2; exit 2; }
for t in ffmpeg ffprobe awk; do
    command -v "$t" >/dev/null 2>&1 || { echo "video_quality: $t is not installed" >&2; exit 2; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
LIM=()
FRAMES=()
if [ "$MAXF" -gt 0 ]; then
    LIM=(-read_intervals "%+#$MAXF")
    FRAMES=(-frames:v "$MAXF")
fi

# one line per video frame: the fields are named, so the column order is read from the "csv" header-less output
# with the explicit key=value writer (-of compact), which does not depend on ffprobe's internal field order
ffprobe -v error "${LIM[@]}" -select_streams v:0 -show_entries frame=pict_type,pkt_size -of compact=p=0 "$IN" >"$WORK/frames.txt"
[ -s "$WORK/frames.txt" ] || { echo "video_quality: no video frames in $IN" >&2; exit 1; }
FPS=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of csv=p=0 "$IN" | awk -F/ '{ if ($2 > 0) printf "%.3f", $1 / $2; else print 0 }')

# mean absolute luma difference to the previous frame, one value per frame (the first frame has none)
ffmpeg -v error -nostdin -i "$IN" "${FRAMES[@]}" -an \
    -vf "tblend=all_mode=difference,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=$WORK/yavg.txt" -f null -
grep -o 'YAVG=[0-9.]*' "$WORK/yavg.txt" | cut -d= -f2 >"$WORK/diff.txt"

awk -F'|' -v fps="$FPS" -v dfile="$WORK/diff.txt" '
BEGIN { while ((getline v < dfile) > 0) d[++nd] = v + 0 }
{
    t = ""; sz = 0
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^pict_type=/) t = substr($i, 11)
        else if ($i ~ /^pkt_size=/) sz = substr($i, 10) + 0
    }
    n++
    type[n] = t; cnt[t]++; sum[t] += sz; total += sz
    if (t == "I") { if (lasti) gaps[++ng] = n - lasti; lasti = n }
}
END {
    printf "frames: %d (I %d, P %d, B %d), %s fps\n", n, cnt["I"], cnt["P"], cnt["B"], fps
    if (ng > 0) {
        mn = gaps[1]; mx = gaps[1]; s = 0
        for (i = 1; i <= ng; i++) { if (gaps[i] < mn) mn = gaps[i]; if (gaps[i] > mx) mx = gaps[i]; s += gaps[i] }
        printf "GOP: I every %.1f frames on average (min %d, max %d)", s / ng, mn, mx
        if (fps > 0) printf " = %.2f s", s / ng / fps
        printf "\n"
    } else print "GOP: fewer than two I frames in the capture"
    ia = cnt["I"] ? sum["I"] / cnt["I"] : 0
    pa = cnt["P"] ? sum["P"] / cnt["P"] : 0
    printf "I frames: average %.0f bytes\n", ia
    printf "P frames: average %.0f bytes\n", pa
    if (pa > 0) printf "I/P size ratio: %.1f\n", ia / pa
    if (fps > 0 && n > 0) printf "bitrate: %.0f kbit/s\n", total * 8 * fps / n / 1000
    ci = 0; cp = 0; si = 0; sp = 0
    for (i = 2; i <= n && i <= nd + 1; i++) {
        v = d[i - 1]
        if (type[i] == "I") { si += v; ci++ } else { sp += v; cp++ }
    }
    if (ci > 0 && cp > 0) {
        mi = si / ci; mp = sp / cp
        printf "frame diff (mean luma): non-I frames %.2f, I frames %.2f, pulse (I / non-I) %.2f\n", mp, mi, (mp > 0 ? mi / mp : 0)
    } else print "frame diff: not enough I and non-I frames"
}' "$WORK/frames.txt"
