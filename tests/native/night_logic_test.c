/* Host test of src/ak_rtsp/night_logic.c: the day / night decision fed with simulated AE traces (lit room, dusk,
 * dark room, the IR lit scene of the night mode, a lamp switched on at night, noise around the threshold).
 * No camera, no clock: one nl_step() is 100 ms of simulated time. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "night_logic.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

/* ---- the simulated world ----
 * Scene brightness B = ambient + IR (when the LEDs are on). The AE needs a total "exposure x gain" P = K / B; the
 * exposure saturates at 16384 and the ISP gain makes up the rest up to its maximum (1.5x in day, 3.0x in night,
 * like the exposure block of the file). P moves toward the need by at most 0.1 octave per poll (a 4x change takes
 * 2 s, like the AE of the camera). */
#define EXP_MAX   16384.0
#define K_SCENE   100000.0   /* ambient 100 (a lit room) needs P=1000; ambient 4 needs 25000 (above 16384 x 1.5) */
#define IR_LIGHT  200.0      /* what the IR LEDs add to the scene (closed room) */
#define P_MIN     2.0

typedef struct {
    nl_state_t     nl;
    night_tuning_t t;
    double         plog;          /* log2 of P */
    int            night;         /* IR-cut position */
    int            probing;
    int            ir_policy;     /* night.ir_led=auto */
    double         ambient;
    unsigned       now;           /* polls */
    int            go_night, go_day, probe_start, probe_end;
    unsigned       first_day_at;  /* poll of the first GO_DAY, 0 = none */
    unsigned       last_switch_at;
    uint32_t       min_exp_night_ir;  /* lowest exposure seen in night mode with the IR on */
    char           why[160];
    char           last_why[160]; /* the text of the last action */
} sim_t;

static night_tuning_t defaults(void)
{
    night_tuning_t t = {
        .trigger_hw_exp = NIGHT_DEF_TRIGGER_HW_EXP, .gain_pct = NIGHT_DEF_GAIN_PCT,
        .day_hw_exp = NIGHT_DEF_DAY_HW_EXP, .confirm_samples = NIGHT_DEF_CONFIRM_SAMPLES,
        .day_confirm_samples = NIGHT_DEF_DAY_CONFIRM, .lock_ms = NIGHT_DEF_LOCK_MS,
        .min_night_ms = NIGHT_DEF_MIN_NIGHT_MS, .probe_ms = NIGHT_DEF_PROBE_MS,
        .probe_settle_ms = NIGHT_DEF_PROBE_SETTLE_MS, .override = 0,
    };
    return t;
}

static void sim_init(sim_t *s, int start_night, double ambient, int ir_policy)
{
    memset(s, 0, sizeof(*s));
    s->t = defaults();
    s->night = start_night;
    s->ir_policy = ir_policy;
    s->ambient = ambient;
    s->plog = log2(K_SCENE / (ambient + ((start_night && ir_policy) ? IR_LIGHT : 0)));
    s->min_exp_night_ir = UINT32_MAX;
    nl_reset(&s->nl, &s->t, start_night);
}

static double scene(const sim_t *s)
{
    int ir = s->night && s->ir_policy && !s->probing;
    return s->ambient + (ir ? IR_LIGHT : 0);
}

static double isp_max(const sim_t *s) { return s->night ? 768.0 : 384.0; }

/* One poll of simulated time. */
static nl_action_t sim_poll(sim_t *s)
{
    double need = K_SCENE / scene(s);
    if (need < P_MIN) need = P_MIN;
    double want = log2(need), d = want - s->plog;
    if (d > 0.1) d = 0.1;
    if (d < -0.1) d = -0.1;
    s->plog += d;
    double p = exp2(s->plog), expo = p > EXP_MAX ? EXP_MAX : p, gain = 256.0;
    if (p > EXP_MAX) {
        gain = 256.0 * p / EXP_MAX;
        if (gain > isp_max(s)) { gain = isp_max(s); expo = EXP_MAX; }   /* the AE cannot reach more */
    }
    nl_sample_t smp = { .hw_exp = (uint32_t)expo, .sgain = 256, .isp = (uint32_t)gain, .lum = 0 };
    nl_limits_t lim = { .sgain_max = 256, .isp_max = (uint32_t)isp_max(s) };
    if (s->night && s->ir_policy && !s->probing && smp.hw_exp < s->min_exp_night_ir) s->min_exp_night_ir = smp.hw_exp;

    s->now++;
    nl_action_t a = nl_step(&s->nl, &s->t, &smp, &lim, s->ir_policy, s->why, sizeof(s->why));
    if (a != NL_NONE) memcpy(s->last_why, s->why, sizeof(s->last_why));
    switch (a) {
    case NL_GO_NIGHT:    s->night = 1; s->probing = 0; s->go_night++; s->last_switch_at = s->now; break;
    case NL_GO_DAY:
        s->night = 0; s->probing = 0; s->go_day++; s->last_switch_at = s->now;
        if (!s->first_day_at) s->first_day_at = s->now;
        break;
    case NL_PROBE_START: s->probing = 1; s->probe_start++; break;
    case NL_PROBE_END:   s->probing = 0; s->probe_end++; break;
    default: break;
    }
    return a;
}

static void run_s(sim_t *s, unsigned seconds) { for (unsigned i = 0; i < seconds * 10; i++) sim_poll(s); }
static unsigned secs(unsigned polls) { return polls / 10; }

/* ---- scenarios ---- */

static void test_lit_room(void)
{
    sim_t s; sim_init(&s, 0, 100.0, 1);          /* P=1000: exposure 1000, far from the ceiling */
    run_s(&s, 3600);
    CHECK(s.go_night == 0 && s.go_day == 0 && !s.night);
    /* the old rule (hw_exp > 3000) would also stay day here; a dim but usable room is the real difference */
    sim_init(&s, 0, 8.0, 1);                     /* P=12500: exposure 12500, below the 95 % ceiling */
    run_s(&s, 3600);
    CHECK(s.go_night == 0 && !s.night);
    sim_init(&s, 0, 5.0, 1);                     /* P=20000: exposure at the ceiling, gain 1.22x < 95 % of 1.5x */
    run_s(&s, 3600);
    CHECK(s.go_night == 0 && !s.night);
}

static void test_dark_room(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);            /* needs P=200000: exposure and gain both at their ceiling */
    run_s(&s, 20);
    CHECK(s.go_night == 1 && s.night);
    CHECK(secs(s.last_switch_at) >= 3 && secs(s.last_switch_at) <= 6);  /* ~3 s confirmation (+ AE ramp) */
    CHECK(strstr(s.last_why, "AE saturated") != NULL);
    run_s(&s, 4 * 3600);
    /* IR lit scene: the exposure is far below the day threshold, yet no return to day, no oscillation */
    CHECK(s.min_exp_night_ir < s.t.day_hw_exp);
    CHECK(s.go_night == 1 && s.go_day == 0 && s.night);
    CHECK(s.probe_start > 0 && s.probe_start == s.probe_end);   /* probes happened and all failed */
    CHECK(!s.probing);
    /* probe back-off: 4 hours need far fewer than 4 h / 5 min = 48 probes */
    CHECK(s.probe_start < 20);
}

static void test_dusk(void)
{
    sim_t s; sim_init(&s, 0, 100.0, 1);
    /* ambient falls from 100 to 0.5 over 40 min (exponentially) */
    for (unsigned i = 0; i < 40 * 600; i++) {
        s.ambient = 100.0 * pow(0.005, (double)i / (40.0 * 600.0));
        sim_poll(&s);
    }
    CHECK(s.go_night == 1 && s.night);
    /* it switched only once the AE was saturated: ambient <= ~4 (P > 16384 x 1.5 x 95 %), not at 18 % of the range */
    CHECK(s.ambient < 100.0 * pow(0.005, 0.5));  /* the switch came in the second half of the ramp */
    run_s(&s, 1800);
    CHECK(s.go_night == 1 && s.go_day == 0);
}

static void test_lamp_at_night(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);
    run_s(&s, 600);                               /* night for 10 minutes, past the minimum dwell */
    CHECK(s.night && s.go_night == 1);
    unsigned t0 = s.now;
    s.ambient = 300.0;                            /* a lamp: the IR lit scene gets brighter, exposure halves or less */
    run_s(&s, 60);
    CHECK(s.go_day == 1 && !s.night);
    CHECK(secs(s.first_day_at - t0) <= 20);       /* lamp detection 3 s + probe about 6 s */
    CHECK(strstr(s.last_why, "lit room") != NULL);
    run_s(&s, 1800);
    CHECK(s.go_day == 1 && s.go_night == 1 && !s.night);   /* stays day */
}

static void test_lamp_before_min_dwell(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);
    run_s(&s, 30);                                /* switched to night at ~5 s, the 120 s dwell is not over */
    CHECK(s.night);
    s.ambient = 300.0;
    run_s(&s, 60);
    CHECK(s.night && s.go_day == 0);              /* too early (the LEDs would flash) */
    run_s(&s, 120);
    CHECK(s.go_day == 1 && !s.night);
}

static void test_lamp_then_dark_again(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);
    run_s(&s, 600);
    s.ambient = 300.0;
    run_s(&s, 25);
    CHECK(!s.night);                              /* day about 15 s after the lamp */
    s.ambient = 0.5;                              /* lamp off */
    run_s(&s, 30);
    CHECK(!s.night);                              /* the 60 s lock after the switch holds */
    run_s(&s, 60);
    CHECK(s.night && s.go_night == 2);
    /* lamp again at once: the dwell doubles since this is a flap (back to night within 10 minutes) */
    s.ambient = 300.0;
    run_s(&s, 200);
    CHECK(s.night);                               /* 2 x 120 s dwell not over */
    run_s(&s, 120);
    CHECK(!s.night && s.go_day == 2);
}

static void test_probe_dark_does_not_flash_long(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);
    run_s(&s, 700);                               /* first periodic probe at 5 min */
    CHECK(s.probe_start >= 1);
    CHECK(s.probe_end == s.probe_start && !s.probing);   /* IR is back on */
    /* no probe stays open longer than ~2.5 s: the AE climbs back, the probe ends */
    sim_init(&s, 0, 0.5, 1);
    unsigned open = 0, longest = 0;
    for (unsigned i = 0; i < 3600u * 10; i++) {
        sim_poll(&s);
        if (s.probing) { open++; if (open > longest) longest = open; } else open = 0;
    }
    CHECK(longest > 0 && longest <= 30);
}

static void test_short_dark_in_lit_room(void)
{
    sim_t s; sim_init(&s, 0, 100.0, 1);
    run_s(&s, 70);
    for (int k = 0; k < 20; k++) {                /* the light is off for 1.5 s, many times (flicker, a hand over the lens) */
        s.ambient = 0.2; run_s(&s, 2);
        s.ambient = 100.0; run_s(&s, 20);
    }
    CHECK(s.go_night == 0 && !s.night);
}

static void test_ir_led_off_policy(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 0);             /* night.ir_led=off: no feedback loop */
    run_s(&s, 20);
    CHECK(s.night);
    run_s(&s, 3600);
    CHECK(s.go_day == 0 && s.probe_start == 0);   /* exposure stays at the ceiling in the dark */
    s.ambient = 100.0;
    run_s(&s, 200);                               /* past the dwell, 5 s below the day threshold */
    CHECK(s.go_day == 1 && !s.night);
    CHECK(s.probe_start == 0);
}

static void test_noise_at_threshold(void)
{
    /* ambient wandering around the saturation point (~4): at most one switch per lock period, never a storm */
    sim_t s; sim_init(&s, 0, 4.2, 1);
    unsigned seed = 12345;
    for (unsigned i = 0; i < 4u * 3600u * 10u; i++) {
        seed = seed * 1103515245u + 12345u;
        if (i % 50 == 0) s.ambient = 3.4 + 1.6 * ((seed >> 16) & 0xff) / 255.0;
        sim_poll(&s);
    }
    CHECK(s.go_night + s.go_day <= 4 * 3600 / 120);       /* at most one switch every two minutes on average */
    CHECK(s.go_night + s.go_day < 40);
}

static void test_saturation_test(void)
{
    night_tuning_t t = defaults();
    nl_limits_t lim = { .sgain_max = 256, .isp_max = 384 };
    nl_sample_t a = { .hw_exp = 16384, .sgain = 256, .isp = 384 };   /* 6 291 456 in Q8: the stock number */
    CHECK(nl_saturated(&t, &a, &lim));
    nl_sample_t b = { .hw_exp = 16384, .sgain = 256, .isp = 300 };   /* gain not at the ceiling */
    CHECK(!nl_saturated(&t, &b, &lim));
    nl_sample_t c = { .hw_exp = 12000, .sgain = 256, .isp = 384 };   /* exposure not at the ceiling */
    CHECK(!nl_saturated(&t, &c, &lim));
    nl_limits_t none = { 0, 0 };                                      /* limits unknown: exposure alone */
    CHECK(nl_saturated(&t, &c, &none) == 0 && nl_saturated(&t, &b, &none) == 1);
    nl_limits_t night = { .sgain_max = 256, .isp_max = 768 };         /* the night ceiling is 3x */
    CHECK(!nl_saturated(&t, &a, &night));
    /* a live-tuned threshold */
    t.trigger_hw_exp = 3000; t.gain_pct = 1;
    CHECK(nl_saturated(&t, &c, &lim));
}

static void test_no_sample_and_reset(void)
{
    sim_t s; sim_init(&s, 0, 0.5, 1);
    for (int i = 0; i < 100; i++) CHECK(nl_step(&s.nl, &s.t, NULL, NULL, 1, NULL, 0) == NL_NONE);
    CHECK(s.nl.confirm == 0 && s.nl.mode == NL_DAY);
    /* reset in night mode starts a dwell and a probe schedule */
    nl_reset(&s.nl, &s.t, 1);
    CHECK(s.nl.mode == NL_NIGHT && s.nl.lock_until == 0 && s.nl.next_probe > 0);
    char line[256];
    nl_status(&s.nl, &s.t, line, sizeof(line));
    CHECK(strstr(line, "mode=night") != NULL);
}

int main(void)
{
    test_saturation_test();
    test_lit_room();
    test_dark_room();
    test_dusk();
    test_lamp_at_night();
    test_lamp_before_min_dwell();
    test_lamp_then_dark_again();
    test_probe_dark_does_not_flash_long();
    test_short_dark_in_lit_room();
    test_ir_led_off_policy();
    test_noise_at_threshold();
    test_no_sample_and_reset();
    if (g_failed) { printf("night_logic_test: %d failure(s)\n", g_failed); return 1; }
    printf("night_logic_test: OK\n");
    return 0;
}
