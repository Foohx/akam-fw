/* ORACLE test of the 1080p LED engine (convergence P6).
 *
 * The status LED engine of src/ak_rtsp/leds.c was re-expressed as light_logic + led_pat rules (default patterns). This test
 * proves that, with the DEFAULT patterns, it does exactly what the engine did before: tests/native/leds_legacy.c is a verbatim
 * copy of the old leds.c (identifiers prefixed legacy_) and both engines are driven with the SAME scripted and pseudo-random
 * traces (status changes through the 8091 keys, start-up end, network up/down, RTSP clients coming and going, night.ir_led,
 * apply, ticks), each on its own fake sysfs directory. After every step the test requires:
 *   - the same sequence of writes to <led_dir>/trigger and <led_dir>/brightness (file, content, order, count: every write()
 *     is logged through a linker wrap, so a redundant write is a difference),
 *   - the same led.state, led.status, night.ir_led, and the same number of network checks and IR hook calls,
 *   - the same saved settings file.
 * Plus the pure policy function leds_compute() against the old one over its whole input space. Nothing touches a camera. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "leds.h"
#include "leds_legacy.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---- the write log: every write() to a file named trigger or brightness, per world ---- */
extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_write(int fd, const void *buf, size_t n);

#define NFD 512
static char g_fdtag[NFD]; /* 0 none, else 'o' (old world) or 'n' (new world) */
static char g_fdfile[NFD][12];
static char g_log[2][32768]; /* [0] old, [1] new */
static size_t g_loglen[2];
static char g_root[96];

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = __real_open(path, flags, mode);
    if (fd >= 0 && fd < NFD) {
        g_fdtag[fd] = 0;
        if (strstr(path, g_root) && (flags & (O_WRONLY | O_RDWR))) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (!strcmp(base, "trigger") || !strcmp(base, "brightness")) {
                g_fdtag[fd] = strstr(path, "/old/") ? 'o' : 'n';
                snprintf(g_fdfile[fd], sizeof(g_fdfile[fd]), "%s", base);
            }
        }
    }
    return fd;
}

int __wrap_close(int fd)
{
    if (fd >= 0 && fd < NFD) g_fdtag[fd] = 0;
    return __real_close(fd);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (fd >= 0 && fd < NFD && g_fdtag[fd]) {
        int w = g_fdtag[fd] == 'o' ? 0 : 1;
        int k = snprintf(g_log[w] + g_loglen[w], sizeof(g_log[w]) - g_loglen[w], "%s=%.*s;", g_fdfile[fd], (int)n, (const char *)buf);
        if (k > 0 && g_loglen[w] + (size_t)k < sizeof(g_log[w])) g_loglen[w] += (size_t)k;
    }
    return __real_write(fd, buf, n);
}

/* ---- the two worlds share one fake environment ---- */
static int g_net;               /* the fake network check */
static int g_net_calls[2];
static int g_ir_calls[2], g_ir_last[2];

static int net_old(void *u) { (void)u; g_net_calls[0]++; return g_net; }
static int net_new(void *u) { (void)u; g_net_calls[1]++; return g_net; }
static void ir_old(void *u, int a) { (void)u; g_ir_calls[0]++; g_ir_last[0] = a; }
static void ir_new(void *u, int a) { (void)u; g_ir_calls[1]++; g_ir_last[1] = a; }

static const legacy_leds_ops_t OPS_OLD = { net_old, ir_old, NULL };
static const leds_ops_t OPS_NEW = { net_new, ir_new, NULL };

static char g_dir[2][128], g_conf[2][128];

static void put(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(s, f);
    fclose(f);
}

static void slurp(const char *path, char *buf, size_t cap)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void world_reset(void)
{
    char cmd[400], p[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/old %s/new", g_root, g_root);
    if (system(cmd) != 0) exit(2);
    for (int w = 0; w < 2; w++) {
        snprintf(g_dir[w], sizeof(g_dir[w]), "%s/%s/led", g_root, w ? "new" : "old");
        snprintf(g_conf[w], sizeof(g_conf[w]), "%s/%s/conf", g_root, w ? "new" : "old");
        snprintf(p, sizeof(p), "%s/%s", g_root, w ? "new" : "old");
        mkdir(p, 0755);
        mkdir(g_dir[w], 0755);
        snprintf(p, sizeof(p), "%s/brightness", g_dir[w]); put(p, "1\n");
        snprintf(p, sizeof(p), "%s/trigger", g_dir[w]);    put(p, "[none] timer mmc0\n");
        g_loglen[w] = 0;
        g_log[w][0] = '\0';
        g_net_calls[w] = g_ir_calls[w] = 0;
        g_ir_last[w] = -1;
    }
    g_net = 1;
}

typedef struct {
    legacy_leds_t o;
    leds_t n;
} pair_t;

static void pair_make(pair_t *pr, int tick_ms)
{
    legacy_leds_cfg_t co = { g_dir[0], g_conf[0], "lo", "/nonexistent-net", tick_ms };
    leds_cfg_t cn = { g_dir[1], g_conf[1], "lo", "/nonexistent-net", tick_ms };
    legacy_leds_init(&pr->o, &OPS_OLD, &co);
    leds_init(&pr->n, &OPS_NEW, &cn);
}

/* Compares everything that is observable. `what` names the step for the message. */
static void compare(pair_t *pr, const char *what)
{
    char a[512], b[512], pa[200], pb[200];
    CHECK(g_loglen[0] == g_loglen[1] && !memcmp(g_log[0], g_log[1], g_loglen[0]),
          "%s: write sequences differ\n  old: %s\n  new: %s", what, g_log[0], g_log[1]);
    CHECK(legacy_leds_get_state(&pr->o) == leds_get_state(&pr->n), "%s: state old %d new %d", what, legacy_leds_get_state(&pr->o),
          leds_get_state(&pr->n));
    CHECK(legacy_leds_get_status(&pr->o) == leds_get_status(&pr->n), "%s: status", what);
    CHECK(legacy_leds_get_ir(&pr->o) == leds_get_ir(&pr->n), "%s: ir", what);
    CHECK(g_net_calls[0] == g_net_calls[1], "%s: network checks old %d new %d", what, g_net_calls[0], g_net_calls[1]);
    CHECK(g_ir_calls[0] == g_ir_calls[1] && g_ir_last[0] == g_ir_last[1], "%s: IR hook calls", what);
    snprintf(pa, sizeof(pa), "%s/leds", g_conf[0]);
    snprintf(pb, sizeof(pb), "%s/leds", g_conf[1]);
    slurp(pa, a, sizeof(a));
    slurp(pb, b, sizeof(b));
    CHECK(!strcmp(a, b), "%s: saved settings differ: '%s' vs '%s'", what, a, b);
    snprintf(pa, sizeof(pa), "%s/brightness", g_dir[0]);
    snprintf(pb, sizeof(pb), "%s/brightness", g_dir[1]);
    slurp(pa, a, sizeof(a));
    slurp(pb, b, sizeof(b));
    CHECK(!strcmp(a, b), "%s: brightness file '%s' vs '%s'", what, a, b);
}

static void pair_destroy(pair_t *pr)
{
    legacy_leds_destroy(&pr->o);
    leds_destroy(&pr->n);
}

/* One operation of a trace, applied to both engines. */
enum { OP_TICK, OP_STATUS_ON, OP_STATUS_OFF, OP_STATUS_AUTO, OP_READY, OP_NET_UP, OP_NET_DOWN, OP_CLIENT_UP, OP_CLIENT_DOWN,
       OP_IR_AUTO, OP_IR_OFF, OP_APPLY, OP_BAD_STATUS, OP_N };
static const char *const OPNAME[OP_N] = { "tick", "status=on", "status=off", "status=auto", "ready", "net up", "net down", "client+",
                                          "client-", "ir=auto", "ir=off", "apply", "bad status" };

static void op_apply(pair_t *pr, int op)
{
    char mo[200], mn[200];
    switch (op) {
    case OP_TICK: legacy_leds_tick(&pr->o); leds_tick(&pr->n); break;
    case OP_STATUS_ON:
    case OP_STATUS_OFF:
    case OP_STATUS_AUTO:
    case OP_BAD_STATUS: {
        const char *v = op == OP_STATUS_ON ? "on" : op == OP_STATUS_OFF ? "off" : op == OP_STATUS_AUTO ? "auto" : "blink";
        int ro = legacy_leds_ctl_set(&pr->o, "led.status", v, mo, sizeof(mo));
        int rn = leds_ctl_set(&pr->n, "led.status", v, mn, sizeof(mn));
        CHECK(ro == rn && !strcmp(mo, mn), "led.status=%s: reply old '%s'(%d) new '%s'(%d)", v, mo, ro, mn, rn);
        break;
    }
    case OP_READY: legacy_leds_set_ready(&pr->o, 1); leds_set_ready(&pr->n, 1); break;
    case OP_NET_UP: g_net = 1; break;
    case OP_NET_DOWN: g_net = 0; break;
    case OP_CLIENT_UP: legacy_leds_client_delta(&pr->o, 1); leds_client_delta(&pr->n, 1); break;
    case OP_CLIENT_DOWN: legacy_leds_client_delta(&pr->o, -1); leds_client_delta(&pr->n, -1); break;
    case OP_IR_AUTO:
    case OP_IR_OFF: {
        const char *v = op == OP_IR_AUTO ? "auto" : "off";
        int ro = legacy_leds_ctl_set(&pr->o, "night.ir_led", v, mo, sizeof(mo));
        int rn = leds_ctl_set(&pr->n, "night.ir_led", v, mn, sizeof(mn));
        CHECK(ro == rn && !strcmp(mo, mn), "night.ir_led=%s reply", v);
        break;
    }
    case OP_APPLY: legacy_leds_apply(&pr->o); leds_apply(&pr->n); break;
    }
}

/* ---- the pure policy over its whole input space ---- */
static void test_compute_exhaustive(void)
{
    int bad = 0, n = 0;
    for (int status = 0; status <= 2; status++)
        for (int ready = 0; ready <= 1; ready++)
            for (int net = 0; net <= 1; net++)
                for (int clients = 0; clients <= 3; clients++)
                    for (unsigned tick = 0; tick < 64; tick++) {
                        int so, sn;
                        int lo = legacy_leds_compute(status, ready, net, clients, tick, &so);
                        int ln = leds_compute(status, ready, net, clients, tick, &sn);
                        n++;
                        if (lo != ln || so != sn) {
                            if (bad++ < 5)
                                printf("  compute(status %d ready %d net %d clients %d tick %u): old %d/%d new %d/%d\n", status, ready, net,
                                       clients, tick, lo, so, ln, sn);
                        }
                    }
    CHECK(bad == 0, "leds_compute differs from the old one in %d of %d inputs", bad, n);
}

/* ---- scripted traces: the five states of the status LED, in the order a camera goes through them ---- */
static void test_boot_story(int tick_ms)
{
    pair_t pr;
    world_reset();
    pair_make(&pr, tick_ms);
    char tag[80];
    snprintf(tag, sizeof(tag), "story (tick %d)", tick_ms);
    compare(&pr, tag);
    op_apply(&pr, OP_APPLY); compare(&pr, tag);                       /* default: status=on, nothing written */
    for (int i = 0; i < 4; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    CHECK(g_loglen[0] == 0, "status=on at the boot never writes (log '%s')", g_log[0]);
    op_apply(&pr, OP_STATUS_AUTO); compare(&pr, tag);                 /* take over: boot blink */
    for (int i = 0; i < 9; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    CHECK(g_loglen[0] > 0, "the boot blink writes");
    op_apply(&pr, OP_READY); compare(&pr, tag);                       /* ready, network up: solid */
    for (int i = 0; i < 6; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_NET_DOWN); compare(&pr, tag);                    /* no Wi-Fi: blink */
    for (int i = 0; i < 7; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_NET_UP);
    for (int i = 0; i < 3; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_CLIENT_UP); compare(&pr, tag);                   /* a client plays: slow blink */
    for (int i = 0; i < 11; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_NET_DOWN);                                       /* the stream rule still wins */
    for (int i = 0; i < 5; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_NET_UP);
    op_apply(&pr, OP_CLIENT_DOWN);
    for (int i = 0; i < 4; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_STATUS_OFF); compare(&pr, tag);
    for (int i = 0; i < 4; i++) { op_apply(&pr, OP_TICK); compare(&pr, tag); }
    op_apply(&pr, OP_STATUS_ON); compare(&pr, tag);                   /* the module now drives the LED to 1 */
    op_apply(&pr, OP_IR_OFF); compare(&pr, tag);
    op_apply(&pr, OP_IR_AUTO); compare(&pr, tag);
    pair_destroy(&pr);
}

/* ---- the same, from a saved settings file (the camera restarts with status=auto) ---- */
static void test_saved_start(void)
{
    pair_t pr;
    world_reset();
    for (int w = 0; w < 2; w++) {
        mkdir(g_conf[w], 0755);
        char p[200];
        snprintf(p, sizeof(p), "%s/leds", g_conf[w]);
        put(p, "status=auto\nir_led=off\nother=keep\n");
    }
    pair_make(&pr, 0);
    CHECK(legacy_leds_load_config(&pr.o) == 1 && leds_load_config(&pr.n) == 1, "saved file read");
    compare(&pr, "saved start");
    op_apply(&pr, OP_APPLY);
    compare(&pr, "saved start apply");
    for (int i = 0; i < 12; i++) { op_apply(&pr, OP_TICK); compare(&pr, "saved start tick"); }
    op_apply(&pr, OP_READY);
    for (int i = 0; i < 4; i++) { op_apply(&pr, OP_TICK); compare(&pr, "saved start ready"); }
    op_apply(&pr, OP_STATUS_OFF);
    compare(&pr, "saved start off");
    pair_destroy(&pr);
}

/* ---- pseudo-random traces ---- */
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void test_random(int traces, int steps)
{
    int total = 0;
    for (int t = 0; t < traces; t++) {
        pair_t pr;
        char tag[96];
        int before = g_fail;
        g_rng = 12345u + (uint32_t)t * 7919u;
        world_reset();
        pair_make(&pr, t % 3 == 0 ? 10 : 0);
        for (int s = 0; s < steps && g_fail == before; s++) {
            /* ticks are the most common step, as in life */
            int op = rnd() % 3 ? OP_TICK : (int)(rnd() % OP_N);
            snprintf(tag, sizeof(tag), "trace %d step %d (%s)", t, s, OPNAME[op]);
            op_apply(&pr, op);
            compare(&pr, tag);
            total++;
        }
        pair_destroy(&pr);
        if (g_fail != before) {
            printf("  trace %d diverged; stopping the random run\n", t);
            return;
        }
    }
    CHECK(total >= traces * steps, "all random steps ran (%d)", total);
}

int main(void)
{
    char tmpl[] = "/tmp/leds_oracle.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_root, sizeof(g_root), "%s", tmpl);
    setvbuf(stdout, NULL, _IONBF, 0);

    test_compute_exhaustive();
    test_boot_story(0);
    test_boot_story(10);
    test_saved_start();
    test_random(300, 300);

    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }
    printf("leds_oracle_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
