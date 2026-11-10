#!/usr/bin/env bash
# Host test of tools/hw_tests/parse_stock_capture.py on a synthetic capture folder
# (made-up numbers in the format written by stock_hook.c; no vendor data, no hardware).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
PARSER=$REPO/tools/hw_tests/parse_stock_capture.py

if ! command -v python3 >/dev/null 2>&1; then
    echo "skip: python3 not installed"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }
has() { if grep -Eq -- "$2" "$3"; then ok "$1"; else ko "$1 (no match: $2)"; sed 's/^/      | /' "$3" | head -n 40; fi; }

D=$WORK/run01
mkdir -p "$D"
cat >"$D/run.meta" <<'EOF'
mtd7_sha256=14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294
stock_firmware=yes
EOF
cat >"$D/stock_hook.log" <<'EOF'
T=0.000 tid=100 HOOK_START version=1 arch=arm comm=anyka_ipc pid=100 wall=1700000000
T=0.002 tid=100 HOOK_SINK dir=/tmp/stock_capture/out cap=12582912
T=1.500 tid=100 OPEN path="/dev/pcmC0D0p" flags=0x1 mode=0x0 cls=pcm fd=7
T=1.501 tid=100 OPEN path="/dev/pcmC0D0l" flags=0x1 mode=0x0 cls=pcm fd=8
T=1.510 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x80045081 dir=2 type=0x50 nr=0x81 size=4 in=01000000 out=same ret=0
T=1.511 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x80045030 dir=2 type=0x50 nr=0x30 size=4 in=03000000 out=same ret=0
T=1.512 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x401c50e0 dir=1 type=0x50 nr=0xe0 size=28 in=00000000401f00000200000010000000000400000800000000000000 out=same ret=0
T=1.513 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x80045010 dir=2 type=0x50 nr=0x10 size=4 in=06000000 out=same ret=0
T=1.514 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x50e1 dir=0 type=0x50 nr=0xe1 size=0 arg=0x0 ret=0
T=1.520 tid=100 WRITE fd=7 dev=/dev/pcmC0D0p count=1024 ret=1024 head=00000000ffff0100ffff0100
T=1.552 tid=100 WRITE fd=7 dev=/dev/pcmC0D0p count=1024 ret=-1 errno=11
T=3.600 tid=100 AUDIO_STATS fd=7 dev=/dev/pcmC0D0p op=w why=window win_ms=2000 calls=62 bytes=63488 gap_ms_min=31 gap_ms_max=33 an_bytes=2048 samples=1024 peak=900 sumabs=204800 zero16=10 pairs=512 lr_eq=512 b00=100 bff=0 baw=0
T=4.000 tid=100 OPEN path="/sys/class/gpio/export" flags=0x1 mode=0x0 cls=gpio ret=-1 errno=2
T=4.010 tid=100 WRITE fd=9 dev=/sys/class/gpio/gpio69/value count=1 ret=1 text="1"
T=5.000 tid=100 OPEN path="/usr/share/prompt.mp3" flags=0x0 mode=0x0 cls=audiofile fd=10
T=5.100 tid=100 CLOSE fd=10 dev=/usr/share/prompt.mp3 ret=0 wr_calls=0 wr_bytes=0 rd_calls=3 rd_bytes=9000 errs=0
T=6.000 tid=100 MMAP fd=11 dev=/dev/mem len=0x1000 prot=0x3 flags=0x1 off=0x20170000 ret=0x40000000
T=7.000 tid=101 ISP_BLOCK name=CCM cmd=0x40044912 flag=1 ret=0 seen=1 len=16 data=0a0000000b0000000c0000000d000000
T=7.001 tid=101 ISP_BLOCK name=CCM cmd=0x40044912 flag=1 ret=0 seen=2 len=16 data=0a0000000b0000000c0000000e000000
T=10.000 tid=100 ALIVE events=40 dropped=0
EOF
cat >"$D/isp_hook.log" <<'EOF'
AE window n=15 lum[min=100 max=110 last=105] hw_exp[min=1 max=2] reversals=0 sgain=1.0x ispgain=1.0x
SETREG addr=0x0000030e val=0x00000001 (1)
EOF
cat >"$D/dmesg.txt" <<'EOF'
[   12.3] ak_pcm: SPEAKER @69(lv 1)
[   12.4] unrelated line
EOF

python3 -I "$PARSER" "$D" >"$WORK/out.txt" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "parser exits 0"; else ko "parser exit $rc"; cat "$WORK/out.txt"; fi
has "run meta shown" 'stock_firmware=yes' "$WORK/out.txt"
has "ordered sequence: speaker enable first, with its note" 'SET_SPEAKER.*value=1.*GPIO 69 is driven by the driver' "$WORK/out.txt"
has "PREPARE decoded" 'PREPARE.*format=0 rate=8000 channels=2 bits=16 period_bytes=1024 periods=8' "$WORK/out.txt"
has "gain value" 'SET_GAIN \(nr 0x30\): playback:3@1\.51s' "$WORK/out.txt"
has "source value" 'SET_SOURCES \(nr 0x10\): playback:6' "$WORK/out.txt"
has "period in milliseconds" 'period = 256 frames = 32\.0 ms, ring = 8192 bytes' "$WORK/out.txt"
has "byte rate hint" '31744 bytes/s' "$WORK/out.txt"
has "stereo identical channels inferred" 'mono duplicated' "$WORK/out.txt"
has "failed write listed" 'WRITE /dev/pcmC0D0p .*errno=11' "$WORK/out.txt"
has "gpio text" 'WRITE +/sys/class/gpio/gpio69/value +1' "$WORK/out.txt"
has "failed gpio export listed" 'OPEN +/sys/class/gpio/export' "$WORK/out.txt"
has "audio file bytes" '/usr/share/prompt.mp3 read 9000 bytes in 3 calls' "$WORK/out.txt"
has "mmap window" '/dev/mem len=0x1000 prot=0x3 off=0x20170000' "$WORK/out.txt"
has "ISP block with distinct count" 'CCM +cmd=0x40044912 dumps=2 distinct=2' "$WORK/out.txt"
has "legacy isp log summarised" 'AE windows: 1; SETREG: 1' "$WORK/out.txt"
has "kernel lines filtered" 'SPEAKER @69' "$WORK/out.txt"
if grep -q 'unrelated line' "$WORK/out.txt"; then ko "dmesg filter keeps unrelated lines"; else ok "dmesg filter drops unrelated lines"; fi

# a log file path works as well, and an empty log does not crash
if python3 -I "$PARSER" "$D/stock_hook.log" >/dev/null 2>&1; then ok "accepts the log file path"; else ko "log file path"; fi
mkdir "$WORK/empty"; : >"$WORK/empty/stock_hook.log"
if python3 -I "$PARSER" "$WORK/empty" >"$WORK/empty.out" 2>&1; then ok "empty log is handled"; else ko "empty log crashes"; cat "$WORK/empty.out"; fi
if python3 -I "$PARSER" "$WORK/nosuch" >/dev/null 2>&1; then ko "missing folder must fail"; else ok "missing folder fails cleanly"; fi

echo
echo "stock_capture_parse_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
