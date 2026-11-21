/* Push-button policy (portable, PURE: no I/O, no clock of its own, no model name; host-tested by tests/rot2k_button_unit.c). Fed with the raw
 * level of one hal_input line and a monotonic clock, it debounces (hal_input.h, the debounce time of the timing config), feeds the gesture
 * engine (button_gesture.h) and hands every finished gesture to the gesture() callback, exactly once. With the DEFAULT bindings (short, long) it
 * does what the button always did:
 *   held < 1 s (short_max_ms)     short press -> gesture "short" at the release + the LED "ack" cue
 *   held >= long_ms (default 2 s) long press  -> gesture "long" at the release
 *   in between                    nothing (a dead zone: an unsure press never does the big thing)
 * While the button stays down: past BTN_HOLD_START_MS (500 ms) the LED "hold" cue (fast blink, "keep holding"); past long_ms the "long" cue
 * (confirm: double blink then solid, "release now"); past very_long_ms the "vlong" cue when a bound gesture has a very long element;
 * "hold_end" when it is let go.
 * While locked() is true (the PTZ calibration) a finished gesture is NOT dispatched (no event, no motion): the LED cues only.
 * A press that was already down when the first sample was read (a held button at start-up) is ignored until released. */
#ifndef BUTTON_LOGIC_H
#define BUTTON_LOGIC_H

#include <stdint.h>

#include "button_gesture.h"
#include "hal_input.h"

#define BTN_SHORT_MAX_MS 1000
#define BTN_HOLD_START_MS 500
#define BTN_LONG_DEFAULT_MS 2000
#define BTN_LONG_MIN_MS 1500
#define BTN_LONG_MAX_MS 10000

enum { BTN_LED_ACK = 1, BTN_LED_HOLD, BTN_LED_HOLD_END, BTN_LED_LONG, BTN_LED_VLONG };
enum { BTN_EV_NONE = 0, BTN_EV_GESTURE, BTN_EV_IGNORED };

typedef struct {
    void *user;
    int (*locked)(void *user);          /* 1: ignore the gesture (calibration). NULL = never */
    /* a finished gesture; bound = 1 when a binding exists for it. Never called while locked. */
    void (*gesture)(void *user, const bg_seq_t *seq, int bound);
    void (*led)(void *user, int cue);   /* BTN_LED_* */
} btn_actions_t;

typedef struct {
    hal_deb_t deb;
    bg_t bg;
    int led_ack;                        /* 1: the ack cue follows a bound gesture that ends with a short press */
    int down, hold_fast, hold_long, hold_vlong, wait_release;
    int64_t t_down;
} btn_t;

/* timing NULL or invalid = the defaults; the bound list is set with btn_set_bound (none = every press is reported at once). */
void btn_init(btn_t *b, const bg_timing_t *timing);
void btn_set_bound(btn_t *b, const bg_seq_t *list, int n);
/* 1 when long_ms is acceptable (BTN_LONG_MIN_MS..BTN_LONG_MAX_MS). */
int btn_long_ms_ok(int long_ms);
/* One sample (raw = the line level, -1 when the read failed). Returns BTN_EV_* of this step (NONE unless a gesture finished). */
int btn_step(btn_t *b, const hal_input_t *in, int64_t now_ms, int raw, const btn_actions_t *a);

#endif
