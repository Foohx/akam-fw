#include <stdio.h>
#include <pthread.h>

#include "leds.h"
#include "ircut.h"

/* Process-wide LED policy used by main.c, control.c and rtsp.c: the engine
 * of leds.c on the camera's real paths, with night.ir_led applied through
 * ircut.c. Kept apart from leds.c so the host test of the engine links
 * without the GPIO code. */

static void ir_hook(void *user, int allowed)
{
    (void)user;
    ircut_set_ir_led_allowed(allowed);
}

static leds_t        g_leds;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The control thread starts before main() reaches ledsys_start(): the first
 * caller of either initialises the instance. */
static void init_once(void)
{
    static const leds_cfg_t cfg = {
        "/sys/class/leds/state_led", "/etc/config/akam", "wlan0", "/sys/class/net", 0
    };
    static const leds_ops_t ops = { NULL, ir_hook, NULL };
    leds_init(&g_leds, &ops, &cfg);
}

static leds_t *single(void)
{
    pthread_once(&g_once, init_once);
    return &g_leds;
}

int ledsys_start(void)
{
    leds_t *l = single();
    int had = leds_load_config(l);
    leds_apply(l);
    printf("[leds] status=%s night.ir_led=%s (%s)\n", leds_status_name(leds_get_status(l)),
           leds_ir_name(leds_get_ir(l)), had ? "saved settings" : "defaults");
    if (leds_thread_start(l) < 0) {
        printf("[leds] cannot start the LED thread\n");
        return -1;
    }
    return 0;
}

void ledsys_stop(void)
{
    leds_thread_stop(single());
}

void ledsys_boot_done(void) { leds_set_ready(single(), 1); }

void ledsys_stream_delta(int delta) { leds_client_delta(single(), delta); }

int ledsys_ctl_get(const char *key, char *out, size_t cap) { return leds_ctl_get(single(), key, out, cap); }

int ledsys_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    return leds_ctl_set(single(), key, val, msg, cap);
}

void ledsys_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    leds_ctl_list(single(), emit, arg);
}
