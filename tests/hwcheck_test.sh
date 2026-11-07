#!/usr/bin/env bash
# Host-side tests for tools/hw_tests/camyn_hwcheck.sh. The script runs under
# $SH_UNDER_TEST (default: "$CAMYN_BUSYBOX sh", else "busybox sh", else "sh")
# against a fake root ($CAMYN_ROOT with proc/, etc/, mnt/, tmp/) and a
# restricted PATH made of symlinks to host tools plus stubs. Nothing here
# touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/tools/hw_tests/camyn_hwcheck.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${CAMYN_BUSYBOX:-}" ]; then SH_UNDER_TEST="$CAMYN_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }

# check_has NAME PATTERN FILE: extended regex must match the output.
has() { if grep -Eq -- "$2" "$3"; then ok "$1"; else ko "$1 (no match: $2)"; sed 's/^/      | /' "$3" | head -n 40; fi; }
hasnt() { if grep -Eq -- "$2" "$3"; then ko "$1 (unexpected: $2)"; else ok "$1"; fi; }

# --- base PATH: symlinks to the host tools the script may use -------------
BASEBIN=$WORK/basebin
mkdir -p "$BASEBIN"
for t in cat head tail sed tr cut grep wc ls rm cp mktemp uname sh; do
    p=$(command -v "$t") && ln -s "$p" "$BASEBIN/$t"
done

# --- fake root --------------------------------------------------------------
# mkroot DIR [tcp-lines...]: a healthy camera, listening on 22/554/8091.
tcp_line() { printf '   0: %s:%04X 00000000:0000 %s 00000000:00000000 00:00000000 00000000     0        0 1 1 0\n' "$1" "$2" "${3:-0A}"; }
mkroot() {
    r=$1
    mkdir -p "$r"/proc/net "$r"/proc/sys/kernel/random "$r"/etc/config/dropbear "$r"/mnt "$r"/tmp
    mkdir -p "$r"/proc/self
    : > "$r/proc/self/exe"
    {
        echo "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode"
        tcp_line 00000000 22
        tcp_line 00000000 554
        tcp_line 0100007F 8091
        tcp_line 0100A8C0 5555 01
    } > "$r/proc/net/tcp"
    mkproc "$r" 101 ak_rtsp 4321
    mkproc "$r" 102 dropbear 1234
    mkproc "$r" 103 "sh /usr/sbin/camyn_lockdown.sh" 600
    mkproc "$r" 104 wpa_supplicant 800
    mkproc "$r" 105 udhcpc 300
    echo 300 > "$r/proc/sys/kernel/random/entropy_avail"
    echo "root:x:0:0:root:/root:/bin/sh" > "$r/etc/passwd"
    printf '/bin/sh\n/bin/ash\n' > "$r/etc/shells"
    chmod 755 "$r" "$r/etc" "$r/etc/config"
    chmod 700 "$r/etc/config/dropbear"
    echo "ssh-ed25519 AAAAFAKE test" > "$r/etc/config/dropbear/authorized_keys"
    chmod 600 "$r/etc/config/dropbear/authorized_keys"
    echo "mtd7 /usr squashfs ro 0 0" > "$r/proc/mounts"
    printf 'Processor\t: ARM926EJ-S rev 5 (v5l)\nHardware\t: AK3918\n' > "$r/proc/cpuinfo"
    printf 'MemTotal: 60000 kB\nMemFree: 12000 kB\nCached: 9000 kB\n' > "$r/proc/meminfo"
    printf 'dev: size erasesize name\nmtd7: 00500000 00010000 "APP"\n' > "$r/proc/mtd"
    echo "step one ok" > "$r/tmp/camyn_ssh.log"
    echo "lockdown round" > "$r/tmp/camyn_lockdown.log"
}
# mkproc ROOT PID CMDLINE-with-spaces VMRSS
mkproc() {
    mkdir -p "$1/proc/$2"
    printf '%s' "$3" | tr ' ' '\0' > "$1/proc/$2/cmdline"
    printf 'Name:\t%s\nVmRSS:\t%s kB\n' "$3" "$4" > "$1/proc/$2/status"
}

# --- stubs ------------------------------------------------------------------
stub() { # stub DIR NAME BODY
    mkdir -p "$1"
    printf '#!/bin/sh\n%s\n' "$3" > "$1/$2"
    chmod +x "$1/$2"
}

# run NAME ROOT STUBDIR [args...]: output in $WORK/NAME.out, status in RC.
run() {
    name=$1; root=$2; sdir=$3; shift 3
    mkdir -p "$sdir"
    [ -e "$sdir/sleep" ] || stub "$sdir" sleep 'exit 0'
    env -i PATH="$sdir:$BASEBIN" CAMYN_ROOT="$root" HWCHECK_INTERVAL=0 TMPDIR="$WORK" \
        "${SHCMD[@]}" "$SCRIPT" "$@" > "$WORK/$name.out" 2>&1
    RC=$?
}

# 1. healthy camera, netstat available -----------------------------------
R1=$WORK/r1; mkroot "$R1"
S1=$WORK/s1
stub "$S1" netstat 'echo "Active Internet connections (only servers)"
echo "Proto Recv-Q Send-Q Local Address           Foreign Address         State"
echo "tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN"
echo "tcp        0      0 0.0.0.0:554             0.0.0.0:*               LISTEN"
echo "tcp        0      0 192.168.1.9:8091        0.0.0.0:*               LISTEN"'
stub "$S1" nc 'cat >/dev/null; echo "PARAM_A 1"; echo "PARAM_B 2"'
stub "$S1" free 'echo "free stub"'
run t1 "$R1" "$S1"
has "netstat: source reported" 'source: netstat' "$WORK/t1.out"
has "netstat: port 22 pass" '^PASS +port 22 listening' "$WORK/t1.out"
has "netstat: 8091 address info" '8091 listen address: 192.168.1.9' "$WORK/t1.out"
has "netstat: port 23 closed" '^PASS +port 23 closed' "$WORK/t1.out"
has "processes: ak_rtsp running" '^PASS +ak_rtsp running \(pid 101\)' "$WORK/t1.out"
has "processes: lockdown matched via argv1" '^PASS +camyn_lockdown running \(pid 103\)' "$WORK/t1.out"
has "processes: telnetd absent" '^PASS +telnetd absent' "$WORK/t1.out"
has "rss: ak_rtsp" 'ak_rtsp pid 101 VmRSS 4321 kB' "$WORK/t1.out"
has "rss: dropbear" 'dropbear pid 102 VmRSS 1234 kB' "$WORK/t1.out"
has "8091 LIST answered" '^PASS +8091 answered LIST' "$WORK/t1.out"
has "maintenance flag absent" '^PASS +maintenance flag absent' "$WORK/t1.out"
has "authorized_keys counted, not shown" 'holds 1 key line' "$WORK/t1.out"
hasnt "no key material printed" 'AAAAFAKE' "$WORK/t1.out"
has "root shell in /etc/shells" '^PASS +root shell is listed' "$WORK/t1.out"
has "entropy pass" '^PASS +entropy_avail=300' "$WORK/t1.out"
has "/proc/self/exe pass" '^PASS +/proc/self/exe' "$WORK/t1.out"
has "camyn log tail shown" 'step one ok' "$WORK/t1.out"
has "cpuinfo fact" 'ARM926EJ-S' "$WORK/t1.out"
has "mtd fact" 'mtd7: 00500000' "$WORK/t1.out"
if [ "$RC" -eq 0 ]; then ok "healthy: exit 0"; else ko "healthy: exit $RC"; grep -E '^FAIL' "$WORK/t1.out"; fi

# 2. no ss/netstat -> /proc/net/tcp hex parsing; no nc; no stat ---------------
R2=$WORK/r2; mkroot "$R2"
tcp_line 0100007F 23 >> "$R2/proc/net/tcp"      # telnet on loopback
tcp_line 00000000 80 >> "$R2/proc/net/tcp"      # HTTP
run t2 "$R2" "$WORK/s2"
has "proc: source /proc/net/tcp" 'source: /proc/net/tcp' "$WORK/t2.out"
has "proc: 8091 on 127.0.0.1" '8091 listen address: 127\.0\.0\.1' "$WORK/t2.out"
has "proc: port 22 on 0.0.0.0" '^PASS +port 22 listening \(0\.0\.0\.0\)' "$WORK/t2.out"
has "proc: ESTABLISHED socket ignored" '^PASS +port 24 closed' "$WORK/t2.out"
hasnt "proc: state 01 socket not listed" '^ +192\.168\.0\.1 5555' "$WORK/t2.out"
has "proc: telnet 23 is FAIL" '^FAIL +port 23 listening' "$WORK/t2.out"
has "proc: port 80 is WARN" '^WARN +port 80 listening' "$WORK/t2.out"
has "missing nc: UNKNOWN" '^UNKNOWN +nc not available' "$WORK/t2.out"
has "missing applet reported" '^WARN +applet nc: MISSING' "$WORK/t2.out"
has "missing stat reported" '^WARN +applet stat: MISSING' "$WORK/t2.out"
if [ "$RC" -eq 1 ]; then ok "forbidden port: exit 1"; else ko "forbidden port: exit $RC"; fi

# 3. ss preferred over netstat; ss output with IPv6 and header ------------------
R3=$WORK/r3; mkroot "$R3"
S3=$WORK/s3
stub "$S3" ss 'echo "State  Recv-Q Send-Q Local Address:Port Peer Address:Port"
echo "LISTEN 0      128    0.0.0.0:22   0.0.0.0:*"
echo "LISTEN 0      128    [::]:554   [::]:*"
echo "LISTEN 0      128    127.0.0.1:8091   0.0.0.0:*"'
stub "$S3" netstat 'echo netstat should not be used; exit 1'
run t3 "$R3" "$S3"
has "ss: source" 'source: ss' "$WORK/t3.out"
has "ss: ipv6 554" '^PASS +port 554 listening \(\[::\]\)' "$WORK/t3.out"

# 4. broken netstat (empty output) falls back to /proc/net/tcp -----------------
S4=$WORK/s4
stub "$S4" netstat 'exit 1'
run t4 "$R1" "$S4"
has "empty netstat: fallback" 'source: /proc/net/tcp' "$WORK/t4.out"

# 5. failures: no dropbear, telnetd running, maintenance flag, bad modes ---------
R5=$WORK/r5; mkroot "$R5"
rm -rf "$R5/proc/102" "$R5/proc/103"
mkproc "$R5" 110 "telnetd -p 24 -l /bin/sh" 200
chmod 777 "$R5/etc/config"
echo "root:x:0:0:root:/root:/bin/zsh" > "$R5/etc/passwd"
run t5 "$R5" "$WORK/s5"
has "dropbear missing is FAIL" '^FAIL +dropbear not running' "$WORK/t5.out"
has "lockdown missing is FAIL" '^FAIL +camyn_lockdown not running' "$WORK/t5.out"
has "telnetd running is FAIL" '^FAIL +telnetd running \(pid 110\)' "$WORK/t5.out"
has "writable /etc/config is FAIL" '^FAIL +/etc/config mode (777|drwxrwxrwx)' "$WORK/t5.out"
has "shell not in /etc/shells is FAIL" '^FAIL +root shell /bin/zsh' "$WORK/t5.out"
R6=$WORK/r6; mkroot "$R6"
rm -rf "$R6/proc/103"
mkproc "$R6" 110 "telnetd -p 24 -l /bin/sh" 200
: > "$R6/mnt/CAMYN_MAINTENANCE"
run t6 "$R6" "$WORK/s6"
has "maintenance: flag WARN" '^WARN +maintenance flag present' "$WORK/t6.out"
has "maintenance: telnetd only WARN" '^WARN +telnetd running' "$WORK/t6.out"
has "maintenance: lockdown absent WARN" '^WARN +camyn_lockdown not running' "$WORK/t6.out"

# 7. read-only: the fake root is unchanged and nothing is killed -----------------
(cd "$R1" && find . -type f -exec cksum {} + | sort) > "$WORK/before.sum"
run t7 "$R1" "$S1"
(cd "$R1" && find . -type f -exec cksum {} + | sort) > "$WORK/after.sum"
if cmp -s "$WORK/before.sum" "$WORK/after.sum"; then ok "fake root unchanged by a run"; else ko "fake root modified"; fi

# 8. --boot-window: telnet appears at sample 2 and goes away at sample 4 -----------
R8=$WORK/r8; mkroot "$R8"
S8=$WORK/s8
mkdir -p "$S8"
cat > "$S8/sleep" <<EOF
#!/bin/sh
# Each "second" advances a scripted state: uptime and the socket table.
n=\$(cat "$WORK/tick" 2>/dev/null || echo 0)
n=\$((n + 1)); echo \$n > "$WORK/tick"
echo "\$((10 + n)).00 0.00" > "$R8/proc/uptime"
cp "$R8/proc/net/tcp.base" "$R8/proc/net/tcp"
case \$n in 2|3) printf '   1: 00000000:0017 00000000:0000 0A 0\n' >> "$R8/proc/net/tcp" ;; esac
EOF
chmod +x "$S8/sleep"
cp "$R8/proc/net/tcp" "$R8/proc/net/tcp.base"
echo "10.00 0.00" > "$R8/proc/uptime"
rm -f "$WORK/tick"
run t8 "$R8" "$S8" --boot-window 5
if [ "$RC" -eq 0 ]; then ok "boot-window: exit 0"; else ko "boot-window: exit $RC"; fi
has "boot-window: first sample" '^t=\+0s u=10\.00 listening: 22 554 8091' "$WORK/t8.out"
has "boot-window: telnet visible" '^t=\+2s u=12\.00 listening: 22 554 8091 23' "$WORK/t8.out"
has "boot-window: telnet gone" '^t=\+4s u=14\.00 listening: 22 554 8091$' "$WORK/t8.out"
has "boot-window: summary port 23" 'port 23: first u=12\.00 last u=13\.00' "$WORK/t8.out"
has "boot-window: summary never seen" 'port 21: never seen' "$WORK/t8.out"

# 9. argument handling ----------------------------------------------------------
run t9 "$R1" "$S1" --bogus
if [ "$RC" -eq 2 ]; then ok "unknown option: exit 2"; else ko "unknown option: exit $RC"; fi
run t10 "$R1" "$S1" --boot-window abc
if [ "$RC" -eq 2 ]; then ok "bad duration: exit 2"; else ko "bad duration: exit $RC"; fi
run t11 "$R1" "$S1" --help
has "help prints usage" 'Usage:' "$WORK/t11.out"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
