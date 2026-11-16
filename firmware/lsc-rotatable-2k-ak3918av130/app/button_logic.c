/* See button_logic.h. */
#include <string.h>

#include "button_logic.h"

int btn_long_ms_ok(int long_ms) { return long_ms >= BTN_LONG_MIN_MS && long_ms <= BTN_LONG_MAX_MS; }

void btn_init(btn_t *b, int long_ms)
{
    memset(b, 0, sizeof *b);
    hal_deb_init(&b->deb);
    b->long_ms = btn_long_ms_ok(long_ms) ? long_ms : BTN_LONG_DEFAULT_MS;
}

static void cue(const btn_actions_t *a, int c)
{
    if (a && a->led)
        a->led(a->user, c);
}

int btn_step(btn_t *b, const hal_input_t *in, int64_t now_ms, int raw, const btn_actions_t *a)
{
    int first = !b->deb.init;
    int pressed = hal_deb_update(&b->deb, in, now_ms, raw);
    int ev = BTN_EV_NONE;
    if (pressed && !b->down) {
        b->down = 1;
        b->hold_fast = b->hold_long = 0;
        b->t_down = now_ms;
        b->wait_release = first; /* already held when we first looked: not a press of ours */
    } else if (pressed && b->down) {
        if (!b->wait_release && !b->hold_fast && now_ms - b->t_down >= BTN_HOLD_START_MS) {
            b->hold_fast = 1;
            cue(a, BTN_LED_HOLD);
        }
        if (!b->wait_release && !b->hold_long && now_ms - b->t_down >= b->long_ms) {
            b->hold_long = 1;
            cue(a, BTN_LED_LONG);
        }
    } else if (!pressed && b->down) {
        int64_t held = now_ms - b->t_down;
        int locked = a && a->locked && a->locked(a->user);
        b->down = 0;
        if (b->hold_fast || b->hold_long)
            cue(a, BTN_LED_HOLD_END);
        if (!b->wait_release) {
            if (held >= b->long_ms) {
                ev = locked ? BTN_EV_IGNORED : BTN_EV_LONG;
                if (!locked && a && a->long_press)
                    a->long_press(a->user);
            } else if (held < BTN_SHORT_MAX_MS) {
                ev = locked ? BTN_EV_IGNORED : BTN_EV_SHORT;
                cue(a, BTN_LED_ACK);
                if (!locked && a && a->short_press)
                    a->short_press(a->user);
            }
        }
        b->hold_fast = b->hold_long = 0;
        b->wait_release = 0;
    }
    return ev;
}
