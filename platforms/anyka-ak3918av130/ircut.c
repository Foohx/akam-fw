#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "ircut.h"

/* A signal (SIGTERM, SIGALRM) must not cut the 300 ms pulse short: the filter would stop half way. */
static void real_sleep(unsigned ms)
{
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static const a2k_ircut_ops_t g_real = { a2k_pin_open, a2k_pin_set, real_sleep };
static const a2k_ircut_ops_t *g_ops = &g_real;

void a2k_ircut_test_ops(const a2k_ircut_ops_t *ops) { g_ops = ops ? ops : &g_real; }

int a2k_ircut_init(void)
{
    static const int pins[3] = { A2K_PIN_IRCUT_P, A2K_PIN_IRCUT_M, A2K_PIN_IRLED };
    int rc = 0;
    for (int i = 0; i < 3; i++)
        if (g_ops->claim(pins[i], 0) != 0 && rc == 0)
            rc = -1;
    return rc;
}

int a2k_ircut_release(void)
{
    int rc = 0;
    for (int tries = 0; tries < 2; tries++) {
        int a = g_ops->set(A2K_IRCUT_DAY_PIN, 0), b = g_ops->set(A2K_IRCUT_NIGHT_PIN, 0);
        rc = (a == 0 && b == 0) ? 0 : -1;
        if (rc == 0)
            break;
    }
    return rc;
}

int a2k_ircut_pulse(int day)
{
    int on = day ? A2K_IRCUT_DAY_PIN : A2K_IRCUT_NIGHT_PIN, off = day ? A2K_IRCUT_NIGHT_PIN : A2K_IRCUT_DAY_PIN;
    /* the other pin is low before the active one goes high: the two are never high together */
    if (g_ops->set(off, 0) != 0 || g_ops->set(on, 1) != 0) {
        (void)a2k_ircut_release();
        return -1;
    }
    g_ops->sleep_ms(A2K_IRCUT_PULSE_MS);
    return g_ops->set(on, 0) == 0 ? 0 : (a2k_ircut_release(), -1);
}

int a2k_ircut_led(int on) { return g_ops->set(A2K_PIN_IRLED, on ? 1 : 0); }
