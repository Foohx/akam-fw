/*
 * time32_shim.c -- link-time replacement for musl's time64 clock entry points.
 *
 * WHY
 * The Anyka AK3918 runs a vendor Linux 4.4.302 kernel that has no time64
 * system calls, yet answers some of them with "success" instead of ENOSYS.
 * Measured on the camera (tests/native/time64_probe.c,
 * docs/akam_v1_kernel_time64.md):
 *   - syscall 403 (clock_gettime64) returns 0 and does NOT write the
 *     timespec, at least for CLOCK_MONOTONIC and CLOCK_BOOTTIME;
 *   - syscall 263 (clock_gettime32) is correct for every clock.
 * Our binaries are built against musl >= 1.2 with a 64-bit time_t, and musl's
 * clock_gettime() tries syscall 403 first and only falls back to 263 on
 * ENOSYS. It therefore never falls back and hands the caller an unwritten
 * buffer: every monotonic timeout (Dropbear, the ak_rtsp day/night lock) was
 * garbage.
 *
 * WHAT
 * This file defines, with 32-bit syscalls only:
 *   clock_gettime, __clock_gettime   (musl calls __clock_gettime internally:
 *                                     time(), clock(), timespec_get(),
 *                                     pthread_cond_timedwait, sem_timedwait,
 *                                     pthread_mutex_timedlock, ...)
 *   timerfd_settime, timerfd_gettime (musl always uses the time64 syscalls
 *                                     for the "old value" / get paths)
 *   timerfd_create                   (no time argument; defined only because
 *                                     it lives in the same musl archive member
 *                                     as the two above, which must not be
 *                                     extracted or the link has duplicates)
 * Every other time-taking musl call (nanosleep, clock_nanosleep, poll, ppoll,
 * select, pselect, futex timed waits, utimensat, clock_getres) already uses
 * the 32-bit syscall for any value that fits in 32 bits, which is every
 * value we pass; they are checked on the camera by time64_probe, not wrapped.
 *
 * HOW IT WINS OVER libc.a
 * This object is linked explicitly. In a static link the archive member that
 * defines clock_gettime/__clock_gettime (musl clock_gettime.o) is only pulled
 * in to satisfy an undefined symbol; both of its symbols are already defined
 * here, so it is never extracted and there is no duplicate definition.
 * tests/time32_shim_test.sh proves this on the final binaries.
 *
 * LIMITS
 * Seconds are 32-bit on the wire, so CLOCK_REALTIME wraps in 2038 (the
 * vendor kernel cannot do better). Values from the kernel are sign extended.
 * The functions keep no state, so they are thread safe.
 *
 * ARM EABI only (the camera); on other targets the file compiles to nothing
 * so a host build of the sources is not affected.
 */
#if defined(__arm__) && defined(__linux__)

#include <errno.h>
#include <stdint.h>
#include <sys/timerfd.h>
#include <time.h>

#define AKAM_NR_CLOCK_GETTIME32 263
#define AKAM_NR_TIMERFD_CREATE 350
#define AKAM_NR_TIMERFD_SETTIME32 353
#define AKAM_NR_TIMERFD_GETTIME32 354

/* Marker used by the probe to tell whether the shim is linked in. */
const int akam_time32_shim_active = 1;

static inline long akam_svc4(long nr, long a, long b, long c, long d)
{
    register long r7 __asm__("r7") = nr;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    __asm__ __volatile__("svc 0"
                         : "+r"(r0)
                         : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                         : "memory");
    return r0;
}

static int akam_ret(long r)
{
    if ((unsigned long)r > (unsigned long)-4096L)
        return errno = (int)-r, -1;
    return (int)r;
}

static int akam_fits32(time_t s)
{
    return s >= INT32_MIN && s <= INT32_MAX;
}

int __clock_gettime(clockid_t clk, struct timespec *ts)
{
    long ts32[2] = {0, 0};
    long r;

    if (!ts) /* the kernel would answer EFAULT; do not fault in user space */
        return errno = EFAULT, -1;
    r = akam_svc4(AKAM_NR_CLOCK_GETTIME32, (long)clk, (long)ts32, 0, 0);
    if (r == 0) {
        ts->tv_sec = ts32[0];
        ts->tv_nsec = ts32[1];
        return 0;
    }
    return akam_ret(r);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
    __attribute__((alias("__clock_gettime")));

int timerfd_create(int clockid, int flags)
{
    return akam_ret(akam_svc4(AKAM_NR_TIMERFD_CREATE, clockid, flags, 0, 0));
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value)
{
    long in32[4], old32[4] = {0, 0, 0, 0};
    long r;

    if (!new_value)
        return errno = EFAULT, -1;
    if (!akam_fits32(new_value->it_interval.tv_sec) ||
        !akam_fits32(new_value->it_value.tv_sec))
        return errno = ENOTSUP, -1;
    in32[0] = new_value->it_interval.tv_sec;
    in32[1] = new_value->it_interval.tv_nsec;
    in32[2] = new_value->it_value.tv_sec;
    in32[3] = new_value->it_value.tv_nsec;
    r = akam_svc4(AKAM_NR_TIMERFD_SETTIME32, fd, flags, (long)in32, (long)old32);
    if (r == 0 && old_value) {
        old_value->it_interval.tv_sec = old32[0];
        old_value->it_interval.tv_nsec = old32[1];
        old_value->it_value.tv_sec = old32[2];
        old_value->it_value.tv_nsec = old32[3];
    }
    return akam_ret(r);
}

int timerfd_gettime(int fd, struct itimerspec *cur)
{
    long cur32[4] = {0, 0, 0, 0};
    long r;

    if (!cur)
        return errno = EFAULT, -1;
    r = akam_svc4(AKAM_NR_TIMERFD_GETTIME32, fd, (long)cur32, 0, 0);
    if (r == 0) {
        cur->it_interval.tv_sec = cur32[0];
        cur->it_interval.tv_nsec = cur32[1];
        cur->it_value.tv_sec = cur32[2];
        cur->it_value.tv_nsec = cur32[3];
    }
    return akam_ret(r);
}

#endif /* __arm__ && __linux__ */
