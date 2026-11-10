#!/usr/bin/env python3
"""image_noise.py - objective grain / bitrate numbers for side-by-side camera tests (host side, stdlib + ffmpeg).

usage: python3 -I image_noise.py [--frames N] [--roi X,Y,W,H] [--skip N] [LABEL=]SOURCE [[LABEL=]SOURCE ...]

SOURCE is a file or an RTSP URL, decoded with ffmpeg (RTSP over TCP) and probed with ffprobe. Point both
cameras at the SAME static scene and pick --roi inside a flat, evenly lit area (wall, floor): the numbers
are only comparable between sources when the ROI shows the same thing. For each source it prints

  luma       mean luma of the ROI (0..255): both sources must be close, or the exposure differs
  sigma_t    temporal noise: robust (MAD) standard deviation of the difference of two consecutive
             frames, divided by sqrt(2), median over all pairs. Moving objects in the ROI do not matter
             much (MAD); an auto-exposure or WB drift does
  sigma_s    spatial noise, Immerkaer's estimator on the ROI (flat area assumed: texture inflates it)
  kbps       average bit rate of the first --frames frames (ffprobe packet sizes)
  I / P      mean size in bytes of intra and other frames
  q_ratio    I-frame size / P-frame size: a low value means the encoder spends bits on noise in P frames

Rule of thumb: a camera that looks grainier has a higher sigma_t (and usually a higher kbps and a lower
q_ratio) on the same scene. Nothing here is a quality score: it is one more column next to your eyes.
Exit status 0, or 1 when a source cannot be decoded.
"""
import math
import re
import shutil
import statistics
import subprocess
import sys

DEFAULT_ROI = (640, 360, 320, 180)
SAMPLE_STRIDE = 2
TOOL_TIMEOUT = 180  # seconds: a stalled RTSP source must not hang the test run
CRED_RE = re.compile(r"(://)[^/@\s]+@")


def mask(text):
    """Hide user:password@ of URLs in anything that is printed."""
    return CRED_RE.sub(r"\1***@", text)


def parse_args(argv):
    frames, roi, skip, sources = 60, DEFAULT_ROI, 5, []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            sys.stdout.write(__doc__)
            sys.exit(0)
        if a == "--frames" and i + 1 < len(argv):
            frames = int(argv[i + 1]); i += 2
        elif a == "--skip" and i + 1 < len(argv):
            skip = int(argv[i + 1]); i += 2
        elif a == "--roi" and i + 1 < len(argv):
            parts = [int(x) for x in argv[i + 1].split(",")]
            if len(parts) != 4 or min(parts) < 0 or parts[2] < 8 or parts[3] < 8:
                raise ValueError("--roi wants X,Y,W,H with W,H >= 8")
            roi = tuple(parts); i += 2
        elif a.startswith("--"):
            raise ValueError("unknown option " + a)
        else:
            sources.append(a); i += 1
    if frames < 3 or frames > 600:
        raise ValueError("--frames must be between 3 and 600")
    if skip < 0 or skip > 600:
        raise ValueError("--skip must be between 0 and 600")
    if not sources:
        raise ValueError("no source given")
    return frames, roi, skip, sources


def split_label(src):
    if "=" in src and not src.startswith(("rtsp://", "http://", "/", ".")) and "://" not in src.split("=", 1)[0]:
        label, path = src.split("=", 1)
        return label, path
    return mask(src).rstrip("/").rsplit("/", 1)[-1][:24] or "source", src


def decode_gray(src, frames, skip, roi):
    """ROI of `frames` consecutive gray frames (after `skip` frames of auto-exposure settling): list of bytes."""
    x, y, w, h = roi
    cmd = ["ffmpeg", "-v", "error", "-nostdin"]
    if src.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd += ["-i", src, "-vf", "crop=%d:%d:%d:%d,format=gray" % (w, h, x, y),
            "-frames:v", str(frames + skip), "-f", "rawvideo", "-"]
    res = run_tool(cmd)
    data, size = res.stdout, w * h
    n = len(data) // size
    if n < 3 + skip:
        raise RuntimeError("only %d frame(s) decoded from %s: %s"
                           % (n, mask(src), mask(res.stderr.decode("utf-8", "replace").strip()[:200])))
    return [data[i * size:(i + 1) * size] for i in range(skip, n)]


def run_tool(cmd):
    """Run ffmpeg / ffprobe; subprocess.run kills the child when the timeout expires."""
    try:
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=TOOL_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise RuntimeError("%s timed out after %d s (source stalled?)" % (cmd[0], TOOL_TIMEOUT))


def mad_sigma(values):
    med = statistics.median(values)
    return 1.4826 * statistics.median([abs(v - med) for v in values])


def temporal_sigma(frames):
    sigmas = []
    for a, b in zip(frames, frames[1:]):
        diff = [a[i] - b[i] for i in range(0, len(a), SAMPLE_STRIDE)]
        sigmas.append(mad_sigma(diff) / math.sqrt(2.0))
    return statistics.median(sigmas)


def spatial_sigma(frame, w, h):
    """Immerkaer (1996): fast noise variance estimation, flat-area assumption."""
    total, count = 0, 0
    for yy in range(1, h - 1, SAMPLE_STRIDE):
        r0, r1, r2 = yy * w - w, yy * w, yy * w + w
        for xx in range(1, w - 1, SAMPLE_STRIDE):
            v = (frame[r0 + xx - 1] - 2 * frame[r0 + xx] + frame[r0 + xx + 1]
                 - 2 * frame[r1 + xx - 1] + 4 * frame[r1 + xx] - 2 * frame[r1 + xx + 1]
                 + frame[r2 + xx - 1] - 2 * frame[r2 + xx] + frame[r2 + xx + 1])
            total += abs(v)
            count += 1
    return math.sqrt(math.pi / 2.0) * total / (6.0 * count)


def bitrate_stats(src, frames, skip):
    cmd = ["ffprobe", "-v", "error"]
    if src.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd += ["-select_streams", "v:0", "-show_entries", "packet=size,flags,pts_time", "-of", "csv=p=0",
            "-read_intervals", "%%+#%d" % (frames + skip), src]
    res = run_tool(cmd)
    sizes_i, sizes_p, first, last = [], [], None, None
    for line in res.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            pts, size = float(parts[0]), int(parts[1])
        except ValueError:
            continue
        (sizes_i if "K" in parts[2] else sizes_p).append(size)
        first = pts if first is None else first
        last = pts
    total = sum(sizes_i) + sum(sizes_p)
    n = len(sizes_i) + len(sizes_p)
    if n < 2 or first is None or last is None or last <= first:
        return None
    span = (last - first) * n / (n - 1)
    return {
        "kbps": total * 8.0 / span / 1000.0,
        "i": statistics.mean(sizes_i) if sizes_i else float("nan"),
        "p": statistics.mean(sizes_p) if sizes_p else float("nan"),
    }


def measure(src, frames, skip, roi):
    w, h = roi[2], roi[3]
    gray = decode_gray(src, frames, skip, roi)
    out = {
        "luma": statistics.mean(statistics.mean(f[::SAMPLE_STRIDE * 4]) for f in gray),
        "sigma_t": temporal_sigma(gray),
        "sigma_s": statistics.mean(spatial_sigma(f, w, h) for f in gray[:: max(1, len(gray) // 5)]),
        "frames": len(gray),
    }
    out["bits"] = bitrate_stats(src, frames, skip)
    return out


def format_table(rows):
    head = "%-24s %7s %8s %8s %8s %9s %9s %8s" % ("source", "luma", "sigma_t", "sigma_s", "kbps", "I bytes", "P bytes", "q_ratio")
    lines = [head, "-" * len(head)]
    for label, m in rows:
        b = m["bits"]
        if b:
            ratio = b["i"] / b["p"] if b["p"] and b["p"] == b["p"] else float("nan")
            bits = "%8.0f %9.0f %9.0f %8.2f" % (b["kbps"], b["i"], b["p"], ratio)
        else:
            bits = "%8s %9s %9s %8s" % ("n/a", "n/a", "n/a", "n/a")
        lines.append("%-24s %7.1f %8.2f %8.2f %s" % (label[:24], m["luma"], m["sigma_t"], m["sigma_s"], bits))
    return "\n".join(lines)


def main(argv):
    try:
        frames, roi, skip, sources = parse_args(argv)
    except ValueError as exc:
        sys.stderr.write("image_noise: %s\n" % exc)
        return 2
    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            sys.stderr.write("image_noise: %s not found (install ffmpeg)\n" % tool)
            return 1
    rows, failed = [], 0
    for src in sources:
        label, path = split_label(src)
        try:
            rows.append((label, measure(path, frames, skip, roi)))
        except (RuntimeError, OSError) as exc:
            sys.stderr.write("image_noise: %s: %s\n" % (label, mask(str(exc))))
            failed += 1
    if rows:
        sys.stdout.write("ROI x=%d y=%d w=%d h=%d, %d frame(s) after %d skipped\n" % (roi + (frames, skip)))
        sys.stdout.write(format_table(rows) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
