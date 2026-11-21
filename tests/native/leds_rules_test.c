/* Host test of what the LED engine of platforms/anyka-ak3918/leds.c has beyond the old one (convergence P6): the rule table, the pattern of a
 * rule (get, set, validation, the saved led_<rule> lines and their reload), the night rule, the transient blip and the thread
 * that follows a pattern of any period. The default behaviour is proven against the old engine by leds_oracle_test.c. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "leds.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char g_root[64], g_led[160], g_conf[160];
static int g_net = 1, g_night;
static int fake_net(void *u) { (void)u; return g_net; }
static int fake_night(void *u) { (void)u; return g_night; }
static const leds_ops_t OPS = { fake_net, NULL, NULL };

static void put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(s, f);
    fclose(f);
}

static const char *slurp(const char *path)
{
    static char buf[1024];
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static const char *bright(void)
{
    char p[200];
    snprintf(p, sizeof(p), "%s/brightness", g_led);
    return slurp(p);
}

static const char *conf(void)
{
    char p[200];
    snprintf(p, sizeof(p), "%s/leds", g_conf);
    return slurp(p);
}

static void reset_world(void)
{
    char cmd[400], p[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/led %s/conf", g_root, g_root);
    if (system(cmd) != 0) exit(2);
    snprintf(g_led, sizeof(g_led), "%s/led", g_root);
    snprintf(g_conf, sizeof(g_conf), "%s/conf", g_root);
    mkdir(g_led, 0755);
    snprintf(p, sizeof(p), "%s/brightness", g_led); put(p, "1\n");
    snprintf(p, sizeof(p), "%s/trigger", g_led);    put(p, "[none] timer mmc0\n");
    g_net = 1;
    g_night = 0;
}

static void make(leds_t *l, int tick_ms)
{
    leds_cfg_t cfg = { g_led, g_conf, "lo", "/nonexistent-net", tick_ms };
    leds_init(l, &OPS, &cfg);
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void test_rules(void)
{
    reset_world();
    CHECK(!strcmp(leds_rule_name(LEDS_RULE_STREAM), "stream") && !strcmp(leds_rule_name(LEDS_RULE_IDLE), "idle"), "rule names");
    CHECK(leds_rule_find("nowifi") == LEDS_RULE_NOWIFI && leds_rule_find("person") == -1 && leds_rule_find(NULL) == -1, "rule find");
    leds_t l;
    make(&l, 0);
    char t[40];
    CHECK(leds_pat_get(&l, LEDS_RULE_STREAM, t, sizeof(t)) == 0 && !strcmp(t, "1:1000:1000:0"), "stream default '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_BOOT, t, sizeof(t)) == 0 && !strcmp(t, "1:500:500:0"), "boot default '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_NOWIFI, t, sizeof(t)) == 0 && !strcmp(t, "1:500:500:0"), "nowifi default '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_NIGHT, t, sizeof(t)) == 0 && !strcmp(t, "on"), "night default '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_IDLE, t, sizeof(t)) == 0 && !strcmp(t, "on"), "idle default '%s'", t);
    CHECK(leds_pat_get(&l, 9, t, sizeof(t)) == -1, "bad rule");
    leds_pat_default(&l, LEDS_RULE_STREAM, t, sizeof(t));
    CHECK(!strcmp(t, "1:1000:1000:0"), "default text helper");

    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "bogus") == -1, "bad pattern refused");
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "1:10:10:0") == -1, "under 50 ms refused");
    CHECK(leds_pat_set(&l, 7, "on") == -1, "bad rule refused");
    CHECK(access(g_conf, F_OK) != 0, "a refused pattern saves nothing");
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "on") == 0 && access(g_conf, F_OK) != 0, "the same pattern: no flash write");
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "3:250:250:2000") == 0, "idle set");
    CHECK(!strcmp(conf(), "status=on\nir_led=auto\nled_idle=3:250:250:2000\n"), "saved with the changed rule only: '%s'", conf());
    CHECK(leds_pat_get(&l, LEDS_RULE_IDLE, t, sizeof(t)) == 0 && !strcmp(t, "3:250:250:2000"), "idle read back");
    CHECK(leds_pat_set(&l, LEDS_RULE_STREAM, "on") == 0, "stream set to on");
    CHECK(!strcmp(conf(), "status=on\nir_led=auto\nled_stream=on\nled_idle=3:250:250:2000\n"), "two rules saved in rule order: '%s'", conf());
    /* back to the default: the line goes away */
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "on") == 0 && leds_pat_set(&l, LEDS_RULE_STREAM, "1:1000:1000:0") == 0, "back to the defaults");
    CHECK(!strcmp(conf(), "status=on\nir_led=auto\n"), "defaults leave the two-line file: '%s'", conf());
    leds_destroy(&l);

    /* the file survives a restart, a bad value keeps the default, other lines are kept */
    mkdir(g_conf, 0755);
    char p[200];
    snprintf(p, sizeof(p), "%s/leds", g_conf);
    put(p, "status=auto\nled_boot=2:100:100:1000\nled_nowifi=nope\nled_idle=off\nfoo=bar\nled_bar=on\n");
    make(&l, 0);
    CHECK(leds_load_config(&l) == 1 && leds_get_status(&l) == LEDS_STATUS_AUTO, "file read");
    CHECK(leds_pat_get(&l, LEDS_RULE_BOOT, t, sizeof(t)) == 0 && !strcmp(t, "2:100:100:1000"), "boot loaded '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_NOWIFI, t, sizeof(t)) == 0 && !strcmp(t, "1:500:500:0"), "a bad pattern keeps the default '%s'", t);
    CHECK(leds_pat_get(&l, LEDS_RULE_IDLE, t, sizeof(t)) == 0 && !strcmp(t, "off"), "idle loaded '%s'", t);
    CHECK(leds_ctl_set(&l, "night.ir_led", "off", t, sizeof(t)) == 0, "a setting saves the file again");
    CHECK(!strcmp(conf(), "foo=bar\nled_bar=on\nstatus=auto\nir_led=off\nled_boot=2:100:100:1000\nled_idle=off\n"),
          "rewritten: unknown lines kept (also led_bar), our lines regenerated: '%s'", conf());
    leds_destroy(&l);
}

/* a solid state and the rule that wins, evaluated by hand */
static void test_policy(void)
{
    reset_world();
    leds_t l;
    make(&l, 0);
    leds_set_night_hook(&l, fake_night, NULL);
    char m[100];
    CHECK(leds_ctl_set(&l, "led.status", "auto", m, sizeof(m)) == 0, "auto");
    leds_set_ready(&l, 1);
    CHECK(leds_pat_set(&l, LEDS_RULE_NIGHT, "off") == 0, "night rule = off");
    leds_tick(&l);
    CHECK(leds_rule_now(&l) == LEDS_RULE_IDLE && !strcmp(bright(), "1"), "idle: solid on (rule %d, '%s')", leds_rule_now(&l), bright());
    g_night = 1;
    leds_tick(&l);
    CHECK(leds_rule_now(&l) == LEDS_RULE_NIGHT && !strcmp(bright(), "0"), "night rule wins over idle and its pattern is applied ('%s')", bright());
    CHECK(leds_get_state(&l) == LEDS_ST_ON, "the night rule reports state on");
    g_net = 0;
    leds_tick(&l);
    CHECK(leds_rule_now(&l) == LEDS_RULE_NOWIFI, "no network wins over night");
    leds_client_delta(&l, 1);
    leds_tick(&l);
    CHECK(leds_rule_now(&l) == LEDS_RULE_STREAM, "a client wins over everything");
    leds_client_delta(&l, -1);
    g_net = 1;
    leds_ctl_set(&l, "led.status", "on", m, sizeof(m));
    CHECK(leds_rule_now(&l) == -1 && !strcmp(bright(), "1"), "status=on: no rule, solid");
    leds_destroy(&l);
}

/* the thread follows a pattern of any period, the blip overrides and ends by itself */
static void test_thread_blip(void)
{
    reset_world();
    leds_t l;
    make(&l, 0); /* the production tick: 500 ms */
    char m[100];
    CHECK(leds_blip(&l, "nope", 1000) == -1 && leds_blip(&l, "on", 0) == -1 && leds_blip(&l, "on", 60001) == -1, "bad blips refused");
    leds_ctl_set(&l, "led.status", "auto", m, sizeof(m));
    leds_set_ready(&l, 1);
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "1:100:100:0") == 0, "a fast idle pattern");
    CHECK(leds_thread_start(&l) == 0, "thread");
    int saw0 = 0, saw1 = 0, flips = 0, last = -1;
    for (int i = 0; i < 100; i++) { /* 1 s at 10 ms */
        const char *b = bright();
        int v = !strcmp(b, "0") ? 0 : !strcmp(b, "1") ? 1 : -1;
        if (v == 0) saw0 = 1;
        if (v == 1) saw1 = 1;
        if (v >= 0 && v != last) { if (last >= 0) flips++; last = v; }
        msleep(10);
    }
    /* a 100 ms half period at a 500 ms tick: the thread wakes at the pattern edges, not once a tick */
    CHECK(saw0 && saw1 && flips >= 6, "the thread follows a 100 ms pattern (flips %d in 1 s)", flips);
    CHECK(leds_pat_set(&l, LEDS_RULE_IDLE, "on") == 0, "back to solid");
    msleep(300);
    CHECK(!strcmp(bright(), "1"), "solid again ('%s')", bright());

    CHECK(leds_blip(&l, "off", 400) == 0, "blip off for 400 ms");
    msleep(100);
    CHECK(!strcmp(bright(), "0"), "the blip shows ('%s')", bright());
    msleep(700);
    CHECK(!strcmp(bright(), "1"), "the rules come back after the blip ('%s')", bright());
    CHECK(leds_blip(&l, "off", 30000) == 0, "long blip");
    msleep(100);
    CHECK(!strcmp(bright(), "0"), "dark");
    leds_blip_clear(&l);
    msleep(100);
    CHECK(!strcmp(bright(), "1"), "cleared at once ('%s')", bright());
    leds_ctl_set(&l, "led.status", "off", m, sizeof(m));
    CHECK(leds_blip(&l, "on", 1000) == 0, "blip accepted while off");
    msleep(100);
    CHECK(!strcmp(bright(), "1"), "an explicit blip overrides status=off ('%s')", bright());
    msleep(1000);
    CHECK(!strcmp(bright(), "0"), "status=off dark again after the blip ('%s')", bright());
    leds_thread_stop(&l);
    leds_destroy(&l);

    /* status=on and untouched: a blip takes the LED over, then hands solid on back */
    reset_world();
    make(&l, 0);
    CHECK(leds_get_state(&l) == LEDS_ST_UNMANAGED, "untouched");
    leds_thread_start(&l);
    CHECK(leds_blip(&l, "off", 300) == 0, "blip while status=on");
    msleep(100);
    CHECK(!strcmp(bright(), "0"), "dark during the blip");
    msleep(500);
    CHECK(!strcmp(bright(), "1"), "solid on after it ('%s')", bright());
    leds_thread_stop(&l);
    leds_destroy(&l);
}

int main(void)
{
    char tmpl[] = "/tmp/leds_rules.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_root, sizeof(g_root), "%s", tmpl);
    setvbuf(stdout, NULL, _IONBF, 0);
    test_rules();
    test_policy();
    test_thread_blip();
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }
    printf("leds_rules_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
