/*
 * time64_probe.c -- hardware probe for the time64 syscalls of the Camyn
 * camera kernel (static ARM binary).
 *
 * Why it exists: the camera's vendor kernel (Linux 4.4.302, ARM926, no vDSO)
 * has no time64 syscalls, but it does not answer ENOSYS to all of them:
 * clock_gettime64 (403) returns 0 and leaves the buffer untouched, so musl
 * (which only falls back to the 32-bit call on ENOSYS) gives callers garbage.
 * This program shows, per syscall, whether the kernel really executes it, and
 * whether the libc functions our programs use (with or without
 * src/common/time32_shim.c linked in) give the right result.
 *
 * Run it on the camera (from the SD card or /tmp), optionally with a writable
 * directory for the utimensat test (default /tmp):
 *     ./time64_probe [dir] | tee /tmp/sd/time64_probe.txt
 * It takes about 10 seconds and changes nothing on the system (it creates and
 * removes one empty temporary file and one timerfd; it never sets a clock).
 *
 * Output sections:
 *   1. basics + 3 rounds of every clock source (same lines as the first
 *      time_probe, so old and new captures can be compared);
 *   2. "RAW time64 syscalls": the kernel, called with inline "svc 0" exactly
 *      the way musl calls it, each cross-checked against the 32-bit twin;
 *   3. "LIBC / SHIM": the libc functions our programs use;
 *   4. SUMMARY with one verdict per line:
 *        WORKS   the call did what it should
 *        NOOP    returned success but did nothing (the camera's failure mode)
 *        ENOSYS  the kernel says it does not have the call (musl falls back)
 *        WRONG   did something, but not the right thing
 *        ERR     other error;  HANG: did not return within 4 s (alarm)
 *
 * Exit status: the number of libc-level checks that did not WORK (0 = good).
 * The raw section is informational: on the camera it is expected to contain
 * NOOP lines, which is exactly why the shim exists.
 *
 * The raw calls use "svc 0" so musl's errno mapping cannot hide anything.
 * ARM EABI only. Build: tests/native/build_probe.sh
 * (-DPROBE_SIMULATE_NOOP64 makes the raw calls pretend to be the camera's
 * kernel; used by tests/time32_shim_test.sh to check the verdicts.)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

/* ARM EABI syscall numbers (32-bit "time32" ones, then time64 ones). */
#define NR_TIMES 43
#define NR_FUTEX32 240
#define NR_CLOCK_GETTIME32 263
#define NR_CLOCK_GETRES32 264
#define NR_CLOCK_NANOSLEEP32 265
#define NR_PSELECT6_32 335
#define NR_PPOLL32 336
#define NR_UTIMENSAT32 348
#define NR_TIMERFD_SETTIME32 353
#define NR_TIMERFD_GETTIME32 354
#define NR_CLOCK_GETTIME64 403
#define NR_CLOCK_GETRES64 406
#define NR_CLOCK_NANOSLEEP64 407
#define NR_TIMERFD_GETTIME64 410
#define NR_TIMERFD_SETTIME64 411
#define NR_UTIMENSAT64 412
#define NR_PSELECT6_64 413
#define NR_PPOLL64 414
#define NR_FUTEX64 422

#define FUTEX_WAIT_PRIV 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG */
#define SLEEP_MS 200
#define PATTERN 0x5a

#if defined(__arm__)
static long raw6(long nr, long a, long b, long c, long d, long e, long f)
{
#ifdef PROBE_SIMULATE_NOOP64
    /* Self-test of the verdict logic: behave like the camera's kernel, which
     * acknowledges the time64 syscalls without executing them. */
    if (nr >= 403)
        return 0;
#endif
    register long r7 __asm__("r7") = nr;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    __asm__ volatile("svc 0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "memory");
    return r0;
}
#else
static long raw6(long nr, long a, long b, long c, long d, long e, long f)
{
    (void)nr; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return -38;
}
#endif

#define RAW2(nr, a, b) raw6((nr), (long)(a), (long)(b), 0, 0, 0, 0)
#define RAW3(nr, a, b, c) raw6((nr), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define RAW4(nr, a, b, c, d) raw6((nr), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

/* The shim marker (defined by src/common/time32_shim.c when linked in). */
extern const int camyn_time32_shim_active __attribute__((weak));

/* ---------- verdict bookkeeping ---------- */

enum verdict { V_WORKS, V_NOOP, V_ENOSYS, V_WRONG, V_ERR, V_HANG };
static const char *const vname[] = {"WORKS", "NOOP", "ENOSYS", "WRONG", "ERR", "HANG"};

#define MAX_RES 96
static struct {
    char name[64];
    enum verdict v;
    int libc_level;
} results[MAX_RES];
static int nres;
static int in_libc_section;

static void report(const char *name, enum verdict v, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void report(const char *name, enum verdict v, const char *fmt, ...)
{
    char detail[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    printf("  VERDICT %-40s %-6s %s\n", name, vname[v], detail);
    if (nres < MAX_RES) {
        snprintf(results[nres].name, sizeof results[nres].name, "%s", name);
        results[nres].v = v;
        results[nres].libc_level = in_libc_section;
        nres++;
    }
}

/* ---------- time helpers ---------- */

static volatile sig_atomic_t alarm_fired;
static void on_alarm(int sig)
{
    (void)sig;
    alarm_fired = 1;
}

static void arm_guard(void)
{
    alarm_fired = 0;
    alarm(4);
}

static void disarm_guard(void)
{
    alarm(0);
}

/* Milliseconds of CLOCK_MONOTONIC, taken with the 32-bit syscall (the one
 * known to work on the camera), independent of libc and of the shim. */
static long long now_ms(void)
{
    long t[2] = {0, 0};
    if (RAW2(NR_CLOCK_GETTIME32, 1, t) != 0)
        return -1;
    return (long long)t[0] * 1000 + t[1] / 1000000;
}

static long real_sec32(void)
{
    long t[2] = {0, 0};
    if (RAW2(NR_CLOCK_GETTIME32, 0, t) != 0)
        return -1;
    return t[0];
}

/*
 * Judge a call that should block for want_ms and then return `expected`.
 * `ret` uses the raw convention (negative errno on failure).
 */
static void judge_timed(const char *name, long ret, long expected, long long el_ms,
                        long want_ms)
{
    if (alarm_fired) {
        report(name, V_HANG, "did not return within 4 s (ret=%ld)", ret);
    } else if (ret == -38) {
        report(name, V_ENOSYS, "kernel answered ENOSYS (musl would fall back)");
    } else if (ret != expected && ret != 0) {
        report(name, V_ERR, "ret=%ld (expected %ld)", ret, expected);
    } else if (el_ms < want_ms / 4) {
        report(name, V_NOOP, "ret=%ld after only %lld ms (wanted %ld ms): returned without waiting",
               ret, el_ms, want_ms);
    } else if (el_ms >= want_ms - 20 && el_ms <= want_ms + 400 && ret == expected) {
        report(name, V_WORKS, "ret=%ld, slept %lld ms (wanted %ld ms)", ret, el_ms, want_ms);
    } else {
        report(name, V_WRONG, "ret=%ld, slept %lld ms (wanted %ld ms)", ret, el_ms, want_ms);
    }
}

/* ---------- section 1: legacy rounds (same lines as the first time_probe) ---------- */

static void libc_clock(const char *name, clockid_t id)
{
    struct timespec ts;
    int r;
    memset(&ts, PATTERN, sizeof ts);
    errno = 0;
    r = clock_gettime(id, &ts);
    printf("  libc  %-9s ret=%d errno=%d sec=%lld nsec=%ld%s\n", name, r, errno,
           (long long)ts.tv_sec, (long)ts.tv_nsec,
           (ts.tv_nsec == 0x5a5a5a5a) ? "  <-- buffer NOT written" : "");
}

static void raw32(long id, const char *name)
{
    long t[2] = {0x5a5a5a5a, 0x5a5a5a5a};
    long r = RAW2(NR_CLOCK_GETTIME32, id, t);
    printf("  raw263 %-8s ret=%ld sec=%ld nsec=%ld\n", name, r, t[0], t[1]);
}

static void raw64(long id, const char *name)
{
    long t[4] = {0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a};
    long r = RAW2(NR_CLOCK_GETTIME64, id, t);
    printf("  raw403 %-8s ret=%ld words=%08lx %08lx %08lx %08lx\n", name, r,
           (unsigned long)t[0], (unsigned long)t[1], (unsigned long)t[2],
           (unsigned long)t[3]);
}

static void uptime_line(void)
{
    char buf[96];
    int fd = open("/proc/uptime", O_RDONLY);
    ssize_t n;
    if (fd < 0) {
        printf("  uptime: open failed errno=%d\n", errno);
        return;
    }
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = 0;
    printf("  uptime: %s\n", buf);
}

static void section_rounds(void)
{
    int round;
    struct timeval tv;

    for (round = 0; round < 3; round++) {
        printf("round %d\n", round);
        libc_clock("MONOTONIC", CLOCK_MONOTONIC);
        libc_clock("REALTIME", CLOCK_REALTIME);
        libc_clock("BOOTTIME", CLOCK_BOOTTIME);
        raw32(CLOCK_MONOTONIC, "MONOTONIC");
        raw32(CLOCK_REALTIME, "REALTIME");
        raw32(CLOCK_BOOTTIME, "BOOTTIME");
        raw64(CLOCK_MONOTONIC, "MONOTONIC");
        raw64(CLOCK_REALTIME, "REALTIME");
        raw64(CLOCK_BOOTTIME, "BOOTTIME");
        memset(&tv, PATTERN, sizeof tv);
        errno = 0;
        printf("  gtod  ret=%d errno=%d sec=%lld usec=%ld\n", gettimeofday(&tv, NULL), errno,
               (long long)tv.tv_sec, (long)tv.tv_usec);
        printf("  times raw=%ld\n", RAW2(NR_TIMES, 0, 0));
        uptime_line();
        if (round < 2)
            sleep(1);
    }
}

/* ---------- section 2: raw time64 syscalls ---------- */

static int all_pattern(const void *p, size_t n)
{
    const unsigned char *c = p;
    size_t i;
    for (i = 0; i < n; i++)
        if (c[i] != PATTERN)
            return 0;
    return 1;
}

static void raw_clock_gettime64(long id, const char *cname)
{
    char name[64];
    long long t[2];
    long ref_before, ref_after, r;
    long t32[2];

    snprintf(name, sizeof name, "clock_gettime64(%s)", cname);
    memset(t, PATTERN, sizeof t);
    t32[0] = 0;
    RAW2(NR_CLOCK_GETTIME32, id, t32);
    ref_before = t32[0];
    r = RAW2(NR_CLOCK_GETTIME64, id, t);
    RAW2(NR_CLOCK_GETTIME32, id, t32);
    ref_after = t32[0];
    if (r == -38)
        report(name, V_ENOSYS, "kernel answered ENOSYS");
    else if (r != 0)
        report(name, V_ERR, "ret=%ld", r);
    else if (all_pattern(t, sizeof t))
        report(name, V_NOOP, "ret=0 but the timespec was NOT written");
    else if (t[0] >= ref_before - 1 && t[0] <= ref_after + 1 && t[1] >= 0 && t[1] < 1000000000LL)
        report(name, V_WORKS, "sec=%lld nsec=%lld (32-bit twin: %ld)", t[0], t[1], ref_after);
    else
        report(name, V_WRONG, "sec=%lld nsec=%lld, 32-bit twin says %ld", t[0], t[1], ref_after);
}

static void raw_clock_getres64(void)
{
    long long t[2];
    long t32[2] = {0, 0};
    long r;

    memset(t, PATTERN, sizeof t);
    RAW2(NR_CLOCK_GETRES32, CLOCK_MONOTONIC, t32);
    r = RAW2(NR_CLOCK_GETRES64, CLOCK_MONOTONIC, t);
    if (r == -38)
        report("clock_getres_time64(MONOTONIC)", V_ENOSYS, "kernel answered ENOSYS");
    else if (r != 0)
        report("clock_getres_time64(MONOTONIC)", V_ERR, "ret=%ld", r);
    else if (all_pattern(t, sizeof t))
        report("clock_getres_time64(MONOTONIC)", V_NOOP, "ret=0 but the timespec was NOT written");
    else if (t[0] == t32[0] && t[1] == t32[1])
        report("clock_getres_time64(MONOTONIC)", V_WORKS, "sec=%lld nsec=%lld", t[0], t[1]);
    else
        report("clock_getres_time64(MONOTONIC)", V_WRONG, "got %lld.%lld, 32-bit twin %ld.%ld",
               t[0], t[1], t32[0], t32[1]);
}

/* ppoll/pselect6 write the remaining time back into the timespec, so the
 * timespecs are rebuilt before every call. */
#define RESET_TS()                           \
    do {                                     \
        ts64[0] = 0;                         \
        ts64[1] = SLEEP_MS * 1000000LL;      \
        ts32[0] = 0;                         \
        ts32[1] = SLEEP_MS * 1000000L;       \
    } while (0)

static void raw_sleepers(void)
{
    long long ts64[2];
    long ts32[2];
    long long t0;
    long r;
    int dummy = 0;

    /* clock_nanosleep: relative sleep on CLOCK_MONOTONIC. */
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = RAW4(NR_CLOCK_NANOSLEEP32, 1, 0, ts32, 0);
    judge_timed("clock_nanosleep_time32 (control)", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = RAW4(NR_CLOCK_NANOSLEEP64, 1, 0, ts64, 0);
    judge_timed("clock_nanosleep_time64", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    /* ppoll: no fds, timeout only. */
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_PPOLL32, 0, 0, (long)ts32, 0, 8, 0);
    judge_timed("ppoll (32-bit, control)", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_PPOLL64, 0, 0, (long)ts64, 0, 8, 0);
    judge_timed("ppoll_time64", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    /* pselect6: no fds, timeout only. */
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_PSELECT6_32, 0, 0, 0, 0, (long)ts32, 0);
    judge_timed("pselect6 (32-bit, control)", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_PSELECT6_64, 0, 0, 0, 0, (long)ts64, 0);
    judge_timed("pselect6_time64", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    /* futex wait on a word that nobody wakes: must end with ETIMEDOUT. */
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_FUTEX32, (long)&dummy, FUTEX_WAIT_PRIV, 0, (long)ts32, 0, 0);
    judge_timed("futex wait (32-bit, control)", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    RESET_TS();
    arm_guard();
    t0 = now_ms();
    r = raw6(NR_FUTEX64, (long)&dummy, FUTEX_WAIT_PRIV, 0, (long)ts64, 0, 0);
    judge_timed("futex_time64 wait", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();
}

static void raw_timerfd(void)
{
    long long its64[4] = {0, 0, 0, SLEEP_MS * 1000000LL};
    long long cur64[4];
    long its32[4] = {0, 0, 0, SLEEP_MS * 1000000L};
    long cur32[4];
    long r;
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);

    if (fd < 0) {
        report("timerfd_create", V_ERR, "errno=%d", errno);
        return;
    }
    /* control: the 32-bit pair */
    r = RAW4(NR_TIMERFD_SETTIME32, fd, 0, its32, 0);
    memset(cur32, PATTERN, sizeof cur32);
    RAW2(NR_TIMERFD_GETTIME32, fd, cur32);
    if (r == 0 && cur32[2] == 0 && cur32[3] > 0 && cur32[3] <= SLEEP_MS * 1000000L)
        report("timerfd_settime/gettime (32-bit, control)", V_WORKS, "armed, %ld ns left", cur32[3]);
    else
        report("timerfd_settime/gettime (32-bit, control)", V_WRONG, "set ret=%ld left=%ld.%ld", r,
               cur32[2], cur32[3]);
    memset(cur32, 0, sizeof cur32);
    RAW4(NR_TIMERFD_SETTIME32, fd, 0, cur32, 0); /* disarm */

    /* time64: set, then look with the 32-bit getter whether it armed. */
    r = RAW4(NR_TIMERFD_SETTIME64, fd, 0, its64, 0);
    memset(cur32, PATTERN, sizeof cur32);
    RAW2(NR_TIMERFD_GETTIME32, fd, cur32);
    if (r == -38)
        report("timerfd_settime64", V_ENOSYS, "kernel answered ENOSYS");
    else if (r != 0)
        report("timerfd_settime64", V_ERR, "ret=%ld", r);
    else if (cur32[2] == 0 && cur32[3] > 0 && cur32[3] <= SLEEP_MS * 1000000L)
        report("timerfd_settime64", V_WORKS, "armed, %ld ns left", cur32[3]);
    else
        report("timerfd_settime64", V_NOOP, "ret=0 but the timer is not armed (left=%ld.%ld)",
               cur32[2], cur32[3]);

    /* gettime64 on a timer that is armed (re-arm with the 32-bit call so
     * this verdict does not depend on settime64). */
    RAW4(NR_TIMERFD_SETTIME32, fd, 0, its32, 0);
    memset(cur64, PATTERN, sizeof cur64);
    r = RAW2(NR_TIMERFD_GETTIME64, fd, cur64);
    if (r == -38)
        report("timerfd_gettime64", V_ENOSYS, "kernel answered ENOSYS");
    else if (r != 0)
        report("timerfd_gettime64", V_ERR, "ret=%ld", r);
    else if (all_pattern(cur64, sizeof cur64))
        report("timerfd_gettime64", V_NOOP, "ret=0 but the itimerspec was NOT written");
    else if (cur64[2] == 0 && cur64[3] > 0 && cur64[3] <= SLEEP_MS * 1000000LL)
        report("timerfd_gettime64", V_WORKS, "%lld ns left", cur64[3]);
    else
        report("timerfd_gettime64", V_WRONG, "it_value=%lld.%lld", cur64[2], cur64[3]);
    close(fd);
}

static int stat_times(const char *path, long *at, long *mt)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *at = (long)st.st_atime;
    *mt = (long)st.st_mtime;
    return 0;
}

static void raw_utimensat(const char *dir)
{
    char path[200];
    long long ts64[4] = {1000000000LL, 0, 1234567890LL, 0};
    long ts32[4] = {1000000000L, 0, 1100000000L, 0};
    long at = 0, mt = 0, r;
    int fd;

    snprintf(path, sizeof path, "%s/.time64_probe_%ld", dir, (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        report("utimensat_time64", V_ERR, "cannot create %s (errno=%d)", path, errno);
        return;
    }
    close(fd);

    r = RAW4(NR_UTIMENSAT32, AT_FDCWD, path, ts32, 0);
    if (r == 0 && stat_times(path, &at, &mt) == 0 && at == 1000000000L && mt == 1100000000L)
        report("utimensat (32-bit, control)", V_WORKS, "mtime set to %ld", mt);
    else
        report("utimensat (32-bit, control)", V_WRONG, "ret=%ld atime=%ld mtime=%ld", r, at, mt);

    r = RAW4(NR_UTIMENSAT64, AT_FDCWD, path, ts64, 0);
    stat_times(path, &at, &mt);
    if (r == -38)
        report("utimensat_time64", V_ENOSYS, "kernel answered ENOSYS");
    else if (r != 0)
        report("utimensat_time64", V_ERR, "ret=%ld", r);
    else if (at == 1000000000L && mt == 1234567890L)
        report("utimensat_time64", V_WORKS, "mtime set to %ld", mt);
    else if (mt == 1100000000L)
        report("utimensat_time64", V_NOOP, "ret=0 but the file times did not change");
    else
        report("utimensat_time64", V_WRONG, "ret=0 atime=%ld mtime=%ld", at, mt);
    unlink(path);
}

/* ---------- section 3: libc functions (through the shim when linked) ---------- */

static void libc_clock_check(const char *cname, clockid_t id)
{
    char name[64];
    struct timespec ts;
    long t32[2] = {0, 0};
    long long a, b;
    int r;

    snprintf(name, sizeof name, "libc clock_gettime(%s)", cname);
    memset(&ts, PATTERN, sizeof ts);
    r = clock_gettime(id, &ts);
    RAW2(NR_CLOCK_GETTIME32, id, t32);
    a = ts.tv_sec;
    b = t32[0];
    if (r != 0)
        report(name, V_ERR, "ret=%d errno=%d", r, errno);
    else if (all_pattern(&ts, sizeof ts))
        report(name, V_NOOP, "ret=0 but the timespec was NOT written");
    else if (a >= b - 2 && a <= b + 2 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000L)
        report(name, V_WORKS, "sec=%lld nsec=%ld", a, (long)ts.tv_nsec);
    else
        report(name, V_WRONG, "sec=%lld nsec=%ld, raw 32-bit says %lld", a, (long)ts.tv_nsec, b);
}

static void libc_clock_advance(void)
{
    struct timespec a, b;
    long long d;
    struct timespec req = {0, SLEEP_MS * 1000000L};

    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    d = ((long long)b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    if (d >= SLEEP_MS - 20 && d <= SLEEP_MS + 400)
        report("libc clock_gettime advances (MONOTONIC)", V_WORKS, "+%lld ms across a %d ms sleep", d, SLEEP_MS);
    else if (d == 0 && a.tv_sec == b.tv_sec)
        report("libc clock_gettime advances (MONOTONIC)", V_NOOP, "no progress across a %d ms sleep", SLEEP_MS);
    else
        report("libc clock_gettime advances (MONOTONIC)", V_WRONG, "+%lld ms across a %d ms sleep", d, SLEEP_MS);
}

static void libc_time_check(void)
{
    time_t t = time(NULL);
    long ref = real_sec32();
    clock_t c = clock();

    if ((long)t >= ref - 2 && (long)t <= ref + 2 && ref > 0)
        report("libc time()", V_WORKS, "%ld", (long)t);
    else
        report("libc time()", V_WRONG, "time()=%ld raw 32-bit says %ld", (long)t, ref);
    if (c != (clock_t)-1)
        report("libc clock()", V_WORKS, "%ld us of CPU time", (long)c);
    else
        report("libc clock()", V_ERR, "returned -1");
}

static void libc_sleepers(void)
{
    struct timespec req = {0, SLEEP_MS * 1000000L};
    struct timeval tv = {0, SLEEP_MS * 1000};
    long long t0;
    long r;

    arm_guard();
    t0 = now_ms();
    r = nanosleep(&req, NULL) == 0 ? 0 : -errno;
    judge_timed("libc nanosleep", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = -clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
    judge_timed("libc clock_nanosleep(MONOTONIC)", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = usleep(SLEEP_MS * 1000) == 0 ? 0 : -errno;
    judge_timed("libc usleep", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = poll(NULL, 0, SLEEP_MS) == 0 ? 0 : -errno;
    judge_timed("libc poll", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = ppoll(NULL, 0, &req, NULL) == 0 ? 0 : -errno;
    judge_timed("libc ppoll", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = select(0, NULL, NULL, NULL, &tv) == 0 ? 0 : -errno;
    judge_timed("libc select", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    arm_guard();
    t0 = now_ms();
    r = pselect(0, NULL, NULL, NULL, &req, NULL) == 0 ? 0 : -errno;
    judge_timed("libc pselect", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();
}

static void deadline_after(clockid_t id, struct timespec *at)
{
    clock_gettime(id, at);
    at->tv_nsec += SLEEP_MS * 1000000L;
    if (at->tv_nsec >= 1000000000L) {
        at->tv_sec++;
        at->tv_nsec -= 1000000000L;
    }
}

static void libc_threads(void)
{
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    pthread_condattr_t ca;
    sem_t sem;
    struct timespec at;
    long long t0;
    long r;

    /* condition variable, CLOCK_REALTIME deadline (default) */
    pthread_cond_init(&cv, NULL);
    pthread_mutex_lock(&mu);
    deadline_after(CLOCK_REALTIME, &at);
    arm_guard();
    t0 = now_ms();
    r = -pthread_cond_timedwait(&cv, &mu, &at);
    judge_timed("libc pthread_cond_timedwait(REALTIME)", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    pthread_mutex_unlock(&mu);

    /* condition variable, CLOCK_MONOTONIC deadline */
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);
    pthread_mutex_lock(&mu);
    deadline_after(CLOCK_MONOTONIC, &at);
    arm_guard();
    t0 = now_ms();
    r = -pthread_cond_timedwait(&cv, &mu, &at);
    judge_timed("libc pthread_cond_timedwait(MONOTONIC)", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    pthread_mutex_unlock(&mu);

    /* semaphore (CLOCK_REALTIME deadline) */
    sem_init(&sem, 0, 0);
    deadline_after(CLOCK_REALTIME, &at);
    arm_guard();
    t0 = now_ms();
    r = sem_timedwait(&sem, &at) == 0 ? 0 : -errno;
    judge_timed("libc sem_timedwait", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();

    /* timed lock of a mutex we already hold */
    pthread_mutex_lock(&mu);
    deadline_after(CLOCK_REALTIME, &at);
    arm_guard();
    t0 = now_ms();
    r = -pthread_mutex_timedlock(&mu, &at);
    judge_timed("libc pthread_mutex_timedlock", r, -ETIMEDOUT, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    pthread_mutex_unlock(&mu);
}

static void libc_timerfd(void)
{
    struct itimerspec its = {{0, 0}, {0, SLEEP_MS * 1000000L}};
    struct itimerspec cur;
    struct pollfd pfd;
    uint64_t n = 0;
    long long t0;
    long r;
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);

    if (fd < 0) {
        report("libc timerfd_create", V_ERR, "errno=%d", errno);
        return;
    }
    if (timerfd_settime(fd, 0, &its, NULL) != 0) {
        report("libc timerfd_settime", V_ERR, "errno=%d", errno);
        close(fd);
        return;
    }
    memset(&cur, PATTERN, sizeof cur);
    r = timerfd_gettime(fd, &cur);
    if (r != 0)
        report("libc timerfd_gettime", V_ERR, "errno=%d", errno);
    else if (all_pattern(&cur, sizeof cur))
        report("libc timerfd_gettime", V_NOOP, "ret=0 but the itimerspec was NOT written");
    else if (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
             cur.it_value.tv_nsec <= SLEEP_MS * 1000000L)
        report("libc timerfd_settime+gettime", V_WORKS, "armed, %ld ns left", (long)cur.it_value.tv_nsec);
    else
        report("libc timerfd_settime+gettime", V_WRONG, "it_value=%lld.%ld", (long long)cur.it_value.tv_sec,
               (long)cur.it_value.tv_nsec);

    pfd.fd = fd;
    pfd.events = POLLIN;
    arm_guard();
    t0 = now_ms();
    r = poll(&pfd, 1, 2000);
    if (r == 1 && read(fd, &n, sizeof n) == (ssize_t)sizeof n && n == 1)
        r = 0;
    else
        r = -1;
    judge_timed("libc timerfd expiry", r, 0, now_ms() - t0, SLEEP_MS);
    disarm_guard();
    close(fd);
}

static void libc_utimensat(const char *dir)
{
    char path[200];
    struct timespec ts[2] = {{1000000000, 0}, {1234567890, 0}};
    long at = 0, mt = 0;
    int fd, r;

    snprintf(path, sizeof path, "%s/.time64_probe_libc_%ld", dir, (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        report("libc utimensat", V_ERR, "cannot create %s (errno=%d)", path, errno);
        return;
    }
    close(fd);
    r = utimensat(AT_FDCWD, path, ts, 0);
    stat_times(path, &at, &mt);
    if (r != 0)
        report("libc utimensat", V_ERR, "errno=%d", errno);
    else if (at == 1000000000L && mt == 1234567890L)
        report("libc utimensat", V_WORKS, "mtime set to %ld", mt);
    else
        report("libc utimensat", V_NOOP, "ret=0 but atime=%ld mtime=%ld", at, mt);
    unlink(path);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    unsigned long vdso = getauxval(AT_SYSINFO_EHDR);
    struct sigaction sa;
    int i, libc_bad = 0, raw_bad = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART: a hung call must return EINTR */
    sigaction(SIGALRM, &sa, NULL);

    printf("time64_probe: sizeof(time_t)=%zu sizeof(struct timespec)=%zu sizeof(long)=%zu\n",
           sizeof(time_t), sizeof(struct timespec), sizeof(long));
    printf("time64_probe: AT_SYSINFO_EHDR=0x%lx (%s)\n", vdso, vdso ? "vDSO present" : "no vDSO");
    printf("time64_probe: time32 shim %s\n",
           &camyn_time32_shim_active ? "LINKED (libc clock_gettime/timerfd use the 32-bit syscalls)"
                                     : "NOT linked (stock musl behaviour)");

    printf("\n== 1. clock sources, 3 rounds one second apart\n");
    section_rounds();

    printf("\n== 2. RAW time64 syscalls (what stock musl would call), with 32-bit controls\n");
    raw_clock_gettime64(CLOCK_MONOTONIC, "MONOTONIC");
    raw_clock_gettime64(CLOCK_REALTIME, "REALTIME");
    raw_clock_gettime64(CLOCK_BOOTTIME, "BOOTTIME");
    raw_clock_getres64();
    raw_sleepers();
    raw_timerfd();
    raw_utimensat(dir);

    printf("\n== 3. LIBC functions (this is what our programs call)\n");
    in_libc_section = 1;
    libc_clock_check("MONOTONIC", CLOCK_MONOTONIC);
    libc_clock_check("REALTIME", CLOCK_REALTIME);
    libc_clock_check("BOOTTIME", CLOCK_BOOTTIME);
    libc_clock_advance();
    libc_time_check();
    libc_sleepers();
    libc_threads();
    libc_timerfd();
    libc_utimensat(dir);

    printf("\n== SUMMARY (shim %s)\n", &camyn_time32_shim_active ? "linked" : "not linked");
    for (i = 0; i < nres; i++) {
        printf("  %-7s %-6s %s\n", results[i].libc_level ? "libc" : "raw", vname[results[i].v],
               results[i].name);
        if (results[i].v != V_WORKS) {
            if (results[i].libc_level)
                libc_bad++;
            else if (strstr(results[i].name, "control") == NULL)
                raw_bad++;
        }
    }
    printf("time64_probe: raw time64 syscalls not working: %d; libc checks not working: %d\n", raw_bad,
           libc_bad);
    printf("time64_probe: %s\n", libc_bad == 0 ? "RESULT PASS (libc time functions all work)"
                                               : "RESULT FAIL (libc time functions broken)");
    return libc_bad > 255 ? 255 : libc_bad;
}
