#!/usr/bin/env bash
# shellcheck disable=SC2012,SC2016  # ls for modes and inodes, single-quoted commands for other shells
# Host-side tests for tools/hw_tests/akam_hwcheck.sh. The script runs under
# $SH_UNDER_TEST (default: "$AKAM_BUSYBOX sh", else "busybox sh", else "sh")
# against a fake root ($AKAM_ROOT with proc/, etc/, mnt/, tmp/) and a
# restricted PATH made of symlinks to host tools plus stubs. Nothing here
# touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/tools/hw_tests/akam_hwcheck.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
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
for t in cat head tail sed strings paste cut grep wc ls rm cp mkdir uname sh; do
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
    mkproc "$r" 103 "sh /usr/sbin/akam_lockdown.sh" 600
    mkproc "$r" 104 wpa_supplicant 800
    mkproc "$r" 105 udhcpc 300
    mkproc "$r" 106 "akam_klog" 200
    echo 300 > "$r/proc/sys/kernel/random/entropy_avail"
    mkstat "$r" 101 ak_rtsp 100 50
    echo "cpu  1000 0 500 8000 0 0 0 0 0 0" > "$r/proc/stat"
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
    echo "step one ok" > "$r/tmp/akam_ssh.log"
    echo "lockdown round" > "$r/tmp/akam_lockdown.log"
}
# mkproc ROOT PID CMDLINE-with-spaces VMRSS
mkproc() {
    mkdir -p "$1/proc/$2"
    printf '%s' "$3" | tr ' ' '\0' > "$1/proc/$2/cmdline"
    printf 'Name:\t%s\nVmRSS:\t%s kB\n' "$3" "$4" > "$1/proc/$2/status"
}

# mkstat ROOT PID COMM UTIME STIME: /proc/PID/stat with utime/stime at fields 14/15.
mkstat() {
    printf '%s (%s) S 1 1 1 0 -1 0 0 0 0 0 %s %s 0 0 20 0 5 0 1 2 3\n' "$2" "$3" "$4" "$5" > "$1/proc/$2/stat"
}

# --- stubs ------------------------------------------------------------------
stub() { # stub DIR NAME BODY
    mkdir -p "$1"
    printf '#!/bin/sh\n%s\n' "$3" > "$1/$2"
    chmod +x "$1/$2"
}

EXTRA_ENV=(HWCHECK_X=1)
# run NAME ROOT STUBDIR [args...]: output in $WORK/NAME.out, status in RC.
run() {
    name=$1; root=$2; sdir=$3; shift 3
    mkdir -p "$sdir"
    [ -e "$sdir/sleep" ] || stub "$sdir" sleep 'exit 0'
    env -i PATH="$sdir:$BASEBIN" AKAM_ROOT="$root" HWCHECK_INTERVAL=0 HWCHECK_CPU_WINDOW=0 HWCHECK_NIGHT_INTERVAL=0 "${EXTRA_ENV[@]}" TMPDIR="$WORK" \
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
echo "tcp        0      0 192.0.2.9:8091        0.0.0.0:*               LISTEN"'
stub "$S1" nc 'cat >/dev/null; echo "PARAM_A 1"; echo "PARAM_B 2"'
stub "$S1" free 'echo "free stub"'
run t1 "$R1" "$S1"
has "netstat: source reported" 'source: netstat' "$WORK/t1.out"
has "netstat: port 22 pass" '^PASS +port 22 listening' "$WORK/t1.out"
has "netstat: 8091 address info" '8091 listen address: 192.0.2.9' "$WORK/t1.out"
has "netstat: port 23 closed" '^PASS +port 23 closed' "$WORK/t1.out"
has "processes: ak_rtsp running" '^PASS +ak_rtsp running \(pid 101\)' "$WORK/t1.out"
has "processes: lockdown matched via argv1" '^PASS +akam_lockdown running \(pid 103\)' "$WORK/t1.out"
has "processes: telnetd absent" '^PASS +telnetd absent' "$WORK/t1.out"
has "rss: ak_rtsp" 'ak_rtsp pid 101 VmRSS 4321 kB' "$WORK/t1.out"
has "rss: dropbear" 'dropbear pid 102 VmRSS 1234 kB' "$WORK/t1.out"
has "8091 LIST answered" '^PASS +8091 answered LIST' "$WORK/t1.out"
# hostname section: script absent, then present with a stored name
has "hostname: helper absent reported" 'akam_hostname.sh not installed' "$WORK/t1.out"
mkdir -p "$R1/usr/sbin" "$R1/sys/class/net/wlan0" "$R1/proc/sys/kernel"
install -m 755 "$REPO/tools/firmware_patch_templates/akam_hostname.sh" "$R1/usr/sbin/akam_hostname.sh"
echo "aa:bb:cc:dd:12:34" > "$R1/sys/class/net/wlan0/address"
echo "akam-1234" > "$R1/proc/sys/kernel/hostname"
run t1h "$R1" "$S1"
has "hostname: default effective, nothing configured" 'hostname: effective akam-1234, configured none .*kernel akam-1234' "$WORK/t1h.out"
mkdir -p "$R1/etc/config/akam"
echo "cam-salon" > "$R1/etc/config/akam/hostname"
run t1h2 "$R1" "$S1"
has "hostname: configured name reported" 'effective cam-salon, configured cam-salon, kernel akam-1234' "$WORK/t1h2.out"
has "hostname: kernel/effective mismatch warns" '^WARN +hostname: kernel name .akam-1234. differs' "$WORK/t1h2.out"
rm -f "$R1/etc/config/akam/hostname" "$R1/usr/sbin/akam_hostname.sh"
has "maintenance flag absent" '^PASS +maintenance mode off' "$WORK/t1.out"
has "authorized_keys counted, not shown" 'holds 1 key line' "$WORK/t1.out"
hasnt "no key material printed" 'AAAAFAKE' "$WORK/t1.out"
has "root shell in /etc/shells" '^PASS +root shell is listed' "$WORK/t1.out"
has "entropy pass" '^PASS +entropy_avail=300' "$WORK/t1.out"
has "/proc/self/exe pass" '^PASS +/proc/self/exe' "$WORK/t1.out"
has "akam log tail shown" 'step one ok' "$WORK/t1.out"
has "cpuinfo fact" 'ARM926EJ-S' "$WORK/t1.out"
has "mtd fact" 'mtd7: 00500000' "$WORK/t1.out"
if [ "$RC" -eq 0 ]; then ok "healthy: exit 0"; else ko "healthy: exit $RC"; grep -E '^FAIL' "$WORK/t1.out"; fi

# 1b. RTSP authentication line (info only, never a FAIL, never the hash) ---
has "rtsp auth: disabled by default" '^INFO +RTSP auth: disabled' "$WORK/t1.out"
has "talk: not offered without the password" '^INFO +talk: not offered' "$WORK/t1.out"
R1B=$WORK/r1b; mkroot "$R1B"
mkdir -p "$R1B/etc/config/akam"
printf 'viewer:akam:0123456789abcdef0123456789abcdef\n' > "$R1B/etc/config/akam/rtsp_auth"
chmod 600 "$R1B/etc/config/akam/rtsp_auth"
run t1b "$R1B" "$S1"
has "rtsp auth: enabled, user shown" '^INFO +RTSP auth: enabled \(user viewer' "$WORK/t1b.out"
has "talk: offered with the password" '^INFO +talk: offered' "$WORK/t1b.out"
hasnt "rtsp auth: hash never printed" '0123456789abcdef0123456789abcdef' "$WORK/t1b.out"
hasnt "rtsp auth: enabled is not a failure" '^FAIL.*RTSP' "$WORK/t1b.out"
chmod 644 "$R1B/etc/config/akam/rtsp_auth"
run t1c "$R1B" "$S1"
has "rtsp auth: world-readable file warned" '^WARN +RTSP auth:.*readable by group/others' "$WORK/t1c.out"
printf 'garbage\n' > "$R1B/etc/config/akam/rtsp_auth"
run t1d "$R1B" "$S1"
has "rtsp auth: invalid file warned" '^WARN +RTSP auth: INVALID' "$WORK/t1d.out"
if [ "$RC" -eq 0 ]; then ok "rtsp auth: an invalid file does not fail the check"; else ko "rtsp auth: invalid file made hwcheck fail (rc $RC)"; fi

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
has "proc: port 80 is FAIL while ONVIF is off" '^FAIL +port 80 listening.*ONVIF is off' "$WORK/t2.out"
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
has "lockdown missing is FAIL" '^FAIL +akam_lockdown not running' "$WORK/t5.out"
has "telnetd running is FAIL" '^FAIL +telnetd running \(pid 110\)' "$WORK/t5.out"
has "writable /etc/config is FAIL" '^FAIL +/etc/config mode (777|drwxrwxrwx)' "$WORK/t5.out"
has "shell not in /etc/shells is FAIL" '^FAIL +root shell /bin/zsh' "$WORK/t5.out"
R6=$WORK/r6; mkroot "$R6"
rm -rf "$R6/proc/103"
mkproc "$R6" 110 "telnetd -p 24 -l /bin/sh" 200
: > "$R6/mnt/AKAM_MAINTENANCE"
run t6 "$R6" "$WORK/s6"
has "maintenance: flag WARN" '^WARN +maintenance mode \(.*present\)' "$WORK/t6.out"
has "maintenance: telnetd only WARN" '^WARN +telnetd running' "$WORK/t6.out"
has "maintenance: lockdown absent WARN" '^WARN +akam_lockdown not running' "$WORK/t6.out"

# 6b. deprecated CAMYN_MAINTENANCE flag name (one release): maintenance + rename WARN
R6B=$WORK/r6b; mkroot "$R6B"
: > "$R6B/mnt/CAMYN_MAINTENANCE"
run t6b "$R6B" "$WORK/s6b"
has "legacy flag: maintenance WARN" '^WARN +maintenance mode \(.*present\)' "$WORK/t6b.out"
has "legacy flag: deprecation WARN" '^WARN +DEPRECATED flag name CAMYN_MAINTENANCE: rename it to AKAM_MAINTENANCE' "$WORK/t6b.out"

# 6c. vanishing PIDs: the files are "readable" ([ -r ] true) but opening them
# fails (a unix socket gives ENXIO), as when a process exits between the test
# and the open -> no shell error, no bogus match ------------------------------
R6C=$WORK/r6c; mkroot "$R6C"
mkdir -p "$R6C/proc/120" "$R6C/proc/121"
rm -f "$R6C/proc/101/status" "$R6C/proc/101/stat"
if command -v python3 >/dev/null 2>&1 &&
    python3 -I -c 'import socket,sys
for p in sys.argv[1:]:
    s = socket.socket(socket.AF_UNIX); s.bind(p)' \
        "$R6C/proc/121/cmdline" "$R6C/proc/121/comm" "$R6C/proc/101/status" "$R6C/proc/101/stat"; then
    run t6c "$R6C" "$WORK/s6c"
    hasnt "vanished pid: no can't open error" "can't open|No such file|cannot open|No such device" "$WORK/t6c.out"
    has "vanished pid: scan still finds ak_rtsp" '^PASS +ak_rtsp running \(pid 101\)' "$WORK/t6c.out"
    has "vanished pid: unreadable status gives ?" 'ak_rtsp pid 101 VmRSS \? kB' "$WORK/t6c.out"
else
    echo "skip - vanished pid (python3 unavailable)"
fi

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

# 12. HOME moved to a private tmpfs dir: "/" and "/etc" at 775 are INFO only ----
R12=$WORK/r12; mkroot "$R12"
mkdir -p "$R12/tmp/akam_home"
chmod 700 "$R12/tmp/akam_home"
echo "ssh-ed25519 AAAAFAKE test" > "$R12/tmp/akam_home/authorized_keys"
chmod 600 "$R12/tmp/akam_home/authorized_keys"
echo "root:x:0:0:root:/tmp/akam_home:/bin/sh" > "$R12/etc/passwd"
chmod 775 "$R12" "$R12/etc"
run t12 "$R12" "$WORK/s12"
has "HOME: reported" "root HOME in passwd: '/tmp/akam_home'" "$WORK/t12.out"
has "HOME: / is INFO" '^INFO +/ mode (775|drwxrwxr-x)' "$WORK/t12.out"
has "HOME: /etc is INFO" '^INFO +/etc mode (775|drwxrwxr-x)' "$WORK/t12.out"
hasnt "HOME: no FAIL on / or /etc" '^FAIL +/(etc)? ' "$WORK/t12.out"
has "HOME: key path pass" '^PASS +key path \(/tmp/akam_home\) is not group/other writable' "$WORK/t12.out"
has "HOME: keys counted there" '^PASS +/tmp/akam_home/authorized_keys holds 1 key' "$WORK/t12.out"
if [ "$RC" -eq 0 ]; then ok "HOME moved, / and /etc 775: exit 0"; else ko "HOME moved: exit $RC"; grep -E '^FAIL' "$WORK/t12.out"; fi
chmod 775 "$R12/tmp/akam_home"
run t13 "$R12" "$WORK/s13"
has "HOME writable by group: FAIL" '^FAIL +/tmp/akam_home mode (775|drwxrwxr-x)' "$WORK/t13.out"
chmod 700 "$R12/tmp/akam_home"; chmod 660 "$R12/tmp/akam_home/authorized_keys"
run t13b "$R12" "$WORK/s13b"
has "HOME key file writable by group: FAIL" '^FAIL +/tmp/akam_home/authorized_keys mode (660|-rw-rw----)' "$WORK/t13b.out"
# HOME without keys: the persistent path is walked, so / at 775 is a real FAIL.
R14=$WORK/r14; mkroot "$R14"
chmod 775 "$R14"
run t14 "$R14" "$WORK/s14"
has "no keys in HOME: WARN" "^WARN +root HOME '/root' holds no authorized_keys" "$WORK/t14.out"
has "no keys in HOME: / on the path is FAIL" '^FAIL +/ is group/other writable and on the key path' "$WORK/t14.out"

# 15. ak_rtsp CPU from /proc deltas: +50 jiffies of 200 total = 25.0% -----------
R15=$WORK/r15; mkroot "$R15"
S15=$WORK/s15; mkdir -p "$S15"
cat > "$S15/sleep" <<EOF
#!/bin/sh
if [ ! -e "$WORK/cpu_done" ]; then
    : > "$WORK/cpu_done"
    printf '101 (ak_rtsp) S 1 1 1 0 -1 0 0 0 0 0 130 70 0 0 20 0 5 0 1 2 3\n' > "$R15/proc/101/stat"
    echo "cpu  1100 0 550 8050 0 0 0 0 0 0" > "$R15/proc/stat"
fi
exit 0
EOF
chmod +x "$S15/sleep"
run t15 "$R15" "$S15"
has "cpu: 25.0 percent" 'ak_rtsp pid 101 CPU 25\.0% of the machine over 0s \(50 of 200 jiffies\)' "$WORK/t15.out"
# no ak_rtsp -> UNKNOWN, not a crash
R15b=$WORK/r15b; mkroot "$R15b"; rm -r "$R15b/proc/101"
run t15b "$R15b" "$WORK/s15b"
has "cpu: ak_rtsp absent -> UNKNOWN" '^UNKNOWN +ak_rtsp not running: CPU use skipped' "$WORK/t15b.out"

# 16. night mode and flap check through a scripted 8091 -----------------------------
nc_night() { # nc_night DIR "state1 state2 ..." : GET night.state walks the list
    mkdir -p "$1"
    echo "$2" > "$1/states"
    cat > "$1/nc" <<EOF
#!/bin/sh
read -r cmd
case "\$cmd" in
    "GET night.mode") echo "night.mode=auto" ;;
    "GET night.state")
        n=\$(cat "$1/count" 2>/dev/null || echo 0); n=\$((n + 1)); echo \$n > "$1/count"
        set -- \$(cat "$1/states")
        [ "\$n" -le "\$#" ] || n=\$#
        eval "echo night.state=\\\${\$n}" ;;
    *) echo "ERR" ;;
esac
EOF
    chmod +x "$1/nc"
}
S16=$WORK/s16; nc_night "$S16" "night night night night"
run t16 "$R1" "$S16"
has "night: mode shown" 'night\.mode=auto' "$WORK/t16.out"
has "night: stable PASS" '^PASS +night\.state stable \(0 change' "$WORK/t16.out"
has "night: samples listed" 'samples \(4 x 0s\): night night night night' "$WORK/t16.out"
S17=$WORK/s17; nc_night "$S17" "night day day day"
run t17 "$R1" "$S17"
has "night: one change is PASS" '^PASS +night\.state stable \(1 change' "$WORK/t17.out"
S18=$WORK/s18; nc_night "$S18" "night day night day"
run t18 "$R1" "$S18"
has "night: flapping WARN" '^WARN +night\.state changed 3 times in 4 samples' "$WORK/t18.out"
EXTRA_ENV=(HWCHECK_NIGHT_SAMPLES=0)
run t19 "$R1" "$S18"
has "night: disabled" 'night check disabled' "$WORK/t19.out"
EXTRA_ENV=(HWCHECK_X=1)

# 20. ONVIF ports: 80/tcp and 3702/udp closed by default, accepted only when the flag says on ---------------
udp_line() { printf '  1: %s:%04X 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 1 2 0\n' "$1" "$2"; }
mkudp() { { echo "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops"; [ "$2" != "" ] && udp_line 00000000 "$2"; } > "$1/proc/net/udp"; }
R20=$WORK/r20; mkroot "$R20"; mkudp "$R20" ""
run t20 "$R20" "$WORK/s20"
has "onvif off: port 80 closed passes" '^PASS +port 80 closed \(ONVIF off\)' "$WORK/t20.out"
hasnt "onvif off: nothing says UDP 3702 is bound" 'UDP 3702' "$WORK/t20.out"
R21=$WORK/r21; mkroot "$R21"; mkudp "$R21" 3702
run t21 "$R21" "$WORK/s20"
has "onvif off: UDP 3702 bound is FAIL" '^FAIL +UDP 3702 bound while ONVIF is off' "$WORK/t21.out"
R22=$WORK/r22; mkroot "$R22"; mkudp "$R22" 3702
tcp_line 00000000 80 >> "$R22/proc/net/tcp"
mkdir -p "$R22/etc/config/akam"; printf 'enabled=1\nport=80\n' > "$R22/etc/config/akam/onvif"
run t22 "$R22" "$WORK/s20"
has "onvif on: port 80 listening passes" '^PASS +ONVIF is on: port 80 listening' "$WORK/t22.out"
has "onvif on: UDP 3702 bound passes" '^PASS +ONVIF is on: UDP 3702' "$WORK/t22.out"
hasnt "onvif on: no FAIL about 80" '^FAIL +port 80' "$WORK/t22.out"
if [ "$RC" -eq 0 ]; then ok "onvif on with 80 and 3702: exit 0"; else ko "onvif on with 80 and 3702: exit $RC"; fi
R23=$WORK/r23; mkroot "$R23"; mkudp "$R23" ""
mkdir -p "$R23/etc/config/akam"; printf 'enabled=1\nport=80\n' > "$R23/etc/config/akam/onvif"
run t23 "$R23" "$WORK/s20"
has "onvif on but 80 not listening: FAIL" '^FAIL +ONVIF is on but port 80 is not listening' "$WORK/t23.out"
has "onvif on but 3702 not bound: WARN" '^WARN +ONVIF is on but UDP 3702 is not bound' "$WORK/t23.out"
R24=$WORK/r24; mkroot "$R24"; mkudp "$R24" 3702
tcp_line 00000000 8080 >> "$R24/proc/net/tcp"
mkdir -p "$R24/etc/config/akam"; printf 'enabled=1\nport=8080\n' > "$R24/etc/config/akam/onvif"
run t24 "$R24" "$WORK/s20"
has "onvif on, port 8080: listening passes" '^PASS +ONVIF is on: port 8080 listening' "$WORK/t24.out"
has "onvif on, port 8080: port 80 closed passes" '^PASS +port 80 closed$' "$WORK/t24.out"
R25=$WORK/r25; mkroot "$R25"; mkudp "$R25" ""
tcp_line 00000000 80 >> "$R25/proc/net/tcp"
mkdir -p "$R25/etc/config/akam"; printf 'enabled=0\nport=80\n' > "$R25/etc/config/akam/onvif"
run t25 "$R25" "$WORK/s20"
has "onvif flag 0 with port 80 open: FAIL" '^FAIL +port 80 listening' "$WORK/t25.out"
R26=$WORK/r26; mkroot "$R26"; mkudp "$R26" 3702
tcp_line 00000000 80 >> "$R26/proc/net/tcp"
: > "$R26/mnt/AKAM_MAINTENANCE"
run t26 "$R26" "$WORK/s20"
has "maintenance, onvif off, 80 open: WARN" '^WARN +port 80 listening' "$WORK/t26.out"
has "maintenance, onvif off, 3702 bound: WARN" '^WARN +UDP 3702 bound' "$WORK/t26.out"
hasnt "maintenance, onvif off: no FAIL on 80/3702" '^FAIL +(port 80|UDP 3702)' "$WORK/t26.out"

# 30. UDP listeners and the process allow-list (G14) ------------------------------------------------------
udp_at() { printf '  1: %s:%04X 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 %s 2 0\n' 00000000 "$2" "${3:-1}" >> "$1/proc/net/udp"; }
R30=$WORK/r30; mkroot "$R30"; mkudp "$R30" ""
udp_at "$R30" 68; udp_at "$R30" 40000; udp_at "$R30" 3702
mkdir -p "$R30/etc/config/akam"; printf 'enabled=1\nport=80\n' > "$R30/etc/config/akam/onvif"; tcp_line 00000000 80 >> "$R30/proc/net/tcp"
run t30 "$R30" "$WORK/s20"
has "udp: 68, an ephemeral port and 3702 (ONVIF on) are fine" '^PASS +no unexpected UDP listener' "$WORK/t30.out"
has "udp: DHCP client reported" 'UDP 68 bound \(DHCP client\)' "$WORK/t30.out"
has "udp: ephemeral port reported, not failed" 'UDP 40000 bound \(ephemeral' "$WORK/t30.out"
R31=$WORK/r31; mkroot "$R31"; mkudp "$R31" ""
udp_at "$R31" 5353 777; udp_at "$R31" 5353 777; udp_at "$R31" 1900 778
mkproc "$R31" 130 "mdnsd -f" 500; mkdir -p "$R31/proc/130/fd"; ln -s 'socket:[777]' "$R31/proc/130/fd/3"
run t31 "$R31" "$WORK/s20"
has "udp: mDNS port is a FAIL, owner found through the inode" '^FAIL +UDP 5353 bound \(mdnsd -f\)' "$WORK/t31.out"
has "udp: SSDP port is a FAIL, owner unknown" '^FAIL +UDP 1900 bound \(\?\)' "$WORK/t31.out"
if [ "$(grep -Ec '^FAIL +UDP 5353' "$WORK/t31.out")" = 1 ]; then ok "udp: a port bound twice is reported once"; else ko "udp: duplicate report"; fi
if [ "$RC" -ne 0 ]; then ok "udp: an unexpected listener makes the exit status 1"; else ko "udp: exit status"; fi
R32=$WORK/r32; mkroot "$R32"; mkudp "$R32" ""; udp_at "$R32" 1900
: > "$R32/mnt/AKAM_MAINTENANCE"
run t32 "$R32" "$WORK/s20"
has "udp: maintenance flag: WARN instead of FAIL" '^WARN +UDP 1900 bound' "$WORK/t32.out"
hasnt "udp: maintenance flag: no FAIL" '^FAIL +UDP' "$WORK/t32.out"
R33=$WORK/r33; mkroot "$R33"
run t33 "$R33" "$WORK/s20"
has "udp: unreadable /proc/net/udp is said, not failed" '^INFO +/proc/net/udp not readable' "$WORK/t33.out"
hasnt "udp: unreadable: no FAIL about UDP" '^FAIL +UDP' "$WORK/t33.out"
udp6_at() { printf '  1: 00000000000000000000000000000000:%04X 00000000000000000000000000000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 5 2 0\n' "$2" >> "$1/proc/net/udp6"; }
R34=$WORK/r34; mkroot "$R34"; mkudp "$R34" ""; : > "$R34/proc/net/udp6"; udp6_at "$R34" 5683
run t34 "$R34" "$WORK/s20"
has "udp6: an IPv6 listener is audited too" '^FAIL +UDP 5683 bound' "$WORK/t34.out"

# kernel log reader: the filter passes, the stock klogd alone is WARN, nothing at all is WARN
has "klog: the filter running is PASS" '^PASS +kernel log filter akam_klog running' "$WORK/t1.out"
R35K=$WORK/r35k; mkroot "$R35K"; rm -rf "$R35K/proc/106"; mkproc "$R35K" 120 "klogd -n" 100
run t35k "$R35K" "$WORK/s20"
has "klog: stock klogd instead of the filter is WARN" '^WARN +stock klogd runs instead of the kernel log filter' "$WORK/t35k.out"
R35L=$WORK/r35l; mkroot "$R35L"; rm -rf "$R35L/proc/106"
run t35l "$R35L" "$WORK/s20"
has "klog: no reader at all is WARN" '^WARN +no kernel log reader' "$WORK/t35l.out"

# process allow-list: the healthy fake camera is clean; vendor daemons are WARN "review"; strangers are WARN
has "allow-list: healthy camera passes" '^PASS +all [0-9]+ processes are on the allow-list' "$WORK/t1.out"
R35=$WORK/r35; mkroot "$R35"
mkproc "$R35" 140 "msgserver" 900; mkproc "$R35" 141 "/usr/bin/cmdserver" 900; mkproc "$R35" 142 "mdnsd -f" 100
mkproc "$R35" 143 "syslogd -s 200 -b 3" 100; mkproc "$R35" 144 "klogd" 100; mkproc "$R35" 145 "/usr/bin/watchdog" 100
mkproc "$R35" 146 "sh /usr/bin/ak_rtsp_wrapper.sh" 100; mkproc "$R35" 147 "sh /usr/sbin/akam_net.sh start" 100
mkproc "$R35" 148 "sh /etc/init.d/drop_caches.sh" 100; mkproc "$R35" 149 "sleep 60" 100; mkproc "$R35" 150 "-sh" 100
mkproc "$R35" 151 "init" 100; mkproc "$R35" 152 "ntpd -n -q -p pool.ntp.org" 100
mkdir -p "$R35/proc/160"; : > "$R35/proc/160/cmdline"     # a kernel thread: no cmdline, skipped
run t35 "$R35" "$WORK/s20"
has "allow-list: msgserver is WARN vendor daemon, review" '^WARN +vendor daemon, review: msgserver \(pid 140\)' "$WORK/t35.out"
has "allow-list: cmdserver (full path) is WARN vendor daemon, review" '^WARN +vendor daemon, review: cmdserver \(pid 141\)' "$WORK/t35.out"
has "allow-list: an unknown daemon is WARN unexpected" '^WARN +unexpected process: mdnsd \(pid 142\)' "$WORK/t35.out"
hasnt "allow-list: syslogd, klogd, watchdog, wrapper, akam_*, drop_caches, sleep, sh, init, ntpd are fine" 'unexpected process: (syslogd|klogd|watchdog|ak_rtsp_wrapper|akam_net|drop_caches|sleep|sh|init|ntpd)' "$WORK/t35.out"
hasnt "allow-list: msgserver/cmdserver are not FAIL" '^FAIL +.*(msgserver|cmdserver)' "$WORK/t35.out"
has "allow-list: summary counts what needs review" 'processes, 3 to review' "$WORK/t35.out"

# process allow-list without strings/paste (the camera's BusyBox may lack them): tr is the fallback; with no tool
# at all the check must not pass vacuously
BASEBIN_FULL=$BASEBIN
BASEBIN=$WORK/basebin_notools
mkdir -p "$BASEBIN"
for t in cat head tail sed cut grep wc ls rm cp mkdir uname sh; do
    p=$(command -v "$t") && ln -s "$p" "$BASEBIN/$t"
done
run t35n "$R35" "$WORK/s35n"
has "allow-list: no strings/paste/tr: not a vacuous PASS" '^FAIL +process allow-list could not be checked' "$WORK/t35n.out"
hasnt "allow-list: no tool: no 'all 0 processes' pass" 'all 0 processes' "$WORK/t35n.out"
p=$(command -v tr) && ln -s "$p" "$BASEBIN/tr"
run t35t "$R35" "$WORK/s35n"
has "allow-list: tr fallback still finds the strangers" '^WARN +unexpected process: mdnsd \(pid 142\)' "$WORK/t35t.out"
has "allow-list: tr fallback names the script (argv1)" 'processes, 3 to review' "$WORK/t35t.out"
BASEBIN=$BASEBIN_FULL

# 31b. wakeups per second from the threads' voluntary context switches (G10)
R36=$WORK/r36; mkroot "$R36"
for tid in 201 202 203; do
    mkdir -p "$R36/proc/101/task/$tid"
    printf 'Name:\tak_rtsp\nvoluntary_ctxt_switches:\t100\nnonvoluntary_ctxt_switches:\t9\n' > "$R36/proc/101/task/$tid/status"
done
S36=$WORK/s36
stub "$S36" sleep 'for f in '"$R36"'/proc/101/task/*/status; do sed "s/^voluntary_ctxt_switches:.*/voluntary_ctxt_switches:\t160/" "$f" > "$f.n"; cat "$f.n" > "$f"; rm -f "$f.n"; done'
EXTRA_ENV=(HWCHECK_CPU_WINDOW=2)
run t36 "$R36" "$S36"
EXTRA_ENV=(HWCHECK_X=1)
has "wakeups: 3 threads x 60 switches in 2 s = 90/s" 'ak_rtsp pid 101 wakeups 90/s over 2s' "$WORK/t36.out"
run t37 "$R1" "$S1"
hasnt "wakeups: no per-thread data, nothing invented" 'wakeups' "$WORK/t37.out"

# 38. MQTT: off = no outbound TCP connection of ak_rtsp, on = exactly one ---------------------------------------
tcp_conn() { printf '   1: 0900A8C0:%04X 0A00A8C0:075B %s 00000000:00000000 00:00000000 00000000     0        0 %s 1 0\n' "$2" "$3" "$4" >> "$1/proc/net/tcp"; }
sock_of() { mkdir -p "$1/proc/$2/fd"; ln -s "socket:[$3]" "$1/proc/$2/fd/$4"; }
R38=$WORK/r38; mkroot "$R38"
sock_of "$R38" 101 900 3                                    # an RTSP client accepted on 554 does not count
tcp_conn "$R38" 554 01 900
run t38 "$R38" "$WORK/s20"
has "mqtt off, only a client on 554: PASS" '^PASS +MQTT is off: ak_rtsp holds no outbound TCP connection' "$WORK/t38.out"
R39=$WORK/r39; mkroot "$R39"
sock_of "$R39" 101 901 3
tcp_conn "$R39" 40001 01 901
run t39 "$R39" "$WORK/s20"
has "mqtt off but an outbound connection: FAIL" '^FAIL +MQTT is off but ak_rtsp holds 1 outbound' "$WORK/t39.out"
R40=$WORK/r40; mkroot "$R40"
mkdir -p "$R40/etc/config/akam"; printf 'enabled=1\nhost=192.0.2.10\nport=1883\nuser=u\n' > "$R40/etc/config/akam/mqtt"
sock_of "$R40" 101 902 3
tcp_conn "$R40" 40002 01 902
run t40 "$R40" "$WORK/s20"
has "mqtt on with one outbound connection: PASS" '^PASS +MQTT is on: ak_rtsp holds one outbound TCP connection' "$WORK/t40.out"
hasnt "mqtt on: the password file and the host are not printed" '192\.0\.2\.10' "$WORK/t40.out"
R41=$WORK/r41; mkroot "$R41"
mkdir -p "$R41/etc/config/akam"; printf 'enabled=1\nhost=h\n' > "$R41/etc/config/akam/mqtt"
mkdir -p "$R41/proc/101/fd"
run t41 "$R41" "$WORK/s20"
has "mqtt on, no connection (backoff): WARN" '^WARN +MQTT is on but ak_rtsp holds no outbound' "$WORK/t41.out"
R42=$WORK/r42; mkroot "$R42"
mkdir -p "$R42/etc/config/akam"; printf 'enabled=1\nhost=h\n' > "$R42/etc/config/akam/mqtt"
sock_of "$R42" 101 903 3; sock_of "$R42" 101 904 4
tcp_conn "$R42" 40003 01 903; tcp_conn "$R42" 40004 02 904
run t42 "$R42" "$WORK/s20"
has "mqtt on with two outbound connections: FAIL" '^FAIL +MQTT is on but ak_rtsp holds 2 outbound' "$WORK/t42.out"
R43=$WORK/r43; mkroot "$R43"
mkdir -p "$R43/etc/config/akam"; printf 'enabled=0\nhost=h\n' > "$R43/etc/config/akam/mqtt"
sock_of "$R43" 101 905 3
tcp_conn "$R43" 40005 01 905
run t43 "$R43" "$WORK/s20"
has "mqtt enabled=0 with a connection: FAIL" '^FAIL +MQTT is off but ak_rtsp holds 1 outbound' "$WORK/t43.out"
run t44 "$R1" "$S1"
has "mqtt: no fd directory for ak_rtsp: UNKNOWN, not a guess" '^UNKNOWN +the sockets of ak_rtsp are not readable' "$WORK/t44.out"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
