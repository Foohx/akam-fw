/* See button_logic.h. */
#include <string.h>

#include "button_logic.h"

int btn_long_ms_ok(int long_ms) { return long_ms >= BTN_LONG_MIN_MS && long_ms <= BTN_LONG_MAX_MS; }

void btn_init(btn_t *b, const bg_timing_t *timing)
{
    memset(b, 0, sizeof *b);
    hal_deb_init(&b->deb);
    bg_init(&b->bg, timing);
    b->led_ack = 1;
}

void btn_set_bound(btn_t *b, const bg_seq_t *list, int n) { bg_set_bound(&b->bg, list, n); }

static void cue(const btn_actions_t *a, int c)
{
    if (a && a->led)
        a->led(a->user, c);
}

/* A finished gesture: the ack cue (a bound gesture that ends with a short press, even while locked), then the callback unless locked. */
static int emit(btn_t *b, const bg_seq_t *seq, const btn_actions_t *a)
{
    int bound = bg_is_bound(&b->bg, seq);
    int locked = a && a->locked && a->locked(a->user);
    if (b->led_ack && bound && seq->el[seq->n - 1] == BG_S)
        cue(a, BTN_LED_ACK);
    if (locked)
        return BTN_EV_IGNORED;
    if (a && a->gesture)
        a->gesture(a->user, seq, bound);
    return BTN_EV_GESTURE;
}

int btn_step(btn_t *b, const hal_input_t *in, int64_t now_ms, int raw, const btn_actions_t *a)
{
    hal_input_t din = *in;
    int first = !b->deb.init, pressed, ev = BTN_EV_NONE;
    bg_seq_t seq;
    din.debounce_ms = b->bg.t.debounce_ms;
    pressed = hal_deb_update(&b->deb, &din, now_ms, raw);
    if (!b->down && bg_tick(&b->bg, now_ms, &seq) == BG_GESTURE) /* the gap ran out: the sequence is over (before a new press starts) */
        ev = emit(b, &seq, a);
    if (pressed && !b->down) {
        b->down = 1;
        b->hold_fast = b->hold_long = b->hold_vlong = 0;
        b->t_down = now_ms;
        b->wait_release = first; /* already held when we first looked: not a press of ours */
        if (!b->wait_release)
            bg_press(&b->bg, now_ms);
    } else if (pressed && b->down) {
        if (!b->wait_release && !b->hold_fast && now_ms - b->t_down >= BTN_HOLD_START_MS) {
            b->hold_fast = 1;
            cue(a, BTN_LED_HOLD);
        }
        if (!b->wait_release && !b->hold_long && now_ms - b->t_down >= b->bg.t.long_ms) {
            b->hold_long = 1;
            cue(a, BTN_LED_LONG);
        }
        if (!b->wait_release && !b->hold_vlong && b->bg.use_v && now_ms - b->t_down >= b->bg.t.very_long_ms) {
            b->hold_vlong = 1;
            cue(a, BTN_LED_VLONG);
        }
    } else if (!pressed && b->down) {
        b->down = 0;
        if (b->hold_fast || b->hold_long)
            cue(a, BTN_LED_HOLD_END);
        if (!b->wait_release && bg_release(&b->bg, now_ms, &seq) == BG_GESTURE) {
            int e = emit(b, &seq, a);
            if (e != BTN_EV_NONE)
                ev = e;
        }
        b->hold_fast = b->hold_long = b->hold_vlong = 0;
        b->wait_release = 0;
    }
    return ev;
}
