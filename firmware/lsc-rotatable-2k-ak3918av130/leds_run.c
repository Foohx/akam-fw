/* The status LED thread of the app (see leds.h): the real "blue" LED through hal_light, the real inputs (Wi-Fi route, RTSP
 * clients in PLAY, person detection, night mode). One small thread, 50 ms tick, never touches capture, the network or the NPU. */
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "hal.h"
#include "leds.h"
#include "log.h"
#include "night_ctl.h"
#include "rtsp_session.h"
#include "hal_sys.h"

#define TICK_US 50000
#define NET_EVERY_TICKS 20 /* the route table is read once a second */

static a2k_leds_t g_l;
static pthread_t g_tid;
static volatile int g_started, g_stop, g_person;
static int g_net_ok, g_net_n;

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void inputs(void *user, a2k_leds_in_t *in)
{
    a2k_night_info_t n;
    (void)user;
    if (g_net_n++ % NET_EVERY_TICKS == 0)
        g_net_ok = hal_sys_net_ipv4(NULL, "wlan0");
    in->nowifi = !g_net_ok;
    in->person = g_person;
    in->stream = a2k_rtsp_playing() > 0;
    a2k_night_get(&n);
    in->night = n.valid && n.night;
}

static void *thread_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        a2k_leds_tick(&g_l, mono_ms());
        usleep(TICK_US);
    }
    return NULL;
}

void a2k_leds_person(int present) { g_person = present ? 1 : 0; }

int a2k_leds_start(int mode, const char *const pats[A2K_RULE_N])
{
    static const a2k_leds_ops_t ops = { inputs, 0 };
    pthread_attr_t at;
    if (g_started)
        return 0;
    if (a2k_leds_init(&g_l, &a2k_light_desc, &hal_light_sysfs_ops, &ops, mono_ms()) != 0) {
        a2k_log("led: cannot initialise the status LED policy");
        return -1;
    }
    (void)a2k_leds_set_mode(&g_l, mode);
    for (int r = 0; pats && r < A2K_RULE_N; r++)
        if (pats[r] && a2k_leds_set_pattern(&g_l, r, pats[r]) != 0)
            a2k_log("led: %s: invalid pattern, default kept", a2k_rule_key(r));
    g_stop = 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32 * 1024);
    if (pthread_create(&g_tid, &at, thread_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("led: cannot start the thread, the LED stays as it was");
        return -1;
    }
    pthread_attr_destroy(&at);
    g_started = 1;
    a2k_log("led: status LED %s", a2k_leds_mode_name(mode));
    return 0;
}

void a2k_leds_stop(void)
{
    if (!g_started)
        return;
    g_stop = 1;
    pthread_join(g_tid, NULL);
    g_started = 0;
    a2k_leds_release(&g_l); /* back to the stock state: on */
}

int a2k_leds_run_set_mode(int mode) { return g_started ? a2k_leds_set_mode(&g_l, mode) : -1; }
int a2k_leds_run_mode(void) { return g_started ? a2k_leds_get_mode(&g_l) : A2K_LEDS_ON; }
int a2k_leds_run_set_pattern(int rule, const char *text) { return g_started ? a2k_leds_set_pattern(&g_l, rule, text) : -1; }

void a2k_leds_run_pattern(int rule, char *out, size_t cap)
{
    if (g_started)
        a2k_leds_get_pattern(&g_l, rule, out, cap);
    else
        snprintf(out, cap, "%s", a2k_rule_default(rule));
}

int a2k_leds_run_blip(const char *pattern, long dur_ms) { return g_started ? a2k_leds_blip(&g_l, pattern, dur_ms, mono_ms()) : -1; }
void a2k_leds_run_blip_clear(void)
{
    if (g_started)
        a2k_leds_blip_clear(&g_l);
}
