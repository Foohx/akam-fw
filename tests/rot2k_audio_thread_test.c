/* Host test of the microphone capture thread (app/akam2k_audio.c) WITHOUT a microphone: /dev/pcmC0D0c does not exist on the
 * host, so the open fails. The thread must log it, stay down (the SDP then has no audio track), never crash, and stop at
 * once, also while it waits in its retry backoff. Prints "rot2k audio thread: N failed". */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_audio.h"

volatile sig_atomic_t a2k_stop;
static int nlog; /* written by the capture thread, read by main: atomic */

void a2k_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    __atomic_add_fetch(&nlog, 1, __ATOMIC_RELAXED);
}

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    double t0;

    CHECK(!a2k_audio_enabled() && !a2k_audio_up(), "never started: not enabled, not up");
    a2k_audio_stop(); /* safe when never started */
    CHECK(a2k_audio_start() == 0, "start returns at once (the thread opens the device)");
    CHECK(a2k_audio_enabled(), "enabled once started");
    usleep(300000);
    CHECK(!a2k_audio_up(), "no device: the capture is not up (no audio track in the SDP)");
    CHECK(__atomic_load_n(&nlog, __ATOMIC_RELAXED) >= 1, "the failure is logged");
    CHECK(a2k_aring_head(a2k_audio_ring()) == 0, "nothing was published");
    usleep(1500000); /* the second attempt (after the 1 s backoff) fails too, still no crash */
    CHECK(!a2k_audio_up(), "still down after a retry");
    t0 = now_s();
    a2k_audio_stop();
    CHECK(now_s() - t0 < 1.0, "stop returns within a second even while the thread waits in its backoff");
    CHECK(!a2k_audio_enabled() && !a2k_audio_up(), "stopped");
    a2k_audio_stop(); /* twice is harmless */

    a2k_stop = 1; /* the app-wide stop flag ends a freshly started thread too */
    CHECK(a2k_audio_start() == 0, "restart works");
    t0 = now_s();
    a2k_audio_stop();
    CHECK(now_s() - t0 < 1.0, "a2k_stop ends the thread");

    printf("rot2k audio thread: %d failed\n", fails);
    return fails ? 1 : 0;
}
