#!/usr/bin/env bash
# Host-side test of src/common/time32_shim.c (the fix for the camera kernel
# that acknowledges clock_gettime64 without executing it). Nothing here
# touches hardware. It proves, on the FINAL static ARM binaries, that:
#   - with the shim, clock_gettime/timerfd go through the 32-bit syscalls
#     (qemu-arm -strace sees clock_gettime, never clock_gettime64);
#   - without it, stock musl does call clock_gettime64 (control: the test
#     would notice if the shim stopped being necessary or stopped working);
#   - no forbidden time64 syscall number is loaded into r7 anywhere in the
#     machine code of the probe, ak_rtsp, flash_tool and dropbearmulti
#     (gdb disassembly; skipped when gdb is missing);
#   - the probe's verdict logic reports NOOP when the time64 syscalls are
#     simulated as "success without effect" and WORKS otherwise.
# Skips (exit 0) when the Zig toolchain is not installed (tools/build/bootstrap.sh).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tools/build/env.sh
# shellcheck disable=SC1091
if ! . "$REPO/tools/build/env.sh" 2>/dev/null; then
    echo "skip: pinned Zig not installed (run tools/build/bootstrap.sh)"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}

CHECK="$REPO/tools/build/check_time32.sh"

echo "== build"
export PROBE_KEEP_SYMBOLS=1
check "probe builds with the shim" "$REPO/tests/native/build_probe.sh" "$WORK/probe_shim"
check "probe builds without the shim" env PROBE_SHIM=0 "$REPO/tests/native/build_probe.sh" "$WORK/probe_stock"
check "probe builds with simulated NOOP time64 syscalls" env PROBE_CFLAGS=-DPROBE_SIMULATE_NOOP64 "$REPO/tests/native/build_probe.sh" "$WORK/probe_sim"

# A small program using every time64-prone libc entry point we rely on.
cat >"$WORK/users.c" <<'EOF'
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
int main(void)
{
    struct timespec ts, at;
    struct itimerspec its = {{0, 0}, {0, 100000000}}, cur;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    sem_t sem;
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_REALTIME, &ts);
    clock_gettime(CLOCK_BOOTTIME, &ts);
    (void)time(NULL);
    (void)clock();
    timerfd_settime(fd, 0, &its, NULL);
    timerfd_gettime(fd, &cur);
    clock_gettime(CLOCK_REALTIME, &at);
    at.tv_nsec += 1000000;
    pthread_mutex_lock(&mu);
    pthread_cond_timedwait(&cv, &mu, &at);
    pthread_mutex_unlock(&mu);
    sem_init(&sem, 0, 0);
    sem_timedwait(&sem, &at);
    return 0;
}
EOF
build_users() { # <output> [extra source...]
    local out=$1
    shift
    # shellcheck disable=SC2086  # CAMYN_CC is a command line by design
    $CAMYN_CC -static -Os -mfloat-abi=soft -g -o "$out" "$WORK/users.c" "$@"
}
check "users program builds with the shim" build_users "$WORK/users_shim" "$REPO/src/common/time32_shim.c"
check "users program builds without the shim" build_users "$WORK/users_stock"

echo "== machine code: no unconditional time64 syscall in the shim builds"
if command -v gdb >/dev/null 2>&1; then
    check "users_shim: only 32-bit clock syscalls, clock_gettime32 present" "$CHECK" --require-263 "$WORK/users_shim"
    check "users_stock (control): stock musl is caught loading clock_gettime64" bash -c "! '$CHECK' '$WORK/users_stock' >/dev/null 2>&1"
    # The gate must also see a number that is only loaded conditionally.
    cat >"$WORK/cond.c" <<'EOF'
int main(int argc, char **argv)
{
    (void)argv;
    __asm__ volatile("mov r0, %0\n cmp r0, #1\n moveq r7, #192\n movne r7, #256\n"
                     " orrne r7, r7, #147\n mov r1, #0\n svc 0"
                     : : "r"(argc) : "r0", "r1", "r7", "memory");
    return 0;
}
EOF
    # shellcheck disable=SC2086  # CAMYN_CC is a command line by design
    check "conditional-syscall control builds" $CAMYN_CC -static -Os -mfloat-abi=soft -o "$WORK/cond" "$WORK/cond.c"
    check "gate catches a conditionally loaded clock_gettime64 (403)" bash -c "! '$CHECK' '$WORK/cond' >/dev/null 2>&1"
    for b in src/ak_rtsp/flash_tool src/ak_rtsp/isp_dump src/ak_rtsp/patch_div0; do
        if [ -f "$REPO/$b" ]; then
            check "$b: no forbidden time64 syscall" "$CHECK" "$REPO/$b"
        else
            echo "  skip - $b not built"
        fi
    done
    if [ -f "$REPO/src/ak_rtsp/ak_rtsp" ]; then
        check "ak_rtsp: no forbidden time64 syscall, clock_gettime32 present" "$CHECK" --require-263 "$REPO/src/ak_rtsp/ak_rtsp"
    else
        echo "  skip - src/ak_rtsp/ak_rtsp not built"
    fi
    if [ -f "$REPO/dist/dropbear/dropbearmulti" ]; then
        check "dropbearmulti: no forbidden time64 syscall, clock_gettime32 present" "$CHECK" --require-263 "$REPO/dist/dropbear/dropbearmulti"
    else
        echo "  skip - dist/dropbear/dropbearmulti not built"
    fi
else
    echo "  skip - gdb not installed"
fi

echo "== runtime under qemu-arm -strace"
if command -v qemu-arm >/dev/null 2>&1; then
    qemu-arm -strace "$WORK/users_shim" >/dev/null 2>"$WORK/strace_shim.txt" || true
    qemu-arm -strace "$WORK/users_stock" >/dev/null 2>"$WORK/strace_stock.txt" || true
    check "shim: clock_gettime syscall seen" grep -q 'clock_gettime(' "$WORK/strace_shim.txt"
    check "shim: timerfd_settime/gettime 32-bit seen" grep -q 'timerfd_settime(' "$WORK/strace_shim.txt"
    check "shim: no clock_gettime64" bash -c "! grep -q 'clock_gettime64' '$WORK/strace_shim.txt'"
    check "shim: no timerfd *time64" bash -c "! grep -q 'timerfd_[gs]ettime64' '$WORK/strace_shim.txt'"
    check "stock musl (control): calls clock_gettime64" grep -q 'clock_gettime64' "$WORK/strace_stock.txt"

    # Shim edge cases: NULL pointers fail with EFAULT instead of faulting; every
    # clock id is passed through; EINVAL comes back as errno.
    cat >"$WORK/edge.c" <<'EOF'
#include <errno.h>
#include <sys/timerfd.h>
#include <time.h>
int main(void)
{
    struct timespec a, b;
    struct itimerspec its = {{0, 0}, {0, 0}};
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    int ids[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_MONOTONIC_RAW,
                 CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_PROCESS_CPUTIME_ID,
                 CLOCK_THREAD_CPUTIME_ID};
    unsigned i;
    if (fd < 0) return 1;
    errno = 0;
    if (clock_gettime(CLOCK_MONOTONIC, NULL) != -1 || errno != EFAULT) return 2;
    errno = 0;
    if (clock_gettime((clockid_t)999, &a) != -1 || errno != EINVAL) return 3;
    errno = 0;
    if (timerfd_settime(fd, 0, NULL, NULL) != -1 || errno != EFAULT) return 4;
    errno = 0;
    if (timerfd_gettime(fd, NULL) != -1 || errno != EFAULT) return 5;
    errno = 0;
    if (timerfd_create(999, 0) != -1 || errno != EINVAL) return 6;
    for (i = 0; i < sizeof ids / sizeof *ids; i++)
        if (clock_gettime(ids[i], &a) != 0 || a.tv_nsec < 0 || a.tv_nsec >= 1000000000L) return 10 + i;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (b.tv_sec < a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) return 30;
    if (timerfd_settime(fd, 0, &its, &its) != 0) return 31;
    return 0;
}
EOF
    # shellcheck disable=SC2086  # CAMYN_CC is a command line by design
    check "edge-case program builds with the shim" $CAMYN_CC -static -Os -mfloat-abi=soft -o "$WORK/edge" "$WORK/edge.c" "$REPO/src/common/time32_shim.c"
    check "shim edge cases (NULL, EINVAL, all clock ids) under qemu" qemu-arm "$WORK/edge"

    qemu-arm "$WORK/probe_shim" "$WORK" >"$WORK/probe_shim.txt" 2>&1 || true
    qemu-arm "$WORK/probe_sim" "$WORK" >"$WORK/probe_sim.txt" 2>&1 || true
    check "probe: shim reported as linked" grep -q 'shim LINKED' "$WORK/probe_shim.txt"
    check "probe under qemu: RESULT PASS" grep -q 'RESULT PASS' "$WORK/probe_shim.txt"
    check "probe under qemu: no libc check fails" bash -c "! grep -E '^  libc +(NOOP|WRONG|ERR|HANG|ENOSYS)' '$WORK/probe_shim.txt'"
    check "probe verdict logic: simulated camera kernel gives NOOP for clock_gettime64" \
        grep -q '^  raw  *NOOP  *clock_gettime64(MONOTONIC)' "$WORK/probe_sim.txt"
    check "probe verdict logic: ...and the libc checks still pass through the shim" \
        grep -q 'RESULT PASS' "$WORK/probe_sim.txt"
    check "probe verdict logic: 32-bit controls stay WORKS" \
        grep -q '^  raw  *WORKS  *clock_nanosleep_time32 (control)' "$WORK/probe_sim.txt"
else
    echo "  skip - qemu-arm not installed"
fi

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" = 0 ]
