/* Microphone capture thread, see audio.h. The ioctl sequence is the one of audio/mic.c (ground truth: the
 * stock anyka_ipc capture, platforms/anyka-ak3918/audio.c audio_init_capture), run on the hardware on 2026-11-15. */
#include "akpcm.h"
#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cap.h"
#include "log.h"
#include "rtp_clock.h"

#define PCM_CAPTURE "/dev/pcmC0D0c"
#define PERIOD_BYTES 512u
#define PERIOD_COUNT 16u
#define MAX_CONSEC_FAILS 5
#define FAIL_SLEEP_US 200000


static a2k_aring_t g_ring;
static pthread_t g_th;
/* Flags shared by the capture thread and the others: relaxed atomics (plain word loads/stores on ARMv5, and no data race
 * for TSan; the ring has its own mutex). */
static int g_started, g_run, g_done, g_up;
static unsigned g_restarts;
#define LD(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define ST(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELAXED)

#define STEP(name, expr) do { if ((expr) < 0) { a2k_log("audio: step '%s' failed: %s (errno=%d)", name, strerror(errno), errno); goto fail; } } while (0)

/* Opens and configures the capture; the fd, or -1 (logged, nothing left open). */
static int open_capture(void)
{
    int32_t v = 2;
    struct akpcm_pars pars;
    int fd = open(PCM_CAPTURE, O_RDONLY);

    if (fd < 0) {
        a2k_log("audio: open %s: %s", PCM_CAPTURE, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        a2k_log("audio: %s is locked by another process (akam2k_mic running?)", PCM_CAPTURE);
        goto fail;
    }
    STEP("SET_STATE(2)", ioctl(fd, AKPCM_IOC_SET_STATE, &v));
    v = 0;
    if (ioctl(fd, AKPCM_IOC_GET_PERIODS, &v) < 0)
        a2k_log("audio: GET_PERIODS (non-fatal): %s", strerror(errno));
    memset(&pars, 0, sizeof pars);
    pars.rate = A2K_AUDIO_RATE;
    pars.channels = 1;
    pars.bits = 16;
    pars.period_size = PERIOD_BYTES;
    pars.period_count = PERIOD_COUNT;
    STEP("GET_PARS (commit params)", ioctl(fd, AKPCM_IOC_GET_PARS, &pars));
    STEP("RESET_BUF(-1)", ioctl(fd, AKPCM_IOC_RESET_BUF, (void *)(long)-1));
    STEP("SET_MODE(0)", ioctl(fd, AKPCM_IOC_SET_MODE, 0));
    v = A2K_AUDIO_RATE;
    STEP("SET_RATE", ioctl(fd, AKPCM_IOC_SET_RATE, &v));
    v = 2;
    STEP("SET_STATE(2)#2", ioctl(fd, AKPCM_IOC_SET_STATE, &v));
    v = 2;
    STEP("SET_PERIODS(2)", ioctl(fd, AKPCM_IOC_SET_PERIODS, &v));
    return fd;
fail:
    close(fd);
    return -1;
}

/* Retry delay in seconds for the nth consecutive failed (re)open, n from 0: 1, 2, 4, 8, 16, 32, then 60. */
static int backoff_s(unsigned attempt)
{
    static const int t[] = { 1, 2, 4, 8, 16, 32 };
    return attempt < sizeof t / sizeof t[0] ? t[attempt] : 60;
}

/* Sleeps in short slices; 1 when the app is stopping. */
static int wait_s(int s)
{
    long ms = (long)s * 1000;
    while (ms > 0 && LD(g_run) && !a2k_stop) {
        long step = ms > 100 ? 100 : ms;
        usleep((useconds_t)(step * 1000));
        ms -= step;
    }
    return !LD(g_run) || a2k_stop;
}

/* Periods (1 s of audio) a capture must deliver before the backoff of the retries is forgotten: a device that opens but
 * then fails to read again must not be retried (and logged) every couple of seconds for ever. */
#define STABLE_PERIODS 32

static void *capture_thread(void *arg)
{
    int fd = -1, fails = 0, was_up = 0;
    unsigned attempt = 0, good = 0;
    int64_t last_pub_us = 0; /* when the last published period ended: the start of a hole if the capture is lost */
    int first_open = 1;
    int16_t buf[A2K_APERIOD_SAMPLES];
    (void)arg;

    while (LD(g_run) && !a2k_stop) {
        ssize_t n;
        if (fd < 0) {
            if (!first_open && wait_s(backoff_s(attempt)))
                break;
            fd = open_capture();
            if (fd < 0) {
                if (first_open)
                    a2k_log("audio: capture not available, retrying with backoff (video is not affected)");
                first_open = 0;
                attempt++;
                continue;
            }
            if (last_pub_us) { /* keep the RTP timeline: the lost time counts as samples that were not delivered */
                a2k_aring_gap(&g_ring, rtp_mono_us() - last_pub_us);
                ST(g_restarts, LD(g_restarts) + 1); /* one writer */
                a2k_log("audio: capture recovered (attempt %u)", attempt + 1);
            } else if (was_up || attempt) {
                a2k_log("audio: capture running after %u failed attempt(s)", attempt);
            } else {
                a2k_log("audio: capture running (%s, %d Hz mono 16 bit, %u-byte periods)", PCM_CAPTURE, A2K_AUDIO_RATE,
                        PERIOD_BYTES);
            }
            first_open = 0;
            was_up = 1;
            good = 0; /* attempt stays until the capture proves stable */
            fails = 0;
            ST(g_up, 1);
            continue;
        }
        n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (fails == 0) /* first of a streak only: no log flood on a dying device */
                a2k_log("audio: read: %s", strerror(errno));
            if (++fails >= MAX_CONSEC_FAILS) {
                a2k_log("audio: read failed %d times in a row, closing and re-opening the capture", fails);
                ST(g_up, 0);
                close(fd);
                fd = -1;
                attempt++;
                continue;
            }
            usleep(FAIL_SLEEP_US);
            continue;
        }
        fails = 0;
        if (n < 2) { /* nothing (or half a sample): not a period */
            usleep(10000);
            continue;
        }
        last_pub_us = rtp_mono_us();
        (void)a2k_aring_publish(&g_ring, buf, (size_t)n / sizeof(int16_t), last_pub_us);
        if (good < STABLE_PERIODS && ++good == STABLE_PERIODS)
            attempt = 0;
    }
    ST(g_up, 0);
    if (fd >= 0)
        close(fd);
    ST(g_done, 1);
    return NULL;
}

int a2k_audio_start(void)
{
    pthread_attr_t at;
    int rc;

    if (LD(g_started))
        return 0;
    a2k_aring_init(&g_ring);
    ST(g_run, 1);
    ST(g_done, 0);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    rc = pthread_create(&g_th, &at, capture_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        a2k_log("audio: cannot start the capture thread (%d): no audio, video unaffected", rc);
        ST(g_run, 0);
        return -1;
    }
    ST(g_started, 1);
    return 0;
}

void a2k_audio_stop(void)
{
    int i;
    if (!LD(g_started))
        return;
    ST(g_run, 0);
    /* A read() blocks for one period (32 ms): the thread ends at once. If a wedged driver keeps it, do not hang the
     * teardown (the process is about to exit, the OS closes the device). */
    for (i = 0; i < 40 && !LD(g_done); i++)
        usleep(50000);
    if (!LD(g_done))
        a2k_log("audio: capture thread still running after 2 s");
    else
        (void)pthread_join(g_th, NULL);
    ST(g_started, 0);
}

int a2k_audio_up(void) { return LD(g_started) && LD(g_up); }
int a2k_audio_enabled(void) { return LD(g_started); }
a2k_aring_t *a2k_audio_ring(void) { return &g_ring; }
unsigned a2k_audio_restarts(void) { return LD(g_restarts); }
