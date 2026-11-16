/* The day / night thread of the app: the real pins (akam2k_ircut.c), the vendor ISP (akam2k_isp.h) and two threads: the tick thread
 * runs the engine (akam2k_night.c) every 100 ms of wall time, the reader thread does the vendor read (it blocks 1..1.5 s, so
 * it must never sit inside the tick). It never touches the streams, the NPU or the network. */
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akam2k_ircut.h"
#include "akam2k_isp.h"
#include "akam2k_log.h"
#include "akam2k_night.h"

static void real_sleep(unsigned ms) { usleep(ms * 1000u); }

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t epoch_s(void)
{
    time_t t = time(NULL);
    return t < 1577836800 ? 0 : (int64_t)t; /* a clock before 2020 is not set */
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static a2k_night_info_t g_snap;
static void (*g_cb)(const a2k_night_info_t *);
static a2k_night_ctl_t g_ctl;
static a2k_night_ops_t g_ops;
static pthread_t g_tid, g_rtid;
static volatile int g_stop, g_started, g_rstarted, g_engine;
static volatile int g_ir_auto = 1;

int a2k_night_ir_auto(void) { return g_ir_auto; }

int a2k_night_set_ir(int ir_auto)
{
    if (!g_engine)
        return -1;
    a2k_night_ctl_ir_request(&g_ctl, ir_auto);
    g_ir_auto = ir_auto ? 1 : 0;
    if (!g_started)
        a2k_night_ctl_ir_apply(&g_ctl); /* forced mode: no tick thread, nobody else touches the engine */
    return 0;
}

static void on_change(const a2k_night_info_t *i)
{
    pthread_mutex_lock(&g_mu);
    g_snap = *i;
    pthread_mutex_unlock(&g_mu);
    if (g_cb)
        g_cb(i);
}

void a2k_night_set_notify(void (*cb)(const a2k_night_info_t *i)) { g_cb = cb; }

void a2k_night_get(a2k_night_info_t *out)
{
    pthread_mutex_lock(&g_mu);
    *out = g_snap;
    pthread_mutex_unlock(&g_mu);
}

/* Absolute-deadline ticks: a late tick does not shift the following ones. */
static void *thread_main(void *arg)
{
    int64_t next = mono_ms() + NL_POLL_MS;
    (void)arg;
    while (!g_stop) {
        int64_t d = next - mono_ms();
        if (d > 0)
            usleep((unsigned)d * 1000u);
        if (g_stop)
            break;
        a2k_night_ctl_poll(&g_ctl, mono_ms());
        next += NL_POLL_MS;
        if (next < mono_ms()) /* stalled for more than a tick: no burst of catch-up ticks */
            next = mono_ms() + NL_POLL_MS;
    }
    return NULL;
}

/* The vendor read, about every A2K_NIGHT_READ_EVERY * NL_POLL_MS (start to start; at least one tick of rest). */
static void *reader_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        int64_t t0 = mono_ms(), until = t0 + A2K_NIGHT_READ_EVERY * NL_POLL_MS;
        a2k_night_ctl_read_once(&g_ctl);
        if (until < mono_ms() + NL_POLL_MS)
            until = mono_ms() + NL_POLL_MS;
        while (!g_stop && mono_ms() < until) {
            if (mono_ms() >= t0 + NL_POLL_MS && a2k_night_ctl_take_kick(&g_ctl))
                break; /* an ISP / IR LED change: read the new scene without waiting out the period */
            usleep(50 * 1000u);
        }
    }
    return NULL;
}

int a2k_night_start(const a2k_night_cfg_t *cfg)
{
    pthread_attr_t at;
    if (g_engine || !a2k_night_cfg_ok(cfg))
        return -1;
    memset(&g_snap, 0, sizeof g_snap);
    g_ir_auto = cfg->ir_led_auto;
    g_ops.init = a2k_ircut_init;
    g_ops.dn_init = a2k_isp_dn_init;
    g_ops.dn_level = a2k_isp_dn_level;
    g_ops.isp_mode = a2k_isp_mode;
    g_ops.wdr = a2k_isp_wdr;
    g_ops.ircut = a2k_ircut_pulse;
    g_ops.led = a2k_ircut_led;
    g_ops.release = a2k_ircut_release;
    g_ops.sleep_ms = real_sleep;
    g_ops.epoch = epoch_s;
    g_ops.notify = on_change;
    g_ops.now_ms = mono_ms;
    a2k_night_ctl_init(&g_ctl, cfg, &g_ops);
    a2k_night_ctl_set_async(&g_ctl);
    if (a2k_night_ctl_start(&g_ctl, mono_ms()) != 0)
        return -1;
    g_engine = 1;
    if (cfg->mode != A2K_NIGHT_AUTO)
        return 0; /* forced: no thread, nothing to watch */
    g_stop = 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&g_tid, &at, thread_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("day/night: cannot start the thread, staying in day");
        a2k_night_ctl_stop(&g_ctl);
        g_engine = 0;
        return -1;
    }
    g_started = 1;
    if (pthread_create(&g_rtid, &at, reader_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("day/night: cannot start the reader thread, staying in day");
        g_stop = 1;
        pthread_join(g_tid, NULL);
        g_started = 0;
        a2k_night_ctl_stop(&g_ctl);
        g_engine = 0;
        return -1;
    }
    pthread_attr_destroy(&at);
    g_rstarted = 1;
    return 0;
}

void a2k_night_stop(void)
{
    if (!g_engine)
        return;
    if (g_started) {
        g_stop = 1;
        pthread_join(g_tid, NULL);
        g_started = 0;
    }
    if (g_rstarted) {
        pthread_join(g_rtid, NULL); /* may wait for the vendor read in flight (~1.5 s) */
        g_rstarted = 0;
    }
    a2k_night_ctl_stop(&g_ctl);
    a2k_pin_close_all();
    g_engine = 0;
    a2k_log("day/night: stopped, filter in (day position), coil released, IR LED off");
}
