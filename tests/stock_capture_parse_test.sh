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
hasnt() { if grep -Eq -- "$2" "$3"; then ko "$1 (unexpected: $2)"; else ok "$1"; fi; }

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
T=1.515 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0x40045099 dir=1 type=0x50 nr=0x99 size=4 in=2a000000 out=07000000 ret=0
T=1.516 tid=100 IOCTL fd=7 dev=/dev/pcmC0D0p req=0xc0045a01 dir=3 type=0x5a nr=0x01 size=4 in=01000000 out=same ret=-1 errno=22
T=1.517 tid=100 IOCTL fd=12 dev=/dev/akpcm_pdev1 req=0x40045030 dir=1 type=0x50 nr=0x30 size=4 in=02000000 out=same ret=0
T=1.518 tid=100 IOCTL fd=9 dev=/sys/class/gpio/gpio69/value req=0x5401 dir=0 type=0x54 nr=0x01 size=0 arg=0x0 ret=-1 errno=25
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
has "every ioctl shows its raw request number" 'req=0x80045081 ' "$WORK/out.txt"
has "an unknown 'P' ioctl on the playback fd is listed raw with its bytes" 'req=0x40045099 .*in=2a000000 out=07000000' "$WORK/out.txt"
has "a foreign ioctl type on the playback fd is listed raw" 'type=0x5a nr=0x01 req=0xc0045a01|req=0xc0045a01 .*in=01000000' "$WORK/out.txt"
has "the known-wrong SET_SPEAKER number is flagged" 'KNOWN WRONG' "$WORK/out.txt"
has "a request that differs from ak_ioctls.h is flagged" 'req=0x40045030 .*req differs from ak_ioctls.h 0x80045030' "$WORK/out.txt"
has "/dev/akpcm_pdev1 is treated as a playback device" 'playback +SET_GAIN.*req=0x40045030|fd=12 +playback' "$WORK/out.txt"
has "ioctl on a GPIO descriptor is listed" 'gpio69/value .*req=0x5401.*errno=25' "$WORK/out.txt"
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

# --- a v2 capture: full ISP blocks, timeline, a run cut short by a reboot ------------------------------------------
E=$WORK/run02
mkdir -p "$E"
cat >"$E/run.meta" <<'EOF'
script=stock_capture.sh
status=running
start_times_before=3
start_times_reset=yes
phases= 20:COVER_THE_LENS 40:UNCOVER_THE_LENS
EOF
cat >"$E/stock_hook.log" <<'EOF'
T=0.000 tid=100 HOOK_START version=2 arch=arm comm=anyka_ipc pid=100 wall=1700000000 mono_ms=500000000
T=3.600 tid=100 OPEN path="/sys/class/gpio/gpio65/value" flags=0x1 mode=0x0 cls=gpio fd=38
T=3.601 tid=100 WRITE fd=38 dev=/sys/class/gpio/gpio65/value count=1 ret=1 text="1"
T=3.700 tid=100 WRITE fd=38 dev=/sys/class/gpio/gpio65/value count=1 ret=1 text="0"
T=25.000 tid=100 WRITE fd=40 dev=/sys/devices/platform/leds/leds/fill_light_led/brightness count=3 ret=3 text="100"
T=25.100 tid=100 READ fd=41 dev=/sys/bus/iio/devices/iio:device0/in_voltage0_raw count=16 ret=4 text="812\x0a"
T=9.000 tid=100 ISP_STAT name=CCM cmd=0x40044912 size=16 calls=5 changes=2 dumps=3
T=12.000 tid=100 ALIVE events=40 dropped=0 isp_calls=900 isp_dumps=7 isp_skipped=11 gpio_skipped=30 bin_bytes=412 log_bytes=2000
T=14.400 tid=100 CHILD abs_ms=500014400 via=system exe="/bin/sh" cmd="echo 3 > /tmp/camera_start_times; reboot -f"
T=14.500 tid=200 CHILD abs_ms=500014500 ppid=100 exe="/bin/busybox" cmd="sh -c reboot -f"
T=14.600 tid=100 ISP_BLOCK name=CCM cmd=0x40044912 flag=1 ret=0 size=16 hash=0x1 calls=1 changes=0 dump=1 stored=bin head=00
EOF
python3 -I - "$E" <<'PYEOF'
import struct, sys
d = sys.argv[1]
H = struct.Struct("<4sIIIII")
def rec(t, cmd, data):
    return H.pack(b"ISPB", int(t * 1000), cmd, len(data), 0x1000 + len(data), 7) + data
blob = b""
ccm_day = struct.pack("<4I", 1, 256, 0, 256)
ccm_night = struct.pack("<4I", 1, 256, 77, 256)
blob += rec(2.0, 0x40044912, ccm_day)
for i, lum in enumerate((120, 118, 20, 15, 18)):               # AE_RUN_INFO: lum, hw_exp, sgain, ispgain
    blob += rec(10 + i * 10, 0x80044952, bytes([lum, 0, 0, 0]) + struct.pack("<3I", 1000 * (i + 1), 256, 256) + b"\0" * 20)
blob += b"GARBAGE-BETWEEN-RECORDS"                            # a damaged spot: the parser resyncs
blob += rec(26.0, 0x40044912, ccm_night)                      # written after the lens was covered (t=20)
blob += rec(46.0, 0x40044912, ccm_day)                        # ... and back after t=40
blob += rec(30.0, 0x40044925, bytes(range(32)))
open(d + "/isp_blocks.bin", "wb").write(blob)
PYEOF
cat >"$E/timeline.log" <<'EOF'
up=500002.00 t=0 ev=START pid=100 hook=yes
up=500010.00 t=8 ev=TICK ipc=alive pid=100 load=0.50 memfree_kb=9000 start_times=0
up=500020.50 t=20 ev=PHASE text=COVER THE LENS
up=500021.00 t=21 ev=TICK ipc=alive pid=100 load=0.70 memfree_kb=8800 start_times=1
up=500030.00 t=28 ev=NOTE text=IR LEDs on
up=500040.20 t=40 ev=PHASE text=UNCOVER THE LENS
up=500048.00 t=46 ev=TICK ipc=GONE pid=100 load=1.50 memfree_kb=8000 start_times=1
EOF
python3 -I "$PARSER" "$E" --export-isp "$WORK/export" >"$WORK/out2.txt" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "parser reads a v2 capture"; else ko "v2 parse exit $rc"; cat "$WORK/out2.txt"; fi
has "unfinished run announced" 'RUN DID NOT FINISH CLEANLY: run.meta says status=running' "$WORK/out2.txt"
hasnt "no 'most likely rebooted' guess" 'most likely rebooted' "$WORK/out2.txt"
has "unfinished run points to timeline.log" 'See timeline.log' "$WORK/out2.txt"
has "last heartbeat shown on the hook clock" 'last heartbeat: up=500048.00 \(T=48.00\) t=46s ipc=GONE load=1.50 memfree_kb=8000 start_times=1' "$WORK/out2.txt"
has "a real reboot is recognised by the uptime going backwards" 'uptime going backwards' "$WORK/out2.txt"
has "anyka_ipc GONE counted" 'anyka_ipc GONE in 1 of' "$WORK/out2.txt"
has "start counter history" 'camera_start_times over the run: t=8s:0, t=21s:1' "$WORK/out2.txt"
has "free RAM trend" 'free RAM: first 9000 kB, last 8000 kB, min 8000 kB' "$WORK/out2.txt"
has "child process flagged as reboot with its time" 'T=  14.500 pid=200 ppid=100 sh -c reboot -f +<== REBOOT/RESET' "$WORK/out2.txt"
has "system() call of anyka_ipc listed with its thread and flagged" 'T=  14.400 system\(\) by thread 100: echo 3 > /tmp/camera_start_times; reboot -f +<== REBOOT/RESET' "$WORK/out2.txt"
has "ALIVE counters" 'ISP calls=900 dumped=7 skipped=11, sysfs events skipped=30, bin=412 bytes' "$WORK/out2.txt"
has "full ISP section with the damaged record skipped" 'FULL \(isp_blocks.bin: 9 dumps, [0-9]+ bytes, 1 damaged record' "$WORK/out2.txt"
has "block size and kind" 'CCM +cmd=0x40044912 size=16 +written \(SET\) dumps=3' "$WORK/out2.txt"
has "word diff between the dumps (day -> night)" 'T=  26.000 1 word\(s\) changed: \[2\] 0->77' "$WORK/out2.txt"
has "AE ranges" 'lum 15..120, hw_exposure 1000..5000' "$WORK/out2.txt"
has "phase windows listed" 'phase 20..40 s: after "COVER THE LENS"' "$WORK/out2.txt"
has "CCM differs between the phases" 'CCM +differs between phase 0 and 1: 1 word\(s\): \[2\] 0->77' "$WORK/out2.txt"
has "and back" 'CCM +differs between phase 1 and 2: 1 word\(s\): \[2\] 77->0' "$WORK/out2.txt"
has "time line: prompt" 'T=  20.500  >>> PROMPT: COVER THE LENS' "$WORK/out2.txt"
has "time line: note" '>>> NOTE: IR LEDs on' "$WORK/out2.txt"
has "time line: GPIO pulse" 'T=   3.601  WRITE class/gpio/gpio65/value = 1' "$WORK/out2.txt"
has "time line: IR LED brightness" 'WRITE devices/platform/leds/leds/fill_light_led/brightness = 100' "$WORK/out2.txt"
has "time line: light sensor value" 'read  bus/iio/devices/iio:device0/in_voltage0_raw = 812' "$WORK/out2.txt"
has "time line: AE state" 'AE   lum=20 hw_exposure=3000' "$WORK/out2.txt"
has "time line: ISP write with its diff" 'ISP  CCM \(16 bytes\) 1 word\(s\) changed: \[2\] 0->77' "$WORK/out2.txt"
has "time line: the camera reboot command" '!!! CHILD PROCESS: sh -c reboot -f' "$WORK/out2.txt"
if [ "$(find "$WORK/export" -type f | wc -l)" -eq 9 ] && [ -f "$WORK/export/CCM_0x40044912_0003_T0046.000.bin" ]; then ok "--export-isp writes one file per dump"; else ko "export: $(ls "$WORK/export")"; fi
# a later run on the same card tells whether the camera rebooted in between (uptime going backwards)
mkdir "$WORK/run03"; printf 'status=finished rc=0\nuptime=100.5\n' >"$WORK/run03/run.meta"
python3 -I "$PARSER" "$E" >"$WORK/out2b.txt" 2>&1
has "later run with a lower uptime: the camera rebooted" 'run03 started at uptime 100 s, below the last heartbeat .*REBOOTED' "$WORK/out2b.txt"
printf 'status=finished rc=0\nuptime=900000.5\n' >"$WORK/run03/run.meta"
python3 -I "$PARSER" "$E" >"$WORK/out2c.txt" 2>&1
has "later run with a higher uptime: no reboot" 'did not reboot in between' "$WORK/out2c.txt"
rm -rf "$WORK/run03"
# the same run, finished: no interruption notice
sed -i 's/^status=running$/status=finished rc=0/' "$E/run.meta"
python3 -I "$PARSER" "$E" >"$WORK/out3.txt" 2>&1
if grep -q 'DID NOT FINISH' "$WORK/out3.txt"; then ko "finished run reported as interrupted"; else ok "a finished run is not reported as interrupted"; fi
has "its status is shown" 'run status: finished' "$WORK/out3.txt"

# a log file path works as well, and an empty log does not crash
if python3 -I "$PARSER" "$D/stock_hook.log" >/dev/null 2>&1; then ok "accepts the log file path"; else ko "log file path"; fi
mkdir "$WORK/empty"; : >"$WORK/empty/stock_hook.log"
if python3 -I "$PARSER" "$WORK/empty" >"$WORK/empty.out" 2>&1; then ok "empty log is handled"; else ko "empty log crashes"; cat "$WORK/empty.out"; fi
if python3 -I "$PARSER" "$WORK/nosuch" >/dev/null 2>&1; then ko "missing folder must fail"; else ok "missing folder fails cleanly"; fi

echo
echo "stock_capture_parse_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
