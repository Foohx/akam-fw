#!/usr/bin/env python3
"""Test of tools/track_lagfit: synthetic raw recordings (R / I / C lines of track_debug=2) of a pan / tilt sweep over a STATIC person, generated from a KNOWN video lag and
gains, must give those values back (lag within one grid step, gains within 3 %), with a small residual; a recording without motion is refused.
Prints 'rot2k track lagfit test: N failed'."""
import math
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "track_lagfit")
FW, FH = 320, 192
fails = 0


def check(cond, msg):
    global fails
    print(("  ok   - " if cond else "  FAIL - ") + msg)
    if not cond:
        fails += 1


def pose(t):
    """The true camera pose (steps) at monotonic time t ms: a pan sweep with reversals and a slower tilt."""
    return (1500 + 400 * math.sin(t / 1000.0 * 1.3) + 120 * math.sin(t / 1000.0 * 3.1), 1300 + 150 * math.sin(t / 1000.0 * 0.9))


def make(path, lag, gp, gt, world=(1900.0, 1250.0), move=True, noise=0.4, inv_pan=1, inv_tilt=0, c_lag=50, walker=False):
    m = -1 if inv_pan else 1
    kt = 1 if inv_tilt else -1
    seed = [12345]

    def rnd():
        seed[0] = (seed[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return seed[0] / 0x7FFFFFFF - 0.5

    with open(path, "w") as f:
        f.write("C invert_pan=%d invert_tilt=%d gain_pan=1000 gain_tilt=700 lag_ms=%d motion=1\n" % (inv_pan, inv_tilt, c_lag))
        t0 = 100000
        for k in range(0, 40000, 50):  # the motor samples every 50 ms, 40 s
            t = t0 + k
            p = pose(t if move else t0)
            f.write("R %d %d %d\n" % (t, round(p[0]), round(p[1])))
        for k in range(1000, 39000, 200):  # 5 Hz detections
            now = t0 + k
            cap = now - 90
            p = pose(cap - lag if move else t0)  # the frame shows the world as the camera saw it LAG ms before the stamp
            # offset of the box centre (px) so that  W = pose + m * g * off / fw  (pan)  and  W = pose + kt * g * off / fh  (tilt)
            ox = (world[0] - p[0]) / (m * gp) * FW + noise * rnd()
            oy = (world[1] - p[1]) / (kt * gt) * FH + noise * rnd()
            xc, yc = FW / 2 + ox, FH / 2 + oy
            x1, x2, y1, y2 = int(xc - 20), int(xc + 20), int(yc - 40), int(yc + 40)
            if x1 < 4 or x2 > FW - 4 or y1 < 4 or y2 > FH - 4:
                f.write("I %d %d 1500 1300 1 1500 1300 0 0 0 1300 300 3650 300 2100 0 me=0 cr=0 ob=0 rb=0 ct=1\n" % (now, cap))
                continue
            boxes = ["{0,800,%d,%d,%d,%d}" % (x1, y1, x2, y2)]
            if walker:  # somebody else walks across the frame, the camera follows him: his box is anywhere, the decoy stands still in the world
                wx = int(160 + 110 * math.sin(k / 900.0))
                boxes.insert(k // 200 % 2, "{0,900,%d,40,%d,170}" % (wx - 25, wx + 25))
            f.write("I %d %d 1500 1300 1 1500 1300 0 0 0 1300 300 3650 300 2100 %d %s me=0 cr=0 ob=0 rb=0 ct=1\n" % (now, cap, len(boxes), " ".join(boxes)))


def run(path, *extra):
    r = subprocess.run([sys.executable, "-I", TOOL, path] + list(extra), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def num(pat, text):
    m = re.search(pat, text)
    return float(m.group(1)) if m else None


d = tempfile.mkdtemp()
print("== track_lagfit on synthetic sweeps")
for lag, gp, gt in ((50, 1000.0, 700.0), (120, 1180.0, 820.0), (-60, 900.0, 640.0), (0, 1000.0, 700.0)):
    p = os.path.join(d, "s%d.raw" % lag)
    make(p, lag, gp, gt)
    rc, out, err = run(p)
    fl = num(r"FIT lag_ms=(-?\d+)", out)
    fpan = num(r"pan : gain (\d+)", out)
    ftilt = num(r"tilt: gain (\d+)", out)
    rms = num(r"pan : gain \d+ steps per frame width, residual rms ([\d.]+)", out)
    check(rc == 0 and fl is not None and abs(fl - lag) <= 5, "lag %d ms: fitted %s ms (within the 5 ms grid step)" % (lag, fl))
    check(fpan is not None and abs(fpan - gp) <= 0.03 * gp, "gain_pan %.0f: fitted %s" % (gp, fpan))
    check(ftilt is not None and abs(ftilt - gt) <= 0.03 * gt, "gain_tilt %.0f: fitted %s" % (gt, ftilt))
    check(rms is not None and rms < 4.0, "pan residual %s steps rms (< 4, the pose swings +-500)" % rms)
    check(("track_lag_ms=%d" % round(fl)) in out and "track_gain_pan=" in out, "the akam.conf lines are printed")
# a walker in view next to the static decoy: the decoy is picked
p = os.path.join(d, "walker.raw")
make(p, 80, 1100.0, 760.0, walker=True)
rc, out, err = run(p)
check(rc == 0 and abs((num(r"FIT lag_ms=(-?\d+)", out) or 999) - 80) <= 5 and abs((num(r"pan : gain (\d+)", out) or 0) - 1100) <= 33, "a walker next to the decoy: the static box is picked, lag 80 / gain 1100 recovered")
# a wrong sign convention is flagged
p = os.path.join(d, "inv.raw")
make(p, 50, 1000.0, 700.0)
rc, out, err = run(p, "--invert-pan", "0")
check("WARNING: negative pan gain" in out, "--invert-pan 0 on a recording made with invert_pan=1: negative gain flagged")
# the head did not move: refused
p = os.path.join(d, "static.raw")
make(p, 50, 1000.0, 700.0, world=(1530.0, 1290.0), move=False)
rc, out, err = run(p)
check(rc == 1 and "did not move" in err, "a recording without motion: no fit, exit 1")
# no R lines: refused
p = os.path.join(d, "noring.raw")
open(p, "w").write("I 1 1 1 1 1 1 1 0 0 0 1300 300 3650 300 2100 0 me=0\n")
rc, out, err = run(p)
check(rc == 1 and "R lines" in err, "no R lines: exit 1")
print("rot2k track lagfit test: %d failed" % fails)
sys.exit(1 if fails else 0)
