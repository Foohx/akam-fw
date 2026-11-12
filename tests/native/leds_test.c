/* Host test of src/ak_rtsp/leds.c: status LED state machine, saved settings,
 * 8091 keys and the blink thread, against a fake sysfs directory, a fake
 * network check and a fake IR hook. Nothing here touches a camera. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "leds.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---- fake world ----------------------------------------------------------- */

static char g_root[64];
static char g_led[160], g_conf[160], g_netd[160];

static int g_net;            /* what the fake network check answers */
static int g_net_calls;
static int g_ir_calls, g_ir_last = -1;

static int fake_net(void *u) { (void)u; g_net_calls++; return g_net; }
static void fake_ir(void *u, int allowed) { (void)u; g_ir_calls++; g_ir_last = allowed; }

static const leds_ops_t OPS = { fake_net, fake_ir, NULL };

static void put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(s, f);
    fclose(f);
}

static char *slurp(const char *path, char *buf, size_t cap)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return buf;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void path_of(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

static void reset_world(void)
{
    char cmd[512], p[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/led %s/conf %s/net", g_root, g_root, g_root);
    if (system(cmd) != 0) exit(2);
    snprintf(g_led, sizeof(g_led), "%s/led", g_root);
    snprintf(g_conf, sizeof(g_conf), "%s/conf", g_root);
    snprintf(g_netd, sizeof(g_netd), "%s/net", g_root);
    mkdir(g_led, 0755);
    mkdir(g_netd, 0755);
    path_of(p, sizeof(p), g_led, "brightness"); put(p, "1\n");
    path_of(p, sizeof(p), g_led, "trigger");    put(p, "[none] timer mmc0\n");
    g_net = 1; g_net_calls = 0; g_ir_calls = 0; g_ir_last = -1;
}

static void make(leds_t *l, int tick_ms)
{
    leds_cfg_t cfg = { g_led, g_conf, "lo", g_netd, tick_ms };
    leds_init(l, &OPS, &cfg);
}

static const char *bright(void)
{
    static char buf[64], p[200];
    path_of(p, sizeof(p), g_led, "brightness");
    return slurp(p, buf, sizeof(buf));
}

static const char *trig(void)
{
    static char buf[64], p[200];
    path_of(p, sizeof(p), g_led, "trigger");
    return slurp(p, buf, sizeof(buf));
}

static const char *conf(void)
{
    static char buf[512], p[200];
    path_of(p, sizeof(p), g_conf, "leds");
    return slurp(p, buf, sizeof(buf));
}

static char g_msg[200];
static int  setk(leds_t *l, const char *k, const char *v) { g_msg[0] = 0; return leds_ctl_set(l, k, v, g_msg, sizeof(g_msg)); }
static const char *getk(leds_t *l, const char *k)
{
    static char out[128];
    out[0] = 0;
    if (leds_ctl_get(l, k, out, sizeof(out)) != 0) return "(none)";
    return out;
}

/* ---- pure state machine ----------------------------------------------------- */

static void test_compute(void)
{
    int st;
    CHECK(leds_compute(LEDS_STATUS_OFF, 1, 1, 0, 0, &st) == 0 && st == LEDS_ST_OFF, "off");
    CHECK(leds_compute(LEDS_STATUS_OFF, 0, 0, 3, 5, &st) == 0 && st == LEDS_ST_OFF, "off ignores everything");
    CHECK(leds_compute(LEDS_STATUS_ON, 0, 0, 0, 1, &st) == 1 && st == LEDS_ST_ON, "on is always lit");
    CHECK(leds_compute(LEDS_STATUS_AUTO, 1, 1, 0, 0, &st) == 1 && st == LEDS_ST_ON, "auto ok: solid");
    CHECK(leds_compute(LEDS_STATUS_AUTO, 1, 1, 0, 7, &st) == 1 && st == LEDS_ST_ON, "auto ok: solid at any tick");
    /* starting up or no Wi-Fi: 1 Hz */
    int exp_wifi[4] = { 1, 0, 1, 0 };
    for (unsigned t = 0; t < 4; t++) {
        CHECK(leds_compute(LEDS_STATUS_AUTO, 0, 1, 0, t, &st) == exp_wifi[t] && st == LEDS_ST_BLINK_WIFI,
              "auto booting tick %u", t);
        CHECK(leds_compute(LEDS_STATUS_AUTO, 1, 0, 0, t, &st) == exp_wifi[t] && st == LEDS_ST_BLINK_WIFI,
              "auto no net tick %u", t);
    }
    /* a client is playing: 0.5 Hz, wins over the Wi-Fi blink */
    int exp_stream[8] = { 1, 1, 0, 0, 1, 1, 0, 0 };
    for (unsigned t = 0; t < 8; t++) {
        CHECK(leds_compute(LEDS_STATUS_AUTO, 1, 1, 1, t, &st) == exp_stream[t] && st == LEDS_ST_BLINK_STREAM,
              "auto streaming tick %u", t);
        CHECK(leds_compute(LEDS_STATUS_AUTO, 0, 0, 2, t, &st) == exp_stream[t] && st == LEDS_ST_BLINK_STREAM,
              "streaming wins over boot/no net, tick %u", t);
    }
    CHECK(leds_compute(LEDS_STATUS_AUTO, 1, 1, 0, 0, NULL) == 1, "NULL state_out tolerated");
}

/* ---- default: the LED is left alone ------------------------------------------ */

static void test_default_untouched(void)
{
    reset_world();
    leds_t l; make(&l, 0);
    CHECK(leds_get_status(&l) == LEDS_STATUS_ON && leds_get_ir(&l) == LEDS_IR_AUTO, "defaults on/auto");
    CHECK(!leds_load_config(&l), "no file: defaults kept");
    leds_apply(&l);
    for (int i = 0; i < 5; i++) leds_tick(&l);
    CHECK(!strcmp(bright(), "1\n"), "status=on never writes the LED (got '%s')", bright());
    CHECK(!strcmp(trig(), "[none] timer mmc0\n"), "status=on never touches the trigger");
    CHECK(leds_get_state(&l) == LEDS_ST_UNMANAGED, "state is system");
    CHECK(g_net_calls == 0, "status=on never checks the network");
    CHECK(g_ir_calls == 1 && g_ir_last == 1, "apply tells the IR hook: allowed (calls=%d)", g_ir_calls);
    CHECK(access(g_conf, F_OK) != 0, "apply saves nothing");
    leds_destroy(&l);
}

/* ---- 8091 keys ------------------------------------------------------------------ */

static int  g_list_n;
static char g_list[3][64];
static void list_emit(void *a, const char *line)
{
    (void)a;
    if (g_list_n < 3) snprintf(g_list[g_list_n], 64, "%s", line);
    g_list_n++;
}

static void test_ctl_status(void)
{
    reset_world();
    leds_t l; make(&l, 0);
    leds_apply(&l);

    CHECK(!strcmp(getk(&l, "led.status"), "led.status=on"), "get led.status: %s", getk(&l, "led.status"));
    CHECK(!strcmp(getk(&l, "night.ir_led"), "night.ir_led=auto"), "get night.ir_led");
    CHECK(!strcmp(getk(&l, "led.state"), "led.state=system"), "get led.state");
    CHECK(!strcmp(getk(&l, "audio.out_volume"), "(none)"), "foreign key not ours");
    CHECK(leds_ctl_set(&l, "ae.speed", "3", g_msg, sizeof(g_msg)) == -1, "set foreign key -> -1");

    /* off */
    CHECK(setk(&l, "led.status", "off") == 0 && !strcmp(g_msg, "OK led.status=off"), "set off: %s", g_msg);
    CHECK(!strcmp(bright(), "0"), "LED written 0 (got '%s')", bright());
    CHECK(!strcmp(trig(), "none"), "trigger set to none once (got '%s')", trig());
    CHECK(!strcmp(getk(&l, "led.state"), "led.state=off"), "state off");
    CHECK(!strcmp(conf(), "status=off\nir_led=auto\n"), "saved: '%s'", conf());

    /* on again: the module now drives the LED to 1 */
    CHECK(setk(&l, "led.status", "on") == 0, "set on");
    CHECK(!strcmp(bright(), "1"), "LED written 1 (got '%s')", bright());
    CHECK(!strcmp(getk(&l, "led.state"), "led.state=on"), "state on (managed)");
    CHECK(!strcmp(conf(), "status=on\nir_led=auto\n"), "saved on");

    /* same value: no flash write */
    char p[200]; path_of(p, sizeof(p), g_conf, "leds");
    unlink(p);
    CHECK(setk(&l, "led.status", "on") == 0 && !strcmp(g_msg, "OK led.status=on"), "same value ok");
    CHECK(access(p, F_OK) != 0, "same value does not write the file");

    /* auto */
    CHECK(setk(&l, "led.status", "auto") == 0 && !strcmp(g_msg, "OK led.status=auto"), "set auto");
    CHECK(!strcmp(conf(), "status=auto\nir_led=auto\n"), "saved auto");

    /* validation: nothing changes, nothing is written */
    unlink(p);
    const char *bad[] = { "ON", "Off", "1", "", "blink", "on;off", "auto ", "-1", "0", "autoo", "aut" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(setk(&l, "led.status", bad[i]) == 1, "'%s' rejected", bad[i]);
        CHECK(!strncmp(g_msg, "ERR invalid led.status value", 28), "error text for '%s': %s", bad[i], g_msg);
    }
    CHECK(leds_get_status(&l) == LEDS_STATUS_AUTO, "status unchanged after bad values");
    CHECK(access(p, F_OK) != 0, "bad values write nothing");
    CHECK(setk(&l, "led.state", "on") == 1 && !strncmp(g_msg, "ERR led.state is read-only", 26), "led.state read-only");

    /* ir */
    CHECK(g_ir_calls == 1, "hook calls so far: %d", g_ir_calls);
    CHECK(setk(&l, "night.ir_led", "off") == 0 && !strcmp(g_msg, "OK night.ir_led=off"), "set ir off: %s", g_msg);
    CHECK(g_ir_calls == 2 && g_ir_last == 0, "hook told: not allowed");
    CHECK(!strcmp(conf(), "status=auto\nir_led=off\n"), "saved ir off: '%s'", conf());
    CHECK(setk(&l, "night.ir_led", "off") == 0 && g_ir_calls == 2, "same ir value: hook not called again");
    CHECK(setk(&l, "night.ir_led", "auto") == 0 && g_ir_calls == 3 && g_ir_last == 1, "ir back to auto");
    const char *badir[] = { "on", "AUTO", "Off", "", "1", "day" };
    for (size_t i = 0; i < sizeof(badir) / sizeof(badir[0]); i++) {
        CHECK(setk(&l, "night.ir_led", badir[i]) == 1 && !strncmp(g_msg, "ERR invalid night.ir_led value", 30),
              "ir '%s' rejected", badir[i]);
    }
    CHECK(g_ir_calls == 3, "bad ir values never reach the hook");
    CHECK(leds_get_ir(&l) == LEDS_IR_AUTO, "ir unchanged");

    /* LIST */
    g_list_n = 0;
    leds_ctl_list(&l, list_emit, NULL);
    int n = g_list_n;
    char (*lines)[64] = g_list;
    CHECK(n == 3, "list emits 3 lines (got %d)", n);
    CHECK(n == 3 && !strcmp(lines[0], "led.status=auto") && !strcmp(lines[1], "led.state=blink_wifi") &&
          !strcmp(lines[2], "night.ir_led=auto"), "list content: %s | %s | %s", lines[0], lines[1], lines[2]);
    leds_destroy(&l);
}

/* ---- auto state machine ------------------------------------------------------------ */

static void test_auto_machine(void)
{
    reset_world();
    leds_t l; make(&l, 0);
    CHECK(setk(&l, "led.status", "auto") == 0, "auto");

    /* still starting: 1 Hz blink even though the network is fine */
    const char *exp_boot[4] = { "1", "0", "1", "0" };
    for (int i = 0; i < 4; i++) {
        leds_tick(&l);
        CHECK(!strcmp(bright(), exp_boot[i]), "boot blink step %d: '%s'", i, bright());
    }
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_WIFI, "state blink_wifi while booting");

    /* camera up and Wi-Fi ok: solid */
    leds_set_ready(&l, 1);
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1"), "solid on once ready (got '%s')", bright());
    leds_tick(&l);
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1"), "stays solid");
    CHECK(leds_get_state(&l) == LEDS_ST_ON, "state on");
    {   /* The vendor driver logs every write: a solid LED must not be rewritten. A sentinel survives only
         * when no write happens. */
        char p[200]; path_of(p, sizeof(p), g_led, "brightness"); put(p, "sentinel");
        for (int i = 0; i < 5; i++) leds_tick(&l);
        leds_apply(&l);
        CHECK(!strcmp(bright(), "sentinel"), "solid auto and re-apply do not rewrite the LED (got '%s')", bright());
        put(p, "1");
    }

    /* Wi-Fi lost: blink; back: solid */
    g_net = 0;
    int seen0 = 0, seen1 = 0;
    for (int i = 0; i < 4; i++) { leds_tick(&l); if (!strcmp(bright(), "0")) seen0++; else seen1++; }
    CHECK(seen0 == 2 && seen1 == 2, "wifi lost blinks 1 Hz (%d off, %d on)", seen0, seen1);
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_WIFI, "state blink_wifi without net");
    g_net = 1;
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1") && leds_get_state(&l) == LEDS_ST_ON, "net back: solid");

    /* a client plays: slow blink (privacy indicator), whatever the network */
    leds_client_delta(&l, 1);
    CHECK(leds_clients(&l) == 1, "one client");
    seen0 = seen1 = 0;
    int transitions = 0;
    char prev = 'x';
    for (int i = 0; i < 8; i++) {
        leds_tick(&l);
        char c = bright()[0];
        if (c == '0') seen0++; else seen1++;
        if (prev != 'x' && c != prev) transitions++;
        prev = c;
    }
    CHECK(seen0 == 4 && seen1 == 4, "streaming blinks (%d off, %d on over 8 ticks)", seen0, seen1);
    CHECK(transitions >= 3, "streaming blink transitions: %d", transitions);
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_STREAM, "state blink_stream");
    g_net = 0;
    leds_tick(&l);
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_STREAM, "streaming wins over no net");
    g_net = 1;
    leds_client_delta(&l, 1);   /* never happens (single client server) but must be counted */
    leds_client_delta(&l, -1);
    leds_tick(&l);
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_STREAM, "still one client");
    leds_client_delta(&l, -1);
    leds_client_delta(&l, -1);
    leds_client_delta(&l, -5);
    CHECK(leds_clients(&l) == 0, "the count never goes below 0 (%d)", leds_clients(&l));
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1") && leds_get_state(&l) == LEDS_ST_ON, "client gone: solid again");

    /* switching to off wins at once, and ticks keep it dark */
    CHECK(setk(&l, "led.status", "off") == 0, "off");
    leds_client_delta(&l, 1);
    for (int i = 0; i < 4; i++) { leds_tick(&l); CHECK(!strcmp(bright(), "0"), "off stays dark (tick %d)", i); }
    leds_client_delta(&l, -1);
    leds_destroy(&l);
}

static void test_write_only_on_change(void)
{
    reset_world();
    leds_t l; make(&l, 0);
    leds_set_ready(&l, 1);
    CHECK(setk(&l, "led.status", "auto") == 0, "auto");
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1"), "solid");
    char p[200]; path_of(p, sizeof(p), g_led, "brightness");
    put(p, "marker");
    for (int i = 0; i < 5; i++) leds_tick(&l);
    CHECK(!strcmp(bright(), "marker"), "an unchanged level is not written again (got '%s')", bright());
    leds_destroy(&l);
}

/* ---- sysfs failures logged once --------------------------------------------------- */

static int count_in_file(const char *path, const char *needle)
{
    char buf[4096];
    slurp(path, buf, sizeof(buf));
    int n = 0;
    for (const char *p = buf; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static void test_failure_logged_once(void)
{
    reset_world();
    char logp[200], saved_dir[200];
    snprintf(logp, sizeof(logp), "%s/stdout.log", g_root);
    snprintf(saved_dir, sizeof(saved_dir), "%s/led.gone", g_root);

    leds_t l; make(&l, 0);
    leds_set_ready(&l, 1);
    CHECK(setk(&l, "led.status", "auto") == 0, "auto");

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, STDOUT_FILENO);
    close(fd);

    char cmd[500];
    rename(g_led, saved_dir);                 /* the LED vanishes */
    put(g_led, "not a directory");            /* writes now fail with ENOTDIR */
    l.last_level = -1;
    for (int i = 0; i < 20; i++) leds_tick(&l);
    fflush(stdout);
    CHECK(count_in_file(logp, "[leds] cannot write") == 1, "20 failing ticks log once (got %d)",
          count_in_file(logp, "[leds] cannot write"));

    /* recovery, then a new failure is reported again */
    unlink(g_led);
    rename(saved_dir, g_led);
    leds_tick(&l);
    CHECK(!strcmp(bright(), "1"), "written again after recovery (got '%s')", bright());
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_led);
    CHECK(system(cmd) == 0, "rm");
    g_net = 0;                                /* level will change, write fails again */
    for (int i = 0; i < 6; i++) leds_tick(&l);
    fflush(stdout);
    CHECK(count_in_file(logp, "[leds] cannot write") == 2, "a new failure after recovery logs once more (got %d)",
          count_in_file(logp, "[leds] cannot write"));

    dup2(saved, STDOUT_FILENO);
    close(saved);
    leds_destroy(&l);
}

/* ---- saved settings ------------------------------------------------------------------ */

static void test_persistence(void)
{
    reset_world();
    char p[200];
    mkdir(g_conf, 0755);
    path_of(p, sizeof(p), g_conf, "leds");

    /* garbage, CRLF, unknown keys, bad values: bad values keep the defaults */
    put(p, "status=blink\nir_led=maybe\nfoo=bar\nnoequals\n");
    leds_t l; make(&l, 0);
    CHECK(leds_load_config(&l) == 1, "file read");
    CHECK(leds_get_status(&l) == LEDS_STATUS_ON && leds_get_ir(&l) == LEDS_IR_AUTO, "bad values: defaults");
    leds_destroy(&l);

    put(p, "status=auto\r\nir_led=off\r\n");
    make(&l, 0);
    CHECK(leds_load_config(&l) == 1 && leds_get_status(&l) == LEDS_STATUS_AUTO && leds_get_ir(&l) == LEDS_IR_OFF,
          "CRLF tolerated");
    leds_destroy(&l);

    /* the other lines are kept, ours replaced */
    put(p, "mute=1\nstatus=off\nir_led=off\nchime=3\n");
    make(&l, 0);
    leds_load_config(&l);
    CHECK(leds_get_status(&l) == LEDS_STATUS_OFF && leds_get_ir(&l) == LEDS_IR_OFF, "loaded off/off");
    CHECK(setk(&l, "led.status", "auto") == 0, "set auto");
    CHECK(!strcmp(conf(), "mute=1\nchime=3\nstatus=auto\nir_led=off\n"), "foreign lines kept: '%s'", conf());
    struct stat st;
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "file mode 0600 (%o)", (unsigned)(st.st_mode & 0777));
    char tmp[220]; snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    CHECK(access(tmp, F_OK) != 0, "no temporary file left");
    leds_destroy(&l);

    /* restart: the saved settings come back and are applied */
    make(&l, 0);
    leds_load_config(&l);
    leds_apply(&l);
    CHECK(leds_get_status(&l) == LEDS_STATUS_AUTO && leds_get_ir(&l) == LEDS_IR_OFF, "reloaded");
    CHECK(g_ir_calls == 1 && g_ir_last == 0, "startup applies ir off to the hook");
    CHECK(leds_get_state(&l) == LEDS_ST_BLINK_WIFI, "startup applies auto (not ready: blink)");
    CHECK(!strcmp(bright(), "1"), "first blink step is on");
    leds_destroy(&l);

    /* the directory is created 0700 when it is missing */
    reset_world();
    make(&l, 0);
    CHECK(setk(&l, "led.status", "off") == 0 && !strstr(g_msg, "not saved"), "creates the directory: %s", g_msg);
    CHECK(stat(g_conf, &st) == 0 && (st.st_mode & 0777) == 0700, "directory mode 0700 (%o)", (unsigned)(st.st_mode & 0777));
    leds_destroy(&l);

    /* an unwritable place is reported, and the setting still applies */
    reset_world();
    char blocker[200]; snprintf(blocker, sizeof(blocker), "%s/blocker", g_root);
    put(blocker, "x");
    leds_cfg_t cfg = { g_led, blocker, "lo", g_netd, 0 };   /* conf_dir is a regular file */
    leds_init(&l, &OPS, &cfg);
    CHECK(setk(&l, "led.status", "off") == 0 && strstr(g_msg, "(not saved:"), "save failure reported: %s", g_msg);
    CHECK(leds_get_status(&l) == LEDS_STATUS_OFF && !strcmp(bright(), "0"), "applied despite the save failure");
    leds_destroy(&l);
    unlink(blocker);
}

/* ---- built-in network check ---------------------------------------------------------- */

static void test_net_check(void)
{
    reset_world();
    char p[220];
    CHECK(leds_net_check("nonexistent9", g_netd) == 0, "unknown interface: no network");
    CHECK(leds_net_check("", g_netd) == 0, "empty name");
    CHECK(leds_net_check("a-name-much-too-long-for-an-interface", g_netd) == 0, "name too long");
    int lo_has_addr = leds_net_check("lo", NULL);
    if (!lo_has_addr) { printf("note: no IPv4 address on lo here, operstate cases skipped\n"); return; }
    snprintf(p, sizeof(p), "%s/lo", g_netd);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/lo/operstate", g_netd);
    const char *down[] = { "down\n", "dormant\n", "lowerlayerdown\n", "notpresent\n" };
    for (size_t i = 0; i < 4; i++) {
        put(p, down[i]);
        CHECK(leds_net_check("lo", g_netd) == 0, "operstate %s means no network", down[i]);
    }
    put(p, "up\n");
    CHECK(leds_net_check("lo", g_netd) == 1, "operstate up");
    put(p, "unknown\n");
    CHECK(leds_net_check("lo", g_netd) == 1, "operstate unknown is fine");
    unlink(p);
    CHECK(leds_net_check("lo", g_netd) == 1, "operstate absent is fine");
}

/* ---- thread ------------------------------------------------------------------------------ */

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_bright(const char *want, int max_ms)
{
    for (int t = 0; t < max_ms; t += 5) {
        if (!strcmp(bright(), want)) return 1;
        msleep(5);
    }
    return 0;
}

static void *hammer(void *arg)
{
    leds_t *l = arg;
    for (int i = 0; i < 20000; i++) { leds_client_delta(l, 1); leds_client_delta(l, -1); }
    return NULL;
}

static void test_thread(void)
{
    reset_world();
    leds_t l; make(&l, 10);

    /* status on: the thread does not even look at the network, and ends fast */
    CHECK(leds_thread_start(&l) == 0, "thread starts");
    CHECK(leds_thread_start(&l) == 0, "second start is a no-op");
    msleep(120);
    CHECK(g_net_calls == 0, "status=on: no network checks (%d)", g_net_calls);
    CHECK(!strcmp(bright(), "1\n"), "status=on: LED untouched");

    /* auto from the control side: the thread blinks (booting), then goes solid */
    CHECK(setk(&l, "led.status", "auto") == 0, "set auto while the thread runs");
    int saw0 = 0, saw1 = 0;
    for (int i = 0; i < 80; i++) {
        const char *b = bright();
        if (!strcmp(b, "0")) saw0 = 1;
        if (!strcmp(b, "1")) saw1 = 1;
        msleep(5);
    }
    CHECK(saw0 && saw1, "the thread blinks while starting (saw off=%d on=%d)", saw0, saw1);
    int calls_before = g_net_calls;
    leds_set_ready(&l, 1);
    CHECK(wait_bright("1", 300), "solid once ready");
    msleep(60);
    CHECK(!strcmp(bright(), "1"), "stays solid");
    CHECK(g_net_calls > calls_before, "auto mode checks the network each tick");
    /* ~100 Hz at a 10 ms tick: about 20 checks per 200 ms, never a runaway loop */
    calls_before = g_net_calls;
    msleep(200);
    CHECK(g_net_calls - calls_before <= 40, "wakeups bounded: %d in 200 ms at 10 ms ticks", g_net_calls - calls_before);

    /* client counter from another thread while the LED thread ticks */
    pthread_t h1, h2;
    pthread_create(&h1, NULL, hammer, &l);
    pthread_create(&h2, NULL, hammer, &l);
    pthread_join(h1, NULL);
    pthread_join(h2, NULL);
    CHECK(leds_clients(&l) == 0, "concurrent +1/-1 balance to 0 (%d)", leds_clients(&l));

    /* off from the control side takes effect at once, even in a long sleep */
    CHECK(setk(&l, "led.status", "off") == 0, "off");
    CHECK(!strcmp(bright(), "0"), "dark at once");
    msleep(100);
    CHECK(!strcmp(bright(), "0"), "stays dark");

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    leds_thread_stop(&l);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms < 30, "stop returns promptly even in the slow tick (%ld ms)", ms);
    leds_thread_stop(&l);   /* idempotent */
    leds_destroy(&l);
}

int main(void)
{
    char tmpl[] = "/tmp/leds_test.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_root, sizeof(g_root), "%s", tmpl);
    setvbuf(stdout, NULL, _IONBF, 0);

    test_compute();
    test_default_untouched();
    test_ctl_status();
    test_auto_machine();
    test_write_only_on_change();
    test_failure_logged_once();
    test_persistence();
    test_net_check();
    test_thread();

    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }
    printf("leds_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
