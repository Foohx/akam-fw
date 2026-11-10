/* stock_hook_driver.c - host test program for src/ak_rtsp/stock_hook.c (see
 * tests/stock_hook_test.sh). Built as "hook_driver", run with LD_PRELOAD=stock_hook.so
 * (host build of the hook on x86_64, STOCK_HOOK_COMM="hook_driver").
 *
 * It plays the part of anyka_ipc on a fake device tree under ROOT (regular files stand in
 * for /dev/pcm..., gpio sysfs ...) and checks that the hook never changes what the libc
 * calls return (values and errno). Exit status 0 = every self-check passed.
 *
 * usage: hook_driver ROOT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("SELFCHECK FAIL: %s (line %d, errno %d)\n", msg, __LINE__, errno); failures++; } } while (0)

struct isp_wrapper { uint32_t flag; uint32_t inner_cmd; void *payload; };

static void pcmpath(char *out, size_t n, const char *root, const char *rel)
{
    snprintf(out, n, "%s%s", root, rel);
}

/* ---- "threads" mode: anyka_ipc is multithreaded; hammer the hook from several threads on
 * one shared playback fd and on private fds, and check that no call result or errno changes. */
struct tctx { const char *root; int shared_fd; int id; int bad; };

static void *worker(void *arg)
{
    struct tctx *t = arg;
    char p[512];
    int16_t pcm[256];
    for (int i = 0; i < 256; i++) pcm[i] = (int16_t)(i * 37 - 4000);
    for (int it = 0; it < 400; it++) {
        uint32_t v = (uint32_t)(it % 7);
        errno = 0;
        if (ioctl(t->shared_fd, 0x80045030, &v) != -1 || errno != ENOTTY) t->bad++;
        errno = 0;
        if (ioctl(t->shared_fd, 0x401c50e0, (void *)0x10) != -1 || errno != ENOTTY) t->bad++;
        if (write(t->shared_fd, pcm, sizeof pcm) != (ssize_t)sizeof pcm) t->bad++;
        snprintf(p, sizeof p, "%s/dev/pcmC0D0p", t->root);
        int fd = open(p, O_WRONLY);
        if (fd < 0) { t->bad++; continue; }
        if (write(fd, pcm, 100) != 100) t->bad++;
        errno = 0;
        if (ioctl(fd, 0x000050e1, (void *)0) != -1 || errno != ENOTTY) t->bad++;
        if (close(fd) != 0) t->bad++;
        snprintf(p, sizeof p, "%s/sys/class/gpio/gpio69/value", t->root);
        fd = open(p, O_WRONLY);
        if (fd < 0 || write(fd, "1", 1) != 1) t->bad++;
        if (fd >= 0) close(fd);
    }
    return NULL;
}

static int threads_mode(const char *root)
{
    char p[512];
    pcmpath(p, sizeof p, root, "/dev/pcmC0D0p");
    int sfd = open(p, O_WRONLY);
    if (sfd < 0) { printf("SELFCHECK FAIL: shared open\n"); return 1; }
    enum { NT = 6 };
    pthread_t th[NT];
    struct tctx cx[NT];
    for (int i = 0; i < NT; i++) {
        cx[i].root = root; cx[i].shared_fd = sfd; cx[i].id = i; cx[i].bad = 0;
        if (pthread_create(&th[i], NULL, worker, &cx[i]) != 0) { printf("SELFCHECK FAIL: pthread_create\n"); return 1; }
    }
    int bad = 0;
    for (int i = 0; i < NT; i++) { pthread_join(th[i], NULL); bad += cx[i].bad; }
    close(sfd);
    printf("THREADS DONE bad=%d\n", bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s ROOT [threads]\n", argv[0]); return 2; }
    const char *root = argv[1];
    if (argc > 2 && !strcmp(argv[2], "threads")) return threads_mode(root);
    char p[512];
    int r;

    /* --- errno fidelity on a classified path that does not exist */
    pcmpath(p, sizeof p, root, "/dev/pcmC9D9p");
    errno = 0;
    r = open(p, O_WRONLY);
    CHECK(r == -1 && errno == ENOENT, "open of a missing pcm device keeps -1/ENOENT");
    errno = 0;
    r = close(987);
    CHECK(r == -1 && errno == EBADF, "close of a bad fd keeps -1/EBADF");

    /* --- playback device: PREPARE, SET_SOURCES, SET_GAIN, SET_SPEAKER, RESUME */
    pcmpath(p, sizeof p, root, "/dev/pcmC0D0p");
    int pfd = open(p, O_WRONLY);
    CHECK(pfd >= 0, "open playback");
    uint32_t prep[7] = { 0, 8000, 2, 16, 1024, 8, 0 };          /* PREPARE */
    errno = 0;
    r = ioctl(pfd, 0x401c50e0, prep);
    CHECK(r == -1 && errno == ENOTTY, "ioctl on a plain file keeps -1/ENOTTY");
    uint32_t v = 6;
    ioctl(pfd, 0x80045010, &v);                                 /* SET_SOURCES = 6 */
    v = 3; ioctl(pfd, 0x80045030, &v);                          /* SET_GAIN = 3 */
    v = 1; ioctl(pfd, 0x80045081, &v);                          /* SET_SPEAKER = 1 */
    ioctl(pfd, 0x000050e1, (void *)0);                          /* RESUME, no argument */
    for (int i = 0; i < 40; i++) {                              /* throttled when identical */
        v = 3; ioctl(pfd, 0x80045030, &v);
    }
    v = 5; ioctl(pfd, 0x80045030, &v);                          /* changed value always logged */

    /* a successful ioctl: FIONREAD on a regular file */
    int cfd = open(p, O_RDONLY);
    int avail = -1;
    r = ioctl(cfd, FIONREAD, &avail);
    CHECK(r == 0 && avail >= 0, "successful ioctl keeps its result");
    close(cfd);

    /* argument that is only partly mapped: the hook must not crash (safe copy) */
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *two = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(two != MAP_FAILED, "anonymous mmap passes through");
    munmap(two + pg, pg);
    ioctl(pfd, 0x401c50e0, two + pg - 8);                       /* size 28, only 8 bytes valid */

    /* --- audio: 60 writes of 1024 bytes of mono-duplicated stereo s16, ~3 ms apart */
    int16_t pcm[512];
    for (int i = 0; i < 256; i++) { pcm[2 * i] = (int16_t)((i % 32) * 400 - 6000); pcm[2 * i + 1] = pcm[2 * i]; }
    for (int i = 0; i < 60; i++) {
        ssize_t w = write(pfd, pcm, sizeof pcm);
        CHECK(w == (ssize_t)sizeof pcm, "playback write returns the count");
        usleep(3000);
    }
    close(pfd);

    /* --- capture device reads (from a file standing in for the mic) */
    pcmpath(p, sizeof p, root, "/dev/pcmC0D0c");
    int mfd = open(p, O_RDONLY);
    char buf[512];
    ssize_t rd = read(mfd, buf, sizeof buf);
    CHECK(rd == 512, "capture read returns the count");
    close(mfd);

    /* --- GPIO sysfs */
    pcmpath(p, sizeof p, root, "/sys/class/gpio/gpio69/value");
    int gfd = open(p, O_WRONLY);
    CHECK(gfd >= 0, "open gpio value");
    CHECK(write(gfd, "1", 1) == 1, "gpio write returns 1");
    CHECK(write(gfd, "0", 1) == 1, "gpio write returns 1 again");
    close(gfd);
    pcmpath(p, sizeof p, root, "/sys/class/gpio/gpio69/direction");
    gfd = open(p, O_RDWR);
    CHECK(write(gfd, "out", 3) == 3, "gpio direction write");
    close(gfd);

    /* --- /dev/mem style mapping */
    pcmpath(p, sizeof p, root, "/dev/mem");
    int memfd = open(p, O_RDONLY);
    uint8_t *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, memfd, 0);
    CHECK(m != MAP_FAILED && m[0] == 'M', "mmap of the fake /dev/mem works and shows the data");
    if (m != MAP_FAILED) munmap(m, 4096);
    close(memfd);

    /* --- audio file read */
    pcmpath(p, sizeof p, root, "/usr/share/prompt.mp3");
    int afd = open(p, O_RDONLY);
    rd = read(afd, buf, 100);
    CHECK(rd == 100, "audio file read");
    close(afd);

    /* --- writev on a pcm fd */
    pcmpath(p, sizeof p, root, "/dev/pcmC0D0p");
    pfd = open(p, O_WRONLY);
    struct iovec iov[2] = { { pcm, 256 }, { pcm + 128, 256 } };
    CHECK(writev(pfd, iov, 2) == 512, "writev returns the total");
    int pfd2 = dup(pfd);
    CHECK(pfd2 >= 0, "dup of a tracked fd");
    close(pfd2);
    close(pfd);

    /* --- ISP tunnel ioctl with parameter blocks (the fake device is a plain file: the ioctl
     * fails with ENOTTY, the hook still sees the call and the payload) */
    pcmpath(p, sizeof p, root, "/dev/isp-param-0");
    int ifd = open(p, O_RDWR);
    uint32_t ccm[81];                                           /* CCM is 324 bytes */
    for (int i = 0; i < 81; i++) ccm[i] = 0x100u * (uint32_t)i + 7;
    struct isp_wrapper w = { 1, 0x40044912u /* CCM */, ccm };
    ioctl(ifd, 0xc0cc5616u, &w);
    ioctl(ifd, 0xc0cc5616u, &w);                                /* same content: not dumped again */
    usleep(150000);                                             /* > the 100 ms rate limit of SET blocks */
    ccm[80] = 0xdeadbeef;                                       /* LAST word of the full block */
    ioctl(ifd, 0xc0cc5616u, &w);                                /* changed block dumped again */
    /* a polled GET block changing at every call: rate limited to one dump per second */
    uint32_t stat[64];
    struct isp_wrapper ws = { 1, 0x80044959u /* unknown size: 256-byte probe */, stat };
    for (int i = 0; i < 300; i++) {
        for (int k = 0; k < 64; k++) stat[k] = (uint32_t)(i * 64 + k);
        ioctl(ifd, 0xc0cc5616u, &ws);
    }
    /* a big block (SHARP, 46584 bytes) must be dumped whole */
    uint8_t *big = malloc(46584);
    for (int i = 0; i < 46584; i++) big[i] = (uint8_t)(i * 7 + 3);
    struct isp_wrapper wb = { 1, 0x40044925u /* SHARP */, big };
    ioctl(ifd, 0xc0cc5616u, &wb);
    free(big);
    /* payload that is only partly mapped (probe of an unknown command): no crash, shorter dump */
    uint8_t *two2 = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(two2 != MAP_FAILED, "anonymous mmap");
    memset(two2, 0x5a, 2 * pg);
    munmap(two2 + pg, pg);
    struct isp_wrapper wp = { 1, 0x80044999u, two2 + pg - 100 };
    ioctl(ifd, 0xc0cc5616u, &wp);
    /* a payload pointer into unmapped memory: nothing dumped, nothing crashes */
    struct isp_wrapper wu = { 1, 0x80044998u, two2 + pg + 64 };
    ioctl(ifd, 0xc0cc5616u, &wu);
    /* VI scalar tunnel (the "pointer" is a number): ignored */
    struct isp_wrapper wv = { 1, 0x103u, (void *)1 };
    ioctl(ifd, 0xc0cc5616u, &wv);
    close(ifd);

    /* --- day/night sysfs: ADC light sensor polled many times, IR LED PWM written */
    for (int i = 0; i < 60; i++) {
        pcmpath(p, sizeof p, root, "/sys/bus/iio/devices/iio:device0/in_voltage0_raw");
        int afd2 = open(p, O_RDONLY);
        if (afd2 >= 0) { char t[16]; if (read(afd2, t, sizeof t) < 0) failures++; close(afd2); }
    }
    pcmpath(p, sizeof p, root, "/sys/class/pwm/pwmchip0/pwm0/duty_cycle");
    for (int i = 0; i < 60; i++) {
        int pw = open(p, O_WRONLY);
        if (pw >= 0) { if (write(pw, "500000", 6) != 6) failures++; close(pw); }
    }
    int pw = open(p, O_WRONLY);
    if (pw >= 0) { if (write(pw, "0", 1) != 1) failures++; close(pw); }

    /* --- a child process (system()) inherits LD_PRELOAD: it must log one CHILD line only */
    if (getenv("HOOK_DRIVER_CHILD")) {
        int rc = system("cat /dev/null; : reboot-marker");
        CHECK(rc == 0, "child process runs normally under the inherited hook");
    }

    /* --- unrelated file: untouched and unlogged */
    pcmpath(p, sizeof p, root, "/tmp_plain.txt");
    int ufd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(ufd >= 0 && write(ufd, "hello", 5) == 5, "plain file write");
    close(ufd);

    printf("DRIVER DONE failures=%d\n", failures);
    return failures ? 1 : 0;
}
