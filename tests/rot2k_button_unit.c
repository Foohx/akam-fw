/* Host unit test of the front button of the Rotatable 2K app: the portable hal_input debouncer, the gesture engine (button_gesture.c: single / double /
 * triple / long / very long / user sequences, wait-for-gap, longest match, noise) and the press policy of button_logic.c (short, long, dead zone, hold
 * cue, calibration lock, held at start) on a fake line and a fake clock, the whitelist of actions and the modes (button_cfg.c: parse, limits, dispatch with
 * fake ops, all-or-nothing mode apply), the runtime of button_rt.c (defaults identical to the factory button, persistence only on change, the
 * HTTP endpoints), the sysfs input driver on a fake directory (the input pin of pin.c: read-only, outputs untouched) and the transient LED
 * override of the LED engine.
 * Prints "rot2k button unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api_events.h"
#include "button_rt.h"
#include "hal.h"
#include "leds.h"
#include "pin.h"
#include "store.h"
#include "button_cfg.h"
#include "button_gesture.h"
#include "button_logic.h"
#include "hal_input.h"

void a2k_log(const char *fmt, ...)
{
    (void)fmt;
}

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static const hal_input_t BTN = { "button", 71, 1, 50 };

/* the fake world: every finished gesture is logged with its name, whether it was bound and the time it was reported */
#define NG 40
static char gname[NG][BG_NAME_MAX + 4];
static int gbound[NG], ng, locked_now;
static int64_t gtime[NG], g_now;
static int cues[40], ncue;
static btn_t *g_btn; /* for the "never reported while the button is down" invariant */
static int down_at_emit;

static void a_gesture(void *u, const bg_seq_t *s, int bound)
{
    (void)u;
    if (g_btn && g_btn->down)
        down_at_emit++;
    if (ng < NG) {
        bg_seq_name(s, gname[ng], sizeof gname[0]);
        gbound[ng] = bound;
        gtime[ng] = g_now;
        ng++;
    }
}
static int a_locked(void *u) { (void)u; return locked_now; }
static void a_led(void *u, int c) { (void)u; if (ncue < 40) cues[ncue++] = c; }
static const btn_actions_t ACT = { 0, a_locked, a_gesture, a_led };

static void reset(void) { ng = locked_now = ncue = down_at_emit = 0; }
static int count(const char *name)
{
    int i, n = 0;
    for (i = 0; i < ng; i++)
        n += !strcmp(gname[i], name);
    return n;
}

/* the binding list of a test, by name */
static void bind(btn_t *b, const char *const *names, int n)
{
    bg_seq_t l[BG_MAX_BOUND];
    int i;
    for (i = 0; i < n; i++)
        bg_seq_parse(names[i], &l[i]);
    btn_set_bound(b, l, n);
}

static const char *const DEF[] = { "short", "long" };

static void init_btn(btn_t *b, int long_ms, const char *const *names, int n)
{
    bg_timing_t t;
    bg_timing_defaults(&t);
    if (long_ms) {
        t.long_ms = long_ms;
        if (t.very_long_ms < long_ms + 500)
            t.very_long_ms = long_ms + 500;
    }
    btn_init(b, &t);
    bind(b, names, n);
    g_btn = b;
}

/* Holds the line at `raw` from t0 to t1 (inclusive) sampling every 20 ms; returns the last event seen that is not NONE. */
static int run(btn_t *b, int64_t t0, int64_t t1, int raw)
{
    int ev = BTN_EV_NONE;
    for (int64_t t = t0; t <= t1; t += 20) {
        int e;
        g_now = t;
        e = btn_step(b, &BTN, t, raw, &ACT);
        if (e != BTN_EV_NONE)
            ev = e;
    }
    return ev;
}

/* A waveform: the line is low (pressed) in each [down, up) interval and high elsewhere, sampled every 20 ms from t0 to end. */
typedef struct { int64_t down, up; } iv_t;
static void play(btn_t *b, int64_t t0, const iv_t *iv, int n, int64_t end)
{
    for (int64_t t = t0; t <= end; t += 20) {
        int raw = 1;
        for (int i = 0; i < n; i++)
            if (t >= iv[i].down && t < iv[i].up)
                raw = 0;
        g_now = t;
        (void)btn_step(b, &BTN, t, raw, &ACT);
    }
}
#define PLAY(b, iv, end) play(b, 0, iv, (int)(sizeof (iv) / sizeof (iv)[0]), end)

static void test_deb(void)
{
    hal_deb_t d;
    printf("== debouncer\n");
    hal_deb_init(&d);
    CHECK(hal_deb_update(&d, &BTN, 0, 1) == 0, "released (raw 1, active low) = not pressed");
    CHECK(hal_deb_update(&d, &BTN, 20, 0) == 0 && hal_deb_update(&d, &BTN, 40, 0) == 0, "a fresh 0 is not believed at once");
    CHECK(hal_deb_update(&d, &BTN, 60, 0) == 0 && hal_deb_update(&d, &BTN, 70, 0) == 1, "believed after 50 ms of the same level (raw 0 since t = 20 ms: pressed at 70)");
    CHECK(hal_deb_update(&d, &BTN, 100, 1) == 1 && hal_deb_update(&d, &BTN, 120, 0) == 1 && hal_deb_update(&d, &BTN, 140, 1) == 1, "bounces while pressed are ignored");
    CHECK(hal_deb_update(&d, &BTN, 160, -1) == 1, "a failed read keeps the state");
    CHECK(hal_deb_update(&d, &BTN, 150, 1) == 1 && hal_deb_update(&d, &BTN, 200, 1) == 0, "released after 50 ms of 1");
    hal_deb_init(&d);
    CHECK(hal_deb_update(&d, &BTN, 0, 0) == 1, "the first sample is believed at once");
    {
        hal_input_t hi = { "x", 1, 0, 10 };
        hal_deb_init(&d);
        hal_deb_update(&d, &hi, 0, 0);
        hal_deb_update(&d, &hi, 5, 1);
        CHECK(hal_deb_update(&d, &hi, 20, 1) == 1, "active high: raw 1 = pressed");
    }
    CHECK(hal_input_find(&a2k_input_desc, "button") != 0 && hal_input_find(&a2k_input_desc, "nope") == 0, "descriptor lookup");
    CHECK(a2k_input_desc.count == 1 && a2k_input_desc.inputs[0].gpio == 71 && a2k_input_desc.inputs[0].active_low == 1 &&
              a2k_input_desc.inputs[0].debounce_ms == 50,
          "the 2K descriptor: button, gpio 71, active low, 50 ms");
}

static void test_policy(void)
{
    btn_t b;
    int ev;
    printf("== press policy (the default bindings: short, long)\n");
    init_btn(&b, 2000, DEF, 2);
    reset();
    run(&b, 0, 1000, 1); /* released for a while */
    ev = run(&b, 1020, 1400, 0);
    CHECK(ev == BTN_EV_NONE && ng == 0, "nothing happens while the button is down (short press)");
    ev = run(&b, 1420, 1600, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1 && ng == 1 && gbound[0] == 1 && ncue == 1 && cues[0] == BTN_LED_ACK, "a 0.4 s press: one short press, the ack cue");
    CHECK(gtime[0] <= 1420 + 100, "... reported at the release, no delay (nothing bound extends it)");
    /* a contact bounce of 30 ms is no press */
    reset();
    run(&b, 2000, 2000, 0);
    ev = run(&b, 2020, 2200, 1);
    CHECK(ev == BTN_EV_NONE && ng == 0 && ncue == 0, "a 30 ms glitch is debounced away");
    /* long press */
    reset();
    run(&b, 3000, 3000, 1);
    run(&b, 3020, 5200, 0);
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG, "held: fast blink at 0.5 s, then the long confirm cue at 2 s, once each");
    ev = run(&b, 5220, 5400, 1);
    CHECK(ev == BTN_EV_GESTURE && count("long") == 1 && ng == 1 && ncue == 3 && cues[2] == BTN_LED_HOLD_END, "released: the long press (park toggle), the cues end");
    /* held 0.6 s: the keep-holding blink started, release gives the short press ack after the cue end */
    reset();
    run(&b, 5500, 5500, 1);
    run(&b, 5520, 6150, 0);
    ev = run(&b, 6170, 6300, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1 && ncue == 3 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_HOLD_END && cues[2] == BTN_LED_ACK,
          "a 0.65 s press: hold blink, then end and the short ack");
    /* dead zone 1 s .. 2 s */
    reset();
    run(&b, 6000, 6000, 1);
    run(&b, 6020, 7600, 0);
    ev = run(&b, 7620, 7800, 1);
    CHECK(ev == BTN_EV_NONE && ng == 0, "a 1.6 s press is neither: nothing happens");
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_HOLD_END, "... only the keep-holding blink came and went");
    /* exactly at the short limit */
    reset();
    run(&b, 8000, 8000, 1);
    run(&b, 8020, 8980, 0);
    ev = run(&b, 9000, 9100, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1, "just under 1 s is still short");
    /* a long hold with no very long binding stays long, however long */
    reset();
    run(&b, 10000, 10000, 1);
    run(&b, 10020, 16000, 0);
    ev = run(&b, 16020, 16200, 1);
    CHECK(count("long") == 1 && ng == 1 && ncue == 3 && cues[2] == BTN_LED_HOLD_END, "a 6 s hold with only short / long bound is one long press (no very long cue)");
    /* configurable threshold */
    init_btn(&b, 3000, DEF, 2);
    reset();
    run(&b, 0, 0, 1);
    run(&b, 20, 2600, 0);
    ev = run(&b, 2620, 2800, 1);
    CHECK(ev == BTN_EV_NONE && ng == 0 && ncue == 2 && cues[0] == BTN_LED_HOLD, "button_long_ms 3000: 2.6 s is not long (dead zone, no confirm cue)");
    reset();
    run(&b, 3000, 6200, 0);
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG, "button_long_ms 3000: hold at 0.5 s, the confirm cue at 3 s");
    ev = run(&b, 6220, 6400, 1);
    CHECK(ev == BTN_EV_GESTURE && count("long") == 1, "button_long_ms 3000: long press");
    CHECK(btn_long_ms_ok(1500) && btn_long_ms_ok(10000) && !btn_long_ms_ok(1499) && !btn_long_ms_ok(10001) && !btn_long_ms_ok(1000),
          "the long press limit: 1500..10000 (always above the 1 s short limit)");
    {
        bg_timing_t bad;
        bg_timing_defaults(&bad);
        bad.long_ms = 50;
        btn_init(&b, &bad);
        CHECK(b.bg.t.long_ms == BTN_LONG_DEFAULT_MS, "a bad timing falls back to the defaults (long 2000)");
    }
}

static void test_locked(void)
{
    btn_t b;
    int ev;
    printf("== calibration lock\n");
    init_btn(&b, 2000, DEF, 2);
    reset();
    run(&b, 0, 500, 1);
    locked_now = 1;
    run(&b, 520, 700, 0);
    ev = run(&b, 720, 900, 1);
    CHECK(ev == BTN_EV_IGNORED && ng == 0, "short press during the calibration: ignored, no event");
    CHECK(ncue == 1 && cues[0] == BTN_LED_ACK, "... but the LED still answers");
    reset();
    locked_now = 1;
    run(&b, 1000, 1000, 1);
    run(&b, 1020, 3300, 0);
    ev = run(&b, 3320, 3500, 1);
    CHECK(ev == BTN_EV_IGNORED && ng == 0 && ncue == 3 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG && cues[2] == BTN_LED_HOLD_END,
          "long press during the calibration: no motion, the hold blink only");
    locked_now = 0;
    reset();
    run(&b, 4000, 4000, 1);
    run(&b, 4020, 4200, 0);
    ev = run(&b, 4220, 4400, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1, "after the calibration the button works again");
    /* the lock is read at the release: a press that began before it ended counts */
    reset();
    run(&b, 5000, 5000, 1);
    run(&b, 5020, 5200, 0);
    locked_now = 1;
    ev = run(&b, 5220, 5400, 1);
    CHECK(ev == BTN_EV_IGNORED, "the lock is judged at the release");
    locked_now = 0;
}

static void test_start_held(void)
{
    btn_t b;
    int ev;
    printf("== held at start\n");
    init_btn(&b, 2000, DEF, 2);
    reset();
    run(&b, 0, 3000, 0); /* held since before the first sample, for 3 s */
    CHECK(ncue == 0, "a button already down at start gives no cue");
    ev = run(&b, 3020, 3200, 1);
    CHECK(ev == BTN_EV_NONE && ng == 0, "... and no action when released");
    run(&b, 3220, 3400, 0);
    ev = run(&b, 3420, 3600, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1, "the next press works");
    /* a read error mid-press does not release it */
    reset();
    init_btn(&b, 2000, DEF, 2);
    run(&b, 0, 500, 1);
    run(&b, 520, 700, 0);
    run(&b, 720, 800, -1);
    ev = run(&b, 820, 1000, 1);
    CHECK(ev == BTN_EV_GESTURE && count("short") == 1, "failed reads in the middle are ignored");
    /* held at start with sequences bound: the stuck press is not an element */
    {
        static const char *const B[] = { "short", "double", "long" };
        init_btn(&b, 2000, B, 3);
        reset();
        run(&b, 0, 1000, 0);
        run(&b, 1020, 3000, 1);
        CHECK(ng == 0, "held at start, sequences bound: no gesture from the stuck press, none pending");
    }
}

/* ---- the gesture engine: timing tables ---- */
static void test_gesture_names(void)
{
    bg_seq_t s, t;
    char n[BG_NAME_MAX + 4];
    bg_timing_t tm;
    printf("== gesture names and timing limits\n");
    CHECK(bg_seq_parse("short", &s) == 0 && s.n == 1 && s.el[0] == BG_S && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "short"), "short");
    CHECK(bg_seq_parse("S S", &s) == 0 && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "double") && bg_seq_parse("double", &t) == 0 && bg_seq_eq(&s, &t), "S S is double");
    CHECK(bg_seq_parse("s_s_s", &s) == 0 && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "triple"), "s_s_s is triple (case and _ free)");
    CHECK(bg_seq_parse("L", &s) == 0 && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "long") && bg_seq_parse("V", &s) == 0 && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "very_long"), "L, V");
    CHECK(bg_seq_parse("S S L", &s) == 0 && s.n == 3 && s.el[2] == BG_L && bg_seq_name(&s, n, sizeof n) > 0 && !strcmp(n, "S S L"), "S S L stays a sequence");
    CHECK(bg_seq_parse("S L S L S L", &s) == 0 && s.n == 6 && bg_seq_name(&s, n, sizeof n) == 11, "six elements");
    CHECK(bg_seq_parse("S S S S S S S", &s) == -1 && bg_seq_parse("", &s) == -1 && bg_seq_parse("S X", &s) == -1 && bg_seq_parse("SS", &s) == -1 && bg_seq_parse("S  S", &s) == -1 &&
              bg_seq_parse("S ", &s) == -1 && bg_seq_parse(" S", &s) == -1 && bg_seq_parse("shorty", &s) == -1 && bg_seq_parse(NULL, &s) == -1,
          "seven elements, empty, unknown token, no separator, double / trailing / leading separator, unknown word, NULL: refused");
    bg_timing_defaults(&tm);
    CHECK(bg_timing_ok(&tm) && tm.debounce_ms == 50 && tm.short_max_ms == 1000 && tm.gap_ms == 400 && tm.long_ms == 2000 && tm.very_long_ms == 5000, "default timing");
    tm.debounce_ms = 9;
    CHECK(!bg_timing_ok(&tm), "debounce below 10");
    bg_timing_defaults(&tm);
    tm.short_max_ms = 1501;
    CHECK(!bg_timing_ok(&tm), "short_max above 1500");
    bg_timing_defaults(&tm);
    tm.gap_ms = 149;
    CHECK(!bg_timing_ok(&tm), "gap below 150");
    bg_timing_defaults(&tm);
    tm.long_ms = 1499;
    CHECK(!bg_timing_ok(&tm), "long below 1500");
    bg_timing_defaults(&tm);
    tm.very_long_ms = 2400;
    CHECK(!bg_timing_ok(&tm), "very_long under long + 500");
    bg_timing_defaults(&tm);
    tm.short_max_ms = 1500;
    tm.long_ms = 1500;
    CHECK(!bg_timing_ok(&tm), "long must be above short_max");
}

static void test_gestures(void)
{
    btn_t b;
    static const char *const B1[] = { "short", "double", "long" };
    static const char *const B2[] = { "short", "double", "triple", "long" };
    static const char *const B3[] = { "short", "S S", "S S L", "long" };
    static const char *const B4[] = { "short", "long", "very_long" };
    static const char *const B5[] = { "S S S S S S" };
    static const char *const B6[] = { "S L S L S L" };
    printf("== gesture timing tables\n");
    /* wait-for-gap: only when something longer is bound */
    {
        static const iv_t one[] = { { 100, 400 } };
        init_btn(&b, 2000, DEF, 2);
        reset();
        PLAY(&b, one, 2000);
        CHECK(ng == 1 && !strcmp(gname[0], "short") && gtime[0] < 520, "short+long bound: one press is reported at the release (< 120 ms after it)");
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, one, 2000);
        CHECK(ng == 1 && !strcmp(gname[0], "short") && gtime[0] >= 400 + 400 && gtime[0] <= 400 + 60 + 400 + 40, "short+double bound: one press waits the 400 ms gap (reported 800..900 ms)");
    }
    {
        static const iv_t two[] = { { 100, 300 }, { 500, 700 } };
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, two, 2000);
        CHECK(ng == 1 && !strcmp(gname[0], "double") && gbound[0] == 1 && gtime[0] <= 700 + 100, "short+double+long bound: two presses 200 ms apart = double, reported at the 2nd release (nothing longer)");
        init_btn(&b, 2000, B2, 4);
        reset();
        PLAY(&b, two, 2000);
        CHECK(ng == 1 && !strcmp(gname[0], "double") && gtime[0] >= 700 + 400, "with triple bound the double waits the gap");
    }
    {
        static const iv_t three[] = { { 100, 300 }, { 500, 700 }, { 900, 1100 } };
        static const iv_t four[] = { { 100, 300 }, { 500, 700 }, { 900, 1100 }, { 1300, 1500 } };
        init_btn(&b, 2000, B2, 4);
        reset();
        PLAY(&b, three, 3000);
        CHECK(ng == 1 && !strcmp(gname[0], "triple") && gtime[0] <= 1100 + 100, "three quick presses = triple, at once");
        reset();
        init_btn(&b, 2000, B2, 4);
        PLAY(&b, four, 3000);
        CHECK(ng == 2 && !strcmp(gname[0], "triple") && !strcmp(gname[1], "short") && gtime[0] <= 1100 + 100, "four presses: the longest match is the triple, the 4th is a new short (longest match, no 4-gesture bound)");
    }
    /* overlapping sequences: short, S S, S S L, long */
    {
        static const iv_t ssl[] = { { 100, 300 }, { 500, 700 }, { 900, 3100 } };
        static const iv_t ss[] = { { 100, 300 }, { 500, 700 } };
        static const iv_t s1[] = { { 100, 300 } };
        static const iv_t sl[] = { { 100, 300 }, { 500, 2700 } };
        static const iv_t sss[] = { { 100, 300 }, { 500, 700 }, { 900, 1100 } };
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, ssl, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "S S L") && gbound[0] == 1 && gtime[0] <= 3100 + 100, "S S L: one gesture, at the long release");
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, ss, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "double") && gbound[0] == 1 && gtime[0] >= 700 + 400, "S S with S S L bound: a double, after the gap");
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, s1, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "short") && gtime[0] >= 300 + 400, "S: a short, after the gap");
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, sl, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "S L") && gbound[0] == 0 && gtime[0] <= 2700 + 100, "S L: not bound, not a prefix of anything: reported unbound at once (exactly one gesture)");
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, sss, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "triple") && gbound[0] == 0, "S S S with S S L bound: an unbound triple, one gesture");
    }
    /* the gap boundary */
    {
        static const iv_t near_[] = { { 100, 300 }, { 600, 800 } };   /* 300 ms gap: one double */
        static const iv_t far_[] = { { 100, 300 }, { 900, 1100 } };   /* 600 ms gap: two shorts */
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, near_, 3000);
        CHECK(ng == 1 && !strcmp(gname[0], "double"), "300 ms between presses (< 400): one double");
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, far_, 3000);
        CHECK(ng == 2 && !strcmp(gname[0], "short") && !strcmp(gname[1], "short") && gtime[0] < 900, "600 ms between presses: two shorts, the first reported before the second press");
    }
    /* noise */
    {
        static const iv_t bounce[] = { { 100, 250 }, { 270, 400 } };        /* a 20 ms release bounce inside one press */
        static const iv_t bounce2[] = { { 100, 300 }, { 320, 340 }, { 500, 700 } }; /* a 20 ms low glitch in the gap */
        static const iv_t glitch[] = { { 100, 140 } };
        static const iv_t chat[] = { { 100, 120 }, { 140, 160 }, { 180, 200 }, { 220, 240 } };
        init_btn(&b, 2000, DEF, 2);
        reset();
        PLAY(&b, bounce, 2000);
        CHECK(ng == 1 && !strcmp(gname[0], "short"), "a 20 ms release bounce inside a press: still one short press");
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, bounce2, 3000);
        CHECK(ng == 1 && !strcmp(gname[0], "double"), "a 20 ms glitch in the gap between two presses is ignored: one double");
        init_btn(&b, 2000, DEF, 2);
        reset();
        PLAY(&b, glitch, 2000);
        CHECK(ng == 0, "a 40 ms low pulse (< debounce) is no press");
        reset();
        PLAY(&b, chat, 2000);
        CHECK(ng == 0, "four 20 ms pulses 20 ms apart (contact chatter) are no press");
        {
            bg_timing_t t;
            bg_timing_defaults(&t);
            t.debounce_ms = 100;
            btn_init(&b, &t);
            bind(&b, DEF, 2);
            reset();
            {
                static const iv_t p80[] = { { 100, 180 } };
                static const iv_t p200[] = { { 100, 300 } };
                PLAY(&b, p80, 2000);
                CHECK(ng == 0, "configurable debounce 100 ms: an 80 ms press is noise");
                PLAY(&b, p200, 2000);
                CHECK(count("short") == 1, "... a 200 ms press is a short press");
            }
        }
    }
    /* dead zone inside a sequence */
    {
        static const iv_t dead1[] = { { 100, 1600 } };
        static const iv_t dead2[] = { { 100, 300 }, { 500, 2000 } };
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, dead1, 4000);
        CHECK(ng == 0, "a 1.5 s press (dead zone) reports nothing");
        init_btn(&b, 2000, B1, 3);
        reset();
        PLAY(&b, dead2, 4000);
        CHECK(ng == 0, "a dead-zone press after a short drops the whole interaction: nothing is reported, not even the first short");
    }
    /* very long */
    {
        static const iv_t h6[] = { { 100, 6100 } };
        static const iv_t h3[] = { { 100, 3100 } };
        init_btn(&b, 2000, B4, 3);
        reset();
        PLAY(&b, h6, 8000);
        CHECK(ng == 1 && !strcmp(gname[0], "very_long") && gbound[0] == 1, "very_long bound: a 6 s hold is a very long press");
        CHECK(ncue == 4 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG && cues[2] == BTN_LED_VLONG && cues[3] == BTN_LED_HOLD_END, "... hold, long and very long cues, then the end");
        init_btn(&b, 2000, B4, 3);
        reset();
        PLAY(&b, h3, 8000);
        CHECK(ng == 1 && !strcmp(gname[0], "long"), "very_long bound: a 3 s hold is still a long press");
    }
    /* the longest sequences */
    {
        static const iv_t six[] = { { 100, 300 }, { 500, 700 }, { 900, 1100 }, { 1300, 1500 }, { 1700, 1900 }, { 2100, 2300 } };
        static const iv_t seven[] = { { 100, 300 }, { 500, 700 }, { 900, 1100 }, { 1300, 1500 }, { 1700, 1900 }, { 2100, 2300 }, { 2500, 2700 } };
        static const iv_t slsl[] = { { 100, 300 }, { 500, 2700 }, { 2900, 3100 }, { 3300, 5500 }, { 5700, 5900 }, { 6100, 8300 } };
        init_btn(&b, 2000, B5, 1);
        reset();
        PLAY(&b, six, 5000);
        CHECK(ng == 1 && !strcmp(gname[0], "S S S S S S") && gtime[0] <= 2300 + 100, "six short presses: one gesture, at the sixth release");
        init_btn(&b, 2000, B5, 1);
        reset();
        PLAY(&b, seven, 5000);
        CHECK(ng == 2 && !strcmp(gname[0], "S S S S S S") && !strcmp(gname[1], "short") && gbound[1] == 0, "seven presses: the six-element gesture, then a new (unbound) short");
        init_btn(&b, 2000, B6, 1);
        reset();
        PLAY(&b, slsl, 12000);
        CHECK(ng == 1 && !strcmp(gname[0], "S L S L S L") && gbound[0] == 1, "S L S L S L (6 elements, 3 of them long)");
    }
    /* configurable timing */
    {
        bg_timing_t t;
        static const iv_t p400[] = { { 100, 500 } };
        static const iv_t pair[] = { { 100, 300 }, { 400, 600 } };
        bg_timing_defaults(&t);
        t.short_max_ms = 300;
        t.gap_ms = 150;
        t.long_ms = 1500;
        btn_init(&b, &t);
        bind(&b, B1, 3);
        reset();
        PLAY(&b, p400, 3000);
        CHECK(ng == 0, "short_max 300: a 400 ms press is a dead zone");
        reset();
        btn_init(&b, &t);
        bind(&b, B1, 3);
        PLAY(&b, pair, 3000);
        CHECK(ng == 1 && !strcmp(gname[0], "double"), "gap 150: two presses 100 ms apart are a double");
    }
    /* the ack cue */
    {
        static const iv_t s1[] = { { 100, 300 } };
        static const iv_t sl[] = { { 100, 300 }, { 500, 2700 } };
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, s1, 3000);
        CHECK(ncue == 1 && cues[0] == BTN_LED_ACK, "a recognised gesture that ends with a short press: the ack blink, once, when it is reported");
        init_btn(&b, 2000, DEF, 2);
        b.led_ack = 0;
        reset();
        PLAY(&b, s1, 3000);
        CHECK(ng == 1 && ncue == 0, "led_ack off: no blink (the gesture is still reported)");
        init_btn(&b, 2000, B3, 4);
        reset();
        PLAY(&b, sl, 5000);
        CHECK(ng == 1 && count(gname[0]) == 1 && gbound[0] == 0 && ncue == 3 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG && cues[2] == BTN_LED_HOLD_END, "an unbound gesture: no ack blink (hold cues only)");
        init_btn(&b, 2000, DEF, 2);
        reset();
        {
            static const iv_t l[] = { { 100, 2600 } };
            PLAY(&b, l, 4000);
        }
        CHECK(count("long") == 1 && cues[ncue - 1] == BTN_LED_HOLD_END && ncue == 3, "a long press gets no ack (its hold cues were the confirmation)");
    }
    /* the bindings change while a sequence is half made */
    {
        static const iv_t s1[] = { { 100, 300 } };
        init_btn(&b, 2000, B1, 3);
        reset();
        play(&b, 0, s1, 1, 500); /* a short, waiting for a second press */
        bind(&b, DEF, 2);        /* nothing longer is bound any more: the gap still decides */
        play(&b, 520, s1, 1, 1500);
        CHECK(ng == 1 && !strcmp(gname[0], "short"), "bindings changed in the gap: the pending short is still reported once");
    }
}

/* Random waveforms: the invariants of the engine. */
static unsigned rs = 12345u;
static unsigned rnd(void) { rs = rs * 1103515245u + 12345u; return (rs >> 16) & 0x7fff; }

static void test_fuzz(void)
{
    static const char *const NAMES[] = { "short", "double", "triple", "long", "very_long", "S S L", "S L", "L S", "S L S", "V S", "S S S L" };
    printf("== gesture engine invariants on random waveforms\n");
    {
        int run_i, bad = 0, total = 0, over = 0, notparse = 0, later = 0, pend = 0;
        for (run_i = 0; run_i < 3000; run_i++) {
            btn_t b;
            const char *pick[BG_MAX_BOUND];
            int nb = 1 + (int)(rnd() % 6), i, presses = 0, ev;
            int64_t t = 1000, last_release = 0;
            bg_timing_t tm;
            bg_timing_defaults(&tm);
            tm.gap_ms = 150 + (int)(rnd() % 850);
            tm.short_max_ms = 200 + (int)(rnd() % 1300);
            tm.long_ms = tm.short_max_ms + 100 + (int)(rnd() % 3000);
            if (tm.long_ms < 1500)
                tm.long_ms = 1500;
            tm.very_long_ms = tm.long_ms + 500 + (int)(rnd() % 3000);
            for (i = 0; i < nb; i++)
                pick[i] = NAMES[rnd() % (sizeof NAMES / sizeof NAMES[0])];
            btn_init(&b, &tm);
            bind(&b, pick, nb);
            g_btn = &b;
            reset();
            run(&b, 0, 980, 1);
            ev = (int)(1 + rnd() % 9);
            while (ev--) {
                int64_t hold = (rnd() % 4 == 0) ? 20 + (int)(rnd() % 80) : 60 + (int)(rnd() % 7000);
                int64_t gapm = (rnd() % 3 == 0) ? 20 + (int)(rnd() % 120) : 100 + (int)(rnd() % 1500);
                run(&b, t, t + hold, 0);
                t += hold + 20;
                run(&b, t, t + gapm, 1);
                t += gapm + 20;
                presses++;
                last_release = t;
            }
            run(&b, t, t + 3000, 1); /* flush: everything pending must have been reported */
            for (i = 0; i < ng; i++) {
                bg_seq_t s;
                total++;
                if (bg_seq_parse(gname[i], &s) != 0)
                    notparse++;
                else if (s.n < 1 || s.n > BG_MAX_SEQ)
                    over++;
            }
            if (ng > presses)
                bad++;
            if (down_at_emit)
                later++;
            if (b.bg.cur.n != 0 || b.bg.down)
                pend++;
            (void)last_release;
        }
        CHECK(total > 1000, "the random runs produced gestures");
        CHECK(notparse == 0 && over == 0, "every reported gesture is a valid name of 1 to 6 elements");
        CHECK(bad == 0, "never more gestures than presses");
        CHECK(later == 0, "a gesture is never reported while the button is down");
        CHECK(pend == 0, "after the flush time nothing is left pending (no lost, no stuck sequence)");
    }
    g_btn = NULL;
}

/* ---- bindings, actions, modes ---- */
static const char DEFAULT_JSON[] =
    "{\"timing\":{\"debounce_ms\":50,\"short_max_ms\":1000,\"gap_ms\":400,\"long_ms\":2000,\"very_long_ms\":5000},\"led_ack\":true,"
    "\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]},{\"gesture\":\"long\",\"actions\":[\"ptz.toggle_park\"]}]}";

static int key_check(const char *k, const char *v)
{
    (void)v;
    return !strcmp(k, "led.mode") || !strcmp(k, "track.enabled") || !strcmp(k, "image.brightness") || !strcmp(k, "detect.publish_hz") || !strcmp(k, "led.person") ? 0 : -1;
}

static int act_ok(const char *text, int kind)
{
    bc_action_t a;
    char back[BC_ACT_TEXT_MAX + 1];
    if (bc_action_parse(text, &a, NULL, 0) != 0 || a.kind != kind)
        return 0;
    return bc_action_text(&a, back, sizeof back) > 0 && !strcmp(back, text);
}

static void test_cfg(void)
{
    bc_cfg_t c, d;
    bc_diag_t dg;
    char out[BC_TEXT_MAX + 64], big[9000];
    int i;
    printf("== bindings: the action whitelist\n");
    CHECK(act_ok("none", BA_NONE) && act_ok("ptz.home", BA_HOME) && act_ok("ptz.park", BA_PARK) && act_ok("ptz.recalibrate", BA_RECAL) && act_ok("ptz.toggle_park", BA_TOGGLE_PARK),
          "none, ptz.home, ptz.park, ptz.recalibrate, ptz.toggle_park");
    CHECK(act_ok("ptz.privacy on", BA_PRIVACY) && act_ok("ptz.privacy off", BA_PRIVACY) && act_ok("ptz.privacy toggle", BA_PRIVACY) && act_ok("track on", BA_TRACK) &&
              act_ok("track off", BA_TRACK) && act_ok("track toggle", BA_TRACK),
          "ptz.privacy and track: on, off, toggle");
    CHECK(act_ok("ptz.goto_preset door", BA_GOTO) && act_ok("patrol start round", BA_PATROL_START) && act_ok("patrol stop", BA_PATROL_STOP) && act_ok("mode night", BA_MODE) &&
              act_ok("track.lost_policy backsweep,exit,hold:30,home", BA_LOST) && act_ok("led pattern 3:150:150:0", BA_LED) && act_ok("led pattern ack", BA_LED) && act_ok("led pattern on", BA_LED),
          "goto_preset, patrol start / stop, mode, lost_policy, led pattern (number or name)");
    {
        static const char *const NO[] = { "reboot", "settings.reset", "settings.save", "ptz.step left", "ptz.move", "keys new", "api.key.new x", "net.wifi_band 5g", "net wifi set country FR",
                                          "wifi set band 5", "hostname set x", "factory_reset", "reset", "system reboot", "ptz.home now", "none now", "ptz.privacy", "ptz.privacy maybe",
                                          "track", "track maybe", "patrol", "patrol start", "patrol start Bad", "patrol go x", "ptz.goto_preset", "ptz.goto_preset Door", "ptz.goto_preset 1door",
                                          "mode", "mode Night", "mode a b", "led pattern", "led pattern 300:1:1:1", "led pattern 1:5:100:0", "led pattern 1:100:100", "led pattern x",
                                          "track.lost_policy", "track.lost_policy a b", "track.lost_policy a;b", " ptz.home", "ptz.home ", "ptz.home  x", "PTZ.HOME", "", "ptz.park;reboot",
                                          "ptz.privacy on; reboot", "mode night; reset" };
        int bad = 0;
        for (i = 0; i < (int)(sizeof NO / sizeof NO[0]); i++) {
            bc_action_t a;
            if (bc_action_parse(NO[i], &a, NULL, 0) == 0) {
                printf("  accepted: %s\n", NO[i]);
                bad++;
            }
        }
        CHECK(bad == 0, "nothing destructive or off the list parses (reset, reboot, key and network changes, other settings, lookalikes)");
    }
    bc_defaults(&c, 0);
    CHECK(bc_cfg_json(&c, out, sizeof out) > 0 && !strcmp(out, DEFAULT_JSON), "the defaults are short -> none, long -> ptz.toggle_park, default timing, led_ack on (golden JSON)");
    CHECK(bc_cfg_parse(DEFAULT_JSON, strlen(DEFAULT_JSON), &d, &dg) == 0 && d.nbind == 2 && d.led_ack == 1, "the defaults parse back");
    bc_defaults(&c, 3000);
    CHECK(c.t.long_ms == 3000 && c.t.very_long_ms == 5000 && bc_cfg_json(&c, out, sizeof out) > 0 && strstr(out, "\"long_ms\":3000"), "button_long_ms of akam.conf seeds the default long press");
    bc_defaults(&c, 9900);
    CHECK(c.t.very_long_ms == 10400, "... and keeps very_long above it");
    bc_defaults(&c, 0);
    {
        static const char G[] = "{\"timing\":{\"gap_ms\":300},\"bindings\":[{\"gesture\":\"double\",\"actions\":[\"ptz.home\",\"led pattern ack\"]},{\"gesture\":\"S S L\",\"actions\":[\"track toggle\"]},"
                                 "{\"gesture\":\"very_long\",\"actions\":[\"ptz.privacy on\",\"patrol stop\",\"none\",\"ptz.park\"]}]}";
        CHECK(bc_cfg_parse(G, sizeof G - 1, &c, &dg) == 0 && c.nbind == 3 && c.t.gap_ms == 300 && c.t.long_ms == 2000 && c.led_ack == 1 && c.bind[2].nact == 4,
              "a partial timing keeps the other defaults; led_ack defaults on");
        CHECK(bc_cfg_json(&c, out, sizeof out) > 0 && bc_cfg_parse(out, strlen(out), &d, &dg) == 0 && bc_cfg_json(&d, big, sizeof big) > 0 && !strcmp(out, big), "canonical JSON round trip");
    }
    {
        static const char *const BAD[] = {
            "", "[]", "{", "{\"bindings\":[]", "{\"bindings\":{}}", "{\"led_ack\":true}", "{\"bindings\":[],\"x\":1}",
            "{\"bindings\":[{\"gesture\":\"short\"}]}", "{\"bindings\":[{\"actions\":[\"none\"]}]}", "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[]}]}",
            "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"],\"x\":1}]}", "{\"bindings\":[{\"gesture\":\"oops\",\"actions\":[\"none\"]}]}",
            "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"reboot\"]}]}", "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[1]}]}",
            "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]},{\"gesture\":\"S\",\"actions\":[\"none\"]}]}",
            "{\"timing\":{\"gap_ms\":10},\"bindings\":[]}", "{\"timing\":{\"gap\":300},\"bindings\":[]}", "{\"timing\":{\"gap_ms\":\"300\"},\"bindings\":[]}",
            "{\"led_ack\":1,\"bindings\":[]}", "{\"bindings\":[]} x", "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\",\"none\",\"none\",\"none\",\"none\"]}]}",
            "{\"bindings\":[],\"bindings\":[]}x"
        };
        int bad = 0;
        for (i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
            if (bc_cfg_parse(BAD[i], strlen(BAD[i]), &d, &dg) == 0) {
                printf("  accepted: %s\n", BAD[i]);
                bad++;
            }
        }
        CHECK(bad == 0, "malformed, unknown keys, missing parts, unknown gestures / actions, duplicate gesture, bad timing: all refused");
    }
    {
        /* limits: 16 bindings pass, 17 do not; 4 actions pass, 5 do not */
        char *p = big;
        const char *seqs[] = { "short", "double", "triple", "long", "very_long", "S L", "L S", "S S L", "S L S", "L L", "S V", "V S", "L S S", "S S S L", "S S S S", "S S S S S" };
        p += sprintf(p, "{\"bindings\":[");
        for (i = 0; i < 16; i++)
            p += sprintf(p, "%s{\"gesture\":\"%s\",\"actions\":[\"ptz.home\"]}", i ? "," : "", seqs[i]);
        p += sprintf(p, "]}");
        CHECK(bc_cfg_parse(big, strlen(big), &d, &dg) == 0 && d.nbind == 16, "16 bindings are accepted");
        p = big;
        p += sprintf(p, "{\"bindings\":[");
        for (i = 0; i < 16; i++)
            p += sprintf(p, "%s{\"gesture\":\"%s\",\"actions\":[\"ptz.home\"]}", i ? "," : "", seqs[i]);
        p += sprintf(p, ",{\"gesture\":\"S S S S S S\",\"actions\":[\"ptz.home\"]}]}");
        CHECK(bc_cfg_parse(big, strlen(big), &d, &dg) == BC_E_LIMIT, "a 17th binding is refused (limit)");
        {
            static const char A4[] = "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"ptz.home\",\"ptz.park\",\"ptz.home\",\"ptz.park\"]}]}";
            static const char A5[] = "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"ptz.home\",\"ptz.park\",\"ptz.home\",\"ptz.park\",\"ptz.home\"]}]}";
            CHECK(bc_cfg_parse(A4, sizeof A4 - 1, &d, &dg) == 0 && d.bind[0].nact == 4 && bc_cfg_parse(A5, sizeof A5 - 1, &d, &dg) == BC_E_LIMIT, "4 actions per binding are accepted, 5 are refused");
        }
        memset(big, ' ', sizeof big);
        CHECK(bc_cfg_parse(big, sizeof big, &d, &dg) == BC_E_TOOBIG, "a body over 8192 bytes is refused");
    }
    /* mode references */
    {
        bc_modes_t m;
        bc_mode_t mode;
        static const char G[] = "{\"bindings\":[{\"gesture\":\"double\",\"actions\":[\"mode night\"]}]}";
        memset(&m, 0, sizeof m);
        CHECK(bc_cfg_parse(G, sizeof G - 1, &c, &dg) == 0 && bc_cfg_check_refs(&c, &m, &dg) == BC_E_REF && strstr(dg.detail, "night"), "a binding to a mode that does not exist is refused");
        CHECK(bc_mode_parse("night", "{\"settings\":{\"led.mode\":\"off\"}}", strlen("{\"settings\":{\"led.mode\":\"off\"}}"), key_check, &mode, &dg) == 0 && bc_modes_set(&m, &mode) == 0 && bc_cfg_check_refs(&c, &m, &dg) == 0, "... and accepted once the mode exists");
        CHECK(bc_modes_del(&m, "night", &c) == BC_E_REF && bc_modes_del(&m, "night", NULL) == 0 && m.n == 0 && bc_modes_del(&m, "night", NULL) == BC_E_VALUE, "a mode in use cannot be deleted; an unused one can");
    }
}

static void test_modes(void)
{
    bc_mode_t m;
    bc_modes_t ms, t2;
    bc_diag_t dg;
    char out[BC_TEXT_MAX + 64], body[1200];
    int i;
    printf("== modes\n");
    {
        static const char B[] = "{\"settings\":{\"led.mode\":\"off\",\"track.enabled\":true,\"detect.publish_hz\":4}}";
        CHECK(bc_mode_parse("night", B, sizeof B - 1, key_check, &m, &dg) == 0 && m.n == 3 && !strcmp(m.val[1], "1") && !strcmp(m.val[2], "4") && !strcmp(m.name, "night"),
              "a mode: values may be strings, integers or booleans (true = 1)");
        CHECK(bc_mode_json(&m, out, sizeof out) > 0 && !strcmp(out, "{\"name\":\"night\",\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"1\",\"detect.publish_hz\":\"4\"}}"), "mode JSON (values as text)");
    }
    CHECK(bc_mode_parse("night", "{\"settings\":{\"net.hostname\":\"x\"}}", strlen("{\"settings\":{\"net.hostname\":\"x\"}}"), key_check, &m, &dg) == BC_E_VALUE && strstr(dg.detail, "net.hostname"), "a key the registry does not allow (not writable, secret, net.*, ...) is refused by the check");
    CHECK(bc_mode_parse("Night", "{\"settings\":{\"led.mode\":\"off\"}}", strlen("{\"settings\":{\"led.mode\":\"off\"}}"), key_check, &m, &dg) == BC_E_VALUE, "mode name: a-z first");
    CHECK(bc_mode_parse("night", "{\"settings\":{}}", strlen("{\"settings\":{}}"), key_check, &m, &dg) == BC_E_VALUE, "an empty mode is refused");
    CHECK(bc_mode_parse("night", "{\"name\":\"day\",\"settings\":{\"led.mode\":\"off\"}}", strlen("{\"name\":\"day\",\"settings\":{\"led.mode\":\"off\"}}"), key_check, &m, &dg) == BC_E_VALUE, "the body name must match the path");
    CHECK(bc_mode_parse("night", "{\"settings\":{\"led.mode\":\"off\",\"led.mode\":\"on\"}}", strlen("{\"settings\":{\"led.mode\":\"off\",\"led.mode\":\"on\"}}"), key_check, &m, &dg) == BC_E_VALUE, "a key twice is refused");
    CHECK(bc_mode_parse("night", "{\"settings\":{\"LED.mode\":\"off\"}}", strlen("{\"settings\":{\"LED.mode\":\"off\"}}"), NULL, &m, &dg) == BC_E_VALUE, "a key of a-z 0-9 _ . only");
    CHECK(bc_mode_parse("night", "{\"settings\":{\"led.mode\":\"off\"},\"x\":1}", strlen("{\"settings\":{\"led.mode\":\"off\"},\"x\":1}"), key_check, &m, &dg) == BC_E_VALUE, "unknown key");
    CHECK(bc_mode_parse("night", "{\"settings\":{\"led.mode\":[1]}}", strlen("{\"settings\":{\"led.mode\":[1]}}"), key_check, &m, &dg) != 0, "a value that is not a scalar is refused");
    {
        char *p = body;
        p += sprintf(p, "{\"settings\":{");
        for (i = 0; i < 9; i++)
            p += sprintf(p, "%s\"k%d\":\"v\"", i ? "," : "", i);
        sprintf(p, "}}");
        CHECK(bc_mode_parse("big", body, strlen(body), NULL, &m, &dg) == BC_E_LIMIT, "9 pairs: over the limit of 8");
        body[0] = '\0';
        p = body;
        p += sprintf(p, "{\"settings\":{");
        for (i = 0; i < 8; i++)
            p += sprintf(p, "%s\"k%d\":\"v\"", i ? "," : "", i);
        sprintf(p, "}}");
        CHECK(bc_mode_parse("big", body, strlen(body), NULL, &m, &dg) == 0 && m.n == 8, "8 pairs are accepted");
    }
    memset(&ms, 0, sizeof ms);
    for (i = 0; i < 8; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "m%d", i);
        bc_mode_parse(nm, "{\"settings\":{\"led.mode\":\"off\"}}", strlen("{\"settings\":{\"led.mode\":\"off\"}}"), key_check, &m, &dg);
        if (bc_modes_set(&ms, &m) != 0)
            break;
    }
    bc_mode_parse("m8", "{\"settings\":{\"led.mode\":\"off\"}}", strlen("{\"settings\":{\"led.mode\":\"off\"}}"), key_check, &m, &dg);
    CHECK(i == 8 && ms.n == 8 && bc_modes_set(&ms, &m) == BC_E_LIMIT, "8 modes fit, a 9th is refused");
    bc_mode_parse("m3", "{\"settings\":{\"led.mode\":\"on\"}}", strlen("{\"settings\":{\"led.mode\":\"on\"}}"), key_check, &m, &dg);
    CHECK(bc_modes_set(&ms, &m) == 0 && ms.n == 8 && !strcmp(ms.m[3].val[0], "on"), "an existing mode is replaced");
    CHECK(bc_modes_json(&ms, out, sizeof out) > 0 && bc_modes_parse(out, strlen(out), key_check, &t2, &dg) == 0 && t2.n == 8 && !strcmp(t2.m[3].val[0], "on"), "the modes list round trips");
    CHECK(bc_modes_parse("{\"modes\":[{\"name\":\"a\",\"settings\":{\"led.mode\":\"on\"}},{\"name\":\"a\",\"settings\":{\"led.mode\":\"on\"}}]}", strlen("{\"modes\":[{\"name\":\"a\",\"settings\":{\"led.mode\":\"on\"}},{\"name\":\"a\",\"settings\":{\"led.mode\":\"on\"}}]}"), NULL, &t2, &dg) != 0, "a duplicate mode in the file is refused");
    CHECK(bc_modes_parse("{\"modes\":[]}", strlen("{\"modes\":[]}"), NULL, &t2, &dg) == 0 && t2.n == 0 && bc_modes_parse("{\"mode\":[]}", strlen("{\"mode\":[]}"), NULL, &t2, &dg) != 0, "empty list ok, wrong key refused");
}

/* ---- dispatch with fake ops ---- */
static struct { int kind[16]; char arg[16][40]; int n; int apply_calls, apply_n; char apply_keys[8][48]; char apply_vals[8][48]; int fail_kind; int fail_apply; } W;

static int d_run(void *u, int kind, const char *arg)
{
    (void)u;
    if (W.n < 16) {
        W.kind[W.n] = kind;
        snprintf(W.arg[W.n], sizeof W.arg[0], "%s", arg);
        W.n++;
    }
    return kind == W.fail_kind ? -1 : 0;
}

static int d_apply(void *u, int n, const char *const *k, const char *const *v)
{
    int i;
    (void)u;
    W.apply_calls++;
    W.apply_n = n;
    for (i = 0; i < n && i < 8; i++) {
        snprintf(W.apply_keys[i], sizeof W.apply_keys[0], "%s", k[i]);
        snprintf(W.apply_vals[i], sizeof W.apply_vals[0], "%s", v[i]);
    }
    return W.fail_apply ? -1 : 0;
}

static void test_dispatch(void)
{
    static const bc_ops_t ops = { 0, d_run, d_apply };
    bc_cfg_t c;
    bc_modes_t ms;
    bc_mode_t m;
    bc_diag_t dg;
    bg_seq_t s;
    char json[300];
    static const char G[] =
        "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]},{\"gesture\":\"double\",\"actions\":[\"ptz.home\",\"led pattern ack\",\"track toggle\",\"ptz.goto_preset door\"]},"
        "{\"gesture\":\"triple\",\"actions\":[\"mode night\",\"patrol start round\"]},{\"gesture\":\"long\",\"actions\":[\"mode ghost\"]},{\"gesture\":\"very_long\",\"actions\":[\"ptz.privacy on\",\"ptz.park\"]}]}";
    printf("== dispatch (fake ops)\n");
    memset(&ms, 0, sizeof ms);
    bc_mode_parse("night", "{\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\",\"detect.publish_hz\":\"1\"}}", strlen("{\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\",\"detect.publish_hz\":\"1\"}}"), key_check, &m, &dg);
    bc_modes_set(&ms, &m);
    CHECK(bc_cfg_parse(G, sizeof G - 1, &c, &dg) == 0, "test config");
    memset(&W, 0, sizeof W);
    W.fail_kind = -1;
    bg_seq_parse("double", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_OK && W.n == 4 && W.kind[0] == BA_HOME && W.kind[1] == BA_LED && !strcmp(W.arg[1], "2:150:150:0") && W.kind[2] == BA_TRACK &&
              !strcmp(W.arg[2], "toggle") && W.kind[3] == BA_GOTO && !strcmp(W.arg[3], "door"),
          "double: the four actions run in order, the LED name is resolved to its pattern");
    memset(&W, 0, sizeof W);
    W.fail_kind = BA_LED;
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_PARTIAL && W.n == 4, "a failing action does not stop the next ones: partial");
    memset(&W, 0, sizeof W);
    bg_seq_parse("short", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_OK && W.n == 0 && W.apply_calls == 0, "short -> none: ok, nothing runs");
    bg_seq_parse("S S L", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_UNBOUND && W.n == 0, "an unbound gesture runs nothing");
    memset(&W, 0, sizeof W);
    bg_seq_parse("triple", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_OK && W.apply_calls == 1 && W.apply_n == 3 && !strcmp(W.apply_keys[0], "led.mode") && !strcmp(W.apply_vals[0], "off") &&
              !strcmp(W.apply_keys[2], "detect.publish_hz") && W.n == 1 && W.kind[0] == BA_PATROL_START && !strcmp(W.arg[0], "round"),
          "a mode is ONE apply call with the whole list (the registry applies it all or nothing), then the next action");
    memset(&W, 0, sizeof W);
    W.fail_apply = 1;
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_PARTIAL && W.apply_calls == 1 && W.n == 1, "a refused mode apply fails that action only (partial); no pair is retried one by one");
    memset(&W, 0, sizeof W);
    bg_seq_parse("long", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_FAILED && W.apply_calls == 0, "a mode that does not exist: failed, nothing applied");
    memset(&W, 0, sizeof W);
    W.fail_kind = BA_PRIVACY;
    bg_seq_parse("very_long", &s);
    CHECK(bc_dispatch(&c, &ms, &s, &ops) == BC_R_PARTIAL && W.n == 2 && W.kind[0] == BA_PRIVACY && !strcmp(W.arg[0], "on") && W.kind[1] == BA_PARK, "ptz.privacy on then ptz.park");
    CHECK(!strcmp(bc_result_name(BC_R_OK), "ok") && !strcmp(bc_result_name(BC_R_UNBOUND), "unbound") && !strcmp(bc_result_name(BC_R_PARTIAL), "partial") && !strcmp(bc_result_name(BC_R_FAILED), "failed"),
          "result names");
    bg_seq_parse("double", &s);
    CHECK(bc_event_json(&s, bc_cfg_find(&c, &s), BC_R_OK, json, sizeof json, 144) > 0 &&
              !strcmp(json, "{\"gesture\":\"double\",\"actions\":[\"ptz.home\",\"led pattern ack\",\"track toggle\",\"ptz.goto_preset door\"],\"result\":\"ok\"}"),
          "event JSON: gesture, actions, result");
    bg_seq_parse("S S L", &s);
    CHECK(bc_event_json(&s, NULL, BC_R_UNBOUND, json, sizeof json, 144) > 0 && !strcmp(json, "{\"gesture\":\"S S L\",\"actions\":[],\"result\":\"unbound\"}"), "event JSON of an unbound gesture");
    {
        bc_cfg_t big;
        static const char H[] = "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"track.lost_policy backsweep,exit,hold:30,home,backsweep,exit,hold:30,home,backsweep,exit,hold:30\","
                                "\"track.lost_policy backsweep,exit,hold:30,home,backsweep,exit,hold:30,home,backsweep,exit,hold:30\",\"ptz.home\",\"ptz.park\"]}]}";
        bg_seq_parse("short", &s);
        CHECK(bc_cfg_parse(H, sizeof H - 1, &big, &dg) == 0 && bc_event_json(&s, &big.bind[0], BC_R_OK, json, sizeof json, 144) > 0 && strlen(json) <= 144 && strstr(json, "\"more\":") &&
                  strstr(json, "\"result\":\"ok\""),
              "an event that would not fit the 144-byte ring drops trailing actions and says how many (more)");
    }
}

/* ---- the runtime: defaults identical to the factory button, persistence only on change, HTTP ---- */
static struct {
    int kind[16];
    char arg[16][48];
    int nrun;
    char ev[12][300];
    int nev;
    char mq[12][24];
    char mqj[12][300];
    int nmq, cue_ack, cue_total, apply_calls, apply_n, changed, locked, fail_apply;
} R;
static int64_t rt_t;

static int r_run(void *u, int kind, const char *arg)
{
    (void)u;
    if (R.nrun < 16) {
        R.kind[R.nrun] = kind;
        snprintf(R.arg[R.nrun], sizeof R.arg[0], "%s", arg);
        R.nrun++;
    }
    return 0;
}
static int r_apply(void *u, int n, const char *const *k, const char *const *v)
{
    (void)u;
    (void)k;
    (void)v;
    R.apply_calls++;
    R.apply_n = n;
    return R.fail_apply ? -1 : 0;
}
static int r_locked(void *u) { (void)u; return R.locked; }
static void r_led(void *u, int cue)
{
    (void)u;
    R.cue_total++;
    R.cue_ack += cue == BTN_LED_ACK;
}
static void r_event(void *u, const char *json)
{
    (void)u;
    if (R.nev < 12)
        snprintf(R.ev[R.nev++], sizeof R.ev[0], "%s", json);
}
static void r_mqtt(void *u, const char *g, const char *json)
{
    (void)u;
    if (R.nmq < 12) {
        snprintf(R.mq[R.nmq], sizeof R.mq[0], "%s", g);
        snprintf(R.mqj[R.nmq], sizeof R.mqj[0], "%s", json);
        R.nmq++;
    }
}
static void r_changed(void *u) { (void)u; R.changed++; }
static int64_t r_now(void *u) { (void)u; return rt_t; }
static const a2k_btn_ops_t RT_OPS = { 0, r_run, r_apply, r_locked, r_led, r_event, r_mqtt, r_changed, key_check, r_now };

/* one press of `hold` ms, then `after` ms of released line */
static void rt_press(int hold, int after)
{
    int64_t end = rt_t + hold;
    for (; rt_t < end; rt_t += 20)
        (void)a2k_btn_step(&BTN, rt_t, 0);
    end = rt_t + after;
    for (; rt_t < end; rt_t += 20)
        (void)a2k_btn_step(&BTN, rt_t, 1);
}

static int http(const char *method, const char *sub, const char *body, char *out, size_t cap, unsigned *retry)
{
    unsigned r = 0;
    int st = a2k_btn_http(method, sub, body, body ? strlen(body) : 0, out, cap, &r);
    if (retry)
        *retry = r;
    return st;
}

static void test_rt(const char *dir)
{
    char pb[300], pm[300], out[BC_TEXT_MAX + 64], last[BG_NAME_MAX + 4], names[BC_MAX_BIND][BG_NAME_MAX], text[BC_TEXT_MAX + 64];
    unsigned retry = 0, w0;
    int st, i, n;
    FILE *f;
    static const char BODY[] =
        "{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]},{\"gesture\":\"double\",\"actions\":[\"mode night\",\"ptz.home\"]},{\"gesture\":\"long\",\"actions\":[\"ptz.toggle_park\"]},"
        "{\"gesture\":\"S S L\",\"actions\":[\"track toggle\"]}]}";
    printf("== runtime: defaults, persistence, HTTP\n");
    CHECK(A2K_BTN_EVT_MAX == A2K_EVT_DATA, "the event text limit is the API event ring limit");
    snprintf(pb, sizeof pb, "%s/button", dir);
    snprintf(pm, sizeof pm, "%s/modes", dir);
    a2k_btn_set_paths(pb, pm);
    memset(&R, 0, sizeof R);
    rt_t = 0;
    CHECK(a2k_btn_init(&RT_OPS, 2000) == 0, "init without files");
    st = http("GET", "button", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && !strcmp(out, DEFAULT_JSON), "no file: GET /button returns the defaults (short -> none, long -> ptz.toggle_park)");
    n = a2k_btn_gestures(names);
    CHECK(n == 2 && !strcmp(names[0], "short") && !strcmp(names[1], "long"), "the configured gestures: short, long");
    a2k_btn_last_gesture(last, sizeof last);
    CHECK(!strcmp(last, "none"), "button.last_gesture is none before any press");
    rt_press(1000, 1000); /* released line to start the debouncer */
    memset(&R, 0, sizeof R);
    rt_press(300, 1500);
    CHECK(R.nev == 1 && !strcmp(R.ev[0], "{\"gesture\":\"short\",\"actions\":[\"none\"],\"result\":\"ok\"}") && R.nmq == 1 && !strcmp(R.mq[0], "short") && !strcmp(R.mqj[0], R.ev[0]) &&
              R.nrun == 0 && R.apply_calls == 0 && R.cue_ack == 1,
          "DEFAULT short press: one API event + one MQTT message, no action, the ack blink (as the factory button)");
    a2k_btn_last_gesture(last, sizeof last);
    CHECK(!strcmp(last, "short"), "button.last_gesture = short");
    memset(&R, 0, sizeof R);
    rt_press(2500, 1500);
    CHECK(R.nrun == 1 && R.kind[0] == BA_TOGGLE_PARK && R.nev == 1 && !strcmp(R.ev[0], "{\"gesture\":\"long\",\"actions\":[\"ptz.toggle_park\"],\"result\":\"ok\"}") && R.nmq == 1 && R.cue_ack == 0,
          "DEFAULT long press: the park / home toggle, one event, one MQTT message, no ack blink");
    memset(&R, 0, sizeof R);
    R.locked = 1;
    rt_press(300, 1500);
    rt_press(2500, 1500);
    CHECK(R.nev == 0 && R.nmq == 0 && R.nrun == 0 && R.cue_ack == 1, "during the calibration nothing is dispatched or published (the ack blink only)");
    R.locked = 0;
    /* persistence only on change */
    w0 = a2k_store_writes();
    st = http("POST", "modes/night", "{\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\"}}", out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"created\":true") && a2k_store_writes() == w0 + 1 && a2k_btn_writes() == 1, "POST /modes/night: created, one flash write");
    st = http("POST", "modes/night", "{\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\"}}", out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"created\":false") && a2k_store_writes() == w0 + 1 && a2k_btn_writes() == 1, "the same mode again: no write");
    st = http("GET", "modes", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && !strcmp(out, "{\"modes\":[{\"name\":\"night\",\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\"}}]}"), "GET /modes");
    st = http("GET", "modes/night", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && !strcmp(out, "{\"name\":\"night\",\"settings\":{\"led.mode\":\"off\",\"track.enabled\":\"0\"}}"), "GET /modes/night");
    CHECK(http("GET", "modes/nope", NULL, out, sizeof out, NULL) == 404, "GET /modes/nope: 404");
    st = http("POST", "modes/bad", "{\"settings\":{\"net.hostname\":\"x\"}}", out, sizeof out, NULL);
    CHECK(st == 422 && strstr(out, "net.hostname") && a2k_store_writes() == w0 + 1, "a mode with a key the registry refuses: 422, nothing written");
    st = http("POST", "button", "{\"bindings\":[{\"gesture\":\"double\",\"actions\":[\"mode ghost\"]}]}", out, sizeof out, NULL);
    CHECK(st == 422 && strstr(out, "no_such_mode") && a2k_btn_writes() == 1, "a binding to a mode that does not exist: 422");
    st = http("POST", "button", "{\"bindings\":[{\"gesture\":\"double\",\"actions\":[\"reboot\"]}]}", out, sizeof out, NULL);
    CHECK(st == 422 && strstr(out, "not an allowed action"), "a destructive action is refused: 422");
    st = http("POST", "button", "{\"bindings\":", out, sizeof out, NULL);
    CHECK(st == 400 && strstr(out, "syntax"), "malformed JSON: 400");
    st = http("GET", "button", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && !strcmp(out, DEFAULT_JSON) && R.changed == 0, "all those refusals changed nothing (all or nothing)");
    st = http("POST", "button", BODY, out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"changed\":true") && R.changed == 1 && a2k_btn_writes() == 2 && a2k_store_writes() == w0 + 2, "POST /button: applied, one flash write, MQTT told to announce again");
    n = a2k_btn_gestures(names);
    CHECK(n == 4 && !strcmp(names[1], "double") && !strcmp(names[3], "S S L"), "the new gesture list (the MQTT device triggers follow it)");
    w0 = a2k_store_writes();
    st = http("POST", "button", BODY, out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"changed\":false") && a2k_store_writes() == w0 && a2k_btn_writes() == 2, "the same config again: no flash write");
    st = http("GET", "button", NULL, out, sizeof out, NULL);
    f = fopen(pb, "r");
    text[0] = '\0';
    if (f) {
        size_t r = fread(text, 1, sizeof text - 1, f);
        text[r] = '\0';
        fclose(f);
    }
    CHECK(st == 200 && !strcmp(out, text), "the file on the store is the GET text");
    /* simulated presses with the new bindings: no flash write at all */
    memset(&R, 0, sizeof R);
    rt_press(200, 200);
    rt_press(200, 1500);
    CHECK(R.nev == 1 && strstr(R.ev[0], "\"gesture\":\"double\"") && strstr(R.ev[0], "\"actions\":[\"mode night\",\"ptz.home\"]") && strstr(R.ev[0], "\"result\":\"ok\"") && R.apply_calls == 1 &&
              R.apply_n == 2 && R.nrun == 1 && R.kind[0] == BA_HOME,
          "double: mode night (one apply of its 2 pairs) then ptz.home; event with the action list");
    memset(&R, 0, sizeof R);
    R.fail_apply = 1;
    rt_press(200, 200);
    rt_press(200, 1500);
    CHECK(R.nev == 1 && strstr(R.ev[0], "\"result\":\"partial\"") && R.apply_calls == 1 && R.nrun == 1, "the registry refuses the mode: result partial, the other action still ran");
    R.fail_apply = 0;
    memset(&R, 0, sizeof R);
    rt_press(300, 1500);
    CHECK(R.nev == 1 && strstr(R.ev[0], "\"gesture\":\"short\"") && R.cue_ack == 1, "short is still reported after the gap now that double is bound");
    memset(&R, 0, sizeof R);
    rt_press(200, 200);
    rt_press(200, 200);
    rt_press(2400, 1500);
    CHECK(R.nev == 1 && strstr(R.ev[0], "\"gesture\":\"S S L\"") && R.nrun == 1 && R.kind[0] == BA_TRACK && !strcmp(R.arg[0], "toggle"), "S S L: track toggle");
    memset(&R, 0, sizeof R);
    rt_press(200, 200);
    rt_press(200, 200);
    rt_press(200, 1500);
    CHECK(R.nev == 1 && strstr(R.ev[0], "\"gesture\":\"triple\"") && strstr(R.ev[0], "\"actions\":[]") && strstr(R.ev[0], "\"result\":\"unbound\"") && R.nmq == 1 && R.nrun == 0,
          "an unbound gesture (triple) is still an event and an MQTT message, result unbound, no action");
    CHECK(a2k_store_writes() == w0 && a2k_btn_writes() == 2, "pressing the button wrote nothing (zero flash writes at run time)");
    /* delete rules */
    CHECK(http("DELETE", "modes/night", NULL, out, sizeof out, NULL) == 409 && strstr(out, "in_use"), "DELETE of a mode used by a binding: 409");
    st = http("POST", "button", DEFAULT_JSON, out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"changed\":true"), "back to the defaults (no binding uses the mode)");
    CHECK(http("DELETE", "modes/night", NULL, out, sizeof out, NULL) == 200 && http("DELETE", "modes/night", NULL, out, sizeof out, NULL) == 404, "DELETE /modes/night: 200, then 404");
    CHECK(http("DELETE", "button", NULL, out, sizeof out, NULL) == 405 && http("PUT", "button", NULL, out, sizeof out, NULL) == 405 && http("DELETE", "modes", NULL, out, sizeof out, NULL) == 405,
          "DELETE /button, PUT, DELETE /modes: 405");
    CHECK(a2k_btn_path("button") && a2k_btn_path("modes") && a2k_btn_path("modes/x") && !a2k_btn_path("modes/") && !a2k_btn_path("buttons") && !a2k_btn_path("button/x"), "route table");
    /* POST /modes with the name in the body */
    st = http("POST", "modes", "{\"name\":\"day\",\"settings\":{\"led.mode\":\"on\"}}", out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"name\":\"day\""), "POST /modes with the name in the body");
    /* reload */
    st = http("POST", "button", BODY, out, sizeof out, NULL);
    CHECK(st == 422, "the mode night is gone: the body is refused");
    (void)http("POST", "modes/night", "{\"settings\":{\"led.mode\":\"off\"}}", out, sizeof out, NULL);
    st = http("POST", "button", BODY, out, sizeof out, NULL);
    CHECK(st == 200, "recreated, accepted");
    memset(&R, 0, sizeof R);
    CHECK(a2k_btn_init(&RT_OPS, 2000) == 0, "restart");
    n = a2k_btn_gestures(names);
    st = http("GET", "button", NULL, out, sizeof out, NULL);
    CHECK(n == 4 && st == 200 && !strcmp(out, text), "after a restart the persisted bindings are back");
    st = http("GET", "modes", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && strstr(out, "\"day\"") && strstr(out, "\"night\""), "... and the modes");
    /* bad files */
    f = fopen(pb, "w");
    fputs("{\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"reboot\"]}]}", f);
    fclose(f);
    CHECK(a2k_btn_init(&RT_OPS, 2000) == 0, "restart with a bad button file");
    st = http("GET", "button", NULL, out, sizeof out, NULL);
    CHECK(st == 200 && !strcmp(out, DEFAULT_JSON), "an invalid file is ignored as a whole: the defaults (and it is not rewritten)");
    f = fopen(pb, "r");
    n = f ? (int)fread(text, 1, sizeof text - 1, f) : 0;
    if (f)
        fclose(f);
    text[n > 0 ? n : 0] = '\0';
    CHECK(strstr(text, "reboot") != NULL, "the bad file stays as it was");
    /* write rate limit */
    (void)remove(pb);
    (void)remove(pm);
    CHECK(a2k_btn_init(&RT_OPS, 2000) == 0, "restart, clean");
    for (i = 0; i < 12; i++) {
        char body[200];
        snprintf(body, sizeof body, "{\"led_ack\":%s,\"timing\":{\"gap_ms\":%d},\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]}]}", "true", 200 + i);
        st = http("POST", "button", body, out, sizeof out, &retry);
        if (st != 200)
            break;
    }
    CHECK(i == A2K_BTN_WRITE_BURST && st == 429 && retry >= 1 && retry <= A2K_BTN_WRITE_GAP_S && strstr(out, "rate"), "config writes: a burst of 8, then 429 with a retry time (flash wear)");
    (void)remove(pb);
    (void)remove(pm);
}

static void put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(s, f);
    fclose(f);
}

static void test_driver(const char *dir)
{
    char p[300], b[16];
    const hal_input_t *in = hal_input_find(&a2k_input_desc, "button");
    FILE *f;
    printf("== sysfs input driver (fake gpio directory)\n");
    snprintf(p, sizeof p, "%s/export", dir);
    put(p, "");
    snprintf(p, sizeof p, "%s/gpio71", dir);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/gpio71/direction", dir);
    put(p, "");
    snprintf(p, sizeof p, "%s/gpio71/value", dir);
    put(p, "1\n");
    a2k_pin_test_root(dir);
    CHECK(a2k_pin_allowed(71) == 0 && a2k_pin_input_allowed(71) == 1 && a2k_pin_input_allowed(56) == 0 && a2k_pin_input_allowed(24) == 0,
          "71 is an input pin only; the output whitelist is unchanged (24 25 48 56)");
    CHECK(a2k_pin_open(71, 0) == -1 && a2k_pin_set(71, 0) == -1, "the input pin cannot be opened or written as an output");
    CHECK(a2k_pin_open_in(56) == -1 && a2k_pin_open_in(24) == -1 && a2k_pin_open_in(22) == -1, "an output or unknown pin cannot be opened as an input");
    CHECK(hal_input_sysfs_ops.read(in) == -1, "read before setup: -1");
    CHECK(hal_input_sysfs_ops.setup(in) == 0, "setup: export (already there), direction in, value read-only");
    snprintf(p, sizeof p, "%s/gpio71/direction", dir);
    f = fopen(p, "r");
    CHECK(f && fgets(b, sizeof b, f) && !strcmp(b, "in"), "direction is 'in' (never 'out')");
    if (f) fclose(f);
    CHECK(hal_input_sysfs_ops.read(in) == 1, "released reads 1");
    snprintf(p, sizeof p, "%s/gpio71/value", dir);
    put(p, "0\n");
    CHECK(hal_input_sysfs_ops.read(in) == 0, "pressed reads 0");
    put(p, "x\n");
    CHECK(hal_input_sysfs_ops.read(in) == -1, "garbage reads -1");
    f = fopen(p, "r");
    CHECK(f && fgets(b, sizeof b, f) && !strcmp(b, "x\n"), "the value file was never written by the driver");
    if (f) fclose(f);
    a2k_pin_close_all();
    CHECK(hal_input_sysfs_ops.read(in) == -1, "closed: -1");
    a2k_pin_test_root(NULL);
}

static int lvl = -1;
static int fake_set(const hal_light_emitter_t *e, int on) { (void)e; lvl = on; return 0; }
static const hal_light_ops_t fake_drv = { fake_set };
static void no_inputs(void *u, a2k_leds_in_t *in) { (void)u; memset(in, 0, sizeof *in); }
static const a2k_leds_ops_t fake_ops = { no_inputs, 0 };

static void test_blip(void)
{
    a2k_leds_t l;
    printf("== LED transient override\n");
    CHECK(a2k_leds_init(&l, &a2k_light_desc, &fake_drv, &fake_ops, 0) == 0, "engine init");
    (void)a2k_leds_set_mode(&l, A2K_LEDS_ON);
    a2k_leds_tick(&l, 30000);
    CHECK(lvl == -1, "mode on, nothing written (the stock state)");
    CHECK(a2k_leds_blip(&l, "2:150:150:0", 700, 30000) == 0, "blip accepted");
    a2k_leds_tick(&l, 30000);
    CHECK(lvl == 1 && a2k_leds_last_rule(&l) == -3, "blip starts lit (double blink)");
    a2k_leds_tick(&l, 30160);
    CHECK(lvl == 0, "dark after 150 ms");
    a2k_leds_tick(&l, 30310);
    CHECK(lvl == 1, "second blink");
    a2k_leds_tick(&l, 30460);
    CHECK(lvl == 0, "dark");
    a2k_leds_tick(&l, 30800);
    CHECK(lvl == 1 && a2k_leds_last_rule(&l) != -3, "after 700 ms the rules are back (on)");
    CHECK(a2k_leds_blip(&l, "1:100:100:0", 15000, 40000) == 0, "hold blip");
    a2k_leds_tick(&l, 40000);
    a2k_leds_tick(&l, 40110);
    CHECK(lvl == 0, "fast blink");
    a2k_leds_blip_clear(&l);
    a2k_leds_tick(&l, 40120);
    CHECK(lvl == 1 && a2k_leds_last_rule(&l) != -3, "cleared at once");
    CHECK(a2k_leds_blip(&l, "bad", 700, 0) == -1 && a2k_leds_blip(&l, "1:100:100:0", 0, 0) == -1 && a2k_leds_blip(&l, "1:100:100:0", 60001, 0) == -1,
          "bad pattern or duration refused");
    (void)a2k_leds_set_mode(&l, A2K_LEDS_OFF);
    a2k_leds_blip(&l, "1:100:100:0", 700, 50000);
    a2k_leds_tick(&l, 50000);
    CHECK(lvl == 0 && a2k_leds_last_rule(&l) == -1, "mode off: a dark LED stays dark (no blip)");
    (void)a2k_leds_set_mode(&l, A2K_LEDS_AUTO);
    a2k_leds_blip(&l, "1:100:100:0", 700, 60000);
    a2k_leds_tick(&l, 60000);
    CHECK(lvl == 1 && a2k_leds_last_rule(&l) == -3, "mode auto: the blip shows");
    a2k_leds_destroy(&l);
}

int main(void)
{
    char dir[] = "/tmp/rot2k_button_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_deb();
    test_policy();
    test_locked();
    test_start_held();
    test_gesture_names();
    test_gestures();
    test_fuzz();
    test_cfg();
    test_modes();
    test_dispatch();
    test_rt(dir);
    test_driver(dir);
    test_blip();
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k button unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
