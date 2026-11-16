/* Host unit test of light_logic (portable light policy) with a fake driver and a fake clock, plus the 2K descriptor and the
 * generic sysfs driver on a fake LED-class directory. Prints "rot2k light unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akam2k_hal.h"
#include "akam2k_pin.h"
#include "hal_light.h"
#include "light_logic.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int lvl[8], writes[8], fail_next;
static int fake_set(const hal_light_emitter_t *e, int on)
{
    int i = (int)(e - a2k_light_desc.emitters);
    if (fail_next) {
        fail_next = 0;
        return -1;
    }
    lvl[i] = on;
    writes[i]++;
    return 0;
}
static const hal_light_ops_t fake_ops = { fake_set };

static void reset(void)
{
    memset(lvl, 0, sizeof lvl);
    memset(writes, 0, sizeof writes);
    fail_next = 0;
}

static void test_descriptor(void)
{
    const hal_light_desc_t *d = &a2k_light_desc;
    CHECK(d->count == 2, "2K descriptor: two emitters (ir, blue)");
    CHECK(!strcmp(d->emitters[0].name, "ir") && d->emitters[0].gpio == 48 && d->emitters[0].active_high, "ir = gpio48 active high");
    CHECK(!strcmp(d->emitters[1].name, "blue") && d->emitters[1].gpio == 56 && d->emitters[1].active_high, "blue = gpio56 active high");
    CHECK(d->emitters[0].kind == LIGHT_GPIO && d->emitters[1].kind == LIGHT_GPIO, "all raw GPIO");
    CHECK(!d->excl_a && !d->excl_b, "no ir/white exclusion pair");
    {
        light_t t;
        light_init(&t, d, &fake_ops);
        CHECK(light_find(&t, "white") == -1 && light_find(&t, "alarm") == -1, "no white / alarm emitter");
    }
    CHECK(a2k_pin_allowed(24) && a2k_pin_allowed(25) && a2k_pin_allowed(48) && a2k_pin_allowed(56), "pins 24 25 48 56 allowed");
    CHECK(!a2k_pin_allowed(22) && !a2k_pin_allowed(49) && !a2k_pin_allowed(55) && !a2k_pin_allowed(10), "pins 22, 49, 55 (and 10) refused");
    CHECK(a2k_pin_open(49, 1) == -1 && a2k_pin_open(22, 1) == -1 && a2k_pin_open(55, 1) == -1 && a2k_pin_set(49, 1) == -1, "open / set of 22 49 55 fail");
}

static void test_basic(void)
{
    light_t l;
    int m = -1;
    reset();
    CHECK(light_init(&l, &a2k_light_desc, &fake_ops) == 0, "init");
    CHECK(light_find(&l, "blue") == 1 && light_find(&l, "ir") == 0 && light_find(&l, "nope") == -1 && light_find(&l, NULL) == -1, "resolve by name");
    CHECK(light_set(&l, "blue", LIGHT_ON, 0) == 0 && lvl[1] == 1, "blue on");
    CHECK(light_set(&l, "blue", LIGHT_OFF, 10) == 0 && lvl[1] == 0, "blue off");
    CHECK(light_set(&l, "nope", LIGHT_ON, 0) == -1, "unknown name refused");
    CHECK(light_set(&l, "blue", 9, 0) == -1 && light_set(&l, "blue", -1, 0) == -1, "bad mode refused");
    CHECK(light_parse_mode("blink-slow", &m) == 0 && m == LIGHT_BLINK_SLOW && light_parse_mode("blink-fast", &m) == 0 && m == LIGHT_BLINK_FAST, "parse blink words");
    CHECK(light_parse_mode("on", &m) == 0 && m == LIGHT_ON && light_parse_mode("off", &m) == 0 && m == LIGHT_OFF, "parse on/off");
    CHECK(light_parse_mode("blink", &m) == -1 && light_parse_mode("", &m) == -1 && light_parse_mode(NULL, &m) == -1, "parse refuses other words");
    CHECK(light_set(&l, "blue", LIGHT_ON, 0) == 0 && (fail_next = 1, light_set(&l, "blue", LIGHT_OFF, 5)) == -1 && l.mode[1] == LIGHT_ON, "driver failure leaves the mode unchanged");
    {
        hal_light_desc_t bad = a2k_light_desc;
        bad.excl_a = "nope";
        CHECK(light_init(&l, &bad, &fake_ops) == -1, "init refuses an exclusion naming an unknown emitter");
        bad = a2k_light_desc;
        bad.count = 9;
        CHECK(light_init(&l, &bad, &fake_ops) == -1 && light_init(&l, NULL, &fake_ops) == -1, "init refuses too many emitters / no descriptor");
    }
}

static void test_blink(void)
{
    light_t l;
    reset();
    light_init(&l, &a2k_light_desc, &fake_ops);
    CHECK(light_set(&l, "ir", LIGHT_BLINK_FAST, 1000) == 0 && lvl[0] == 1, "fast blink starts on");
    light_tick(&l, 1400);
    CHECK(lvl[0] == 1, "fast: still on at +400 ms");
    light_tick(&l, 1500);
    CHECK(lvl[0] == 0, "fast: off at +500 ms");
    light_tick(&l, 1999);
    CHECK(lvl[0] == 0, "fast: still off at +999 ms");
    light_tick(&l, 2000);
    CHECK(lvl[0] == 1, "fast: on again at +1000 ms (1 Hz)");
    {
        int w = writes[0];
        light_tick(&l, 2100);
        light_tick(&l, 2200);
        CHECK(writes[0] == w, "no redundant writes between transitions");
    }
    CHECK(light_set(&l, "blue", LIGHT_BLINK_SLOW, 5000) == 0 && lvl[1] == 1, "slow blink starts on");
    light_tick(&l, 5999);
    CHECK(lvl[1] == 1, "slow: on at +999 ms");
    light_tick(&l, 6000);
    CHECK(lvl[1] == 0, "slow: off at +1000 ms");
    light_tick(&l, 7000);
    CHECK(lvl[1] == 1, "slow: on at +2000 ms (0.5 Hz)");
    CHECK(light_set(&l, "blue", LIGHT_ON, 7100) == 0, "blink -> on");
    light_tick(&l, 8500);
    CHECK(lvl[1] == 1, "a solid emitter is not touched by tick");
    CHECK(light_set(&l, "blue", LIGHT_OFF, 8600) == 0 && lvl[1] == 0, "blink/on -> off");
    light_tick(&l, 9000);
    CHECK(lvl[1] == 0, "off stays off after tick");
    light_set(&l, "blue", LIGHT_BLINK_FAST, 9000);
    fail_next = 1;
    CHECK(light_tick(&l, 9500) == 1, "tick reports a driver error");
    light_tick(&l, 9400 - 100000);
    CHECK(1, "a clock that goes backwards does not crash");
}

static void wfile(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

static void rfile(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (f) {
        if (!fgets(out, (int)n, f))
            out[0] = 0;
        fclose(f);
    }
}

static void test_ledclass(void)
{
    char dir[64] = "/tmp/rot2k_light_unit.XXXXXX", p[160], v[32];
    hal_light_emitter_t e[2] = { { "led", LIGHT_LEDCLASS, 0, 1, dir }, { "inv", LIGHT_LEDCLASS, 0, 0, dir } };
    hal_light_desc_t d = { e, 1, NULL, NULL };
    light_t l;
    if (!mkdtemp(dir)) {
        CHECK(0, "temporary directory");
        return;
    }
    snprintf(p, sizeof p, "%s/trigger", dir);
    wfile(p, "heartbeat\n");
    snprintf(p, sizeof p, "%s/brightness", dir);
    wfile(p, "7\n");
    light_init(&l, &d, &hal_light_sysfs_ops);
    CHECK(light_set(&l, "led", LIGHT_ON, 0) == 0, "led-class set on");
    rfile(p, v, sizeof v);
    CHECK(v[0] == '1', "brightness 1 for on (active high)");
    snprintf(p, sizeof p, "%s/trigger", dir);
    rfile(p, v, sizeof v);
    CHECK(!strncmp(v, "none", 4), "trigger set to none");
    wfile(p, "heartbeat\n");
    snprintf(p, sizeof p, "%s/brightness", dir);
    light_set(&l, "led", LIGHT_OFF, 1);
    rfile(p, v, sizeof v);
    CHECK(v[0] == '0', "brightness 0 for off");
    snprintf(p, sizeof p, "%s/trigger", dir);
    rfile(p, v, sizeof v);
    CHECK(!strncmp(v, "heartbeat", 9), "trigger written once only");
    d.emitters = &e[1];
    light_init(&l, &d, &hal_light_sysfs_ops);
    light_set(&l, "inv", LIGHT_ON, 0);
    snprintf(p, sizeof p, "%s/brightness", dir);
    rfile(p, v, sizeof v);
    CHECK(v[0] == '0', "active low: on writes 0");
    e[0].led_dir = "/nonexistent/rot2k_light";
    d.emitters = &e[0];
    light_init(&l, &d, &hal_light_sysfs_ops);
    CHECK(light_set(&l, "led", LIGHT_ON, 0) == -1, "a missing LED directory is an error, nothing is created");
    CHECK(access("/nonexistent", F_OK) != 0, "nothing was created");
    snprintf(p, sizeof p, "%s/brightness", dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/trigger", dir);
    unlink(p);
    rmdir(dir);
}

static void test_gpio_driver(void)
{
    char root[64] = "/tmp/rot2k_light_gpio.XXXXXX", p[160], v[16];
    light_t l;
    if (!mkdtemp(root)) {
        CHECK(0, "temporary directory");
        return;
    }
    snprintf(p, sizeof p, "%s/export", root);
    wfile(p, "");
    snprintf(p, sizeof p, "%s/gpio56", root);
    CHECK(mkdir(p, 0700) == 0, "fake gpio56");
    snprintf(p, sizeof p, "%s/gpio56/direction", root);
    wfile(p, "in\n");
    snprintf(p, sizeof p, "%s/gpio56/value", root);
    wfile(p, "0\n");
    a2k_pin_test_root(root);
    light_init(&l, &a2k_light_desc, &hal_light_sysfs_ops);
    CHECK(light_set(&l, "blue", LIGHT_ON, 0) == 0, "gpio driver: blue on");
    rfile(p, v, sizeof v);
    CHECK(v[0] == '1', "gpio56 value 1 (active high)");
    light_set(&l, "blue", LIGHT_OFF, 1);
    rfile(p, v, sizeof v);
    CHECK(v[0] == '0', "gpio56 value 0 after off");
    snprintf(p, sizeof p, "%s/gpio56/direction", root);
    rfile(p, v, sizeof v);
    CHECK(!strncmp(v, "out", 3), "gpio56 direction out");
    CHECK(light_set(&l, "ir", LIGHT_ON, 2) == -1, "a pin that is not exported and cannot be is an error");
    a2k_pin_close_all();
    a2k_pin_test_root(NULL);
    snprintf(p, sizeof p, "%s/gpio56/direction", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/gpio56/value", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/gpio56", root);
    rmdir(p);
    snprintf(p, sizeof p, "%s/export", root);
    unlink(p);
    rmdir(root);
}

int main(void)
{
    test_descriptor();
    test_basic();
    test_blink();
    test_ledclass();
    test_gpio_driver();
    printf("rot2k light unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
