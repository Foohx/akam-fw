/* Host unit test of the front button of the Rotatable 2K app: the portable hal_input debouncer, the press policy of button_logic.c (short,
 * long, dead zone, hold cue, calibration lock, held at start) on a fake line and a fake clock, the sysfs input driver on a fake directory
 * (the input pin of akam2k_pin.c: read-only, outputs untouched) and the transient LED override of the LED engine.
 * Prints "rot2k button unit: N failed". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akam2k_hal.h"
#include "akam2k_leds.h"
#include "akam2k_pin.h"
#include "button_logic.h"
#include "hal_input.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static const hal_input_t BTN = { "button", 71, 1, 50 };

/* the fake world */
static int shorts, longs, locked_now;
static int cues[16], ncue;
static void a_short(void *u) { (void)u; shorts++; }
static void a_long(void *u) { (void)u; longs++; }
static int a_locked(void *u) { (void)u; return locked_now; }
static void a_led(void *u, int c) { (void)u; if (ncue < 16) cues[ncue++] = c; }
static const btn_actions_t ACT = { 0, a_locked, a_short, a_long, a_led };

static void reset(void) { shorts = longs = locked_now = ncue = 0; }

/* Holds the line at `raw` from t0 to t1 (inclusive) sampling every 20 ms; returns the last event seen that is not NONE. */
static int run(btn_t *b, int64_t t0, int64_t t1, int raw)
{
    int ev = BTN_EV_NONE;
    for (int64_t t = t0; t <= t1; t += 20) {
        int e = btn_step(b, &BTN, t, raw, &ACT);
        if (e != BTN_EV_NONE)
            ev = e;
    }
    return ev;
}

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
    printf("== press policy\n");
    btn_init(&b, 2000);
    reset();
    run(&b, 0, 1000, 1); /* released for a while */
    ev = run(&b, 1020, 1400, 0);
    CHECK(ev == BTN_EV_NONE && shorts == 0, "nothing happens while the button is down (short press)");
    ev = run(&b, 1420, 1600, 1);
    CHECK(ev == BTN_EV_SHORT && shorts == 1 && longs == 0 && ncue == 1 && cues[0] == BTN_LED_ACK, "a 0.4 s press: one short press, the ack cue");
    /* a contact bounce of 30 ms is no press */
    reset();
    run(&b, 2000, 2000, 0);
    ev = run(&b, 2020, 2200, 1);
    CHECK(ev == BTN_EV_NONE && shorts == 0 && ncue == 0, "a 30 ms glitch is debounced away");
    /* long press */
    reset();
    run(&b, 3000, 3000, 1);
    run(&b, 3020, 5200, 0);
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG, "held: fast blink at 0.5 s, then the long confirm cue at 2 s, once each");
    ev = run(&b, 5220, 5400, 1);
    CHECK(ev == BTN_EV_LONG && longs == 1 && shorts == 0 && ncue == 3 && cues[2] == BTN_LED_HOLD_END, "released: the long press (park toggle), the cues end");
    /* held 0.6 s: the keep-holding blink started, release gives the short press ack after the cue end */
    reset();
    run(&b, 5500, 5500, 1);
    run(&b, 5520, 6150, 0);
    ev = run(&b, 6170, 6300, 1);
    CHECK(ev == BTN_EV_SHORT && ncue == 3 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_HOLD_END && cues[2] == BTN_LED_ACK, "a 0.65 s press: hold blink, then end and the short ack");
    /* dead zone 1 s .. 2 s */
    reset();
    run(&b, 6000, 6000, 1);
    run(&b, 6020, 7600, 0);
    ev = run(&b, 7620, 7800, 1);
    CHECK(ev == BTN_EV_NONE && shorts == 0 && longs == 0, "a 1.6 s press is neither: nothing happens");
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_HOLD_END, "... only the keep-holding blink came and went");
    /* exactly at the short limit */
    reset();
    run(&b, 8000, 8000, 1);
    run(&b, 8020, 8980, 0);
    ev = run(&b, 9000, 9100, 1);
    CHECK(ev == BTN_EV_SHORT, "just under 1 s is still short");
    /* configurable threshold */
    btn_init(&b, 3000);
    reset();
    run(&b, 0, 0, 1);
    run(&b, 20, 2600, 0);
    ev = run(&b, 2620, 2800, 1);
    CHECK(ev == BTN_EV_NONE && longs == 0 && ncue == 2 && cues[0] == BTN_LED_HOLD, "button_long_ms 3000: 2.6 s is not long (dead zone, no confirm cue)");
    reset();
    run(&b, 3000, 6200, 0);
    CHECK(ncue == 2 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG, "button_long_ms 3000: hold at 0.5 s, the confirm cue at 3 s");
    ev = run(&b, 6220, 6400, 1);
    CHECK(ev == BTN_EV_LONG && longs == 1, "button_long_ms 3000: long press");
    CHECK(btn_long_ms_ok(1500) && btn_long_ms_ok(10000) && !btn_long_ms_ok(1499) && !btn_long_ms_ok(10001) && !btn_long_ms_ok(1000),
          "the long press limit: 1500..10000 (always above the 1 s short limit)");
    btn_init(&b, 50);
    CHECK(b.long_ms == BTN_LONG_DEFAULT_MS, "a bad limit falls back to 2000");
}

static void test_locked(void)
{
    btn_t b;
    int ev;
    printf("== calibration lock\n");
    btn_init(&b, 2000);
    reset();
    run(&b, 0, 500, 1);
    locked_now = 1;
    run(&b, 520, 700, 0);
    ev = run(&b, 720, 900, 1);
    CHECK(ev == BTN_EV_IGNORED && shorts == 0 && longs == 0, "short press during the calibration: ignored, no event");
    CHECK(ncue == 1 && cues[0] == BTN_LED_ACK, "... but the LED still answers");
    reset();
    locked_now = 1;
    run(&b, 1000, 1000, 1);
    run(&b, 1020, 3300, 0);
    ev = run(&b, 3320, 3500, 1);
    CHECK(ev == BTN_EV_IGNORED && longs == 0 && ncue == 3 && cues[0] == BTN_LED_HOLD && cues[1] == BTN_LED_LONG && cues[2] == BTN_LED_HOLD_END,
          "long press during the calibration: no motion, the hold blink only");
    locked_now = 0;
    reset();
    run(&b, 4000, 4000, 1);
    run(&b, 4020, 4200, 0);
    ev = run(&b, 4220, 4400, 1);
    CHECK(ev == BTN_EV_SHORT && shorts == 1, "after the calibration the button works again");
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
    btn_init(&b, 2000);
    reset();
    run(&b, 0, 3000, 0); /* held since before the first sample, for 3 s */
    CHECK(ncue == 0, "a button already down at start gives no cue");
    ev = run(&b, 3020, 3200, 1);
    CHECK(ev == BTN_EV_NONE && shorts == 0 && longs == 0, "... and no action when released");
    run(&b, 3220, 3400, 0);
    ev = run(&b, 3420, 3600, 1);
    CHECK(ev == BTN_EV_SHORT, "the next press works");
    /* a read error mid-press does not release it */
    reset();
    btn_init(&b, 2000);
    run(&b, 0, 500, 1);
    run(&b, 520, 700, 0);
    run(&b, 720, 800, -1);
    ev = run(&b, 820, 1000, 1);
    CHECK(ev == BTN_EV_SHORT && shorts == 1, "failed reads in the middle are ignored");
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
    test_driver(dir);
    test_blip();
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k button unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
