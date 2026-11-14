#!/usr/bin/env bash
# Test of tools/hw_tests/isp_block_diff.py with synthetic "ISPB" files: stock state at a given time,
# equal and differing blocks, the ignored runtime words of SET_EXPOSURE, size mismatches, getter
# commands mapped to their block, bad arguments. Skipped without python3.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TOOL="$REPO/tools/hw_tests/isp_block_diff.py"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v python3 >/dev/null 2>&1 || { echo "skip: python3 not installed"; exit 0; }

python3 -I - "$WORK" <<'PY'
import os, struct, sys
work = sys.argv[1]
def rec(t, cmd, data):
    return struct.pack("<4sIIIII", b"ISPB", int(t * 1000), cmd, len(data), 0, 0) + data
def blk(words):
    return struct.pack("<%dI" % len(words), *words)
os.makedirs(work + "/stock"); os.makedirs(work + "/ours")
day, night = list(range(100, 116)), list(range(200, 216))
expo = list(range(40))
with open(work + "/stock/isp_blocks.bin", "wb") as f:
    f.write(rec(1.0, 0x4004490c, blk(day)))            # NR1 day
    f.write(rec(1.0, 0x40044902, blk(day[:8])))        # BLC
    f.write(rec(1.0, 0x8004498a, blk(expo)))           # SET_EXPOSURE
    f.write(rec(1.0, 0x40044986 + 0x40000000, blk(day[:4])))  # SET_WB (0x80044986)
    f.write(rec(1.5, 0x80044999, blk(day[:4])))        # unknown command: ignored
    f.write(rec(51.0, 0x4004490c, blk(night)))         # NR1 night
with open(work + "/ours/isp_blocks.bin", "wb") as f:
    f.write(rec(0.0, 0x4004490c, blk(day)))            # NR1 equals stock day
    b = day[:8]; b[3] ^= 1
    f.write(rec(0.0, 0x40044902, blk(b)))              # BLC: one word differs
    e = list(expo); e[0] = 999; e[6] = 999             # only runtime words differ
    f.write(rec(0.0, 0x8004498b, blk(e)))              # read back with the GET command of SET_EXPOSURE
    f.write(rec(0.0, 0x80044986, blk(day[:6])))        # SET_WB: size mismatch
    f.write(rec(0.0, 0x4004493b, blk(day[:4])))        # SATURATION: not in the stock capture
PY

fail=0
expect_line() { # description, pattern, text
    if ! printf '%s\n' "$3" | grep -Eq -- "$2"; then
        echo "FAIL: $1 (wanted /$2/)"
        fail=1
    fi
}

out=$(python3 -I "$TOOL" "$WORK/stock" "$WORK/ours" --stock-at 10 --show 3)
echo "$out"
expect_line "NR1 day state equal" '^NR1 .* 0 \(equal\)' "$out"
expect_line "BLC one word differs" '^BLC .* 1 of 8' "$out"
expect_line "BLC word listed" 'word 3 .*ours=0x00000066 stock=0x00000067' "$out"
expect_line "runtime words ignored" '^SET_EXPOSURE .* 0 \(equal\)' "$out"
expect_line "size mismatch" '^SET_WB .* SIZE 24 vs 16' "$out"
expect_line "block missing from stock" '^SATURATION .* not in stock capture' "$out"
expect_line "summary" 'compared 4 block\(s\): 2 equal, 2 differ' "$out"

out=$(python3 -I "$TOOL" "$WORK/stock/isp_blocks.bin" "$WORK/ours" --stock-at 60)
expect_line "night state differs" '^NR1 .* 16 of 16' "$out"

rc=0; python3 -I "$TOOL" "$WORK/stock" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL: one argument must exit 2 (got $rc)"; fail=1; }
rc=0; python3 -I "$TOOL" "$WORK/nope" "$WORK/ours" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 1 ] || { echo "FAIL: unreadable input must exit 1 (got $rc)"; fail=1; }

if [ "$fail" -eq 0 ]; then echo "isp_block_diff_test: OK"; fi
exit "$fail"
