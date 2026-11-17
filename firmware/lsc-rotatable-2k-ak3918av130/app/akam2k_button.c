/* The button thread of the app (see akam2k_button.h): the 2K input descriptor + the generic sysfs input driver feed the runtime of akam2k_buttonrt.c;
 * this file owns the hardware side: the thread, the LED cues and the real actions (PTZ, patrol, tracker, settings, LED, API event, MQTT). */
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "api_events.h"
#include "akam2k_button.h"
#include "akam2k_buttonrt.h"
#include "akam2k_hal.h"
#include "akam2k_leds.h"
#include "akam2k_log.h"
#include "akam2k_mqtt.h"
#include "akam2k_patrol.h"
#include "akam2k_ptz.h"
#include "akam2k_settings.h"
#include "akam2k_track_run.h"
#include "button_logic.h"

static pthread_t g_tid;
static volatile int g_started, g_stop;
static int g_long_ms = BTN_LONG_DEFAULT_MS, g_prepared;
static int64_t g_solid_at; /* button thread only: when the solid light follows the confirm double blink (0 none) */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t op_now(void *u)
{
    (void)u;
    return mono_ms();
}

static int op_locked(void *u)
{
    a2k_ptz_info_t i;
    (void)u;
    a2k_ptz_info(&i);
    return i.state == HAL_PTZ_CALIBRATING;
}

/* "on" / "off" / "toggle" for the tracker and the privacy actions */
static int want(const char *mode, int now)
{
    return !strcmp(mode, "on") ? 1 : !strcmp(mode, "off") ? 0 : !now;
}

static int op_run(void *u, int kind, const char *arg)
{
    a2k_ptz_info_t i;
    (void)u;
    switch (kind) {
    case BA_HOME: return a2k_ptz_command(A2K_PTZ_HOME);
    case BA_PARK: return a2k_ptz_command(A2K_PTZ_PARK);
    case BA_RECAL: return a2k_ptz_command(A2K_PTZ_RECAL);
    case BA_TOGGLE_PARK: return a2k_ptz_toggle_park();
    case BA_PRIVACY: /* privacy = the head parked (the video is withheld while parked with ptz_privacy=1) */
        a2k_ptz_info(&i);
        if (!strcmp(arg, "toggle"))
            return a2k_ptz_toggle_park();
        if (!strcmp(arg, "on"))
            return i.state == HAL_PTZ_PARKED ? 0 : a2k_ptz_command(A2K_PTZ_PARK);
        return i.state == HAL_PTZ_PARKED ? a2k_ptz_command(A2K_PTZ_HOME) : 0;
    case BA_GOTO: return a2k_patrol_goto_preset(arg) == 0 ? 0 : -1;
    case BA_PATROL_START: return a2k_patrol_start_route(arg) == 0 ? 0 : -1;
    case BA_PATROL_STOP: return a2k_patrol_stop_route() == 0 ? 0 : -1;
    case BA_LED: return a2k_leds_run_blip(arg, A2K_BUTTON_LED_ACTION_MS) == 0 ? 0 : -1;
    case BA_TRACK: { /* through the registry: the same setter and the same persistence as track.enabled over the API */
        a2k_track_t *t = a2k_track_run_get();
        const char *k = "track.enabled", *v[1];
        if (!t)
            return -1;
        v[0] = want(arg, a2k_track_enabled(t)) ? "1" : "0";
        return a2k_settings_apply(1, &k, v, NULL, 0);
    }
    case BA_LOST: {
        const char *k = "track.lost_policy", *v[1];
        v[0] = arg;
        return a2k_settings_apply(1, &k, v, NULL, 0);
    }
    default: return -1;
    }
}

static int op_apply(void *u, int n, const char *const *k, const char *const *v)
{
    (void)u;
    return a2k_settings_apply((size_t)n, k, v, NULL, 0);
}

static void op_led(void *u, int cue)
{
    (void)u;
    if (cue == BTN_LED_ACK)
        (void)a2k_leds_run_blip(A2K_BUTTON_ACK_PATTERN, A2K_BUTTON_ACK_MS);
    else if (cue == BTN_LED_HOLD)
        (void)a2k_leds_run_blip(A2K_BUTTON_HOLD_PATTERN, A2K_BUTTON_HOLD_MAX_MS);
    else if (cue == BTN_LED_LONG) {
        (void)a2k_leds_run_blip(A2K_BUTTON_LONG_PATTERN, A2K_BUTTON_LONG_BLINK_MS);
        g_solid_at = mono_ms() + A2K_BUTTON_LONG_BLINK_MS;
    } else if (cue == BTN_LED_VLONG) {
        (void)a2k_leds_run_blip(A2K_BUTTON_VLONG_PATTERN, A2K_BUTTON_LONG_BLINK_MS);
        g_solid_at = mono_ms() + A2K_BUTTON_LONG_BLINK_MS;
    } else if (cue == BTN_LED_HOLD_END) {
        g_solid_at = 0;
        a2k_leds_run_blip_clear();
    }
}

static void op_event(void *u, const char *json)
{
    (void)u;
    a2k_api_event("button", json);
}

static void op_mqtt(void *u, const char *gesture, const char *json)
{
    (void)u;
    a2k_mqtt_button_gesture(gesture, json);
}

static void op_changed(void *u)
{
    (void)u;
    a2k_mqtt_resync();
}

static const a2k_btn_ops_t g_ops = { NULL, op_run, op_apply, op_locked, op_led, op_event, op_mqtt, op_changed, a2k_settings_mode_key_ok, op_now };

static void *thread_main(void *arg)
{
    const hal_input_t *in = hal_input_find(&a2k_input_desc, "button");
    int bad = 0;
    (void)arg;
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
        (void)a2k_btn_step(in, mono_ms(), raw);
        if (g_solid_at && mono_ms() >= g_solid_at) {
            g_solid_at = 0;
            (void)a2k_leds_run_blip(A2K_BUTTON_SOLID_PATTERN, A2K_BUTTON_HOLD_MAX_MS);
        }
        usleep(A2K_BUTTON_POLL_MS * 1000);
    }
    return NULL;
}

void a2k_button_prepare(int long_ms)
{
    g_long_ms = btn_long_ms_ok(long_ms) ? long_ms : BTN_LONG_DEFAULT_MS;
    (void)a2k_btn_init(&g_ops, g_long_ms);
    g_prepared = 1;
}

int a2k_button_start(int long_ms)
{
    const hal_input_t *in = hal_input_find(&a2k_input_desc, "button");
    pthread_attr_t at;
    if (g_started)
        return 0;
    if (!g_prepared)
        a2k_button_prepare(long_ms);
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
    a2k_log("button: on, programmable gestures (defaults: short = event, long press >= %d ms = park / home)", g_long_ms);
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
