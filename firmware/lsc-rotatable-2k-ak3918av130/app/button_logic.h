/* Push-button policy (portable, PURE: no I/O, no clock of its own, no model name; host-tested by tests/rot2k_button_unit.c). Fed with the raw
 * level of one hal_input line and a monotonic clock, it debounces (hal_input.h), measures a press and, at the release, dispatches:
 *   held < 1 s (BTN_SHORT_MAX_MS)        short press -> short_press() + the LED "ack" cue
 *   held >= long_ms (default 2 s)        long press  -> long_press()
 *   in between                           nothing (a dead zone: an unsure press never does the big thing)
 * While the button stays down: past BTN_HOLD_START_MS (500 ms) the LED "hold" cue (fast blink, "keep holding"); past long_ms the "long" cue
 * (confirm: double blink then solid, "release now"); "hold_end" when it is let go.
 * While locked() is true (the PTZ calibration) a release does NOT dispatch anything (no event, no motion): the LED cues only.
 * A press that was already down when the first sample was read (a held button at start-up) is ignored until released. */
#ifndef BUTTON_LOGIC_H
#define BUTTON_LOGIC_H

#include <stdint.h>

#include "hal_input.h"

#define BTN_SHORT_MAX_MS 1000
#define BTN_HOLD_START_MS 500
#define BTN_LONG_DEFAULT_MS 2000
#define BTN_LONG_MIN_MS 1500
#define BTN_LONG_MAX_MS 10000

enum { BTN_LED_ACK = 1, BTN_LED_HOLD, BTN_LED_HOLD_END, BTN_LED_LONG };
enum { BTN_EV_NONE = 0, BTN_EV_SHORT, BTN_EV_LONG, BTN_EV_IGNORED };

typedef struct {
    void *user;
    int (*locked)(void *user);          /* 1: ignore the press (calibration). NULL = never */
    void (*short_press)(void *user);
    void (*long_press)(void *user);
    void (*led)(void *user, int cue);   /* BTN_LED_* */
} btn_actions_t;

typedef struct {
    hal_deb_t deb;
    int long_ms;
    int down, hold_fast, hold_long, wait_release;
    int64_t t_down;
} btn_t;

void btn_init(btn_t *b, int long_ms);
/* 1 when long_ms is acceptable (BTN_LONG_MIN_MS..BTN_LONG_MAX_MS). */
int btn_long_ms_ok(int long_ms);
/* One sample (raw = the line level, -1 when the read failed). Returns BTN_EV_* of this step (NONE except at a release). */
int btn_step(btn_t *b, const hal_input_t *in, int64_t now_ms, int raw, const btn_actions_t *a);

#endif
