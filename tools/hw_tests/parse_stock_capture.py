#!/usr/bin/env python3
"""parse_stock_capture.py - readable summary of a stock_capture.sh run (host side, stdlib only).

usage: python3 -I parse_stock_capture.py RUN_DIR_OR_stock_hook.log [--export-isp OUTDIR]

Reads stock_hook.log (and, next to it when present: isp_blocks.bin, isp_hook.log, timeline.log,
run.meta, dmesg.txt, fd_*.txt) written by src/ak_rtsp/stock_hook.c and
tools/hw_tests/stock_capture.sh, and prints: the state of the run (finished, or not finished cleanly,
with the last heartbeat and, when a later run exists, whether the camera rebooted), the ordered speaker ioctl sequence with decoded arguments, PCM
format / period sizes, gain / source / speaker values, GPIO / PWM / LED / light sensor activity,
audio files, mmap windows, the ISP parameter blocks IN FULL (isp_blocks.bin: which blocks exist,
how often they changed, which words changed and when, which differ between the phases of a
day/night run), one time line of everything that matters for the day/night switch (prompts and
notes, GPIO writes, AE state, ISP block changes) and anything that failed. --export-isp writes
every dumped block as a file (vendor data: keep it private).
Names of the 'P' (0x50) ioctls follow specs/audio.md; both candidate
names are shown where the spec is unsure (the numbers are the facts, the names are hypotheses).
"""
import os
import re
import struct
import sys

# ISP tunnel inner commands: names of isp_hook.c / ak_ioctls.h (keep in sync); the hook prints
# "UNKNOWN" for the ones its own table lacks.
ISP_NAMES = {
    0x40044902: "BLC", 0x40044904: "LSC", 0x4004490a: "RAW_LUT", 0x4004490c: "NR1",
    0x4004492b: "NR2", 0x4004497d: "UVNR", 0x4004492f: "3D_NR", 0x40044906: "GB",
    0x4004490e: "DEMO", 0x4004491a: "RGB_GAMMA", 0x40044912: "CCM", 0x40044936: "FCS",
    0x4004491b: "WDR", 0x40044925: "SHARP", 0x40044927: "SHARP_EX", 0x4004493b: "SATURATION",
    0x40044939: "CONTRAST", 0x4004493f: "RGB2YUV", 0x40044941: "EFFECT", 0x40044910: "DPC",
    0x8004498e: "LCE", 0x40044960: "AF", 0x40044973: "AWB_EX", 0x80044988: "AWB_CALIB",
    0x4004496e: "MISC", 0x40044975: "Y_GAMMA", 0x40044977: "HUE", 0x40044981: "GET_SENSOR_AE",
    0x80044984: "AE_SUSPEND", 0x40044979: "FLIP_MIRROR", 0x4004496b: "SENSOR_SETREG",
    0x8004496c: "SENSOR_GETREG", 0x4004497a: "SET_SENSOR_FPS", 0x8004497b: "GET_SENSOR_FPS",
    0x80044952: "GET_AE_RUN_INFO", 0x8004498b: "GET_EXPOSURE", 0x8004498a: "SET_EXPOSURE",
    0x80044986: "SET_WB", 0x80044987: "GET_WB", 0x4004494e: "SET_FRAME_RATE",
    0x8004494f: "GET_FRAME_RATE", 0x80044970: "GET_3D_NR_STAT", 0x80044991: "GET_AE_STAT_INFO",
    0x40044967: "3D_NR_REF", 0x4004496d: "OSD_USER_PARAMS",
}
GET_AE_RUN_INFO = 0x80044952
ISPB_MAGIC = b"ISPB"
ISPB_HDR = struct.Struct("<4sIIIII")      # magic t_ms cmd len hash tid

# nr of the akpcm 'P' ioctl family -> (name, argument layout)
P_NAMES = {
    0x10: ("SET_SOURCES (=SET_STATE)", "u32"),
    0x30: ("SET_GAIN (=SET_PERIODS)", "u32"),
    0x81: ("SET_SPEAKER (=GET_CAPS)", "u32"),
    0xE0: ("PREPARE (=GET_PARS)", "pars"),
    0xE1: ("RESUME (=SET_MODE)", "int"),
    0xE2: ("POWER_CTRL?", "int"),
    0xE3: ("RESET_BUF", "int"),
    0xE6: ("TIMESTAMP?", "raw"),
    0xEA: ("SET_RATE", "u32"),
}
# Request numbers of src/ak_rtsp/ak_ioctls.h. The number actually issued by the stock app is
# always printed (req=...); it is flagged when it differs from the one ak_rtsp uses.
KNOWN_REQ = {
    0xE0: 0x401c50e0, 0xE1: 0x000050e1, 0x10: 0x80045010, 0x30: 0x80045030,
    0x81: 0x80045081, 0xEA: 0x800450ea, 0xE3: 0x000050e3,
}
PARS_FIELDS = ("format", "rate", "channels", "bits", "period_bytes", "periods", "reserved")
NOTE = {
    0x10: "playback: 4 = DAC, 6 = DAC+LINEIN (reference project)",
    0x81: "amplifier enable, GPIO 69 is driven by the driver; 0x80045081 of ak_ioctls.h is "
          "KNOWN WRONG (EINVAL on the hardware): the req shown is the one stock really uses",
}

LINE_RE = re.compile(r"^T=(\d+\.\d{3}) tid=(\d+) (\S+)(?: (.*))?$")
KV_RE = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S*)')


def parse_line(line):
    m = LINE_RE.match(line.rstrip("\n"))
    if not m:
        return None
    t, tid, ev, rest = m.groups()
    kv = {}
    for k, v in KV_RE.findall(rest or ""):
        kv[k] = v[1:-1] if v.startswith('"') else v
    return {"t": float(t), "tid": int(tid), "ev": ev, "kv": kv, "raw": line.rstrip("\n")}


def u32s(hexstr):
    try:
        b = bytes.fromhex(hexstr)
    except ValueError:
        return []
    b = b[: len(b) // 4 * 4]
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


def tag_of(dev):
    """'playback' / 'capture' / 'loopback' for the PCM devices, 'dev' for everything else.
    Handles /dev/pcmC0D0p style names and /dev/akpcm_* ones (anything with 'pcm' or under
    /dev/snd/); an unrecognised suffix is shown as plain 'pcm' rather than dropped."""
    d = dev or ""
    base = os.path.basename(d)
    if not (base.startswith(("pcm", "akpcm")) or "/dev/snd/" in d or "pcm" in base):
        return "dev"
    for suffix, name in (("c", "capture"), ("p", "playback"), ("l", "loopback")):
        if base.endswith(suffix) or base.endswith("_%sdev1" % suffix) or ("_%sdev" % suffix) in base:
            return name
    return "pcm"


def decode_ioctl(ev):
    kv = ev["kv"]
    typ = int(kv.get("type", "0x0"), 16)
    nr = int(kv.get("nr", "0x0"), 16)
    size = int(kv.get("size", "0"))
    name, layout = ("?", "raw")
    if typ == 0x50 and nr in P_NAMES:
        name, layout = P_NAMES[nr]
    elif typ == 0x50:
        name = "P-nr-0x%02x (unknown)" % nr
    else:
        name = "type=0x%02x nr=0x%02x" % (typ, nr)
    inb = kv.get("in", "")
    outb = kv.get("out", "")
    desc = ""
    if layout == "pars":
        vals = u32s(inb)
        if vals:
            desc = " ".join("%s=%d" % (n, v) for n, v in zip(PARS_FIELDS, vals))
    elif layout == "u32":
        vals = u32s(inb)
        out = u32s(outb) if outb not in ("", "same") else []
        desc = "value=%d" % vals[0] if vals else ""
        if out and out != vals:
            desc += " -> returned %d" % out[0]
    elif layout in ("int",):
        arg = kv.get("arg")
        if arg is not None:
            desc = "arg=%s" % arg
        else:
            vals = u32s(inb)
            desc = "value=%d" % vals[0] if vals else ""
    else:
        if "arg" in kv:
            desc = "arg=%s" % kv["arg"]
        elif inb:
            desc = "in=%s" % inb
        if outb and outb != "same":
            desc += " out=%s" % outb
    req = kv.get("req", "?")
    if layout != "raw" and (inb or outb) and desc:
        desc += " raw=%s" % (inb or "-")
        if outb and outb != "same":
            desc += " out=%s" % outb
    if typ == 0x50 and nr in KNOWN_REQ:
        try:
            if int(req, 16) != KNOWN_REQ[nr] and nr != 0x81:
                desc += " [req differs from ak_ioctls.h 0x%08x]" % KNOWN_REQ[nr]
        except ValueError:
            pass
    if nr in NOTE and typ == 0x50:
        desc += "   [%s]" % NOTE[nr]
    desc = ("req=%s " % req) + desc
    ret = "ret=%s" % kv.get("ret", "?")
    if "errno" in kv:
        ret += " errno=%s" % kv["errno"]
    return name, desc.strip(), ret


# ----------------------------------------------------------------------------- isp_blocks.bin
def read_isp_blocks(d):
    """Records of isp_blocks.bin: [{'t','cmd','hash','tid','data'}]. A damaged record (short write,
    cap hit in the middle) is skipped by searching for the next magic."""
    p = os.path.join(d, "isp_blocks.bin")
    if not os.path.isfile(p):
        return [], 0
    with open(p, "rb") as fh:
        raw = fh.read()
    recs, off, skipped = [], 0, 0
    while off + ISPB_HDR.size <= len(raw):
        magic, t_ms, cmd, n, h, tid = ISPB_HDR.unpack_from(raw, off)
        if magic != ISPB_MAGIC or n > 1 << 20 or off + ISPB_HDR.size + n > len(raw):
            nxt = raw.find(ISPB_MAGIC, off + 1)
            skipped += 1
            if nxt < 0:
                break
            off = nxt
            continue
        recs.append({"t": t_ms / 1000.0, "cmd": cmd, "hash": h, "tid": tid,
                     "data": raw[off + ISPB_HDR.size: off + ISPB_HDR.size + n]})
        off += ISPB_HDR.size + n
    return recs, skipped


def isp_name(cmd, fallback=None):
    return ISP_NAMES.get(cmd) or (fallback if fallback and fallback != "UNKNOWN" else "UNKNOWN")


def words_of(b):
    return list(struct.unpack("<%dI" % (len(b) // 4), b[: len(b) // 4 * 4]))


def diff_blocks(a, b):
    """[(word index, old, new)] between two dumps (a changed length is reported as such)."""
    out = []
    wa, wb = words_of(a), words_of(b)
    for i in range(min(len(wa), len(wb))):
        if wa[i] != wb[i]:
            out.append((i, wa[i], wb[i]))
    if len(a) != len(b):
        out.append(("len", len(a), len(b)))
    tail_a, tail_b = a[len(a) // 4 * 4:], b[len(b) // 4 * 4:]
    if len(a) == len(b) and tail_a != tail_b:
        out.append(("tail", tail_a.hex(), tail_b.hex()))
    return out


def fmt_diff(diffs, limit=6):
    parts = []
    for i, x, y in diffs[:limit]:
        if i in ("len", "tail"):
            parts.append("%s %s->%s" % (i, x, y))
        else:
            parts.append("[%d] %d->%d" % (i, x, y))
    if len(diffs) > limit:
        parts.append("... +%d" % (len(diffs) - limit))
    return " ".join(parts)


def distinct_dumps(recs):
    """Per cmd, the dumps in time order (the hook already dropped repeats of the same content)."""
    by = {}
    for r in recs:
        by.setdefault(r["cmd"], []).append(r)
    return by


def ae_run_info(data):
    """GET_AE_RUN_INFO layout used by isp_hook.c: u8 lum, u32 hw_exposure @4, sensor gain @8, isp gain @12."""
    if len(data) < 16:
        return None
    return data[0], struct.unpack_from("<I", data, 4)[0], struct.unpack_from("<I", data, 8)[0], \
        struct.unpack_from("<I", data, 12)[0]


# ----------------------------------------------------------------------------- timeline.log
TL_RE = re.compile(r"^up=(\S+) t=(\d+) ev=(\S+)(?: (.*))?$")


def load_timeline(d):
    p = os.path.join(d, "timeline.log")
    out = []
    if not os.path.isfile(p):
        return out
    with open(p, "r", errors="replace") as fh:
        for line in fh:
            m = TL_RE.match(line.rstrip("\n"))
            if not m:
                continue
            up, t, ev, rest = m.groups()
            try:
                upf = float(up)
            except ValueError:
                continue
            kv = {}
            if ev in ("PHASE", "NOTE"):
                kv["text"] = (rest or "")[5:] if (rest or "").startswith("text=") else (rest or "")
            else:
                kv = dict(KV_RE.findall(rest or ""))
            out.append({"up": upf, "t": int(t), "ev": ev, "kv": kv})
    return out


def meta_status(meta):
    st = None
    for line in (meta or "").splitlines():
        if line.startswith("status="):
            st = line[len("status="):].split()[0]
    return st


def fmt_t(t):
    return "%8.3f" % t if t is not None else "       ?"


def legacy_isp_section(out, events):
    """ISP blocks of a v1 hook (hex in the log, truncated at 256 bytes) or a v2 log without
    isp_blocks.bin: what the text lines say."""
    section(out, "ISP parameter blocks (first dump of each; no isp_blocks.bin: data is what the log line shows)")
    seen = {}
    for e in events:
        if e["ev"] == "ISP_BLOCK":
            seen.setdefault(e["kv"].get("cmd"), []).append(e)
    for cmd, lst in sorted(seen.items(), key=lambda x: x[1][0]["t"]):
        k = lst[0]["kv"]
        changed = len({x["kv"].get("data") or x["kv"].get("hash") for x in lst})
        out.append("  %-12s cmd=%s dumps=%d distinct=%d  first@%.3fs len=%s" % (
            k.get("name"), cmd, len(lst), changed, lst[0]["t"], k.get("len", k.get("size"))))
        w = u32s(k.get("data", k.get("head", "")))[:8]
        out.append("      u32[0..7]=%s" % " ".join("%d" % v for v in w))
    if not seen:
        out.append("  none captured")


def phase_windows(tl):
    """[(T_start, T_end, label)] on the hook clock, split at the prompts of the run."""
    marks = [(x["T"], x["kv"].get("text", "")) for x in tl if x["ev"] == "PHASE" and x["T"] is not None]
    if not marks:
        return []
    ends = [m[0] for m in marks[1:]] + [1e9]
    wins = [(0.0, marks[0][0], "start (before the first prompt)")]
    for (t0, label), t1 in zip(marks, ends):
        wins.append((t0, t1, 'after "%s"' % label))
    return wins


def isp_full_section(out, events, recs, skipped, tl, mono0):
    by = distinct_dumps(recs)
    stat = {}
    for e in events:
        if e["ev"] == "ISP_STAT":
            stat[e["kv"].get("cmd")] = e["kv"]
    names = {}
    for e in events:
        if e["ev"] == "ISP_BLOCK":
            names[int(e["kv"].get("cmd", "0x0"), 16)] = e["kv"].get("name")
    total = sum(len(r["data"]) for r in recs)
    section(out, "ISP parameter blocks, FULL (isp_blocks.bin: %d dumps, %d bytes%s)" % (
        len(recs), total, ", %d damaged record(s) skipped" % skipped if skipped else ""))
    out.append("  one dump per distinct content; polled blocks (dir=read) at most 1 per second, written ones 10 per second")
    for cmd, lst in sorted(by.items(), key=lambda x: x[1][0]["t"]):
        name = isp_name(cmd, names.get(cmd))
        kind = "polled/read" if (cmd >> 30) == 2 else "written (SET)"
        st = stat.get("0x%08x" % cmd) or stat.get(hex(cmd)) or {}
        extra = ""
        if st:
            extra = " calls=%s changes=%s" % (st.get("calls"), st.get("changes"))
        out.append("  %-16s cmd=0x%08x size=%-5d %-13s dumps=%-3d first@%.3fs last@%.3fs%s" % (
            name, cmd, len(lst[0]["data"]), kind, len(lst), lst[0]["t"], lst[-1]["t"], extra))
        w = words_of(lst[0]["data"])[:8]
        out.append("      first u32[0..7]=%s" % " ".join("%d" % v for v in w))
        if cmd == GET_AE_RUN_INFO:
            rows = [(r["t"], ae_run_info(r["data"])) for r in lst if ae_run_info(r["data"])]
            if rows:
                out.append("      lum %d..%d, hw_exposure %d..%d, sensor gain %d..%d, isp gain %d..%d" % (
                    min(x[1][0] for x in rows), max(x[1][0] for x in rows), min(x[1][1] for x in rows),
                    max(x[1][1] for x in rows), min(x[1][2] for x in rows), max(x[1][2] for x in rows),
                    min(x[1][3] for x in rows), max(x[1][3] for x in rows)))
            continue
        shown = 0
        for prev, cur in zip(lst, lst[1:]):
            dd = diff_blocks(prev["data"], cur["data"])
            if shown < 8 or (cmd >> 30) == 1:
                out.append("      T=%8.3f %d word(s) changed: %s" % (cur["t"], len(dd), fmt_diff(dd, 5)))
            shown += 1
            if shown >= 24:
                out.append("      ... %d more changes (use --export-isp to diff them all)" % (len(lst) - 1 - shown))
                break
        # polled statistics: which words never change / always change
        if (cmd >> 30) == 2 and len(lst) >= 4:
            ws = [words_of(r["data"]) for r in lst]
            n = min(len(w) for w in ws)
            var = [i for i in range(n) if len({w[i] for w in ws}) > 1]
            out.append("      %d of %d words vary over the run (statistics), the other %d never change" % (len(var), n, n - len(var)))
    # state at the end of each phase of a day/night run
    wins = phase_windows(tl)
    if wins and mono0 is not None:
        section(out, "ISP blocks that differ between the phases of the run (state at the end of each phase)")
        for t0, t1, label in wins:
            out.append("  phase %.0f..%.0f s: %s" % (t0, min(t1, max(r["t"] for r in recs)), label))
        def state_at(cmd, t):
            last = None
            for r in by[cmd]:
                if r["t"] <= t:
                    last = r
            return last
        for cmd in sorted(by):
            if (cmd >> 30) != 1 and cmd not in (0x80044986, 0x80044987, 0x8004498a, 0x8004498b):
                continue                                      # polled statistics always differ: only the parameters
            sts = [state_at(cmd, w[1] - 1e-6) for w in wins]
            for i in range(len(sts) - 1):
                if sts[i] and sts[i + 1] and sts[i]["data"] != sts[i + 1]["data"]:
                    dd = diff_blocks(sts[i]["data"], sts[i + 1]["data"])
                    out.append("  %-16s differs between phase %d and %d: %d word(s): %s" % (
                        isp_name(cmd, names.get(cmd)), i, i + 1, len(dd), fmt_diff(dd, 8)))
                elif sts[i] is None and sts[i + 1] is not None:
                    out.append("  %-16s first seen in phase %d (size %d)" % (isp_name(cmd, names.get(cmd)), i + 1, len(sts[i + 1]["data"])))


def daynight_section(out, events, recs, tl, mono0):
    """One time line of what matters for the day/night switch."""
    items = []
    for x in tl:
        if x["T"] is None:
            continue
        if x["ev"] == "PHASE":
            items.append((x["T"], 0, ">>> PROMPT: %s" % x["kv"].get("text", "")))
        elif x["ev"] == "NOTE":
            items.append((x["T"], 0, ">>> NOTE: %s" % x["kv"].get("text", "")))
        elif x["ev"] == "IPC_RESTART":
            items.append((x["T"], 0, "!!! anyka_ipc restarted (pid %s -> %s)" % (x["kv"].get("old"), x["kv"].get("new"))))
    sysfs = ("gpio", "pwm", "leds", "iio")
    last_read = {}
    for e in events:
        k = e["kv"]
        dev = k.get("dev", k.get("path", ""))
        if e["ev"] in ("WRITE", "READ") and "text" in k and any(m in dev for m in ("/sys/class/gpio", "/sys/class/pwm", "/sys/class/leds", "/sys/devices/platform/leds", "/sys/bus/iio/")):
            label = "WRITE" if e["ev"] == "WRITE" else "read "
            text = k["text"].replace("\\x0a", "").replace("\\x00", "")
            if e["ev"] == "READ" and last_read.get(dev) == text:
                continue
            if e["ev"] == "READ":
                last_read[dev] = text
            items.append((e["t"], 1, "%s %s = %s%s" % (label, dev.split("/sys/")[-1] if "/sys/" in dev else dev, text,
                                                      ("  (seen %s)" % k["seen"]) if "seen" in k else "")))
        elif e["ev"] == "CHILD" and re.search(r"reboot|recover_cfg|poweroff|halt", k.get("cmd", "")) and mono0 is not None:
            try:
                items.append((int(k["abs_ms"]) / 1000.0 - mono0, 0, "!!! CHILD PROCESS: %s" % k.get("cmd")))
            except (KeyError, ValueError):
                pass
    prev_by_cmd = {}
    ae_last_t, ae_last_lum = -99.0, None
    for r in recs:
        cmd = r["cmd"]
        if cmd == GET_AE_RUN_INFO:
            v = ae_run_info(r["data"])
            if v and (r["t"] - ae_last_t >= 5 or (ae_last_lum is not None and abs(v[0] - ae_last_lum) >= 10)):
                items.append((r["t"], 2, "AE   lum=%d hw_exposure=%d sensor_gain=%d isp_gain=%d" % v))
                ae_last_t, ae_last_lum = r["t"], v[0]
            continue
        if (cmd >> 30) != 1 and cmd not in (0x80044986, 0x8004498a):
            continue
        prev = prev_by_cmd.get(cmd)
        what = "first dump" if prev is None else "%d word(s) changed: %s" % (
            len(diff_blocks(prev, r["data"])), fmt_diff(diff_blocks(prev, r["data"]), 4))
        prev_by_cmd[cmd] = r["data"]
        items.append((r["t"], 3, "ISP  %s (%d bytes) %s" % (isp_name(cmd), len(r["data"]), what)))
    section(out, "day/night time line (prompts, notes, sysfs GPIO/PWM/LED/ADC, AE state, ISP parameter writes)")
    if not items:
        out.append("  nothing captured")
        return
    items.sort(key=lambda x: (x[0], x[1]))
    shown, prev_text, rep, prev_idx, base = 0, None, 0, -1, ""
    for i, (t, _, text) in enumerate(items):
        if text == prev_text:                       # identical consecutive lines: fold
            rep += 1
            out[prev_idx] = base + "  x%d" % (rep + 1)
            continue
        out.append("  T=%8.3f  %s" % (t, text))
        prev_text, rep, prev_idx, base = text, 0, len(out) - 1, out[-1]
        shown += 1
        if shown >= 400:
            out.append("  ... %d more events (the log has them all)" % (len(items) - i - 1))
            break


def load(path):
    if os.path.isdir(path):
        d = path
        log = os.path.join(d, "stock_hook.log")
    else:
        log = path
        d = os.path.dirname(os.path.abspath(path))
    events, bad = [], 0
    with open(log, "r", errors="replace") as fh:
        for line in fh:
            if not line.strip():
                continue
            ev = parse_line(line)
            if ev:
                events.append(ev)
            elif "LOG_CAP" in line:
                events.append({"t": 0, "tid": 0, "ev": "LOG_CAP", "kv": {}, "raw": line.strip()})
            else:
                bad += 1
    return d, events, bad


def read_text(d, name, limit=None):
    p = os.path.join(d, name)
    if not os.path.isfile(p):
        return None
    with open(p, "r", errors="replace") as fh:
        data = fh.read()
    return data if limit is None else data[:limit]


def section(out, title):
    out.append("")
    out.append("== %s ==" % title)


def later_boot_verdict(d, last_up):
    """Compare the last heartbeat of run `d` with the uptime recorded at the start of the next
    runs on the same card: a lower uptime means the camera rebooted in between. None if unknown."""
    m = re.match(r"^(.*?)run(\d+)$", os.path.basename(os.path.normpath(d)))
    if not m or last_up is None:
        return None
    parent = os.path.dirname(os.path.normpath(d))
    n = int(m.group(2))
    try:
        names = sorted(os.listdir(parent))
    except OSError:
        return None
    for name in names:
        m2 = re.match(r"^run(\d+)$", name)
        if not m2 or int(m2.group(1)) <= n:
            continue
        meta = read_text(os.path.join(parent, name), "run.meta") or ""
        for line in meta.splitlines():
            if line.startswith("uptime="):
                try:
                    up = float(line.split("=", 1)[1].split()[0])
                except (ValueError, IndexError):
                    break
                if up < last_up:
                    return ("The camera uptime went backwards: %s started at uptime %.0f s, below the last heartbeat "
                            "(%.0f s): the camera REBOOTED after this run." % (name, up, last_up))
                return ("%s started at uptime %.0f s, after the last heartbeat (%.0f s): the camera did not reboot in "
                        "between." % (name, up, last_up))
    return None


def summarize(d, events, bad):
    out = []
    section(out, "run")
    meta = read_text(d, "run.meta")
    if meta:
        out.extend("  " + l for l in meta.strip().splitlines())
    mono0 = None
    for e in events:
        if e["ev"] in ("HOOK_START", "HOOK_SINK"):
            out.append("  %s %s" % (e["ev"], " ".join("%s=%s" % kv for kv in e["kv"].items())))
        if e["ev"] == "HOOK_START" and "mono_ms" in e["kv"] and mono0 is None:
            try:
                mono0 = int(e["kv"]["mono_ms"]) / 1000.0
            except ValueError:
                pass
    tl = load_timeline(d)
    for x in tl:                                    # heartbeat time on the hook's clock
        x["T"] = x["up"] - mono0 if mono0 is not None else None
    recs, skipped = read_isp_blocks(d)
    n_ev = len(events)
    out.append("  events parsed: %d, unparsable lines: %d" % (n_ev, bad))
    if any(e["ev"] == "LOG_CAP" for e in events):
        out.append("  WARNING: the size cap was reached, later events are missing")
    alive = [e for e in events if e["ev"] == "ALIVE"]
    if alive:
        k = alive[-1]["kv"]
        out.append("  last ALIVE at T=%.3f: events=%s dropped=%s" % (alive[-1]["t"], k.get("events"), k.get("dropped")))
        if "isp_calls" in k:
            out.append("      ISP calls=%s dumped=%s skipped=%s, sysfs events skipped=%s, bin=%s bytes, log=%s bytes" % (
                k.get("isp_calls"), k.get("isp_dumps"), k.get("isp_skipped"), k.get("gpio_skipped"),
                k.get("bin_bytes"), k.get("log_bytes")))
    last_ev_t = max([e["t"] for e in events if e["ev"] != "CHILD"] or [0])
    st = meta_status(meta)
    if st in ("running", "interrupted"):
        out.append("  *** RUN DID NOT FINISH CLEANLY: run.meta says status=%s, the script never reached its end." % st)
        out.append("      That alone does not mean the camera rebooted: the script is also stopped by SIGKILL, or by")
        out.append("      a lost telnet session when it was not started with --detach (that ends as status=signal).")
        out.append("      See timeline.log (one line per second) for the last moment of the run. Logs are complete")
        out.append("      up to the last fsync (2 s).")
        ticks = [x for x in tl if x["ev"] == "TICK"]
        if ticks:
            x = ticks[-1]
            out.append("      last heartbeat: up=%.2f (T=%s) t=%ds ipc=%s load=%s memfree_kb=%s start_times=%s" % (
                x["up"], ("%.2f" % x["T"]) if x["T"] is not None else "?", x["t"], x["kv"].get("ipc"),
                x["kv"].get("load"), x["kv"].get("memfree_kb"), x["kv"].get("start_times")))
        out.append("      last hook event: T=%.3f." % last_ev_t)
        verdict = later_boot_verdict(d, ticks[-1]["up"] if ticks else None)
        if verdict:
            out.append("      " + verdict)
        else:
            out.append("      A real reboot shows as the camera uptime going backwards (up= in timeline.log, or uptime=")
            out.append("      in the run.meta of a later run); no later run here to compare with.")
    elif st:
        out.append("  run status: %s" % st)
    if tl:
        ticks = [x for x in tl if x["ev"] == "TICK"]
        gone = [x for x in ticks if x["kv"].get("ipc") == "GONE"]
        out.append("  timeline.log: %d heartbeats, anyka_ipc GONE in %d of them" % (len(ticks), len(gone)))
        if gone:
            out.append("      first GONE at up=%.2f (T=%s)" % (gone[0]["up"], ("%.2f" % gone[0]["T"]) if gone[0]["T"] is not None else "?"))
        sts = []
        for x in ticks:
            v = x["kv"].get("start_times")
            if not sts or sts[-1][1] != v:
                sts.append((x["t"], v))
        if sts:
            out.append("      /tmp/camera_start_times over the run: %s" % ", ".join("t=%ds:%s" % a for a in sts[:12]))
        mem = [int(x["kv"]["memfree_kb"]) for x in ticks if x["kv"].get("memfree_kb", "?").isdigit()]
        if mem:
            out.append("      free RAM: first %d kB, last %d kB, min %d kB" % (mem[0], mem[-1], min(mem)))
        for x in tl:
            if x["ev"] == "IPC_RESTART":
                out.append("      anyka_ipc RESTARTED at up=%.2f: pid %s -> %s" % (x["up"], x["kv"].get("old"), x["kv"].get("new")))
    # child processes of anyka_ipc (system(), popen()): a "reboot -f" would show here
    kids = [e for e in events if e["ev"] == "CHILD"]
    section(out, "commands run by anyka_ipc (system() / popen() calls and children that loaded the hook), from the CHILD lines")
    for e in kids[:80]:
        k = e["kv"]
        try:
            rel = int(k.get("abs_ms", "0")) / 1000.0 - mono0 if mono0 is not None else None
        except ValueError:
            rel = None
        flag = "   <== REBOOT/RESET" if re.search(r"reboot|recover_cfg|poweroff|halt", k.get("cmd", "")) else ""
        if "via" in k:      # logged by anyka_ipc itself, from its system() / popen() call
            out.append("  T=%s %s() by thread %s: %s%s" % (fmt_t(rel), k["via"], e["tid"], k.get("cmd"), flag))
        else:               # a process that loaded the preloaded hook (dynamically linked child)
            out.append("  T=%s pid=%s ppid=%s %s%s" % (fmt_t(rel), e["tid"], k.get("ppid"), k.get("cmd"), flag))
    if len(kids) > 80:
        out.append("  ... %d more" % (len(kids) - 80))
    if not kids:
        out.append("  none seen (only the system() / popen() calls of anyka_ipc are visible, hook version 2 or later;")
        out.append("  a command started some other way, e.g. by another process, is not)")

    # ---- devices
    section(out, "devices opened (cls pcm / gpio / mem / audio files / config)")
    for e in events:
        if e["ev"] == "OPEN":
            k = e["kv"]
            res = "fd=%s" % k["fd"] if "fd" in k else "FAILED errno=%s" % k.get("errno")
            out.append("  T=%8.3f %-9s %s flags=%s %s" % (e["t"], k.get("cls"), k.get("path"), k.get("flags"), res))

    # ---- speaker ioctl sequence
    section(out, "ordered ioctl sequence on the PCM devices (playback / capture / loopback)")
    seq = [e for e in events if e["ev"] == "IOCTL" and tag_of(e["kv"].get("dev")) != "dev"]
    gains, sources, speaker, rates = [], [], [], []
    pars = {}
    prev, rep, prev_idx, base_line = None, 0, -1, ""
    for e in seq:
        name, desc, ret = decode_ioctl(e)
        k = e["kv"]
        tag = tag_of(k.get("dev"))
        key = (k.get("fd"), tag, name, desc, ret)
        if key == prev:                      # identical consecutive call: fold into one line
            rep += 1
            out[prev_idx] = base_line + " x%d" % (rep + 1)
        else:
            rep = 0
            prev = key
            out.append("  T=%8.3f fd=%-3s %-8s %-26s %s %s" % (e["t"], k.get("fd"), tag, name, desc, ret))
            prev_idx = len(out) - 1
            base_line = out[prev_idx].rstrip()
        nr = int(k.get("nr", "0x0"), 16)
        if int(k.get("type", "0x0"), 16) == 0x50:
            vals = u32s(k.get("in", ""))
            if nr == 0x30 and vals:
                if not gains or gains[-1][1:] != (tag, vals[0]):
                    gains.append((e["t"], tag, vals[0]))
            elif nr == 0x10 and vals:
                sources.append((e["t"], tag, vals[0]))
            elif nr == 0x81 and vals:
                speaker.append((e["t"], tag, vals[0]))
            elif nr == 0xEA and vals:
                rates.append((e["t"], tag, vals[0]))
            elif nr == 0xE0 and len(vals) >= 6:
                pars.setdefault(tag, []).append(dict(zip(PARS_FIELDS, vals)))
    if not seq:
        out.append("  (none: no ioctl on a PCM device was captured)")

    # ---- values
    section(out, "gain / volume / source / speaker values (hypothetical names, see header)")
    for title, lst in (("SET_GAIN (nr 0x30)", gains), ("SET_SOURCES (nr 0x10)", sources),
                       ("SET_SPEAKER (nr 0x81)", speaker), ("SET_RATE (nr 0xea)", rates)):
        if lst:
            out.append("  %s: %s" % (title, ", ".join("%s:%d@%.2fs" % (tg, v, t) for t, tg, v in lst)))
        else:
            out.append("  %s: not seen" % title)

    # ---- PCM format
    section(out, "PCM format and period sizes")
    for tag, lst in sorted(pars.items()):
        p = lst[-1]
        byte_rate = p["rate"] * p["channels"] * p["bits"] // 8 if p["bits"] else 0
        out.append("  %-8s PREPARE x%d, last: rate=%d Hz channels=%d bits=%d period_bytes=%d periods=%d format=%d"
                   % (tag, len(lst), p["rate"], p["channels"], p["bits"], p["period_bytes"], p["periods"], p["format"]))
        if byte_rate:
            frame = p["channels"] * p["bits"] // 8
            out.append("           => %d bytes/s, period = %d frames = %.1f ms, ring = %d bytes"
                       % (byte_rate, p["period_bytes"] // frame if frame else 0,
                          1000.0 * p["period_bytes"] / byte_rate, p["period_bytes"] * p["periods"]))
    if not pars:
        out.append("  no PREPARE (0xe0) captured")

    # ---- audio data
    section(out, "audio data seen on the PCM fds")
    stats = [e for e in events if e["ev"] == "AUDIO_STATS"]
    byfd = {}
    for e in stats:
        byfd.setdefault((e["kv"].get("fd"), e["kv"].get("dev"), e["kv"].get("op")), []).append(e)
    for (fd, dev, op), lst in sorted(byfd.items(), key=lambda x: str(x[0])):
        win = sum(int(e["kv"].get("win_ms", 0)) for e in lst) or 1
        byt = sum(int(e["kv"].get("bytes", 0)) for e in lst)
        calls = sum(int(e["kv"].get("calls", 0)) for e in lst)
        smp = sum(int(e["kv"].get("samples", 0)) for e in lst)
        pairs = sum(int(e["kv"].get("pairs", 0)) for e in lst)
        lreq = sum(int(e["kv"].get("lr_eq", 0)) for e in lst)
        sumabs = sum(int(e["kv"].get("sumabs", 0)) for e in lst)
        zero = sum(int(e["kv"].get("zero16", 0)) for e in lst)
        peak = max(int(e["kv"].get("peak", 0)) for e in lst)
        anb = sum(int(e["kv"].get("an_bytes", 0)) for e in lst) or 1
        b00 = sum(int(e["kv"].get("b00", 0)) for e in lst)
        bff = sum(int(e["kv"].get("bff", 0)) for e in lst)
        baw = sum(int(e["kv"].get("baw", 0)) for e in lst)
        gmin = min(int(e["kv"].get("gap_ms_min", 0)) for e in lst)
        gmax = max(int(e["kv"].get("gap_ms_max", 0)) for e in lst)
        bps = byt * 1000.0 / win
        out.append("  fd=%s %s op=%s: %d calls, %d bytes in %d ms windows = %.0f bytes/s, avg call %d bytes, gaps %d..%d ms"
                   % (fd, dev, op, calls, byt, win, bps, byt // max(calls, 1), gmin, gmax))
        hints = []
        for rate in (8000, 16000, 44100, 48000):
            for ch in (1, 2):
                for bits in (8, 16):
                    if abs(bps - rate * ch * bits / 8.0) < 0.06 * bps:
                        hints.append("%d Hz %dch %dbit" % (rate, ch, bits))
        out.append("      byte rate matches: %s" % (", ".join(hints) or "nothing standard"))
        if smp:
            out.append("      as s16le: peak=%d mean|x|=%d zero samples=%d%% L==R pairs=%d%%"
                       % (peak, sumabs // smp, 100 * zero // smp, 100 * lreq // max(pairs, 1)))
        out.append("      byte histogram: 0x00=%d%% 0xff/0x7f=%d%% 0xd5/0x55=%d%% (ulaw/alaw silence shows up as 0xff / 0xd5)"
                   % (100 * b00 // anb, 100 * bff // anb, 100 * baw // anb))
        if smp and zero * 100 // smp > 95:
            out.append("      mostly silence: the format could not be inferred from the content")
        elif pairs and lreq * 100 // pairs > 95:
            out.append("      => stereo interleaved with identical channels (mono duplicated)")
    if not byfd:
        out.append("  no AUDIO_STATS window (no data written, or the run was too short)")
    firsts = [e for e in events if e["ev"] == "WRITE" and tag_of(e["kv"].get("dev")) == "playback"][:3]
    for e in firsts:
        out.append("  first write T=%.3f fd=%s count=%s ret=%s head=%s" % (
            e["t"], e["kv"].get("fd"), e["kv"].get("count"), e["kv"].get("ret"), e["kv"].get("head", "")))
    dumps = [e for e in events if e["ev"] == "AUDIO_DUMP"]
    if dumps:
        out.append("  raw playback bytes saved in stock_audio.raw (first 64 KiB; raw, format per the PREPARE above)")
    closes = [e for e in events if e["ev"] == "CLOSE" and tag_of(e["kv"].get("dev")) != "dev"]
    for e in closes:
        k = e["kv"]
        out.append("  CLOSE T=%.3f fd=%s %s wr=%s calls/%s bytes rd=%s calls/%s bytes" % (
            e["t"], k.get("fd"), k.get("dev"), k.get("wr_calls"), k.get("wr_bytes"), k.get("rd_calls"), k.get("rd_bytes")))

    # ---- GPIO
    section(out, "GPIO (sysfs)")
    gp = [e for e in events if "gpio" in e["kv"].get("dev", e["kv"].get("path", ""))
          and e["ev"] in ("OPEN", "WRITE", "READ", "WRITEV", "MMAP")]
    for e in gp:
        k = e["kv"]
        what = k.get("text", k.get("path", ""))
        out.append("  T=%8.3f %-5s %-45s %s %s" % (e["t"], e["ev"], k.get("dev", k.get("path", "")), what,
                   ("errno=%s" % k["errno"]) if "errno" in k else ""))
    if not gp:
        out.append("  no sysfs GPIO access by anyka_ipc (the amplifier GPIO is driven by the driver, per the spec)")
    other = [e for e in events if e["ev"] == "IOCTL" and tag_of(e["kv"].get("dev")) == "dev"]
    section(out, "ioctls on the other tracked descriptors (GPIO, /dev/mem), raw")
    for e in other:
        name, desc, ret = decode_ioctl(e)
        out.append("  T=%8.3f fd=%-3s %-40s %s %s" % (e["t"], e["kv"].get("fd"), e["kv"].get("dev"), desc, ret))
    if not other:
        out.append("  none")

    # ---- other files and mmap
    section(out, "audio files and config files")
    for e in events:
        if e["ev"] == "CLOSE" and tag_of(e["kv"].get("dev")) == "dev" and \
                ("mp3" in e["kv"].get("dev", "") or "/usr/share" in e["kv"].get("dev", "")):
            out.append("  %s read %s bytes in %s calls" % (e["kv"].get("dev"), e["kv"].get("rd_bytes"), e["kv"].get("rd_calls")))
    section(out, "mmap of device files")
    mm = [e for e in events if e["ev"] == "MMAP"]
    for e in mm:
        k = e["kv"]
        out.append("  T=%8.3f %s len=%s prot=%s off=%s -> %s" % (e["t"], k.get("dev"), k.get("len"), k.get("prot"), k.get("off"), k.get("ret")))
    if not mm:
        out.append("  none")

    # ---- ISP
    if recs:
        isp_full_section(out, events, recs, skipped, tl, mono0)
    else:
        legacy_isp_section(out, events)
    daynight_section(out, events, recs, tl, mono0)
    isp = read_text(d, "isp_hook.log")
    if isp:
        lines = isp.splitlines()
        section(out, "isp_hook.log (legacy decoders: AE, WB, sensor registers, VENC)")
        out.append("  %d lines; AE windows: %d; SETREG: %d; CREATE_ENC: %d" % (
            len(lines), sum(1 for l in lines if l.startswith("AE window")),
            sum(1 for l in lines if l.startswith("SETREG")), sum(1 for l in lines if "CREATE_ENC" in l)))
        for l in lines:
            if "STOCK CREATE_ENC" in l or l.startswith("SET_WB"):
                out.append("  " + l[:160])
                if sum(1 for x in out if x.startswith("  ") and "CREATE_ENC" in x) > 6:
                    break
        ae = [l for l in lines if l.startswith("AE window")]
        if ae:
            out.append("  last " + ae[-1][:160])

    # ---- failures
    section(out, "failures and anomalies")
    fails = [e for e in events if e["kv"].get("ret") == "-1" and e["ev"] in ("IOCTL", "WRITE", "READ", "OPEN", "MMAP")]
    for e in fails[:40]:
        k = e["kv"]
        out.append("  T=%8.3f %s %s req=%s errno=%s" % (e["t"], e["ev"], k.get("dev", k.get("path")), k.get("req", ""), k.get("errno")))
    if len(fails) > 40:
        out.append("  ... %d more" % (len(fails) - 40))
    if not fails:
        out.append("  none")

    # ---- kernel side
    dm = read_text(d, "dmesg.txt")
    if dm:
        keep = [l for l in dm.splitlines() if re.search(r"pcm|speak|gpio|audio|dac|akpcm|amp", l, re.I)]
        section(out, "kernel log lines about audio / gpio / speaker (dmesg.txt)")
        out.extend("  " + l[:170] for l in keep[:60])
        if not keep:
            out.append("  none")
    for name in sorted(os.listdir(d)):
        if name.startswith("fd_") and name.endswith(".txt"):
            section(out, "open descriptors of anyka_ipc (%s)" % name)
            txt = read_text(d, name) or ""
            out.extend("  " + l for l in txt.splitlines() if re.search(r"pcm|gpio|snd|mem|mp3|isp", l))
    return "\n".join(out) + "\n"


def export_isp(d, outdir):
    """Write every dumped ISP block as NAME_0xCMD_NNNN_T<seconds>.bin (vendor data: keep it private)."""
    recs, _ = read_isp_blocks(d)
    os.makedirs(outdir, exist_ok=True)
    counts = {}
    for r in recs:
        n = counts[r["cmd"]] = counts.get(r["cmd"], 0) + 1
        name = "%s_0x%08x_%04d_T%08.3f.bin" % (isp_name(r["cmd"]), r["cmd"], n, r["t"])
        with open(os.path.join(outdir, name), "wb") as fh:
            fh.write(r["data"])
    return len(recs)


def main(argv):
    args = argv[1:]
    export = None
    if "--export-isp" in args:
        i = args.index("--export-isp")
        if i + 1 >= len(args):
            sys.stderr.write(__doc__)
            return 2
        export = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1 or args[0] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 2
    try:
        d, events, bad = load(args[0])
    except OSError as exc:
        sys.stderr.write("parse_stock_capture: %s\n" % exc)
        return 1
    sys.stdout.write(summarize(d, events, bad))
    if export:
        n = export_isp(d, export)
        sys.stdout.write("\nexported %d ISP block dump(s) to %s\n" % (n, export))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
