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
#   - static regions (python3 standard library + ffmpeg, no numpy): the picture is cut into blocks of about 16 px
#     (ffmpeg area scaling of the luma and of a Laplacian), a block is "static" when its mean luma stays within a narrow band
#     over the whole capture (p90 - p10 < 6 levels: no real motion, only noise and encoder behaviour), "textured" when it is among
#     the 25 % highest Laplacian blocks of those. On the textured static blocks it prints, per position in the GOP
#     (I frame = 0): the sharpness (mean Laplacian energy), the drift (mean |luma - luma of the GOP's I frame|) and
#     the deviation to the per-block median of the whole capture; then the sharpness drop from the I frame to the last
#     quarter of the P frames, and the I-boundary jump (change at an I frame versus between two P frames). Pumping shows as a
#     sharpness that falls along the GOP (drop of -10 % or worse) and a jump at each I frame well above the P-to-P change.
#     Skipped with a note when python3 is missing. Short captures (< 2 GOPs) give no GOP numbers.
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

# ---- static regions ----------------------------------------------------------------------------------
command -v python3 >/dev/null 2>&1 || { echo "static regions: python3 is not installed, skipped"; exit 0; }
read -r IW IH < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$IN" | awk -Fx '{ print $1, $2 }')
BW=$(((IW + 15) / 16))
BH=$(((IH + 15) / 16))
# one gray16 value per block and frame: the mean luma, and the mean of a (rectified, boosted) Laplacian
ffmpeg -v error -nostdin -i "$IN" "${FRAMES[@]}" -an -vf "format=gray16le,scale=$BW:$BH:flags=area" -f rawvideo "$WORK/luma.raw"
ffmpeg -v error -nostdin -i "$IN" "${FRAMES[@]}" -an \
    -vf "format=gray16le,convolution=0m='-1 -1 -1 -1 8 -1 -1 -1 -1':0rdiv=4,scale=$BW:$BH:flags=area" -f rawvideo "$WORK/lap.raw"
python3 -I - "$WORK/frames.txt" "$WORK/luma.raw" "$WORK/lap.raw" "$BW" "$BH" <<'PY'
import sys, array
ftxt, lf, pf, bw, bh = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
types = []
for line in open(ftxt):
    t = [f[10:] for f in line.strip().split('|') if f.startswith('pict_type=')]
    types.append(t[0] if t else '?')
nb = bw * bh
def load(path):
    a = array.array('H')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder == 'big':
        a.byteswap()
    return a
L, P = load(lf), load(pf)
n = min(len(L), len(P)) // nb
types = types[:n]
if n < 8:
    print("static regions: fewer than 8 frames, skipped"); sys.exit(0)
def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]
# per block: p10/p90 and median of the luma over time (levels 0..255)
med = [0.0] * nb; static = []; lapm = [0.0] * nb
for b in range(nb):
    s = [L[i * nb + b] / 257.0 for i in range(n)]
    med[b] = pct(s, 0.5)
    lapm[b] = sum(P[i * nb + b] for i in range(n)) / n / 257.0
    if pct(s, 0.9) - pct(s, 0.1) < 6.0:
        static.append(b)
if len(static) < 8:
    print("static regions: fewer than 8 static blocks (%d of %d): the scene moves too much" % (len(static), nb)); sys.exit(0)
srt = sorted(static, key=lambda b: lapm[b])
tex = srt[len(srt) * 3 // 4:]
print("static regions: %d of %d blocks static (%.0f %%), %d textured among them (blocks of about 16 px)" % (len(static), nb, 100.0 * len(static) / nb, len(tex)))
sharp = [sum(P[i * nb + b] for b in tex) / len(tex) / 257.0 for i in range(n)]
dev = [sum(abs(L[i * nb + b] / 257.0 - med[b]) for b in tex) / len(tex) for i in range(n)]
print("per frame, textured static blocks: sharpness %.3f mean, deviation to the median %.3f levels mean" % (sum(sharp) / n, sum(dev) / n))
ii = [i for i, t in enumerate(types) if t == 'I']
if len(ii) < 2:
    print("GOP: fewer than two I frames, no per-GOP numbers"); sys.exit(0)
gl = min(b - a for a, b in zip(ii, ii[1:]))
pos_sharp = [[] for _ in range(gl)]; pos_drift = [[] for _ in range(gl)]
for a, b in zip(ii, ii[1:]):  # complete GOPs only
    for k in range(min(gl, b - a)):
        pos_sharp[k].append(sharp[a + k])
        pos_drift[k].append(sum(abs(L[(a + k) * nb + x] - L[a * nb + x]) / 257.0 for x in tex) / len(tex))
m = lambda v: sum(v) / len(v) if v else 0.0
print("GOP position (0 = I frame): sharpness / drift to the I frame (levels)")
step = max(1, gl // 6)
for k in sorted(set(list(range(0, gl, step)) + [gl - 1])):
    print("  %3d  %.3f  %.3f" % (k, m(pos_sharp[k]), m(pos_drift[k])))
s0 = m(pos_sharp[0]); q = max(1, gl // 4)
s1 = m([x for k in range(gl - q, gl) for x in pos_sharp[k]])
if s0 > 0:
    print("sharpness: I frame %.3f, last quarter of the GOP %.3f, drop %+.1f %%" % (s0, s1, 100.0 * (s1 - s0) / s0))
print("drift to the I frame: last quarter of the GOP %.3f levels" % m([x for k in range(gl - q, gl) for x in pos_drift[k]]))
def step_to(i): return sum(abs(L[i * nb + x] - L[(i - 1) * nb + x]) / 257.0 for x in tex) / len(tex)
ji = m([step_to(i) for i in ii if i > 0])
jp = m([step_to(i) for i in range(1, n) if types[i] != 'I' and types[i - 1] != 'I'])
si = m([abs(sharp[i] - sharp[i - 1]) for i in ii if i > 0])
sp = m([abs(sharp[i] - sharp[i - 1]) for i in range(1, n) if types[i] != 'I' and types[i - 1] != 'I'])
print("I-boundary jump of the luma: at I frames %.3f, between P frames %.3f, ratio %.1f" % (ji, jp, ji / jp if jp > 0 else 0))
print("I-boundary jump of the sharpness: at I frames %.3f, between P frames %.3f, ratio %.1f" % (si, sp, si / sp if sp > 0 else 0))
PY
