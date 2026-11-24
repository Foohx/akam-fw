/* Host unit test of the day / night and image modules of the Rotatable 2K app (no camera, no vendor lib):
 *   - pin.c on a temporary directory laid out like /sys/class/gpio: only the four pins 24, 25, 48, 56 are reachable
 *     (negative controls: every other pin of the stock map and a few more are refused and leave no trace), the emergency
 *     release drives the coil pins low;
 *   - ircut.c: the exact switch sequence and its timing on a fake clock (the coil is never left energised, the two
 *     coil pins are never high together, a failing write ends with both low);
 *   - night_ctl.c on top of the UNCHANGED shared policy src/features/night/night_logic.c: simulated vendor decision traces
 *     (steady dark, steady bright, flapping, "not stable" errors), the 60 s rule, forced modes, the safe end state,
 *     failures of the filter, the IR LED setting;
 *   - image.c: value mapping and the calls made.
 * Build with -fsanitize=address,undefined. Prints "rot2k night unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "image.h"
#include "ircut.h"
#include "isp.h"
#include "night_ctl.h"
#include "pin.h"
#include "night_logic.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ---- the app's log, silent here ---- */
void a2k_log(const char *fmt, ...)
{
    va_list ap;
    if (!getenv("ROT2K_NIGHT_VERBOSE"))
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ---- fake ISP (isp.h) used by the image test and by the engine test through the ops ---- */
static struct { int type, value; } fx_log[16];
static int fx_n, fx_fail_type = -1;
int a2k_isp_effect(int type, int value)
{
    if (fx_n < 16) {
        fx_log[fx_n].type = type;
        fx_log[fx_n].value = value;
        fx_n++;
    }
    return type == fx_fail_type ? -1 : 0;
}
int a2k_isp_dn_init(void) { return 0; }
int a2k_isp_dn_level(int c, int *w) { (void)c; (void)w; return -1; }
int a2k_isp_mode(int n) { (void)n; return 0; }
int a2k_isp_wdr(int on) { return a2k_isp_effect(A2K_FX_WDR, on ? 40 : 0); }

/* ---- 1. pins on a temporary sysfs ---- */
static char root[256];

static void wfile(const char *rel, const char *content)
{
    char p[400];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void rfile(const char *rel, char *out, size_t cap)
{
    char p[400];
    FILE *f;
    out[0] = '\0';
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "r");
    if (f) {
        size_t n = fread(out, 1, cap - 1, f);
        out[n] = '\0';
        fclose(f);
    }
}

static void mkpin(int pin)
{
    char d[400];
    snprintf(d, sizeof d, "%s/gpio%d", root, pin);
    mkdir(d, 0700);
    snprintf(d, sizeof d, "gpio%d/direction", pin);
    wfile(d, "in\n");
    snprintf(d, sizeof d, "gpio%d/value", pin);
    wfile(d, "x\n");
}

static void test_pins(void)
{
    static const int refused[] = { 0, 9, 10, 21, 22, 23, 26, 47, 49, 51, 50, 55, 71, 100, -1, 24 + 256 };
    char v[16], d[400];
    size_t i;
    CHECK(a2k_pin_allowed(24) && a2k_pin_allowed(25) && a2k_pin_allowed(48) && a2k_pin_allowed(56) && !a2k_pin_allowed(49) && !a2k_pin_allowed(22), "the four day/night and light pins are allowed, 49 and 22 are refused");
    for (i = 0; i < sizeof refused / sizeof refused[0]; i++)
        if (a2k_pin_allowed(refused[i]))
            break;
    CHECK(i == sizeof refused / sizeof refused[0], "every other pin is refused (negative control: Wi-Fi 10, white 49, alarm 22, LED 55, keys 50 71, speaker 9)");

    snprintf(root, sizeof root, "/tmp/rot2k_night_unit.XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0, "temporary directory");
        return;
    }
    a2k_pin_test_root(root);
    wfile("export", "");
    mkpin(24);
    mkpin(25);
    mkpin(48);
    mkpin(10); /* exists on the stub camera: must stay untouched */
    CHECK(a2k_pin_open(10, 1) == -1 && a2k_pin_open(55, 0) == -1 && a2k_pin_open(51, 0) == -1 && a2k_pin_open(49, 0) == -1 && a2k_pin_open(22, 0) == -1, "open of a pin outside the whitelist fails");
    rfile("gpio10/direction", v, sizeof v);
    CHECK(!strcmp(v, "in\n"), "pin 10 is untouched by a refused open");
    rfile("gpio10/value", v, sizeof v);
    CHECK(!strcmp(v, "x\n"), "pin 10 value is untouched");
    snprintf(d, sizeof d, "%s/gpio55", root);
    CHECK(access(d, F_OK) != 0, "no directory is created for a refused pin");
    rfile("export", v, sizeof v);
    CHECK(v[0] == '\0', "nothing is exported for a refused pin");
    CHECK(a2k_pin_set(24, 1) == -1, "set on a pin that is not open fails");

    CHECK(a2k_pin_open(24, 0) == 0 && a2k_pin_open(25, 0) == 0 && a2k_pin_open(48, 0) == 0, "the three pins open");
    rfile("gpio24/direction", v, sizeof v);
    CHECK(!strncmp(v, "out", 3), "pin 24 is an output");
    rfile("gpio48/value", v, sizeof v);
    CHECK(v[0] == '0', "pin 48 starts low");
    CHECK(a2k_pin_set(24, 1) == 0 && a2k_pin_set(48, 1) == 0, "pins are driven");
    rfile("gpio24/value", v, sizeof v);
    CHECK(v[0] == '1', "pin 24 high");
    CHECK(a2k_pin_set(10, 1) == -1 && a2k_pin_set(55, 1) == -1, "set of another pin fails");
    a2k_pin_set(25, 1);
    a2k_pin_emergency_low();
    rfile("gpio24/value", v, sizeof v);
    CHECK(v[0] == '0', "emergency release: pin 24 low");
    rfile("gpio25/value", v, sizeof v);
    CHECK(v[0] == '0', "emergency release: pin 25 low");
    rfile("gpio48/value", v, sizeof v);
    CHECK(v[0] == '1', "emergency release leaves the IR LED alone");
    a2k_pin_close_all();
    CHECK(a2k_pin_set(24, 1) == -1, "after close the pin is no longer writable");

    /* not exported yet: the export file gets the number, the open then fails (no sysfs behind it here) */
    snprintf(d, sizeof d, "%s/gpio48", root);
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", d);
        if (system(cmd) != 0) { }
    }
    CHECK(a2k_pin_open(48, 0) == -1, "a pin whose directory never appears fails");
    rfile("export", v, sizeof v);
    CHECK(!strncmp(v, "48", 2), "the pin number was written to export");
    a2k_pin_close_all();
    a2k_pin_test_root(NULL);
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
        if (system(cmd) != 0) { }
    }
}

/* ---- 2. the IR-cut sequence on a fake clock ---- */
#define MAXEV 64
static struct { int64_t t; int pin, level; } ev[MAXEV];
static int ev_n, pin_state[64], fake_ms, fail_set_pin = -1, fail_set_level = -1, fail_set_times, both_high, max_on_ms, on_since[64];
static int opened[64];

static void ev_reset(void)
{
    ev_n = 0;
    memset(pin_state, 0, sizeof pin_state);
    memset(on_since, 0, sizeof on_since);
    memset(opened, 0, sizeof opened);
    both_high = 0;
    max_on_ms = 0;
    fail_set_pin = fail_set_level = -1;
    fail_set_times = 0;
}

static int f_open(int pin, int level)
{
    opened[pin] = 1;
    pin_state[pin] = level;
    return 0;
}

static int f_set(int pin, int level)
{
    if (pin == fail_set_pin && level == fail_set_level && fail_set_times > 0) {
        fail_set_times--;
        return -1;
    }
    if (ev_n < MAXEV) {
        ev[ev_n].t = fake_ms;
        ev[ev_n].pin = pin;
        ev[ev_n].level = level;
        ev_n++;
    }
    if (level && !pin_state[pin])
        on_since[pin] = fake_ms;
    if (!level && pin_state[pin] && (pin == 24 || pin == 25) && fake_ms - on_since[pin] > max_on_ms)
        max_on_ms = fake_ms - on_since[pin];
    pin_state[pin] = level;
    if (pin_state[24] && pin_state[25])
        both_high = 1;
    return 0;
}

static void f_sleep(unsigned ms) { fake_ms += (int)ms; }

static const a2k_ircut_ops_t fake_pin_ops = { f_open, f_set, f_sleep };

static void test_ircut(void)
{
    a2k_ircut_test_ops(&fake_pin_ops);
    ev_reset();
    fake_ms = 1000;
    CHECK(a2k_ircut_init() == 0 && opened[24] && opened[25] && opened[48] && !pin_state[24] && !pin_state[25] && !pin_state[48],
          "init opens pins 24, 25, 48, all low");
    CHECK(a2k_ircut_pulse(1) == 0, "day pulse");
    CHECK(ev_n == 3 && ev[0].pin == 25 && ev[0].level == 0 && ev[1].pin == 24 && ev[1].level == 1 && ev[2].pin == 24 && ev[2].level == 0,
          "day: pin 25 low, pin 24 high, pin 24 low");
    CHECK(ev[2].t - ev[1].t == 300 && max_on_ms == 300, "day: the coil is energised exactly 300 ms");
    CHECK(!pin_state[24] && !pin_state[25] && !both_high, "day: both coil pins low afterwards, never high together");
    ev_n = 0;
    CHECK(a2k_ircut_pulse(0) == 0, "night pulse");
    CHECK(ev_n == 3 && ev[0].pin == 24 && ev[0].level == 0 && ev[1].pin == 25 && ev[1].level == 1 && ev[2].pin == 25 && ev[2].level == 0,
          "night: pin 24 low, pin 25 high, pin 25 low");
    CHECK(!pin_state[24] && !pin_state[25] && !both_high && max_on_ms <= A2K_IRCUT_PULSE_MS, "night: both low afterwards, bounded");
    CHECK(a2k_ircut_led(1) == 0 && pin_state[48] == 1 && a2k_ircut_led(0) == 0 && pin_state[48] == 0, "IR LED on and off");

    /* failure injection: the active pin cannot be raised */
    ev_reset();
    fail_set_pin = 24;
    fail_set_level = 1;
    fail_set_times = 1;
    CHECK(a2k_ircut_pulse(1) == -1 && !pin_state[24] && !pin_state[25], "a failing raise ends with both pins low and an error");
    /* failure injection: the release of the active pin fails once, the retry releases it */
    ev_reset();
    fail_set_pin = 25;
    fail_set_level = 0;
    fail_set_times = 1;
    pin_state[25] = 0;
    CHECK(a2k_ircut_pulse(0) == -1 && !pin_state[24] && !pin_state[25], "a failing release is retried: both low in the end");
    ev_reset();
    a2k_ircut_test_ops(NULL);
}

/* ---- 3. the engine ---- */
typedef int (*trace_fn)(int64_t ms);   /* 1 dark, 0 bright, -1 vendor error */
static trace_fn trace;
static int dn_inited, isp_night_calls, isp_day_calls, last_isp = -1, level_calls, notify_n;
static int fail_pulse, init_fail, dn_init_fail;
static int moves[256][2];  /* (ms, 1 = to day / 0 = to night) */
static int nmoves, led_on_calls;
static a2k_night_info_t last_info;

static int n_init(void)
{
    if (init_fail)
        return -1;
    pin_state[24] = pin_state[25] = pin_state[48] = 0;
    return 0;
}
static int n_dn_init(void)
{
    dn_inited++;
    return dn_init_fail ? -1 : 0;
}
static int n_level(int cur, int *want)
{
    int d;
    (void)cur;
    level_calls++;
    d = trace ? trace(fake_ms) : -1;
    if (d < 0)
        return -1;
    *want = d;
    return 0;
}
static int n_isp(int night)
{
    if (night)
        isp_night_calls++;
    else
        isp_day_calls++;
    last_isp = night;
    return 0;
}
static int n_ircut(int day)
{
    if (fail_pulse > 0) {
        fail_pulse--;
        return -1;
    }
    /* the real one, on the fake pins: a pulse with its timing */
    a2k_ircut_test_ops(&fake_pin_ops);
    {
        int rc = a2k_ircut_pulse(day);
        a2k_ircut_test_ops(NULL);
        if (rc == 0 && nmoves < 256) {
            moves[nmoves][0] = fake_ms;
            moves[nmoves][1] = day;
            nmoves++;
        }
        return rc;
    }
}
static int n_led(int on)
{
    if (on)
        led_on_calls++;
    pin_state[48] = on ? 1 : 0;
    return 0;
}
static int n_release(void)
{
    pin_state[24] = pin_state[25] = 0;
    return 0;
}
static int64_t n_epoch(void) { return 1700000000 + fake_ms / 1000; }
static void n_notify(const a2k_night_info_t *i)
{
    notify_n++;
    last_info = *i;
}

static const a2k_night_ops_t nops = { n_init, n_dn_init, n_level, n_isp, n_ircut, n_led, n_release, f_sleep, n_epoch, n_notify, NULL, NULL };

static a2k_night_ctl_t ctl;

static void n_reset(trace_fn t, int mode, int ir_auto)
{
    a2k_night_cfg_t cfg;
    ev_reset();
    memset(moves, 0, sizeof moves);
    nmoves = 0;
    dn_inited = isp_night_calls = isp_day_calls = level_calls = notify_n = led_on_calls = 0;
    last_isp = -1;
    fail_pulse = init_fail = dn_init_fail = 0;
    fake_ms = 0;
    trace = t;
    cfg.mode = mode;
    cfg.ir_led_auto = ir_auto;
    a2k_night_ctl_init(&ctl, &cfg, &nops);
}

/* Runs `ms` of 100 ms ticks; after EVERY tick the coil pins must be low (a pulse is atomic in the fake: nothing may be left on). */
static int coil_always_low;
static void run_ms(int ms)
{
    int end = fake_ms + ms;
    while (fake_ms < end) {
        fake_ms += 100;
        a2k_night_ctl_poll(&ctl, fake_ms);
        if (pin_state[24] || pin_state[25])
            coil_always_low = 0;
    }
}

static int t_dark(int64_t ms) { (void)ms; return 1; }
static int t_bright(int64_t ms) { (void)ms; return 0; }
static int t_err(int64_t ms) { (void)ms; return -1; }
/* dark for the first 10 minutes, then bright */
static int t_dusk_dawn(int64_t ms) { return ms < 600000 ? 1 : 0; }
/* flapping with a 2 s period */
static int t_flap2(int64_t ms) { return (ms / 1000) % 2 == 0 ? 1 : 0; }
/* flapping with a 10 s period (5 s dark, 5 s bright) */
static int t_flap10(int64_t ms) { return (ms / 5000) % 2 == 0 ? 1 : 0; }
/* dark with an error every 20th second (the AE was not stable) */
static int t_dark_err(int64_t ms) { return (ms / 1000) % 20 == 19 ? -1 : 1; }

static void test_engine_basic(void)
{
    n_reset(t_dusk_dawn, A2K_NIGHT_AUTO, 1);
    coil_always_low = 1;
    CHECK(a2k_night_ctl_start(&ctl, 0) == 0, "auto: start");
    CHECK(dn_inited == 1, "auto: the vendor parameters are registered once");
    CHECK(nmoves == 1 && moves[0][1] == 1, "start: one filter move, to DAY (unknown state after a crash)");
    CHECK(last_isp == 0 && isp_day_calls == 1 && isp_night_calls == 0, "start: ISP in day mode");
    CHECK(pin_state[48] == 0, "start: IR LED off");
    CHECK(last_info.valid && !last_info.night && last_info.mode == A2K_NIGHT_AUTO && !last_info.ir_on, "info after start: day, auto, IR off");
    run_ms(55000);
    CHECK(nmoves == 1, "dark from the start: no switch in the first minute (the start counts for the 60 s rule)");
    run_ms(15000);
    CHECK(nmoves == 2 && moves[1][1] == 0, "dark: the filter goes to NIGHT after the lock and the confirmation");
    CHECK(moves[1][0] >= 60000 && moves[1][0] <= 66000, "dark: ... at 60..66 s");
    CHECK(isp_night_calls == 1 && led_on_calls == 1 && pin_state[48] == 1, "night: ISP night mode, IR LED on");
    CHECK(last_info.night && last_info.ir_on && last_info.switches == 1, "info: night, IR on, 1 switch");
    run_ms(900000);
    CHECK(nmoves == 3 && moves[2][1] == 1, "bright after 10 minutes: back to DAY");
    CHECK(moves[2][0] >= 600000 && moves[2][0] <= 900000, "... within 5 minutes of the light coming back (the IR-off probe confirms it)");
    CHECK(pin_state[48] == 0 && last_isp == 0 && !last_info.night && last_info.switches == 2, "day: IR LED off, ISP day, 2 switches");
    CHECK(coil_always_low, "the coil pins were low after every tick");
    {
        int ok = 1;
        for (int i = 1; i < nmoves; i++)
            if (moves[i][0] - moves[i - 1][0] < 60000)
                ok = 0;
        CHECK(ok, "at least 60 s between two filter moves");
    }
    a2k_night_ctl_stop(&ctl);
    CHECK(!pin_state[24] && !pin_state[25] && !pin_state[48] && !both_high, "stop in day: coil released, IR LED off");
}

static void test_engine_night_dwell(void)
{
    /* dark 70 s, bright from then on: the night dwell is at least 120 s even though it is bright at once */
    n_reset(t_dusk_dawn, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(900000);
    CHECK(nmoves == 3, "dusk then dawn: exactly three moves (start, night, day)");
    CHECK(moves[2][0] - moves[1][0] >= 120000, "the night dwell is at least 120 s");
}

static void test_engine_flap(void)
{
    int n;
    n_reset(t_flap2, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(1800000);
    CHECK(nmoves == 1, "2 s flapping for 30 minutes: no switch at all (the confirmation never completes)");
    CHECK(level_calls >= 1790 && level_calls <= 1810, "the vendor decision is read about once a second");

    n_reset(t_flap10, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    coil_always_low = 1;
    run_ms(3600000);
    n = nmoves - 1;
    CHECK(n >= 1 && n <= 30, "10 s flapping for an hour: bounded number of switches");
    {
        int ok = 1;
        for (int i = 1; i < nmoves; i++)
            if (moves[i][0] - moves[i - 1][0] < 60000)
                ok = 0;
        CHECK(ok, "10 s flapping: never two moves within 60 s");
    }
    CHECK(coil_always_low && !both_high, "10 s flapping: the coil is never left on");
}

static void test_engine_errors(void)
{
    n_reset(t_err, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(900000);
    CHECK(nmoves == 1 && !last_info.night, "vendor errors (AE not stable) for 15 minutes: nothing happens, stays in day");
    n_reset(t_dark_err, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(300000);
    CHECK(nmoves == 2 && moves[1][1] == 0, "dark with a vendor error every 20 s: the night still comes (30 samples held between errors)");
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    dn_init_fail = 1;
    CHECK(a2k_night_ctl_start(&ctl, 0) == 0 && last_info.auto_off, "vendor parameters refused: automatic off, still running");
    run_ms(300000);
    CHECK(nmoves == 1 && level_calls == 0, "automatic off: no vendor read, no move");
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    init_fail = 1;
    CHECK(a2k_night_ctl_start(&ctl, 0) == -1 && nmoves == 0 && isp_day_calls == 0, "pins cannot be opened: start fails, nothing moved");
}

static void test_engine_forced(void)
{
    n_reset(t_bright, A2K_NIGHT_NIGHT, 1);
    CHECK(a2k_night_ctl_start(&ctl, 0) == 0, "forced night: start");
    CHECK(nmoves == 1 && moves[0][1] == 0 && isp_night_calls == 1 && pin_state[48] == 1 && last_info.night, "forced night: filter out, ISP night, IR LED on");
    CHECK(dn_inited == 0, "forced night: no vendor parameters");
    run_ms(600000);
    CHECK(nmoves == 1 && level_calls == 0, "forced night: the vendor decision is ignored");
    a2k_night_ctl_stop(&ctl);
    CHECK(nmoves == 2 && moves[1][1] == 1 && !pin_state[24] && !pin_state[25] && !pin_state[48] && last_isp == 0 && !both_high,
          "stop from night: filter back to DAY, coil released, IR LED off, ISP day");

    n_reset(t_dark, A2K_NIGHT_DAY, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(600000);
    CHECK(nmoves == 1 && isp_night_calls == 0 && !last_info.night, "forced day: dark scene, stays in day");

    n_reset(t_dark, A2K_NIGHT_NIGHT, 0);
    a2k_night_ctl_start(&ctl, 0);
    CHECK(pin_state[48] == 0 && led_on_calls == 0 && last_info.night && !last_info.ir_on, "ir_led off: night without the IR LED");
    a2k_night_ctl_stop(&ctl);

    n_reset(t_dark, A2K_NIGHT_AUTO, 0);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(100000);
    CHECK(nmoves == 2 && led_on_calls == 0 && pin_state[48] == 0 && last_info.night, "auto with ir_led off: switches to night, the IR LED stays off");

    /* runtime ir_led (Home Assistant "IR LED" select): forced night, no tick thread, applied by the caller */
    n_reset(t_dark, A2K_NIGHT_NIGHT, 1);
    a2k_night_ctl_start(&ctl, 0);
    CHECK(pin_state[48] == 1 && last_info.ir_on, "runtime ir: forced night, IR LED on");
    a2k_night_ctl_ir_request(&ctl, 0);
    a2k_night_ctl_ir_apply(&ctl);
    CHECK(pin_state[48] == 0 && last_info.night && !last_info.ir_on, "runtime ir off: the LED goes dark at once, night (filter, image) stays");
    a2k_night_ctl_ir_request(&ctl, 0);
    a2k_night_ctl_ir_apply(&ctl);
    CHECK(pin_state[48] == 0, "same value again: nothing changes");
    a2k_night_ctl_ir_request(&ctl, 1);
    a2k_night_ctl_ir_apply(&ctl);
    CHECK(pin_state[48] == 1 && last_info.ir_on, "runtime ir auto: the LED is back on in night");
    a2k_night_ctl_stop(&ctl);
    n_reset(t_dark, A2K_NIGHT_DAY, 1);
    a2k_night_ctl_start(&ctl, 0);
    a2k_night_ctl_ir_request(&ctl, 0);
    a2k_night_ctl_ir_apply(&ctl);
    a2k_night_ctl_ir_request(&ctl, 1);
    a2k_night_ctl_ir_apply(&ctl);
    CHECK(pin_state[48] == 0 && led_on_calls == 0, "runtime ir in day: the LED is never lit");
}

static void test_engine_failures(void)
{
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    fail_pulse = 1;
    run_ms(100000);
    CHECK(!last_info.night && nmoves == 1, "a failing filter pulse: stays in day, nothing recorded");
    CHECK(pin_state[48] == 0 && last_isp == 0, "... and the IR LED is off and the ISP back in day mode (filter still in)");
    run_ms(300000);
    CHECK(nmoves >= 2 && last_info.night, "... and the switch is retried later and succeeds");

    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    fail_pulse = 100;
    run_ms(1800000);
    CHECK(pin_state[48] == 0 && last_isp == 0 && !pin_state[24] && !pin_state[25], "failed switches leave the IR LED off, the ISP in day, the coil low");
    CHECK(last_info.auto_off && !last_info.night, "three failures in a row: automatic mode switches itself off, in day");
    CHECK(nmoves == 1, "... and no further attempt is made");
}

static void test_engine_stop_night(void)
{
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_start(&ctl, 0);
    run_ms(100000);
    CHECK(last_info.night, "in night before the stop");
    a2k_night_ctl_stop(&ctl);
    CHECK(moves[nmoves - 1][1] == 1 && !pin_state[24] && !pin_state[25] && !pin_state[48] && last_isp == 0, "stop from auto night: DAY position, coil released, IR LED off");
    CHECK(!both_high, "the coil pins were never high together");
}


/* ---- the camera's own IR light fools the vendor verdict (hardware 2026-11-15) ---- */
static int ambient_dawn_ms = -1; /* the room gets light at this time, -1 never */
static int ir_fools;             /* with the IR LED on the vendor verdict says "day" whatever the room is */
static int ir_flick_ms = -1;     /* a "day" verdict for 5 s from this time, IR on, dark room */
/* the vendor verdict: night (1) while the room is dark, unless the IR LED lights it (fooled) */
static int t_ir(int64_t ms)
{
    int room_bright = ambient_dawn_ms >= 0 && ms >= ambient_dawn_ms;
    if (room_bright)
        return 0;
    if (ir_fools && pin_state[48])
        return 0;
    if (ir_flick_ms >= 0 && ms >= ir_flick_ms && ms < ir_flick_ms + 5000)
        return 0;
    return 1;
}
/* the longest time the IR LED was off while in night mode (a probe) and the number of such periods */
static int ir_off_ms, ir_off_max, ir_off_runs;
static void run_ir(int ms)
{
    int end = fake_ms + ms;
    while (fake_ms < end) {
        fake_ms += 100;
        a2k_night_ctl_poll(&ctl, fake_ms);
        if (pin_state[24] || pin_state[25])
            coil_always_low = 0;
        if (last_info.night && !pin_state[48]) {
            if (ir_off_ms == 0)
                ir_off_runs++;
            ir_off_ms += 100;
            if (ir_off_ms > ir_off_max)
                ir_off_max = ir_off_ms;
        } else {
            ir_off_ms = 0;
        }
    }
}

static void test_engine_ir_selfillum(void)
{
    /* dark room all night, the IR light makes the verdict say "day": it must NOT switch back, for 8 hours */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = -1;
    ir_fools = 1;
    ir_flick_ms = -1;
    ir_off_ms = ir_off_max = ir_off_runs = 0;
    coil_always_low = 1;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(120000);
    CHECK(last_info.night && nmoves == 2, "IR-fooled verdict: the night switch itself comes (dark room, IR still off)");
    run_ir(8 * 3600 * 1000);
    CHECK(nmoves == 2 && last_info.night, "dark room, verdict says day under the IR light: no switch back in 8 hours");
    CHECK(ir_off_runs >= 5, "... the policy probes with the IR LED off meanwhile");
    CHECK(ir_off_max <= 13000, "... each probe keeps the IR LED off for 13 s at most");
    CHECK(pin_state[48] == 1 && coil_always_low && !both_high, "... the IR LED is back on and the coil is low");
    a2k_night_ctl_stop(&ctl);
    CHECK(!pin_state[48] && !pin_state[24] && !pin_state[25], "stop: IR LED off, coil released");

    /* a 5 s "day" verdict under IR light (the 2026-11-15 pattern, shorter): nothing */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ir_fools = 0;
    ir_flick_ms = 300000;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(1800000);
    CHECK(nmoves == 2 && last_info.night, "a 5 s day verdict in a dark room: no switch");
    ir_flick_ms = -1;
}

/* hardware 2026-11-18 (night, dark room, nobody switched a lamp on): under the IR light the vendor verdict flickers between night and day;
 * the policy sees "day" as exposure 0 and its lamp detector (exposure below half of its slow average for 3 s) fired 3 times in 15 min,
 * each time an IR-off probe (picture dark, then an AE flash). A verdict under our own IR light says nothing about the room. */
static int t_ir_flaky(int64_t ms)
{
    if (pin_state[48] && (ms / 1000) % 20 < 4) /* IR on: "day" for 4 s of every 20 s (the room stays dark) */
        return 0;
    return 1;
}
static int t_ir_flaky_err(int64_t ms)
{
    if (pin_state[48] && (ms / 1000) % 30 < 3) /* IR on: reads fail 3 s of every 30 s (exposure not stable) */
        return -1;
    return t_ir_flaky(ms);
}

static void test_engine_lamp_false_trigger(void)
{
    trace_fn tr[2] = { t_ir_flaky, t_ir_flaky_err };
    int i;
    for (i = 0; i < 2; i++) {
        n_reset(tr[i], A2K_NIGHT_AUTO, 1);
        ambient_dawn_ms = -1;
        ir_fools = 0;
        ir_flick_ms = -1;
        ir_off_ms = ir_off_max = ir_off_runs = 0;
        coil_always_low = 1;
        a2k_night_ctl_start(&ctl, 0);
        run_ir(120000);
        CHECK(last_info.night && nmoves == 2, "flaky verdict under IR: night switch");
        run_ir(2 * 3600 * 1000);
        CHECK(nmoves == 2 && last_info.night, "flaky verdict under IR, dark room: no switch back");
        /* periodic probes only: first at the end of the dwell, then 600 s, 1200 s, 2400 s, ... = 5 at most in 2 h */
        CHECK(ir_off_runs >= 3 && ir_off_runs <= 5, "flaky verdict under IR, dark room: only the periodic probes (no lamp probe)");
        CHECK(ir_off_max <= 13000 && pin_state[48] == 1 && coil_always_low, "... each short, IR back on");
        a2k_night_ctl_stop(&ctl);
    }
}

static void test_engine_probe_busy(void)
{
    int saw_busy = 0, busy_outside = 0, i;
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = -1;
    ir_fools = 0;
    a2k_night_ctl_start(&ctl, 0);
    CHECK(!a2k_night_ctl_busy(&ctl, fake_ms), "probe busy: not at start");
    for (i = 0; i < 20000; i++) { /* 2000 s of night: the first probe at the end of the dwell */
        run_ms(100);
        if (ctl.st.mode == NL_PROBING && !a2k_night_ctl_busy(&ctl, fake_ms))
            busy_outside = 1;
        if (a2k_night_ctl_busy(&ctl, fake_ms))
            saw_busy = 1;
        if (nmoves >= 2 && pin_state[48] && !a2k_night_ctl_busy(&ctl, fake_ms) && saw_busy)
            break; /* a quiet night image again */
    }
    CHECK(saw_busy && !busy_outside, "probe busy: true during the IR-off probe and the night switch");
    CHECK(!a2k_night_ctl_busy(&ctl, fake_ms + A2K_NIGHT_BUSY_MS + 1000), "probe busy: over 10 s after the last edge");
    a2k_night_ctl_stop(&ctl);
}

static void test_engine_real_dawn(void)
{
    int day_at;
    /* real dawn at 20 minutes into the night, the IR light fools too: back to day within a bounded time */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = 1200000;
    ir_fools = 1;
    ir_flick_ms = -1;
    ir_off_ms = ir_off_max = ir_off_runs = 0;
    coil_always_low = 1;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(1200000);
    CHECK(last_info.night && nmoves == 2, "before the dawn: in night");
    run_ir(3000000);
    day_at = nmoves == 3 ? moves[2][0] : -1;
    CHECK(nmoves == 3 && moves[2][1] == 1 && !last_info.night, "real dawn: back to DAY");
    CHECK(day_at > 1200000 && day_at <= 1200000 + 2460000, "... within 41 minutes of the light (the probe backoff at its largest: 40 min)");
    CHECK(pin_state[48] == 0 && last_isp == 0 && coil_always_low && !both_high, "... IR LED off, ISP day, coil low");
    CHECK(moves[2][0] - moves[1][0] >= 120000, "... after the minimum night dwell");

    /* the same without the IR fooling and with no lamp history: first probe passes */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = 150000;
    ir_fools = 0;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(600000);
    CHECK(nmoves == 3 && moves[2][1] == 1, "dawn before the first probe: day at the first probe");
    CHECK(moves[2][0] >= 180000 && moves[2][0] <= 300000, "... after the minimum dwell, at the first probe");

    /* IR LED setting off: no probe, no LED, the verdict is read directly and held 15 s */
    n_reset(t_ir, A2K_NIGHT_AUTO, 0);
    ambient_dawn_ms = 400000;
    ir_fools = 1;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(300000);
    CHECK(last_info.night && led_on_calls == 0, "ir_led off: night, no IR LED");
    run_ir(400000);
    CHECK(nmoves == 3 && moves[2][0] >= 415000 && moves[2][0] <= 430000, "ir_led off: day 15 s after the verdict turns to day");
    CHECK(led_on_calls == 0 && pin_state[48] == 0, "ir_led off: the LED never lit");
    ambient_dawn_ms = -1;
}

static void test_engine_probe_safety(void)
{
    /* the probe drops the IR LED; a failing day switch must give it back */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = 150000;
    ir_fools = 1;
    a2k_night_ctl_start(&ctl, 0);
    run_ir(150000);
    CHECK(last_info.night, "in night");
    fail_pulse = 1;
    while (fail_pulse && fake_ms < 600000)
        run_ir(100);
    run_ir(1000);
    CHECK(!fail_pulse && last_info.night && pin_state[48] == 1 && ctl.st.mode == NL_NIGHT, "a failing day switch after the probe: still night, IR LED back on");
    CHECK(!pin_state[24] && !pin_state[25], "... coil low");
    run_ir(3600000);
    CHECK(nmoves == 3 && !last_info.night && pin_state[48] == 0, "... and the day switch is retried later and succeeds");

    /* stop in the middle of a probe: the IR LED is off after the stop, filter in */
    n_reset(t_ir, A2K_NIGHT_AUTO, 1);
    ambient_dawn_ms = -1;
    ir_fools = 1;
    a2k_night_ctl_start(&ctl, 0);
    ir_off_ms = ir_off_max = ir_off_runs = 0;
    while (fake_ms < 3600000 && ctl.st.mode != NL_PROBING)
        run_ir(100);
    CHECK(ctl.st.mode == NL_PROBING && !pin_state[48], "a probe is running (IR LED off)");
    a2k_night_ctl_stop(&ctl);
    CHECK(!pin_state[48] && !pin_state[24] && !pin_state[25] && last_isp == 0 && !both_high, "stop during a probe: IR LED off, coil released, ISP day");
    ambient_dawn_ms = -1;
}

static void test_parse(void)
{
    a2k_night_cfg_t c;
    CHECK(a2k_night_parse_mode("auto") == 0 && a2k_night_parse_mode("day") == 1 && a2k_night_parse_mode("night") == 2, "night = auto|day|night");
    CHECK(a2k_night_parse_mode("") == -1 && a2k_night_parse_mode("Night") == -1 && a2k_night_parse_mode("nigh") == -1 &&
              a2k_night_parse_mode("night ") == -1 && a2k_night_parse_mode("1") == -1 && a2k_night_parse_mode("auto\n") == -1,
          "night: anything else is refused");
    CHECK(a2k_night_parse_ir("auto") == 1 && a2k_night_parse_ir("off") == 0, "ir_led = auto|off");
    CHECK(a2k_night_parse_ir("on") == -1 && a2k_night_parse_ir("") == -1 && a2k_night_parse_ir("OFF") == -1, "ir_led: anything else is refused (there is no forced on)");
    a2k_night_defaults(&c);
    CHECK(c.mode == A2K_NIGHT_AUTO && c.ir_led_auto == 1 && a2k_night_cfg_ok(&c), "defaults: night auto, ir_led auto");
    c.mode = 3;
    CHECK(!a2k_night_cfg_ok(&c), "an out of range mode is invalid");
}

/* ---- 4. image ---- */
static void test_image(void)
{
    a2k_image_cfg_t c;
    a2k_image_defaults(&c);
    CHECK(a2k_image_effect_value(0) == -50 && a2k_image_effect_value(50) == 0 && a2k_image_effect_value(99) == 49 &&
              a2k_image_effect_value(100) == 49,
          "0..100 maps to -50..49 (100 is 99, like the stock)");
    CHECK(a2k_image_effect_value(-1) == -1000 && a2k_image_effect_value(101) == -1000, "out of range: refused");
    fx_n = 0;
    CHECK(a2k_image_apply(&c) == 0 && fx_n == 1 && fx_log[0].type == A2K_FX_MAINS_HZ && fx_log[0].value == 50,
          "defaults: only the mains frequency (50 Hz) is applied, the conf file values stay");
    c.brightness = 70;
    c.contrast = 0;
    c.sharpness = 100;
    c.mains_hz = 60;
    fx_n = 0;
    CHECK(a2k_image_apply(&c) == 0 && fx_n == 4, "all set: four calls");
    CHECK(fx_log[0].type == A2K_FX_BRIGHTNESS && fx_log[0].value == 20 && fx_log[1].type == A2K_FX_CONTRAST && fx_log[1].value == -50 &&
              fx_log[2].type == A2K_FX_SHARPNESS && fx_log[2].value == 49 && fx_log[3].type == A2K_FX_MAINS_HZ && fx_log[3].value == 60,
          "values: brightness 70 -> 20, contrast 0 -> -50, sharpness 100 -> 49, 60 Hz");
    CHECK(A2K_FX_BRIGHTNESS == 1 && A2K_FX_CONTRAST == 3 && A2K_FX_SHARPNESS == 4 && A2K_FX_MAINS_HZ == 7, "effect type numbers are the stock's");
    fx_n = 0;
    fx_fail_type = A2K_FX_CONTRAST;
    CHECK(a2k_image_apply(&c) == 1 && fx_n == 4, "a failing effect is counted and the others still run");
    fx_fail_type = -1;
    c.brightness = 101;
    fx_n = 0;
    CHECK(!a2k_image_cfg_ok(&c) && a2k_image_apply(&c) == 1 && fx_n == 0, "an invalid setting applies nothing");
    a2k_image_defaults(&c);
    c.mains_hz = 55;
    CHECK(!a2k_image_cfg_ok(&c), "mains 55 Hz is invalid");

    /* the replay after a day / night switch (ak_vi_switch_mode reloads the conf set): the last valid configuration is applied again, nothing when none was set */
    a2k_image_defaults(&c);
    CHECK(a2k_image_apply(&c) == 0, "defaults applied once (mains only)");
    fx_n = 0;
    CHECK(a2k_image_replay() == 0 && fx_n == 0, "replay with brightness, contrast and sharpness all unset sends nothing");
    c.sharpness = 70;
    CHECK(a2k_image_apply(&c) == 0, "sharpness 70 applied");
    fx_n = 0;
    CHECK(a2k_image_replay() == 0 && fx_n == 2 && fx_log[0].type == A2K_FX_SHARPNESS && fx_log[0].value == 20 && fx_log[1].type == A2K_FX_MAINS_HZ,
          "replay puts sharpness 70 (effect 20) and the mains back, nothing else");
    c.sharpness = 101;
    fx_n = 0;
    CHECK(a2k_image_apply(&c) == 1 && fx_n == 0, "an invalid configuration is refused");
    CHECK(a2k_image_replay() == 0 && fx_n == 2 && fx_log[0].value == 20, "...and does not replace what the replay holds");
}


/* The vendor read blocks ~1.3 s: with the reader thread the ticks stay on a real 100 ms cadence, so the probe interval that
 * the policy logs ("next in N s") is the wall interval. The reader is simulated by interleaving begin / end around the ticks
 * (no thread, the fake clock). Also: one verdict is consumed once, and a reading that began before an LED change is dropped. */
static void test_engine_async_slow_read(void)
{
    enum { READ_MS = 1300 };
    int64_t rd_end = -1, next_read = 0, fail_at = -1, probe_at = -1;
    int reading = 0, was_ir = 0, was_probe = 0, logged_s = -1, reads = 0, i;
    a2k_night_rd_t rd = { 0, 0 };
    a2k_night_cfg_t cfg;
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    cfg.mode = A2K_NIGHT_AUTO;
    cfg.ir_led_auto = 1;
    a2k_night_ctl_init(&ctl, &cfg, &nops);
    a2k_night_ctl_set_async(&ctl);
    a2k_night_ctl_start(&ctl, 0);
    for (i = 0; i < 20000 && probe_at < 0; i++) { /* up to 2000 s of fake wall time */
        fake_ms += 100;
        if (reading && fake_ms >= rd_end) {
            a2k_night_ctl_read_end(&ctl, &rd, 0, trace(fake_ms), fake_ms);
            reading = 0;
            reads++;
        }
        if (!reading && fake_ms >= next_read) {
            a2k_night_ctl_read_begin(&ctl, &rd);
            reading = 1;
            rd_end = fake_ms + READ_MS;
            next_read = fake_ms + 1000;
        }
        a2k_night_ctl_poll(&ctl, fake_ms);
        {
            int probing = last_info.night && !last_info.ir_on;
            if (was_probe && !probing && fail_at < 0 && last_info.night) {
                fail_at = fake_ms; /* the first probe failed (dark in the ambient light too): IR back on */
                sscanf(strstr(last_info.why, "next in ") + 8, "%d", &logged_s);
            } else if (!was_probe && probing && fail_at >= 0)
                probe_at = fake_ms;
            was_probe = probing;
            was_ir = last_info.ir_on;
        }
    }
    (void)was_ir;
    CHECK(reads > 100, "async: the slow reader published a verdict about every 1.3 s");
    CHECK(fail_at > 0 && logged_s > 0, "async: a probe failed and logged its next interval");
    CHECK(probe_at > 0 && llabs((long long)(probe_at - fail_at) - logged_s * 1000LL) <= 200,
          "async: the next probe starts when the log said (wall time, not stretched by the 1.3 s read)");
    a2k_night_ctl_stop(&ctl);

    /* a reading that began before an LED / ISP change is dropped; a verdict is consumed once */
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    a2k_night_ctl_set_async(&ctl);
    a2k_night_ctl_start(&ctl, 0);
    a2k_night_ctl_read_begin(&ctl, &rd);
    ctl.ops->led(0);
    a2k_night_ctl_read_end(&ctl, &rd, 0, 1, 1); /* same gen: kept */
    CHECK(ctl.seq == 1, "async: a reading under the same generation is published");
    a2k_night_ctl_poll(&ctl, 100);
    CHECK(ctl.seen_seq == 1 && ctl.held_valid, "async: the tick consumed it");
    ctl.held_valid = 0;
    a2k_night_ctl_poll(&ctl, 200);
    CHECK(!ctl.held_valid, "async: it is not consumed twice");
    a2k_night_ctl_read_begin(&ctl, &rd);
    ctl.gen++; /* what an ISP / IR LED change does */
    a2k_night_ctl_read_end(&ctl, &rd, 0, 1, 300);
    CHECK(ctl.seq == 1, "async: a reading that began before a change is discarded");
    a2k_night_ctl_stop(&ctl);
}

/* The vendor lock: a slow dn_level on a reader thread and the engine's ISP switches never overlap. */
static volatile int v_busy, v_overlap;
static int vend_level(int cur, int *want)
{
    (void)cur;
    if (v_busy++)
        v_overlap = 1;
    usleep(20000);
    *want = 0;
    v_busy--;
    return 0;
}
static int vend_mode(int night)
{
    (void)night;
    if (v_busy++)
        v_overlap = 1;
    usleep(3000);
    v_busy--;
    return 0;
}
static void *vend_reader(void *a)
{
    int i;
    (void)a;
    for (i = 0; i < 60; i++)
        a2k_night_ctl_read_once(&ctl);
    return NULL;
}
static void test_engine_vendor_lock(void)
{
    pthread_t th;
    a2k_night_ops_t o = nops;
    a2k_night_cfg_t cfg;
    int i;
    n_reset(t_dark, A2K_NIGHT_AUTO, 1);
    o.dn_level = vend_level;
    o.isp_mode = vend_mode;
    cfg.mode = A2K_NIGHT_AUTO;
    cfg.ir_led_auto = 1;
    a2k_night_ctl_init(&ctl, &cfg, &o);
    a2k_night_ctl_set_async(&ctl);
    a2k_night_ctl_start(&ctl, 0);
    pthread_create(&th, NULL, vend_reader, NULL);
    for (i = 0; i < 40; i++) {
        ctl.night = 1; /* stop() then switches back to day through apply(): isp_mode on this thread */
        a2k_night_ctl_stop(&ctl);
        usleep(2000);
    }
    pthread_join(th, NULL);
    CHECK(!v_overlap, "vendor lock: dn_level (reader) and isp_mode (tick) never overlap");
}

/* WDR replay after every ISP mode switch (the stock re-applies it, the switch resets it) */
static int n_isp_fail_stub(int n) { (void)n; return -1; }
static int w_n, w_fail, w_seq[8];
static int w_mode(int night) { w_seq[w_n < 8 ? w_n : 7] = night ? 1 : 0; return 0; }
static int w_wdr(int on)
{
    w_seq[w_n < 8 ? w_n : 7] += 10 * (on ? 1 : 0); /* 10 / 11 = the replay came after the mode call of this step */
    w_n++;
    return w_fail ? -1 : 0;
}
static void test_engine_wdr(void)
{
    a2k_night_ops_t o = nops;
    a2k_night_cfg_t cfg;
    o.isp_mode = w_mode;
    o.wdr = w_wdr;
    a2k_night_defaults(&cfg);
    CHECK(cfg.wdr == 1, "wdr: on by default (the stock [dsp] wdr_onoff 1)");
    cfg.mode = A2K_NIGHT_NIGHT;
    n_reset(t_dark, A2K_NIGHT_NIGHT, 1);
    w_n = 0;
    a2k_night_ctl_init(&ctl, &cfg, &o);
    a2k_night_ctl_start(&ctl, 0); /* forced night: the first apply() runs isp_mode(1) then the WDR */
    CHECK(w_n == 1 && w_seq[0] == 11, "wdr: replayed once right after a successful night isp_mode, before anything else");
    a2k_night_ctl_stop(&ctl); /* back to day: isp_mode(0) + WDR */
    CHECK(w_n == 2 && w_seq[1] == 10, "wdr: replayed after the day isp_mode too");
    /* failure: logged once, the switch result unchanged */
    w_fail = 1;
    w_n = 0;
    a2k_night_ctl_init(&ctl, &cfg, &o);
    CHECK(a2k_night_ctl_start(&ctl, 0) == 0, "wdr: a failing WDR call never fails the switch");
    a2k_night_ctl_stop(&ctl);
    CHECK(w_n == 2 && ctl.wdr_warned == 1, "wdr: failure tried every time, warned once");
    w_fail = 0;
    /* off in the config: never called */
    cfg.wdr = 0;
    w_n = 0;
    a2k_night_ctl_init(&ctl, &cfg, &o);
    a2k_night_ctl_start(&ctl, 0);
    a2k_night_ctl_stop(&ctl);
    CHECK(w_n == 0, "wdr: wdr=0 never calls it");
    /* the isp_mode failure: no replay */
    cfg.wdr = 1;
    o.isp_mode = n_isp_fail_stub;
    w_n = 0;
    a2k_night_ctl_init(&ctl, &cfg, &o);
    a2k_night_ctl_start(&ctl, 0);
    CHECK(w_n == 0, "wdr: not replayed when the ISP mode switch failed");
    a2k_isp_wdr(1);
    a2k_isp_wdr(0);
    CHECK(fx_n >= 2 && fx_log[fx_n - 2].type == 5 && fx_log[fx_n - 2].value == 40 && fx_log[fx_n - 1].type == 5 && fx_log[fx_n - 1].value == 0,
          "wdr: a2k_isp_wdr is effect 5 with 40 on / 0 off");
    cfg.wdr = 2;
    CHECK(!a2k_night_cfg_ok(&cfg), "wdr: only 0 and 1 are valid");
}

int main(void)
{
    printf("== pins\n");
    test_pins();
    printf("== IR-cut sequence\n");
    test_ircut();
    printf("== engine\n");
    test_engine_basic();
    test_engine_night_dwell();
    test_engine_flap();
    test_engine_errors();
    test_engine_forced();
    test_engine_failures();
    test_engine_stop_night();
    test_engine_ir_selfillum();
    test_engine_lamp_false_trigger();
    test_engine_probe_busy();
    test_engine_real_dawn();
    test_engine_probe_safety();
    test_engine_async_slow_read();
    test_engine_vendor_lock();
    test_engine_wdr();
    printf("== parse\n");
    test_parse();
    printf("== image\n");
    test_image();
    printf("rot2k night unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
