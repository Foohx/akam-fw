#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <pthread.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "audio.h"
#include "audio_out.h"
#include "rtp_clock.h"

static int g_audio_fd = -1;

/* Opened alongside capture. A prior debugging session
 * found the likely reason SET_MODE(0) crashed the kernel on this fd in
 * every earlier session: a fresh, path-tagged ground-truth capture (isp_hook.c
 * extended with open()/read()/write() hooks) shows real anyka_ipc opens this
 * exact device O_WRONLY (flags=0x1), not O_RDWR like this code used to —
 * and calls the identical SET_MODE(0) successfully there. Switched the open()
 * call below to O_WRONLY and re-added SET_MODE(0) to test that theory
 * directly. If it still crashes, that disproves the open-mode theory (still
 * useful — narrows the search further) rather than being a regression to
 * avoid. */
static int g_audio_playback_fd = -1;
static int g_audio_loopback_fd = -1;

static int g_audio_out_active = 0; /* audio_out owns the playback device */
static volatile int g_playback_running = 0;
static volatile int g_playback_thread_started = 0;
static pthread_t    g_playback_tid;

static volatile int g_audio_running = 0;
static pthread_t    g_audio_tid;

static void audio_close_aux(void); /* closes g_audio_playback_fd/g_audio_loopback_fd */

#define AUDIO_RING_SLOTS 8

struct audio_slot {
    int16_t  samples[AUDIO_PERIOD_SAMPLES];
    size_t   n_samples;
    uint64_t first_sample; /* capture-stream index of samples[0] */
    int64_t  t_end_us;     /* CLOCK_MONOTONIC when this period's read() returned */
};

/* Samples delivered by read() since audio_start(); written by the capture
 * thread only. */
static uint64_t g_samples_total = 0;

static struct audio_slot g_ring[AUDIO_RING_SLOTS];
static volatile uint32_t g_ring_write = 0; /* total periods produced */
static volatile uint32_t g_ring_read  = 0; /* total periods consumed */
/* Single-core ARM926 (ARMv5: no ldrex, no __atomic libcalls linked): a
 * compiler barrier orders the slot accesses against the counters. */
#define RING_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Reports exactly which step failed (added after a real hardware run came
 * back with a bare "audio_init: config ioctl: Not a tty" (ENOTTY) and no
 * way to tell which of the 9 calls it was — turned out to be GET_CAPS
 * specifically). */
#define AUDIO_IOC(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed: %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
            goto fail; \
        } \
    } while (0)

/* Same as AUDIO_IOC but non-fatal — for calls confirmed to be pure
 * informational reads in the real capture (their result was never fed into
 * any later SET call, only logged). GET_CAPS (nr=0x81) failed with ENOTTY
 * on a real hardware test of a capture-only client — best-supported theory
 * is it needs some cross-stream precondition only met when anyka_ipc's
 * playback+capture+loopback streams are all open together, which a
 * capture-only client like this one has no reason to replicate. Applied to
 * the other pure-GET informational
 * calls too on the same reasoning, since they were never load-bearing. */
#define AUDIO_IOC_WARN(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed (non-fatal): %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
        } \
    } while (0)

/* Hard-capped retry count for both the capture and playback streaming loops,
 * added 2026-07-04 after a real hardware test: with read() returning EPERM
 * immediately (not blocking), the previous unbounded "retry every 10ms
 * forever" loop hammered the driver at ~100Hz with no backoff, and the
 * camera hard-rebooted partway through that run. A capture/playback loop must never retry
 * a failing syscall indefinitely at that rate — give up after a handful of
 * consecutive failures and stop cleanly instead. */
#define AUDIO_MAX_CONSECUTIVE_FAILS 5

/* G07: after that many consecutive read errors the capture is not abandoned: the device is closed and the
 * whole open+configure sequence is retried after 1, 2, 4 ... 60 s (a driver hiccup used to leave the stream
 * video-only until reboot). The first try after a failure never comes sooner than 1 s, so the 100 Hz
 * hammering that once rebooted the camera cannot happen. The units are macros so that the host test can
 * shrink them. */
#ifndef AUDIO_BACKOFF_UNIT_MS
#define AUDIO_BACKOFF_UNIT_MS 1000
#endif
#ifndef AUDIO_FAIL_SLEEP_US
#define AUDIO_FAIL_SLEEP_US 200000
#endif
#define AUDIO_BACKOFF_MAX_STEPS 6 /* 1,2,4,8,16,32 then 60 */

/* Retry delay in units for the nth consecutive failed (re)open, n from 0: 1, 2, 4, 8, 16, 32, 60, 60 ... */
int audio_backoff_units(unsigned attempt)
{
    if (attempt >= AUDIO_BACKOFF_MAX_STEPS) return 60;
    return 1 << attempt;
}

/* Configures the playback fd matching the real, path-tagged ground-truth
 * order: SET_STATE(0) -> SET_MODE(0) ->
 * SET_RATE(8000) -> SET_PERIODS(4) -> SET_STATE(4). SET_MODE(0) is skipped
 * below — confirmed to still crash the kernel even with the
 * O_WRONLY open fix, same signature every time. Root cause is
 * unresolved (unlike capture_ioctl's dispatch, playback_ioctl's real
 * dispatch hasn't been decompiled/read yet — that would be the next Ghidra
 * step if playback support is ever needed). Skipped for now since playback
 * isn't required for the actual goal (capture) and this crash was
 * repeatedly blocking us from observing capture's own read() result. All
 * steps here are non-fatal (AUDIO_IOC_WARN) — this is a best-effort peer
 * stream for capture's benefit, not something we depend on ourselves. */
static void audio_configure_playback(void)
{
    if (g_audio_playback_fd < 0) return;

    int32_t v;

    v = 0;
    AUDIO_IOC_WARN("playback SET_STATE(0)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC_WARN("playback SET_RATE", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_RATE, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_PERIODS(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_PERIODS, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_STATE(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    printf("[ak_rtsp] audio: %s configured+started (%dHz/%dch/%dbit)\n",
           DEV_PCM_PLAYBACK, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
}

/* Keeps the playback fd actively streaming for the duration of capture —
 * the real test of a theory:
 * whether capture's read() EPERM needs a sibling stream genuinely running,
 * not merely open/configured. Writes silence; audible output is a non-issue
 * since the speaker path is out of scope for this project. Same
 * retry-cap/backoff discipline as the capture loop. */
/* Minimum duration of one playback loop pass: 90% of a period
 * (256 samples at 8 kHz = 32 ms), so the stream stays slightly ahead. */
#define PLAYBACK_MIN_PERIOD_US \
    ((long)AUDIO_PERIOD_SAMPLES * 1000000L / AUDIO_SAMPLE_RATE * 9 / 10)

static void *audio_playback_loop(void *arg)
{
    (void)arg;

    int16_t silence[AUDIO_PERIOD_SAMPLES];
    memset(silence, 0, sizeof(silence));

    int consecutive_fails = 0;
    while (g_playback_running) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ssize_t n = write(g_audio_playback_fd, silence, sizeof(silence));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] playback write");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr,
                        "[audio] playback write failed %d times in a row — "
                        "stopping playback stream thread\n", consecutive_fails);
                g_playback_running = 0;
                break;
            }
            usleep(200000);
            continue;
        }
        consecutive_fails = 0;

        /* Safety net against a CPU spin: a healthy playback write blocks for
         * about one period (32 ms) once the driver buffer is full, so this
         * never sleeps. If the driver ever accepts the data without blocking
         * (stream not really running, overrun dropped), pace the loop at
         * slightly faster than real time instead of spinning at 100% CPU. */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed_us = (long)(t1.tv_sec - t0.tv_sec) * 1000000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000L;
        if (elapsed_us < 0) elapsed_us = 0;
        if (elapsed_us < PLAYBACK_MIN_PERIOD_US)
            usleep((useconds_t)(PLAYBACK_MIN_PERIOD_US - elapsed_us));
    }
    return NULL;
}

/* Config sequence replayed byte-for-byte from a real, path-tagged anyka_ipc
 * capture session — superseding an earlier
 * sequence, which had the wrong SET_STATE/SET_PERIODS values and was
 * missing RESET_BUF entirely, due to a since-corrected fd mislabeling bug in
 * the original capture method). Several ioctl semantics are still
 * unconfirmed (see ak_ioctls.h comments) — this deliberately replays the
 * exact same calls/values in the exact same order rather than trying to
 * "clean up" a sequence we don't fully understand yet.
 *
 * Split from playback config: the real
 * capture shows playback's SET_MODE(0) succeeds while capture is ALREADY
 * ACTIVELY READING (interleaved reads visible in the log between playback's
 * config ioctls), not merely after capture's config ioctls returned success.
 * An earlier "configure capture fully, then playback" reorder crashed on
 * SET_MODE again — this function now only opens+configures the capture fd;
 * the caller (audio_start()) starts the capture read thread before calling
 * audio_init_playback(), so playback configures while capture is genuinely
 * mid-stream, matching the real timing as closely as this code can. */
static int audio_init_capture(void)
{
    g_audio_fd = open(DEV_PCM_CAPTURE, O_RDONLY);
    if (g_audio_fd < 0) {
        perror("audio_init: open " DEV_PCM_CAPTURE);
        return -1;
    }

    int32_t v;
    struct akpcm_pars pars;

    /* Real order (ground truth): SET_STATE(2) -> GET_PERIODS -> GET_PARS
     * -> RESET_BUF(-1) -> SET_MODE(0) -> SET_RATE(8000) -> SET_STATE(2) again
     * -> SET_PERIODS(2). Note SET_STATE's value here is 2, not 0/4 as an
     * earlier attempt incorrectly assumed — replay the exact value, its
     * meaning is still unconfirmed. */
    v = 2;
    AUDIO_IOC("SET_STATE(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    AUDIO_IOC_WARN("GET_PERIODS#1", ioctl(g_audio_fd, AKPCM_IOC_GET_PERIODS, &v));

    /* GET_PARS is misnamed — a Ghidra relocation-fix session
     * decompiled ak_pcm.ko's real capture_ioctl dispatch and found this exact
     * ioctl (req=0x401c50e0) does a copy_FROM_user of this 28-byte struct into the
     * kernel, validates rate!=0, period_size!=0 && period_size%64==0, and
     * period_count in [1,0x50], computes the hardware rate divisor, and ONLY ON
     * SUCCESS sets the ready bit at +0x304 that capture_read() requires (bit 0x2).
     * Every previous session called this with a zeroed struct (treating
     * it as a pure informational GET) — that fails validation every time
     * (rate=0), which is exactly the observed EINVAL, and is why the ready bit
     * was never set regardless of what SET_MODE/SET_RATE/SET_STATE/SET_PERIODS
     * did. Fill it with the real captured values instead. */
    memset(&pars, 0, sizeof(pars));
    pars.format       = 0;
    pars.rate         = AUDIO_SAMPLE_RATE;
    pars.channels     = AUDIO_CHANNELS;
    pars.bits         = AUDIO_BITS;
    pars.period_size  = AUDIO_PERIOD_BYTES;
    pars.period_count = 16;
    AUDIO_IOC("GET_PARS (commit params)", ioctl(g_audio_fd, AKPCM_IOC_GET_PARS, &pars));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer.
     * Real capture calls this with arg=-1, not NULL/0 (see ak_ioctls.h) —
     * previously entirely missing from this sequence, a leading candidate
     * for (part of) the actual EPERM fix. */
    AUDIO_IOC("RESET_BUF(-1)", ioctl(g_audio_fd, AKPCM_IOC_RESET_BUF, (void *)(long)-1));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer */
    AUDIO_IOC("SET_MODE(0)", ioctl(g_audio_fd, AKPCM_IOC_SET_MODE, 0));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC("SET_RATE(8000)", ioctl(g_audio_fd, AKPCM_IOC_SET_RATE, &v));

    v = 2;
    AUDIO_IOC("SET_STATE(2)#2", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    v = 2;
    AUDIO_IOC("SET_PERIODS(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_PERIODS, &v));

    printf("[ak_rtsp] audio: %s opened, capture configured (%dHz/%dch/%dbit, "
           "period=%d samples)\n",
           DEV_PCM_CAPTURE, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS,
           AUDIO_PERIOD_SAMPLES);
    return 0;

fail:
    close(g_audio_fd);
    g_audio_fd = -1;
    return -1;
}

/* Opens+configures playback/loopback — called AFTER the capture read thread
 * is already running (see audio_start() and the comment above
 * audio_init_capture()). Best-effort: not fatal if these fail, capture is
 * already working by this point regardless. O_WRONLY for playback matches
 * real anyka_ipc's own open() flags — was O_RDWR in every earlier
 * session. */
static void audio_init_playback(void)
{
    /* Speaker output (specs/audio.md): when enabled and the device sequence works,
     * audio_out owns the playback fd and its own thread, and the legacy
     * silence loop below is not used. When disabled, blocked by the crash
     * guard or failed, audio_out leaves nothing open and the legacy path
     * runs exactly as before. */
    if (audio_out_start() == 0) {
        g_audio_out_active = 1;
        g_audio_loopback_fd = open(DEV_PCM_LOOPBACK, O_RDWR);
        if (g_audio_loopback_fd < 0)
            fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                    DEV_PCM_LOOPBACK, strerror(errno));
        printf("[ak_rtsp] audio: playback owned by audio_out, loopback_fd=%d "
               "(held open, unconfigured)\n", g_audio_loopback_fd);
        return;
    }

    g_audio_playback_fd = open(DEV_PCM_PLAYBACK, O_WRONLY);
    if (g_audio_playback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_PLAYBACK, strerror(errno));
    else
        audio_configure_playback();
    g_audio_loopback_fd = open(DEV_PCM_LOOPBACK, O_RDWR);
    if (g_audio_loopback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_LOOPBACK, strerror(errno));
    printf("[ak_rtsp] audio: playback_fd=%d (configured+started) loopback_fd=%d "
           "(held open, unconfigured)\n",
           g_audio_playback_fd, g_audio_loopback_fd);
}

static void audio_close_aux(void)
{
    if (g_audio_playback_fd >= 0) { close(g_audio_playback_fd); g_audio_playback_fd = -1; }
    if (g_audio_loopback_fd >= 0) { close(g_audio_loopback_fd); g_audio_loopback_fd = -1; }
}

static void audio_close(void)
{
    if (g_audio_fd >= 0) {
        close(g_audio_fd);
        g_audio_fd = -1;
    }
    audio_close_aux();
}

/* Sleeps ms in short slices so that audio_stop() never waits long. Returns 0, or 1 when stopping. */
static int audio_wait_ms(long ms)
{
    while (ms > 0 && g_audio_running) {
        long step = ms > 100 ? 100 : ms;
        usleep((useconds_t)(step * 1000));
        ms -= step;
    }
    return !g_audio_running;
}

static volatile uint32_t g_audio_reopens; /* successful re-opens after a failure */
static volatile int g_playback_pending;   /* start failed to open the capture: playback/audio_out not started yet */

/* Starts audio_out (or the legacy silence stream) once the capture is really streaming. */
static void audio_begin_playback(void)
{
    audio_init_playback();
    if (g_audio_playback_fd >= 0) {
        g_playback_running = 1;
        if (pthread_create(&g_playback_tid, NULL, audio_playback_loop, NULL) != 0) {
            perror("[audio] playback pthread_create");
            g_playback_running = 0;
        } else {
            g_playback_thread_started = 1;
        }
    }
}

static void *audio_capture_loop(void *arg)
{
    (void)arg;

    int consecutive_fails = 0;
    unsigned attempt = 0;        /* consecutive failed (re)opens */
    int64_t lost_since_us = 0;   /* when the stream was lost, 0 = it is not */
    while (g_audio_running) {
        if (g_audio_fd < 0) {
            /* Capture is down: wait, then replay the full open+configure sequence. */
            if (!lost_since_us) lost_since_us = rtp_mono_us();
            int units = audio_backoff_units(attempt);
            fprintf(stderr, "[audio] capture down, retrying in %d x %d ms (attempt %u)\n",
                    units, AUDIO_BACKOFF_UNIT_MS, attempt + 1);
            if (audio_wait_ms((long)units * AUDIO_BACKOFF_UNIT_MS)) break;
            if (audio_init_capture() < 0) { attempt++; continue; }
            /* Keep the RTP timeline continuous: the lost time counts as samples that were not delivered, so
             * the next frame's timestamp jumps by the gap instead of overlapping the previous audio. */
            int64_t gap_us = rtp_mono_us() - lost_since_us;
            if (gap_us > 0) g_samples_total += (uint64_t)gap_us * AUDIO_SAMPLE_RATE / 1000000;
            printf("[ak_rtsp] audio: capture recovered after %u failed attempt(s)\n", attempt);
            g_audio_reopens++;
            if (g_playback_pending) { /* the speaker was waiting for a running capture (start order, see above) */
                g_playback_pending = 0;
                audio_begin_playback();
            }
            attempt = 0;
            lost_since_us = 0;
            consecutive_fails = 0;
            continue;
        }
        struct audio_slot *slot = &g_ring[g_ring_write % AUDIO_RING_SLOTS];
        ssize_t n = read(g_audio_fd, slot->samples, sizeof(slot->samples));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] read");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr, "[audio] read failed %d times in a row, closing and re-opening the capture\n",
                        consecutive_fails);
                close(g_audio_fd);
                g_audio_fd = -1;
                lost_since_us = rtp_mono_us();
                attempt = 0;
                continue;
            }
            usleep(AUDIO_FAIL_SLEEP_US); /* 200 ms backoff, not 10 ms: a slow retry, not a poll */
            continue;
        }
        consecutive_fails = 0;
        if (n == 0) {
            usleep(10000);
            continue;
        }
        slot->n_samples = (size_t)n / sizeof(int16_t);
        slot->first_sample = g_samples_total;
        slot->t_end_us = rtp_mono_us();
        g_samples_total += slot->n_samples;
        /* Publish: the slot contents are written before the new count. */
        RING_BARRIER();
        g_ring_write = g_ring_write + 1;
    }
    return NULL;
}

/* Tracks whether pthread_create succeeded, independent of g_audio_running
 * (which the capture thread itself may clear on a fatal read() streak — see
 * audio_capture_loop). Needed so audio_stop() always joins a thread that was
 * actually started, even if it already exited on its own. */
static volatile int g_audio_thread_started = 0;

/* audio.mic_enable at start (saved in the audio_out file): 0 = the capture device is never opened. */
static int g_mic_on = 1;

int audio_mic_enabled(void) { return g_mic_on; }
uint32_t audio_mic_restarts(void) { return g_audio_reopens; }

int audio_start(void)
{
    g_mic_on = audio_out_mic_enable();
    if (!g_mic_on) {
        /* G07: no microphone at all (privacy, power): capture is not opened, no capture thread, and the SDP
         * has no audio track (rtsp.c asks audio_mic_enabled()). The speaker is independent of the capture and
         * runs through audio_out when enabled; the legacy silence stream only existed for the capture's sake. */
        printf("[ak_rtsp] audio: microphone disabled (audio.mic_enable=0), capture not opened\n");
        if (audio_out_start() == 0) g_audio_out_active = 1;
        return 0;
    }

    int cap_ok = audio_init_capture() == 0;
    if (!cap_ok)
        printf("[ak_rtsp] audio: capture not available at start, retrying with backoff\n");

    g_ring_write = 0;
    g_ring_read  = 0;
    g_samples_total = 0;
    g_audio_running = 1;

    if (pthread_create(&g_audio_tid, NULL, audio_capture_loop, NULL) != 0) {
        perror("[audio] pthread_create");
        g_audio_running = 0;
        audio_close();
        return -1;
    }
    g_audio_thread_started = 1;
    if (!cap_ok) { /* the thread opens the capture as soon as it can and then starts the speaker */
        g_playback_pending = 1;
        return 0;
    }

    /* Deliberately started only now — see the comment above
     * audio_init_capture(): real anyka_ipc's playback SET_MODE succeeds
     * while capture is already mid-read, not merely after capture's config
     * ioctls returned success. */
    audio_begin_playback();

    return 0;
}

void audio_stop(void)
{
    /* The capture thread may still start the speaker after a late first open: stop and join it first. */
    g_audio_running = 0;
    if (g_audio_thread_started) {
        pthread_join(g_audio_tid, NULL);
        g_audio_thread_started = 0;
    }
    if (g_audio_out_active) {
        audio_out_stop(); /* amp off, thread joined, device closed */
        g_audio_out_active = 0;
    }
    g_playback_running = 0;
    if (g_playback_thread_started) {
        pthread_join(g_playback_tid, NULL);
        g_playback_thread_started = 0;
    }
    audio_close();
}

int audio_get_frame(int16_t *buf, size_t max_samples, size_t *out_samples)
{
    uint64_t first;
    int64_t t_end;
    return audio_get_frame_ts(buf, max_samples, out_samples, &first, &t_end);
}

int audio_get_frame_ts(int16_t *buf, size_t max_samples, size_t *out_samples,
                       uint64_t *first_sample, int64_t *t_end_us)
{
    if (!g_audio_running) return -1;
    /* The producer overwrites slot (write % SLOTS) while filling it, so only
     * SLOTS-1 completed periods are safe to read. The audio start gate can
     * leave the ring unread for seconds (it laps many times), which makes the
     * lapped case the normal one at release: clamp to SLOTS-1 and re-check
     * after the copy that the producer did not reach the slot meanwhile. */
    uint32_t r = g_ring_read;
    size_t n;
    for (;;) {
        RING_BARRIER();
        uint32_t w = g_ring_write;
        if (r >= w) { g_ring_read = r; return 0; }
        if (w - r > AUDIO_RING_SLOTS - 1) r = w - (AUDIO_RING_SLOTS - 1);

        struct audio_slot *slot = &g_ring[r % AUDIO_RING_SLOTS];
        n = slot->n_samples;
        if (n > max_samples) n = max_samples;
        memcpy(buf, slot->samples, n * sizeof(int16_t));
        *first_sample = slot->first_sample;
        *t_end_us = slot->t_end_us;

        RING_BARRIER();
        w = g_ring_write;
        if (w - r <= AUDIO_RING_SLOTS - 1) break; /* slot untouched */
        r = w - (AUDIO_RING_SLOTS - 1);           /* overwritten: retry */
    }
    *out_samples = n;
    g_ring_read = r + 1;
    return 1;
}
