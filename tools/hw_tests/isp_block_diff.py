#!/usr/bin/env python3
"""isp_block_diff.py - compare the ISP blocks of OUR firmware with a stock capture (host side, stdlib only).

usage: python3 -I isp_block_diff.py STOCK_RUN_DIR_OR_BIN OUR_DIR_OR_BIN [--stock-at SECONDS] [--show N]

STOCK_RUN_DIR is a run of tools/hw_tests/stock_capture.sh (it holds isp_blocks.bin). OUR_DIR is what
`isp_dump --blocks --out DIR` or the 8091 key `img.dump` wrote (img.dump: /tmp/isp_live), the same format.
Both files are "ISPB" records: magic, t_ms, cmd, len, hash, tid (6 x u32 little endian) and the block.

For every block present in both it prints the number of 32-bit words that differ. The stock state is
the last dump of each block at or before --stock-at seconds of the hook clock (default 10: the day state
after start-up; use 60 for the night state of a --daynight run; day again after 180). Blocks that are only
guessed readable on our side are reported as they come. The leading runtime words of SET_EXPOSURE (AE
state) are ignored. --show N prints the first N differing words of each block (index, ours, stock).

The captures are vendor data: keep them private; this tool prints words only on your own terminal.
Exit status: 0 whatever the differences are, 2 on bad arguments, 1 on unreadable files.
"""
import os
import struct
import sys

MAGIC = b"ISPB"
HDR = struct.Struct("<4sIIIII")
NAMES = {
    0x40044902: "BLC", 0x40044904: "LSC", 0x4004490a: "RAW_LUT", 0x4004490c: "NR1", 0x4004492b: "NR2",
    0x4004497d: "UVNR", 0x4004492f: "3D_NR", 0x40044906: "GB", 0x4004490e: "DEMO", 0x4004491a: "RGB_GAMMA",
    0x40044912: "CCM", 0x40044936: "FCS", 0x4004491b: "WDR", 0x40044925: "SHARP", 0x40044927: "SHARP_EX",
    0x4004493b: "SATURATION", 0x40044939: "CONTRAST", 0x4004493f: "RGB2YUV", 0x40044941: "EFFECT",
    0x40044910: "DPC", 0x8004498e: "LCE", 0x40044960: "AF", 0x80044986: "SET_WB", 0x40044973: "AWB_EX",
    0x80044988: "AWB_CALIB", 0x8004498a: "SET_EXPOSURE", 0x4004494e: "SET_FRAME_RATE", 0x4004496e: "MISC",
    0x40044975: "Y_GAMMA", 0x40044977: "HUE",
}
SKIP_WORDS = {"SET_EXPOSURE": 7}
GET_TO_SET = {0x8004498b: 0x8004498a, 0x8004494f: 0x4004494e}


def read_records(path):
    if os.path.isdir(path):
        path = os.path.join(path, "isp_blocks.bin")
    with open(path, "rb") as fh:
        raw = fh.read()
    recs, off = [], 0
    while off + HDR.size <= len(raw):
        magic, t_ms, cmd, n, _h, _tid = HDR.unpack_from(raw, off)
        if magic != MAGIC or n > 1 << 20 or off + HDR.size + n > len(raw):
            nxt = raw.find(MAGIC, off + 1)
            if nxt < 0:
                break
            off = nxt
            continue
        recs.append((t_ms / 1000.0, GET_TO_SET.get(cmd, cmd), raw[off + HDR.size: off + HDR.size + n]))
        off += HDR.size + n
    return recs


def state_at(recs, t_max):
    """name -> (t, data): the last record of each block at or before t_max (the first one if none is)."""
    out, first = {}, {}
    for t, cmd, data in recs:
        name = NAMES.get(cmd)
        if name is None:
            continue
        first.setdefault(name, (t, data))
        if t <= t_max:
            out[name] = (t, data)
    for name, rec in first.items():
        out.setdefault(name, rec)
    return out


def words(data):
    n = len(data) // 4
    return list(struct.unpack("<%dI" % n, data[: n * 4])) + ([int.from_bytes(data[n * 4:], "little")] if len(data) % 4 else [])


def compare(stock, ours, show):
    lines, equal, compared = [], 0, 0
    head = "%-15s %6s %8s %12s" % ("block", "size", "stock t", "words differ")
    lines.append(head)
    lines.append("-" * len(head))
    for name in NAMES.values():
        if name not in ours:
            continue
        if name not in stock:
            lines.append("%-15s %6d %8s %12s" % (name, len(ours[name][1]), "-", "not in stock capture"))
            continue
        ts, sd = stock[name]
        od = ours[name][1]
        compared += 1
        if len(sd) != len(od):
            lines.append("%-15s %6d %8.3f %12s" % (name, len(od), ts, "SIZE %d vs %d" % (len(od), len(sd))))
            continue
        sw, ow, skip = words(sd), words(od), SKIP_WORDS.get(name, 0)
        diffs = [(i, ow[i], sw[i]) for i in range(skip, len(sw)) if sw[i] != ow[i]]
        if not diffs:
            equal += 1
        lines.append("%-15s %6d %8.3f %12s" % (name, len(od), ts, "0 (equal)" if not diffs else "%d of %d" % (len(diffs), len(sw) - skip)))
        for i, o, s in diffs[:show]:
            lines.append("    word %-5d ours=0x%08x stock=0x%08x" % (i, o, s))
    lines.append("")
    lines.append("compared %d block(s): %d equal, %d differ" % (compared, equal, compared - equal))
    return "\n".join(lines) + "\n"


def main(argv):
    args, stock_at, show = argv[1:], 10.0, 0
    pos = []
    try:
        while args:
            a = args.pop(0)
            if a in ("-h", "--help"):
                sys.stdout.write(__doc__)
                return 0
            if a == "--stock-at":
                stock_at = float(args.pop(0))
            elif a == "--show":
                show = int(args.pop(0))
            elif a.startswith("--"):
                raise ValueError("unknown option " + a)
            else:
                pos.append(a)
        if len(pos) != 2:
            raise ValueError("want exactly STOCK and OURS")
    except (IndexError, ValueError) as exc:
        sys.stderr.write("isp_block_diff: %s\n" % exc)
        return 2
    try:
        stock = state_at(read_records(pos[0]), stock_at)
        ours = state_at(read_records(pos[1]), 1e12)
    except OSError as exc:
        sys.stderr.write("isp_block_diff: %s\n" % exc)
        return 1
    if not stock or not ours:
        sys.stderr.write("isp_block_diff: no ISP block found in %s\n" % (pos[0] if not stock else pos[1]))
        return 1
    sys.stdout.write("stock state at t <= %.1f s\n" % stock_at)
    sys.stdout.write(compare(stock, ours, show))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
