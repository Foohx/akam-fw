#!/usr/bin/env python3
"""parse_stock_capture.py - readable summary of a stock_capture.sh run (host side, stdlib only).

usage: python3 -I parse_stock_capture.py RUN_DIR_OR_stock_hook.log

Reads stock_hook.log (and, next to it when present: isp_hook.log, run.meta, dmesg.txt,
fd_*.txt) written by src/ak_rtsp/stock_hook.c and tools/hw_tests/stock_capture.sh, and
prints: the ordered speaker ioctl sequence with decoded arguments, PCM format / period
sizes, gain / source / speaker values, GPIO activity, audio files, mmap windows, ISP
parameter blocks and anything that failed. Names of the 'P' (0x50) ioctls follow
specs/05-spec-audio-out.md section 1.2; both candidate names are shown where the spec is
unsure (the numbers are the facts, the names are hypotheses).
"""
import os
import re
import struct
import sys

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
PARS_FIELDS = ("format", "rate", "channels", "bits", "period_bytes", "periods", "reserved")
NOTE = {
    0x10: "playback: 4 = DAC, 6 = DAC+LINEIN (reference project)",
    0x81: "amplifier enable, GPIO 69 is driven by the driver",
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
    base = os.path.basename(dev or "")
    if base.startswith(("pcm", "akpcm")) and base[-1:] in ("c", "p", "l"):
        return {"c": "capture", "p": "playback", "l": "loopback"}[base[-1]]
    return "dev"


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
            desc = "in=%s" % inb[:64]
        if outb and outb != "same":
            desc += " out=%s" % outb[:64]
    if nr in NOTE and typ == 0x50:
        desc += "   [%s]" % NOTE[nr]
    ret = "ret=%s" % kv.get("ret", "?")
    if "errno" in kv:
        ret += " errno=%s" % kv["errno"]
    return name, desc.strip(), ret


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


def summarize(d, events, bad):
    out = []
    section(out, "run")
    meta = read_text(d, "run.meta")
    if meta:
        out.extend("  " + l for l in meta.strip().splitlines())
    for e in events:
        if e["ev"] in ("HOOK_START", "HOOK_SINK"):
            out.append("  %s %s" % (e["ev"], " ".join("%s=%s" % kv for kv in e["kv"].items())))
    n_ev = len(events)
    out.append("  events parsed: %d, unparsable lines: %d" % (n_ev, bad))
    if any(e["ev"] == "LOG_CAP" for e in events):
        out.append("  WARNING: the size cap was reached, later events are missing")
    alive = [e for e in events if e["ev"] == "ALIVE"]
    if alive:
        out.append("  last ALIVE at T=%.3f: events=%s dropped=%s" % (
            alive[-1]["t"], alive[-1]["kv"].get("events"), alive[-1]["kv"].get("dropped")))

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
            out.append("  T=%8.3f fd=%-3s %-8s %-26s %-60s %s" % (e["t"], k.get("fd"), tag, name, desc, ret))
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
    gp = [e for e in events if e["kv"].get("dev", e["kv"].get("path", "")).find("/sys/class/gpio") >= 0
          and e["ev"] in ("OPEN", "WRITE", "READ")]
    for e in gp:
        k = e["kv"]
        what = k.get("text", k.get("path", ""))
        out.append("  T=%8.3f %-5s %-45s %s %s" % (e["t"], e["ev"], k.get("dev", k.get("path", "")), what,
                   ("errno=%s" % k["errno"]) if "errno" in k else ""))
    if not gp:
        out.append("  no sysfs GPIO access by anyka_ipc (the amplifier GPIO is driven by the driver, per the spec)")

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
    section(out, "ISP parameter blocks (first dump of each; data is truncated at 256 bytes)")
    seen = {}
    for e in events:
        if e["ev"] == "ISP_BLOCK":
            seen.setdefault(e["kv"].get("cmd"), []).append(e)
    for cmd, lst in sorted(seen.items(), key=lambda x: x[1][0]["t"]):
        k = lst[0]["kv"]
        changed = len({x["kv"].get("data") for x in lst})
        out.append("  %-12s cmd=%s dumps=%d distinct=%d  first@%.3fs len=%s" % (k.get("name"), cmd, len(lst), changed, lst[0]["t"], k.get("len")))
        w = u32s(k.get("data", ""))[:8]
        out.append("      u32[0..7]=%s" % " ".join("%d" % v for v in w))
    if not seen:
        out.append("  none captured")
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


def main(argv):
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 2
    try:
        d, events, bad = load(argv[1])
    except OSError as exc:
        sys.stderr.write("parse_stock_capture: %s\n" % exc)
        return 1
    sys.stdout.write(summarize(d, events, bad))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
