/*
 * time_probe.c -- hardware probe for the Camyn clock bug (static ARM binary).
 *
 * Why it exists: on the camera (kernel 4.4, ARM926, no clock_gettime64
 * syscall) both Dropbear and ak_rtsp behave as if CLOCK_MONOTONIC were
 * garbage ("Timeout before auth" at once; the 60 s day/night lock never
 * holds). This program shows, on the real kernel, what every time source
 * returns, so the cause is proven rather than guessed.
 *
 * Run it from the SD card, twice if possible (it already samples 3 times,
 * one second apart), and send the whole output back:
 *     ./time_probe | tee /tmp/sd/time_probe.txt
 *
 * For each round it prints, with the raw return value and errno:
 *   libc  : clock_gettime(MONOTONIC / REALTIME / BOOTTIME) into a buffer
 *           pre-filled with 0x5a (a result that still has 0x5a5a... means
 *           the call "succeeded" without writing the buffer)
 *   raw263: svc with the legacy 32-bit syscall (struct of two 32-bit words)
 *   raw403: svc with clock_gettime64 (ENOSYS = -38 expected on a 4.4 kernel)
 *   gtod  : gettimeofday()
 *   times : raw SYS_times (jiffies, independent of timekeeping/struct ABI)
 *   uptime: first line of /proc/uptime
 * and once: sizeof(time_t), AT_SYSINFO_EHDR (is there a vDSO?).
 *
 * The raw calls use inline "svc 0" so that musl's errno mapping (values in
 * -4095..-1 are errors) cannot hide anything. ARM EABI only.
 *
 * Build: see tests/native/build_probe.sh
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

#define NR_TIMES 43
#define NR_CLOCK_GETTIME32 263
#define NR_CLOCK_GETTIME64 403

#if defined(__arm__)
static long raw_svc(long nr, long a0, long a1)
{
    register long r7 __asm__("r7") = nr;
    register long r0 __asm__("r0") = a0;
    register long r1 __asm__("r1") = a1;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory");
    return r0;
}
#else
static long raw_svc(long nr, long a0, long a1)
{
    (void)nr; (void)a0; (void)a1;
    return -38;
}
#endif

static void libc_clock(const char *name, clockid_t id)
{
    struct timespec ts;
    int r;
    memset(&ts, 0x5a, sizeof ts);
    errno = 0;
    r = clock_gettime(id, &ts);
    printf("  libc  %-9s ret=%d errno=%d sec=%lld nsec=%ld%s\n", name, r, errno,
           (long long)ts.tv_sec, (long)ts.tv_nsec,
           (ts.tv_nsec == 0x5a5a5a5a) ? "  <-- buffer NOT written" : "");
}

static void raw32(long id, const char *name)
{
    long t[2] = {0x5a5a5a5a, 0x5a5a5a5a};
    long r = raw_svc(NR_CLOCK_GETTIME32, id, (long)t);
    printf("  raw263 %-8s ret=%ld sec=%ld nsec=%ld\n", name, r, t[0], t[1]);
}

static void raw64(long id, const char *name)
{
    long t[4] = {0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a, 0x5a5a5a5a};
    long r = raw_svc(NR_CLOCK_GETTIME64, id, (long)t);
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

int main(void)
{
    int round;
    struct timeval tv;
    unsigned long vdso = getauxval(AT_SYSINFO_EHDR);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("time_probe: sizeof(time_t)=%zu sizeof(struct timespec)=%zu sizeof(long)=%zu\n",
           sizeof(time_t), sizeof(struct timespec), sizeof(long));
    printf("time_probe: AT_SYSINFO_EHDR=0x%lx (%s)\n", vdso, vdso ? "vDSO present" : "no vDSO");

    for (round = 0; round < 3; round++) {
        printf("round %d\n", round);
        libc_clock("MONOTONIC", CLOCK_MONOTONIC);
        libc_clock("REALTIME", CLOCK_REALTIME);
        libc_clock("BOOTTIME", CLOCK_BOOTTIME);
        raw32(CLOCK_MONOTONIC, "MONOTONIC");
        raw32(CLOCK_REALTIME, "REALTIME");
        raw64(CLOCK_MONOTONIC, "MONOTONIC");
        memset(&tv, 0x5a, sizeof tv);
        errno = 0;
        printf("  gtod  ret=%d errno=%d sec=%lld usec=%ld\n", gettimeofday(&tv, NULL),
               errno, (long long)tv.tv_sec, (long)tv.tv_usec);
        {
            long tk = raw_svc(NR_TIMES, 0, 0);
            printf("  times raw=%ld (0x%lx)\n", tk, (unsigned long)tk);
        }
        uptime_line();
        if (round < 2)
            sleep(1);
    }
    printf("time_probe: done. Expected sane run: MONOTONIC grows by ~1 s per round "
           "in libc and raw263, raw403 ret=-38.\n");
    return 0;
}
