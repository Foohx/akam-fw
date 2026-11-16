/* The button thread of the app (see akam2k_button.h): the 2K input descriptor + the generic sysfs input driver feed button_logic.c; the
 * actions are the MQTT event, the PTZ park toggle and the transient LED override. */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "akam2k_button.h"
#include "akam2k_hal.h"
#include "akam2k_leds.h"
#include "akam2k_log.h"
#include "akam2k_mqtt.h"
#include "akam2k_ptz.h"
#include "button_logic.h"

static pthread_t g_tid;
static volatile int g_started, g_stop;
static int g_long_ms = BTN_LONG_DEFAULT_MS;
static int64_t g_solid_at; /* button thread only: when the solid light follows the confirm double blink (0 none) */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int act_locked(void *u)
{
    a2k_ptz_info_t i;
    (void)u;
    a2k_ptz_info(&i);
    return i.state == HAL_PTZ_CALIBRATING;
}

static void act_short(void *u)
{
    (void)u;
    a2k_mqtt_button_event("press");
    a2k_log("button: short press");
}

static void act_long(void *u)
{
    a2k_ptz_info_t i;
    (void)u;
    a2k_ptz_info(&i);
    a2k_log("button: long press, %s", i.state == HAL_PTZ_PARKED ? "park -> home" : "-> park");
    (void)a2k_ptz_toggle_park();
}

static void act_led(void *u, int cue)
{
    (void)u;
    if (cue == BTN_LED_ACK)
        (void)a2k_leds_run_blip(A2K_BUTTON_ACK_PATTERN, A2K_BUTTON_ACK_MS);
    else if (cue == BTN_LED_HOLD)
        (void)a2k_leds_run_blip(A2K_BUTTON_HOLD_PATTERN, A2K_BUTTON_HOLD_MAX_MS);
    else if (cue == BTN_LED_LONG) {
        (void)a2k_leds_run_blip(A2K_BUTTON_LONG_PATTERN, A2K_BUTTON_LONG_BLINK_MS);
        g_solid_at = mono_ms() + A2K_BUTTON_LONG_BLINK_MS;
    } else if (cue == BTN_LED_HOLD_END) {
        g_solid_at = 0;
        a2k_leds_run_blip_clear();
    }
}

static void *thread_main(void *arg)
{
    static const btn_actions_t act = { 0, act_locked, act_short, act_long, act_led };
    const hal_input_t *in = hal_input_find(&a2k_input_desc, "button");
    btn_t b;
    int bad = 0;
    (void)arg;
    btn_init(&b, g_long_ms);
    while (!g_stop) {
        int raw = hal_input_sysfs_ops.read(in);
        if (raw < 0) {
            if (++bad == 50) { /* about a second of failures: the line is gone */
                a2k_log("button: the input cannot be read, button off");
                break;
            }
        } else {
            bad = 0;
        }
        (void)btn_step(&b, in, mono_ms(), raw, &act);
        if (g_solid_at && mono_ms() >= g_solid_at) {
            g_solid_at = 0;
            (void)a2k_leds_run_blip(A2K_BUTTON_SOLID_PATTERN, A2K_BUTTON_HOLD_MAX_MS);
        }
        usleep(A2K_BUTTON_POLL_MS * 1000);
    }
    return NULL;
}

int a2k_button_start(int long_ms)
{
    const hal_input_t *in = hal_input_find(&a2k_input_desc, "button");
    pthread_attr_t at;
    if (g_started)
        return 0;
    g_long_ms = btn_long_ms_ok(long_ms) ? long_ms : BTN_LONG_DEFAULT_MS;
    if (!in || hal_input_sysfs_ops.setup(in) != 0) {
        a2k_log("button: the input line cannot be opened, button off");
        return -1;
    }
    g_stop = 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32 * 1024);
    if (pthread_create(&g_tid, &at, thread_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("button: cannot start the thread, button off");
        return -1;
    }
    pthread_attr_destroy(&at);
    g_started = 1;
    a2k_log("button: on, long press >= %d ms", g_long_ms);
    return 0;
}

void a2k_button_stop(void)
{
    if (!g_started)
        return;
    g_stop = 1;
    pthread_join(g_tid, NULL);
    g_started = 0;
}
