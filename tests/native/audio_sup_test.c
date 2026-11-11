/* Host test of audio.c (G07): the capture supervisor (retry with backoff instead of giving up after 5 read
 * errors) and audio.mic_enable=0 (capture never opened). The device is faked by wrapping open/read/ioctl. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "ak_rtsp.h"
#include "audio.h"

#define AUDIO_BACKOFF_UNIT_MS_T 20 /* matches -DAUDIO_BACKOFF_UNIT_MS in audio_sup_test.sh */
static int g_fail;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s:%d: %s (%s)\n", __FILE__, __LINE__, m, #c); g_fail++; } } while (0)

/* ---- fake device ---- */
int __real_open(const char *path, int flags, ...);
static volatile int g_cap_fd = -1;
static volatile int g_cap_opens;
static volatile int g_open_fail;   /* next N opens of the capture device fail */
static volatile int g_read_fail;   /* next N reads fail (EIO) */
static volatile int g_mic = 1;

int __wrap_open(const char *path, int flags, ...)
{
    if (!strncmp(path, "/dev/pcm", 8)) {
        if (!strcmp(path, DEV_PCM_CAPTURE)) {
            if (g_open_fail > 0) { g_open_fail--; errno = ENODEV; return -1; }
            g_cap_opens++;
            g_cap_fd = __real_open("/dev/null", O_RDONLY);
            return g_cap_fd;
        }
        return __real_open("/dev/null", O_RDWR);
    }
    return __real_open(path, flags);
}

ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    if (fd != g_cap_fd || fd < 0) return __real_read(fd, buf, n);
    if (g_read_fail > 0) { g_read_fail--; errno = EIO; return -1; }
    usleep(3000); /* a period takes time */
    memset(buf, 0x11, n);
    return (ssize_t)n;
}

int __wrap_ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return 0; }

/* audio_out stubs */
static int g_ao_starts;
int audio_out_start(void) { g_ao_starts++; return 0; }
void audio_out_stop(void) {}
int audio_out_mic_enable(void) { return g_mic; }

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Polls audio_get_frame until one comes, at most ms. */
static int wait_frame(int ms, uint64_t *first, size_t *n)
{
    int16_t pcm[AUDIO_PERIOD_SAMPLES];
    int64_t t0 = now_ms(), te;
    while (now_ms() - t0 < ms) {
        int r = audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, n, first, &te);
        if (r == 1) return 1;
        if (r < 0) return -1;
        usleep(2000);
    }
    return 0;
}

static void drain(void)
{
    int16_t pcm[AUDIO_PERIOD_SAMPLES];
    size_t n; uint64_t f; int64_t t;
    while (audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, &n, &f, &t) == 1) {}
}

int main(void)
{
    uint64_t first, f2;
    size_t n, n2;

    /* the backoff schedule */
    {
        static const int want[] = { 1, 2, 4, 8, 16, 32, 60, 60, 60 };
        int ok = 1;
        for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) ok &= audio_backoff_units(i) == want[i];
        CHECK(ok, "backoff 1,2,4,8,16,32 then 60 (units)");
        CHECK(audio_backoff_units(1000000) == 60, "backoff stays at 60");
    }

    /* normal start */
    g_mic = 1;
    CHECK(audio_start() == 0, "audio_start ok");
    CHECK(audio_mic_enabled() == 1, "mic enabled");
    CHECK(wait_frame(1000, &first, &n) == 1 && n == AUDIO_PERIOD_SAMPLES, "frames flow");
    CHECK(g_cap_opens == 1, "capture opened once");

    /* G07: 5 read errors in a row do not stop the capture for good */
    drain();
    uint64_t prev_end = 0;
    for (int i = 0; i < 30; i++) {          /* newest frame before the fault */
        if (wait_frame(100, &first, &n) == 1) prev_end = first + n;
    }
    g_read_fail = 5;
    usleep(400000);
    CHECK(g_cap_opens == 2, "after 5 failed reads the device is closed and opened again");
    CHECK(audio_mic_restarts() == 1, "one recovery counted");
    drain();
    CHECK(wait_frame(1000, &f2, &n2) == 1, "frames flow again after the recovery");
    CHECK(f2 >= prev_end + 100, "the sample index jumps by the outage (RTP timeline keeps the gap)");
    {
        int16_t pcm[AUDIO_PERIOD_SAMPLES];
        int64_t t;
        CHECK(audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, &n2, &f2, &t) >= 0, "audio_get_frame still reports running (not -1)");
    }

    /* two re-opens that fail, then success; backoff between them */
    g_open_fail = 2;
    g_read_fail = 5;
    int64_t t0 = now_ms();
    while (g_cap_opens < 3 && now_ms() - t0 < 5000) usleep(5000);
    int64_t dt = now_ms() - t0;
    CHECK(g_cap_opens == 3, "capture recovered after two failed opens");
    CHECK(dt >= 7 * AUDIO_BACKOFF_UNIT_MS_T - 30, "waited 1 + 2 + 4 units between the tries");
    drain();
    CHECK(wait_frame(1000, &f2, &n2) == 1, "frames flow after the second recovery");

    /* audio_stop is prompt even in the middle of a long backoff */
    g_open_fail = 1000;
    g_read_fail = 5;
    usleep(300000);
    t0 = now_ms();
    audio_stop();
    CHECK(now_ms() - t0 < 600, "audio_stop does not wait for the backoff");
    g_open_fail = 0;
    {
        int16_t pcm[AUDIO_PERIOD_SAMPLES];
        int64_t t;
        CHECK(audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, &n, &first, &t) == -1, "stopped: -1");
    }

    /* the device is not available at start: audio_start still succeeds and the thread keeps trying */
    int opens0 = g_cap_opens;
    int ao0 = g_ao_starts;
    g_open_fail = 2;
    CHECK(audio_start() == 0, "start with the device failing: 0, retried in the background");
    t0 = now_ms();
    while (g_cap_opens == opens0 && now_ms() - t0 < 5000) usleep(5000);
    CHECK(g_cap_opens == opens0 + 1, "the first open that works is used");
    CHECK(wait_frame(1000, &first, &n) == 1, "frames flow after a late start");
    CHECK(g_ao_starts - ao0 == 1, "speaker (audio_out) started once the late capture runs");
    audio_stop();

    /* G07: mic disabled: the device is never opened and there is no audio */
    g_mic = 0;
    opens0 = g_cap_opens;
    CHECK(audio_start() == 0, "start with mic off: 0");
    usleep(100000);
    CHECK(audio_mic_enabled() == 0, "mic reported off");
    CHECK(g_cap_opens == opens0, "capture device never opened");
    {
        int16_t pcm[AUDIO_PERIOD_SAMPLES];
        int64_t t;
        CHECK(audio_get_frame_ts(pcm, AUDIO_PERIOD_SAMPLES, &n, &first, &t) == -1, "no frames, reported as not running");
    }
    audio_stop();

    if (g_fail) { printf("audio_sup_test: %d failure(s)\n", g_fail); return 1; }
    printf("audio_sup_test: all passed\n");
    return 0;
}
