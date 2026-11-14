#!/usr/bin/env bash
# Host test of src/ak_rtsp/stock_hook.c, the LD_PRELOAD capture of the stock anyka_ipc.
# The camera build is ARM; here the same source is built for x86_64 (the raw syscall layer
# has an x86_64 branch for exactly this) and preloaded into tests/native/stock_hook_driver.c,
# which plays anyka_ipc on a fake device tree. Checks: what is logged, that the real libc
# results (values, errno) never change, size caps, a missing card, a non-anyka_ipc process.
# Also checks that the ARM build (zig) is warning-free and has no undefined symbol except
# the weak __errno_location and dlsym. Needs a host C compiler; touches no hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SRC=$REPO/src/ak_rtsp
CC=${HOST_CC:-cc}
PARSER=$REPO/tools/hw_tests/parse_stock_capture.py

if [ "$(uname -m)" != x86_64 ] || ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: needs an x86_64 host and a C compiler"
    exit 0
fi
# shellcheck source=tools/build/env.sh
[ -f "$REPO/tools/build/env.sh" ] && . "$REPO/tools/build/env.sh" >/dev/null 2>&1

WORK=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }
has() { if grep -Eq -- "$2" "$3"; then ok "$1"; else ko "$1 (no match: $2)"; sed 's/^/      | /' "$3" | head -n 30; fi; }
hasnt() { if grep -Eq -- "$2" "$3"; then ko "$1 (unexpected: $2)"; else ok "$1"; fi; }

# --- builds -------------------------------------------------------------------
HOOKFLAGS=(-std=gnu99 -O1 -Wall -Wextra -Werror -fPIC -shared -nostdlib -ffreestanding
    -fno-builtin -fno-stack-protector -fvisibility=hidden -fno-tree-loop-distribute-patterns
    -I"$SRC" -DSTOCK_HOOK_COMM='"hook_driver"' -DSTATS_WIN_MS=40u -DALIVE_MS=100u)
dirs_macro() { # $1 = base dir
    printf '"%s/out","%s/nosuch/sd","%s/tmp"' "$1" "$1" "$1"
}
mkdir -p "$WORK/a" "$WORK/b" "$WORK/c" "$WORK/d"
if ! "$CC" "${HOOKFLAGS[@]}" -DSTOCK_HOOK_DIRS_TEST="$(dirs_macro "$WORK/a")" \
        -o "$WORK/hook_a.so" "$SRC/stock_hook.c" 2>"$WORK/cc.log"; then
    ko "stock_hook.c builds for the host with -Wall -Wextra -Werror"; cat "$WORK/cc.log"
else
    ok "stock_hook.c builds for the host with -Wall -Wextra -Werror"
fi
"$CC" "${HOOKFLAGS[@]}" -DSTOCK_HOOK_DIRS_TEST="$(dirs_macro "$WORK/b")" \
    -o "$WORK/hook_b.so" "$SRC/stock_hook.c" || ko "build variant b"
"$CC" "${HOOKFLAGS[@]}" -DSTOCK_HOOK_DIRS_TEST="$(dirs_macro "$WORK/c")" \
    -DCAP_MAIN_SD=3000u -DCAP_ISP_SD=600u -DCAP_TMP=3000u -DCAP_BIN_SD=1000u \
    -o "$WORK/hook_c.so" "$SRC/stock_hook.c" || ko "build variant c"
"$CC" -std=gnu99 -O1 -Wall -Wextra -Werror -pthread -o "$WORK/hook_driver" \
    "$HERE/native/stock_hook_driver.c" || ko "build the driver"

# --- fake device tree ---------------------------------------------------------------
R=$WORK/root
mkdir -p "$R/dev" "$R/sys/class/gpio/gpio69" "$R/usr/share"
: > "$R/dev/pcmC0D0p"
head -c 1024 /dev/zero > "$R/dev/pcmC0D0c"
: > "$R/sys/class/gpio/gpio69/value"
: > "$R/sys/class/gpio/gpio69/direction"
{ printf 'M'; head -c 4095 /dev/zero; } > "$R/dev/mem"
head -c 300 /dev/zero > "$R/usr/share/prompt.mp3"
: > "$R/dev/isp-param-0"
mkdir -p "$R/sys/bus/iio/devices/iio:device0" "$R/sys/class/pwm/pwmchip0/pwm0"
printf '1234\n' > "$R/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
: > "$R/sys/class/pwm/pwmchip0/pwm0/duty_cycle"

# --- run A: card present (out -> card folder) ---------------------------------------------
mkdir -p "$WORK/card/run1"
ln -s "$WORK/card/run1" "$WORK/a/out"
LD_PRELOAD="$WORK/hook_a.so" "$WORK/hook_driver" "$R" >"$WORK/a.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "driver self-checks pass under the hook (return values and errno unchanged)"; else ko "driver exit $rc"; cat "$WORK/a.out"; fi
LOG=$WORK/card/run1/stock_hook.log
if [ -f "$LOG" ]; then ok "log written to the card folder"; else ko "no log on the card folder"; find "$WORK/a" "$WORK/card" | head; fi
has "HOOK_START banner (version 2, with the monotonic clock at T=0)" '^T=[0-9]+\.[0-9]{3} tid=[0-9]+ HOOK_START version=2 arch=x86_64 comm=hook_driver pid=[0-9]+ wall=[0-9]+ mono_ms=[0-9]+$' "$LOG"
has "sink line names the card directory" 'HOOK_SINK dir=.*/a/out' "$LOG"
has "failed open of a missing pcm device logged with errno 2" 'OPEN path="[^"]*/dev/pcmC9D9p" .*cls=pcm ret=-1 errno=2' "$LOG"
has "playback open" 'OPEN path="[^"]*/dev/pcmC0D0p" flags=0x1 mode=0x0 cls=pcm fd=[0-9]+' "$LOG"
has "PREPARE ioctl argument bytes (rate 8000 = 401f0000)" 'req=0x401c50e0 .*in=00000000401f00000200000010000000' "$LOG"
has "ioctl failure reported as errno 25" 'req=0x401c50e0 .* ret=-1 errno=25' "$LOG"
has "SET_SOURCES value 6" 'req=0x80045010 .*in=06000000' "$LOG"
has "SET_SPEAKER" 'req=0x80045081 .*in=01000000' "$LOG"
has "no-argument ioctl shows its integer arg" 'req=0x50e1 dir=0 type=0x50 nr=0xe1 size=0 arg=0x0' "$LOG"
n=$(grep -c 'req=0x80045030 ' "$LOG")
if [ "$n" -ge 3 ] && [ "$n" -le 40 ]; then ok "identical SET_GAIN calls are throttled ($n lines for 42 calls)"; else ko "SET_GAIN lines: $n"; fi
has "changed value after throttling is still logged" 'req=0x80045030 .*in=05000000' "$LOG"
has "successful ioctl (FIONREAD) shows ret=0" 'req=0x541b .*ret=0' "$LOG"
has "partially mapped argument: only the valid bytes are shown, no crash" 'req=0x401c50e0 .*in=[0-9a-f]{2,16} out=same' "$LOG"
has "first writes logged with size and head bytes" 'WRITE fd=[0-9]+ dev=.*pcmC0D0p count=1024 ret=1024 head=[0-9a-f]{64}' "$LOG"
n=$(grep -c ' WRITE fd=[0-9]* dev=.*pcmC0D0p ' "$LOG")
if [ "$n" -le 20 ]; then ok "per-write lines are bounded ($n)"; else ko "too many WRITE lines: $n"; fi
has "audio statistics window" 'AUDIO_STATS fd=[0-9]+ dev=.*pcmC0D0p op=w why=window win_ms=[0-9]+ calls=[0-9]+ bytes=[0-9]+ gap_ms_min=[0-9]+ gap_ms_max=[0-9]+' "$LOG"
has "L==R pairs counted (mono duplicated to stereo)" 'AUDIO_STATS .* pairs=[1-9][0-9]* lr_eq=[1-9][0-9]*' "$LOG"
has "CLOSE with totals" 'CLOSE fd=[0-9]+ dev=.*pcmC0D0p ret=0 wr_calls=[0-9]+ wr_bytes=[0-9]+' "$LOG"
has "capture reads logged" 'READ fd=[0-9]+ dev=.*pcmC0D0c count=512 ret=512' "$LOG"
has "gpio value write shows the text" 'WRITE fd=[0-9]+ dev=.*gpio69/value count=1 ret=1 text="1"' "$LOG"
has "gpio direction write shows the text" 'WRITE fd=[0-9]+ dev=.*gpio69/direction count=3 ret=3 text="out"' "$LOG"
has "mmap of /dev/mem logged" 'MMAP fd=[0-9]+ dev=.*dev/mem len=0x1000 prot=0x1 flags=0x1 off=0x0 ret=0x' "$LOG"
has "audio file open classified" 'OPEN path="[^"]*prompt.mp3" .*cls=audiofile fd=' "$LOG"
has "audio file read bytes counted at close" 'CLOSE fd=[0-9]+ dev=.*prompt.mp3 ret=0 wr_calls=0 wr_bytes=0 rd_calls=1 rd_bytes=100' "$LOG"
has "writev logged" 'WRITEV fd=[0-9]+ dev=.*pcmC0D0p iovcnt=2 ret=512' "$LOG"
has "dup of a tracked fd followed" 'DUP fd=[0-9]+ newfd=[0-9]+ dev=.*pcmC0D0p' "$LOG"
has "ISP block logged with its real size (CCM = 324 bytes), hash and counters" 'ISP_BLOCK name=CCM cmd=0x40044912 flag=1 ret=-1 size=324 hash=0x[0-9a-f]+ calls=1 changes=0 dump=1 stored=bin head=07000000' "$LOG"
has "changed ISP block logged again (3rd call, 2nd distinct content)" 'ISP_BLOCK name=CCM .* calls=3 changes=1 dump=2 stored=bin' "$LOG"
n=$(grep -c 'ISP_BLOCK name=CCM ' "$LOG")
if [ "$n" -eq 2 ]; then ok "an identical block is not logged twice (3 calls, 2 dumps)"; else ko "CCM dumps: $n"; fi
n=$(grep -c 'ISP_BLOCK name=UNKNOWN cmd=0x80044959 ' "$LOG")
if [ "$n" -ge 1 ] && [ "$n" -le 3 ]; then ok "a polled block that changes at every call is rate limited (300 calls, $n dump lines)"; else ko "0x80044959 dumps: $n"; fi
has "size of an unknown command: 256-byte probe" 'ISP_BLOCK name=UNKNOWN cmd=0x80044959 .* size=256 ' "$LOG"
has "big block (SHARP) logged with its full size" 'ISP_BLOCK name=SHARP cmd=0x40044925 .* size=46584 ' "$LOG"
has "partly mapped payload: only the mapped bytes" 'ISP_BLOCK name=UNKNOWN cmd=0x80044999 .* size=100 ' "$LOG"
hasnt "payload in unmapped memory: nothing logged" 'cmd=0x80044998' "$LOG"
hasnt "VI scalar tunnel call ignored" 'cmd=0x00000103|cmd=0x103 ' "$LOG"
has "ALIVE carries the ISP and sysfs counters" 'ALIVE events=[0-9]+ dropped=[0-9]+ isp_calls=[0-9]+ isp_dumps=[0-9]+ isp_skipped=[0-9]+ gpio_skipped=[0-9]+ bin_bytes=[0-9]+ log_bytes=[0-9]+' "$LOG"
has "ISP_STAT counters per command" 'ISP_STAT name=CCM cmd=0x40044912 size=324 calls=3 changes=1 dumps=2' "$LOG"
# sysfs light sensor and IR LED: bounded
n=$(grep -c 'READ .*in_voltage0_raw' "$LOG")
if [ "$n" -ge 1 ] && [ "$n" -le 8 ]; then ok "60 ADC reads of the light sensor: $n lines"; else ko "ADC READ lines: $n"; fi
has "light sensor value shown" 'READ fd=[0-9]+ dev=.*in_voltage0_raw count=16 ret=5 text="1234\\x0a"' "$LOG"
n=$(grep -c 'WRITE .*pwm0/duty_cycle' "$LOG")
if [ "$n" -ge 8 ] && [ "$n" -le 12 ]; then ok "60 identical PWM writes: $n lines"; else ko "PWM WRITE lines: $n"; fi
has "the PWM write that changes is logged" 'WRITE fd=[0-9]+ dev=.*pwm0/duty_cycle count=1 ret=1 text="0"' "$LOG"
n=$(grep -c 'OPEN path=".*pwm0/duty_cycle' "$LOG")
if [ "$n" -le 10 ]; then ok "OPEN of a polled sysfs file is bounded ($n)"; else ko "PWM OPEN lines: $n"; fi
# the binary dump
BIN=$WORK/card/run1/isp_blocks.bin
if [ -s "$BIN" ] && command -v python3 >/dev/null 2>&1; then
    python3 -I - "$BIN" >"$WORK/bin.out" 2>&1 <<'PYEOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
off, recs = 0, []
while off + 24 <= len(d):
    m, t, cmd, n, h, tid = struct.unpack_from("<4sIIIII", d, off)
    if m != b"ISPB":
        print("BAD MAGIC at", off); break
    recs.append((cmd, t, bytes(d[off + 24: off + 24 + n]), h))
    off += 24 + n
print("RECORDS", len(recs), "TRAILING", len(d) - off)
ccm = [r for r in recs if r[0] == 0x40044912]
print("CCM", len(ccm), [len(r[2]) for r in ccm])
if len(ccm) == 2:
    a, b = ccm[0][2], ccm[1][2]
    print("CCM_FIRST_WORD", struct.unpack_from("<I", a, 0)[0], "CCM_LAST_OLD", hex(struct.unpack_from("<I", a, 320)[0]),
          "CCM_LAST_NEW", hex(struct.unpack_from("<I", b, 320)[0]))
sh = [r for r in recs if r[0] == 0x40044925]
print("SHARP", len(sh), [len(r[2]) for r in sh])
if sh:
    ok = all(sh[0][2][i] == (i * 7 + 3) & 255 for i in range(46584))
    print("SHARP_CONTENT", "exact" if ok else "WRONG")
print("POLL", len([r for r in recs if r[0] == 0x80044959]))
part = [r for r in recs if r[0] == 0x80044999]
print("PARTIAL", [len(r[2]) for r in part], all(x == 0x5a for r in part for x in r[2]))
print("UNMAPPED", len([r for r in recs if r[0] == 0x80044998]))
PYEOF
    has "isp_blocks.bin: well-formed records, nothing left over" '^RECORDS [0-9]+ TRAILING 0$' "$WORK/bin.out"
    has "isp_blocks.bin: two CCM dumps of 324 bytes" '^CCM 2 \[324, 324\]$' "$WORK/bin.out"
    has "isp_blocks.bin: the LAST word of the full block is captured and differs between the dumps" 'CCM_LAST_OLD 0x[0-9a-f]+ CCM_LAST_NEW 0xdeadbeef' "$WORK/bin.out"
    has "isp_blocks.bin: the 46584-byte SHARP block is stored whole and exact" '^SHARP_CONTENT exact$' "$WORK/bin.out"
    has "isp_blocks.bin: partly mapped payload stored with its mapped bytes only" '^PARTIAL \[100\] True$' "$WORK/bin.out"
    has "isp_blocks.bin: nothing for the unmapped payload" '^UNMAPPED 0$' "$WORK/bin.out"
    np=$(sed -n 's/^POLL //p' "$WORK/bin.out")
    if [ "${np:-99}" -le 3 ]; then ok "isp_blocks.bin: polled block rate limited ($np records)"; else ko "polled block records: $np"; fi
else
    ko "isp_blocks.bin missing or python3 absent"
fi
hasnt "unrelated files are not logged" 'tmp_plain' "$LOG"
if [ -s "$WORK/card/run1/stock_audio.raw" ] && [ "$(stat -c %s "$WORK/card/run1/stock_audio.raw")" -le 65536 ]; then ok "first playback bytes dumped to stock_audio.raw (<= 64 KiB)"; else ko "stock_audio.raw missing or too big"; fi
has "legacy decoders still run (isp_hook.log, CCM with its name, time stamped)" '^T=[0-9]+\.[0-9]{3} CCM cmd=0x40044912 payload\[0\.\.3\]=' "$WORK/card/run1/isp_hook.log"
n=$(grep -c 'cmd=0x80044959' "$WORK/card/run1/isp_hook.log")
if [ "$n" -le 3 ]; then ok "isp_hook.log: a polled unknown command is not repeated for every call ($n of 300)"; else ko "isp_hook.log repeats the polled command: $n lines"; fi
# our own descriptors stay far above the ones the app sees
hasnt "no stray descriptors: the driver sees the same fd numbers (fd<20) for its own files" 'fd=9[0-9][0-9]' "$LOG"

# --- run B: card missing -> RAM fallback, no crash ---------------------------------------------
LD_PRELOAD="$WORK/hook_b.so" "$WORK/hook_driver" "$R" >"$WORK/b.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "card missing: the driver still passes every self-check"; else ko "card missing: driver exit $rc"; cat "$WORK/b.out"; fi
mkdir -p "$WORK/b/tmp"
rm -f "$WORK/b/tmp/stock_hook.log"
LD_PRELOAD="$WORK/hook_b.so" "$WORK/hook_driver" "$R" >"$WORK/b2.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "fallback directory present: driver passes"; else ko "fallback run exit $rc"; fi
if [ -s "$WORK/b/tmp/stock_hook.log" ]; then ok "without the card the log falls back to the RAM directory"; else ko "no fallback log"; fi
if [ ! -e "$WORK/b/nosuch" ]; then ok "a missing fallback folder is never created"; else ko "the hook created a fallback folder"; fi
bsz=$(stat -c %s "$WORK/b/tmp/stock_hook.log")
if [ "$bsz" -le 1048576 ]; then ok "RAM fallback log is under its 1 MiB cap ($bsz bytes)"; else ko "RAM fallback too big: $bsz"; fi

# --- run C: size caps ------------------------------------------------------------------------------
mkdir -p "$WORK/card_c"
ln -s "$WORK/card_c" "$WORK/c/out"
LD_PRELOAD="$WORK/hook_c.so" "$WORK/hook_driver" "$R" >"$WORK/c.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "tiny caps: the driver still passes"; else ko "capped run exit $rc"; cat "$WORK/c.out"; fi
has "cap notice written" 'LOG_CAP reached' "$WORK/card_c/stock_hook.log"
sz=$(stat -c %s "$WORK/card_c/stock_hook.log")
if [ "$sz" -le 4600 ]; then ok "stock_hook.log stays near its cap ($sz bytes for a 3000 cap)"; else ko "cap not enforced: $sz bytes"; fi
bsz2=$(stat -c %s "$WORK/card_c/isp_blocks.bin" 2>/dev/null || echo 0)
if [ "$bsz2" -le 1000 ]; then ok "isp_blocks.bin stays under its cap ($bsz2 bytes for a 1000 cap)"; else ko "bin cap not enforced: $bsz2 bytes"; fi
isz=$(stat -c %s "$WORK/card_c/isp_hook.log" 2>/dev/null || echo 0)
if [ "$isz" -le 1400 ]; then ok "isp_hook.log stays near its cap ($isz bytes for a 600 cap)"; else ko "isp cap not enforced: $isz bytes"; fi

# --- run E: several threads on the same descriptors (no crash, no hang, results unchanged) -----------
mkdir -p "$WORK/card_e"
rm -f "$WORK/a/out"; ln -s "$WORK/card_e" "$WORK/a/out"
timeout 120 env LD_PRELOAD="$WORK/hook_a.so" "$WORK/hook_driver" "$R" threads >"$WORK/e.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "6 threads hammering the hook: results and errno unchanged, no hang"; else ko "thread run exit $rc"; cat "$WORK/e.out"; fi
has "thread run reached its end" 'THREADS DONE bad=0' "$WORK/e.out"
has "thread run logged events" 'IOCTL fd=' "$WORK/card_e/stock_hook.log"
if awk '!/^(T=[0-9]+\.[0-9]{3} tid=[0-9]+ [A-Z_]+( |$)|LOG_CAP)/ { bad++ } END { exit bad ? 1 : 0 }' "$WORK/card_e/stock_hook.log"; then ok "no log line of any thread is torn or mixed"; else ko "malformed lines in the multi-thread log"; fi

# --- run D: another process name: LD_PRELOAD inherited by a child must stay silent ------------------
cp "$WORK/hook_driver" "$WORK/other_prog"
mkdir -p "$WORK/card_d"
rm -f "$WORK/a/out"; ln -s "$WORK/card_d" "$WORK/a/out"
LD_PRELOAD="$WORK/hook_a.so" "$WORK/other_prog" "$R" >"$WORK/d.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "other process: self-checks pass"; else ko "other process exit $rc"; fi
if [ "$(ls "$WORK/card_d")" = "stock_hook.log" ] && [ "$(wc -l <"$WORK/card_d/stock_hook.log")" -eq 1 ]; then ok "a process that is not anyka_ipc writes one CHILD line and nothing else"; else ko "unexpected files for a foreign process: $(ls "$WORK/card_d")"; fi
has "CHILD line: parent, executable, command line, absolute clock" 'CHILD abs_ms=[0-9]+ ppid=[0-9]+ exe="[^"]*" cmd="[^"]*other_prog [^"]*"' "$WORK/card_d/stock_hook.log"

# --- run F: anyka_ipc starts a child with system(): the child's command line is logged -----------------
mkdir -p "$WORK/card_f"
rm -f "$WORK/a/out"; ln -s "$WORK/card_f" "$WORK/a/out"
HOOK_DRIVER_CHILD=1 LD_PRELOAD="$WORK/hook_a.so" "$WORK/hook_driver" "$R" >"$WORK/f.out" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then ok "system() of a child under the inherited hook: the driver passes"; else ko "child run exit $rc"; cat "$WORK/f.out"; fi
has "child of anyka_ipc logged with its command line" 'CHILD abs_ms=[0-9]+ ppid=[0-9]+ exe="[^"]*" cmd="[^"]*reboot-marker' "$WORK/card_f/stock_hook.log"
has "system() of anyka_ipc itself is logged (the camera's sh is a static BusyBox, no inherited hook)" 'CHILD abs_ms=[0-9]+ via=system exe="/bin/sh" cmd="exit 3"' "$WORK/card_f/stock_hook.log"
has "popen() of anyka_ipc itself is logged" 'CHILD abs_ms=[0-9]+ via=popen exe="/bin/sh" cmd="echo popen-marker-out"' "$WORK/card_f/stock_hook.log"
has "system(NULL) is logged without crashing" 'via=system exe="/bin/sh" cmd="\(null\)"' "$WORK/card_f/stock_hook.log"

# --- parser smoke on the real hook output (when the parser exists) -------------------------------------
if [ -f "$PARSER" ] && command -v python3 >/dev/null 2>&1; then
    if python3 -I "$PARSER" "$WORK/card/run1" >"$WORK/parse.out" 2>&1; then
        ok "parser reads the hook's own log"
    else
        ko "parser failed on the hook log"; head -n 30 "$WORK/parse.out"
    fi
    has "parser names the speaker ioctls" 'SET_SPEAKER' "$WORK/parse.out"
fi

# --- ARM build: warning free, only __errno_location undefined ------------------------------------------
if command -v zig >/dev/null 2>&1 && command -v readelf >/dev/null 2>&1; then
    if (cd "$SRC" && make -s stock_hook.so >"$WORK/arm.log" 2>&1); then
        ok "ARM stock_hook.so builds"
        if [ -s "$WORK/arm.log" ]; then ko "ARM build printed warnings"; head -n 20 "$WORK/arm.log"; else ok "ARM build prints no warning"; fi
        und=$(readelf --dyn-syms -W "$SRC/stock_hook.so" | awk '$7=="UND" && $8!="" {print $8}' | grep -v -E '^(__errno_location|dlsym)$' || true)
        if [ -z "$und" ]; then ok "ARM hook: only the weak __errno_location and dlsym are undefined (no libc/compiler-rt dependency)"; else ko "unexpected undefined symbols: $und"; fi
        exp=$(readelf --dyn-syms -W "$SRC/stock_hook.so" | awk '$7!="UND" && $5=="GLOBAL" {print $8}' | sort | tr '\n' ' ')
        if [ "$exp" = "close dup dup2 ioctl mmap open popen read system write writev " ]; then ok "ARM hook exports only the hooked libc functions"; else ko "unexpected exports: $exp"; fi
        rm -f "$SRC/stock_hook.so"
    else
        ko "ARM stock_hook.so does not build"; head -n 30 "$WORK/arm.log"
    fi
else
    echo "skip-part: zig/readelf not available, ARM build not checked"
fi

echo
echo "stock_hook_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
