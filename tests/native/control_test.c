/* Host test of the control port (8091): the read-only gate, DEBUG, client slots, golden read output.
 * Part 1 tests the pure logic of src/ak_rtsp/ctl_gate.c (no sockets, injected clock).
 * Part 2 links the REAL src/ak_rtsp/control.c with stub modules (built with -DCONTROL_PORT=18091) and talks to it
 * over TCP: loopback clients, the 4-client limit, LOG forwarding that never blocks, `ak_rtsp ctl`. A "remote" client
 * is a connection to this host's own non-loopback address (skipped when the host has none). No camera. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ae.h"
#include "night.h"
#include "ctl_gate.h"
#include "ctl_sys.h"
#include "control.h"
#include "isp_tune.h"
#include "onvif_cfg.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want))) { \
    fprintf(stderr, "FAIL line %d: got [%s] want [%s]\n", __LINE__, (got), (want)); g_failed++; } } while (0)

/* ------------------------------------------------------------------ stub modules (control.c dependencies) */
static ae_tuning_t    g_ae = { 3, 6, 4, 16384, 1 };
static night_tuning_t g_nt;
static int            g_stub_val = 5;

void ae_get_tuning(ae_tuning_t *o) { *o = g_ae; }
int  ae_set_tuning(const ae_tuning_t *i) { g_ae = *i; return 0; }
void night_get_tuning(night_tuning_t *o) { *o = g_nt; }
int  g_night_persist = -1, g_night_err;
int  night_set_tuning(const night_tuning_t *i) { g_nt = *i; return g_night_err; }
void night_set_persist(int on) { g_night_persist = on; }
int  g_imgq_persist = -1;
void imgq_set_persist(int on) { g_imgq_persist = on; }
int  night_is_night(void) { return 0; }
int  isp_set_saturation(int v) { (void)v; return 0; }
int  isp_set_contrast(int v) { (void)v; return 0; }
int  isp_set_brightness(int v) { (void)v; return 0; }
int  isp_set_sharpness(int v) { (void)v; return 0; }
int  night_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  night_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void night_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "night.stub=1"); }
int  audio_out_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  audio_out_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void audio_out_set_persist(int p) { (void)p; }
void audio_out_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "audio.out_stub=2"); }
int  imgq_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  imgq_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void imgq_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "imgq.stub=3"); }
int  talk_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  talk_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void talk_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "talk.stub=4"); }
int  ledsys_ctl_get(const char *k, char *o, size_t c)
{
    if (strcmp(k, "led.stub")) return -1;
    snprintf(o, c, "led.stub=%d", g_stub_val);
    return 0;
}
int  ledsys_ctl_set(const char *k, const char *v, char *m, size_t c)
{
    if (strcmp(k, "led.stub")) return -1;
    g_stub_val = atoi(v);
    snprintf(m, c, "OK led.stub=%d", g_stub_val);
    return 0;
}
void ledsys_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "led.stub=5"); }

/* onvif_cfg stub: the real state machine is tested in onvif_svc_test.c */
static char g_onvif_state[ONVIF_STATE_MAX] = "off";
static int  g_onvif_reloads;
int  onvif_svc_reload(char *s, size_t c) { g_onvif_reloads++; snprintf(g_onvif_state, sizeof(g_onvif_state), "on:8080"); snprintf(s, c, "%s", g_onvif_state); return 0; }
void onvif_svc_state(char *s, size_t c) { snprintf(s, c, "%s", g_onvif_state); }
void onvif_svc_stop(void) {}

/* ctl_sys stub: two metrics, one of them hostile (an SSID with a quote, '=', a space and CR/LF). */
static const char *const k_stub_keys[] = { "sys.stub", "net.ssid" };
static const char *const k_stub_vals[] = { "6", "my \"net\"=x\r\ny" };
int ctl_sys_get(const char *k, char *o, size_t c)
{
    for (int i = 0; i < 2; i++)
        if (!strcmp(k, k_stub_keys[i])) { snprintf(o, c, "%s", k_stub_vals[i]); return 0; }
    return -1;
}
void ctl_sys_list(void (*e)(const char *, const char *, void *), void *u)
{
    for (int i = 0; i < 2; i++) e(k_stub_keys[i], k_stub_vals[i], u);
}

/* ------------------------------------------------------------------ part 1: pure logic */
static void test_pure(void)
{
    struct sockaddr_in a4;
    struct sockaddr_in6 a6;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a4.sin_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a4) == 1);
    inet_pton(AF_INET, "127.5.6.7", &a4.sin_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a4) == 1);
    inet_pton(AF_INET, "192.0.2.20", &a4.sin_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a4) == 0);
    inet_pton(AF_INET, "128.0.0.1", &a4.sin_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a4) == 0);
    inet_pton(AF_INET, "10.127.0.1", &a4.sin_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a4) == 0);
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &a6.sin6_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a6) == 1);
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &a6.sin6_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a6) == 1);
    inet_pton(AF_INET6, "::ffff:192.0.2.2", &a6.sin6_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a6) == 0);
    inet_pton(AF_INET6, "fe80::1", &a6.sin6_addr);
    CHECK(ctl_peer_is_loopback((struct sockaddr *)&a6) == 0);
    struct sockaddr sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_family = AF_UNIX;
    CHECK(ctl_peer_is_loopback(&sa) == 0);
    CHECK(ctl_peer_is_loopback(NULL) == 0);

    CHECK(ctl_write_allowed(0, 0) == 0);
    CHECK(ctl_write_allowed(0, 1) == 1);
    CHECK(ctl_write_allowed(1, 0) == 1);
    CHECK(ctl_write_allowed(1, 1) == 1);

    CHECK(ctl_is_write_cmd("SET") && ctl_is_write_cmd("set") && ctl_is_write_cmd("SAVE") && ctl_is_write_cmd("Reset"));
    CHECK(!ctl_is_write_cmd("GET") && !ctl_is_write_cmd("LIST") && !ctl_is_write_cmd("DEBUG") && !ctl_is_write_cmd(NULL));

    /* DEBUG with an injected clock (ms) */
    ctl_debug_t d;
    char out[128];
    memset(&d, 0, sizeof(d));
    CHECK(ctl_debug_active(&d, 0) == 0);
    int64_t t = 5000000;  /* an arbitrary monotonic origin */
    ctl_debug_command(&d, t, 1, "on", NULL, out, sizeof(out));
    CHECK_STR(out, "OK ctl.debug=1 minutes=60");
    CHECK(ctl_debug_active(&d, t) == 1);
    CHECK(ctl_debug_remaining_s(&d, t) == 3600);
    CHECK(ctl_debug_remaining_s(&d, t + 59 * 60000LL) == 60);
    CHECK(ctl_debug_active(&d, t + 3600000LL - 1) == 1);
    CHECK(ctl_debug_active(&d, t + 3600000LL) == 0);
    CHECK(ctl_debug_remaining_s(&d, t + 3600000LL) == 0);
    ctl_debug_command(&d, t + 3600001LL, 1, "STATUS", NULL, out, sizeof(out));
    CHECK_STR(out, "ctl.debug=0");
    ctl_debug_command(&d, t, 1, "on", "2", out, sizeof(out));
    CHECK_STR(out, "OK ctl.debug=1 minutes=2");
    ctl_debug_command(&d, t + 30000, 1, "status", NULL, out, sizeof(out));
    CHECK_STR(out, "ctl.debug=1 remaining=90");
    ctl_debug_command(&d, t + 30000, 1, "off", NULL, out, sizeof(out));
    CHECK_STR(out, "OK ctl.debug=0");
    CHECK(ctl_debug_active(&d, t + 30000) == 0);
    ctl_debug_command(&d, t, 1, "on", "1440", out, sizeof(out));
    CHECK_STR(out, "OK ctl.debug=1 minutes=1440");
    ctl_debug_command(&d, t, 1, "on", "1", out, sizeof(out));
    CHECK_STR(out, "OK ctl.debug=1 minutes=1");
    ctl_debug_command(&d, t, 1, "off", NULL, out, sizeof(out));
    const char *bad[] = { "0", "1441", "-5", "abc", "", "10x" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ctl_debug_command(&d, t, 1, "on", bad[i], out, sizeof(out));
        CHECK(!strncmp(out, "ERR DEBUG minutes", 17));
        CHECK(ctl_debug_active(&d, t) == 0);
    }
    ctl_debug_command(&d, t, 1, "maybe", NULL, out, sizeof(out));
    CHECK(!strncmp(out, "ERR ", 4));
    ctl_debug_command(&d, t, 1, NULL, NULL, out, sizeof(out));
    CHECK(!strncmp(out, "ERR ", 4));
    /* remote: refused whatever the argument, flag untouched */
    ctl_debug_command(&d, t, 0, "on", NULL, out, sizeof(out));
    CHECK(!strncmp(out, "ERR DEBUG is loopback-only", 26));
    CHECK(ctl_debug_active(&d, t) == 0);
    ctl_debug_command(&d, t, 1, "on", NULL, out, sizeof(out));
    ctl_debug_command(&d, t, 0, "off", NULL, out, sizeof(out));
    CHECK(!strncmp(out, "ERR DEBUG is loopback-only", 26));
    CHECK(ctl_debug_active(&d, t) == 1);
    ctl_debug_command(&d, t, 0, "status", NULL, out, sizeof(out));
    CHECK(!strncmp(out, "ERR ", 4));

    /* slots: loopback may use all of them, a remote peer at most CTL_MAX_REMOTE */
    ctl_slots_t s;
    memset(&s, 0, sizeof(s));
    int idx[CTL_MAX_CLIENTS];
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) { idx[i] = ctl_slots_acquire(&s, 1); CHECK(idx[i] == i); }
    CHECK(ctl_slots_count(&s) == CTL_MAX_CLIENTS);
    CHECK(ctl_slots_acquire(&s, 1) == -1);
    CHECK(ctl_slots_acquire(&s, 0) == -1);
    ctl_slots_release(&s, 2);
    CHECK(ctl_slots_count(&s) == CTL_MAX_CLIENTS - 1);
    CHECK(ctl_slots_acquire(&s, 0) == 2);   /* a remote takes the freed one while it is the only remote */
    ctl_slots_release(&s, -1);
    ctl_slots_release(&s, CTL_MAX_CLIENTS);
    CHECK(ctl_slots_count(&s) == CTL_MAX_CLIENTS);
    memset(&s, 0, sizeof(s));
    for (int i = 0; i < CTL_MAX_REMOTE; i++) CHECK(ctl_slots_acquire(&s, 0) == i);
    CHECK(ctl_slots_acquire(&s, 0) == -1);                 /* a 4th remote is refused ... */
    CHECK(ctl_slots_acquire(&s, 1) == CTL_MAX_REMOTE);     /* ... the last slot is for loopback */
    CHECK(ctl_slots_acquire(&s, 1) == -1);
    ctl_slots_release(&s, 0);
    CHECK(ctl_slots_acquire(&s, 0) == 0);
    memset(&s, 0, sizeof(s));
    CHECK(ctl_slots_acquire(&s, 1) == 0 && ctl_slots_acquire(&s, 1) == 1);   /* loopback does not eat the remote quota */
    for (int i = 0; i < CTL_MAX_CLIENTS - 2; i++) CHECK(ctl_slots_acquire(&s, 0) >= 0);

    /* idle limits: short for remote, long for loopback or while DEBUG is on */
    CHECK(ctl_idle_limit_s(1, 0) == CTL_IDLE_LOCAL_S);
    CHECK(ctl_idle_limit_s(0, 1) == CTL_IDLE_LOCAL_S);
    CHECK(ctl_idle_limit_s(0, 0) == CTL_IDLE_REMOTE_S);
    CHECK(CTL_IDLE_REMOTE_S < CTL_IDLE_LOCAL_S);
}

/* ------------------------------------------------------------------ part 2: the real server */
#define PORT 18091

static int dial(const char *ip)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(PORT);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* Read what the server sends until `idle_ms` of silence or `want` (substring) is seen. */
static int slurp(int fd, char *buf, size_t cap, int idle_ms, const char *want)
{
    size_t n = 0;
    buf[0] = '\0';
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, idle_ms) <= 0) break;
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = '\0';
        if (want && strstr(buf, want)) break;
        if (n >= cap - 1) break;
    }
    return (int)n;
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)(t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* A fake server for ctl_client_run: sends the 3 lines of `script` 150 ms apart, then stays silent. */
static void *slow_server(void *arg)
{
    int ls = *(int *)arg;
    int fd = accept(ls, NULL, NULL);
    if (fd < 0) return NULL;
    char buf[64];
    (void)!read(fd, buf, sizeof(buf));
    (void)!write(fd, "LOG noise\nline1\n", 16);
    usleep(150000);
    (void)!write(fd, "line2\n", 6);
    usleep(150000);
    (void)!write(fd, "line3\n", 6);
    usleep(1500000);
    close(fd);
    return NULL;
}

/* Send one line, return the reply (LOG lines removed). */
static void ask(int fd, const char *line, char *out, size_t cap)
{
    char buf[8192], l[300];
    snprintf(l, sizeof(l), "%s\n", line);
    if (write(fd, l, strlen(l)) < 0) { out[0] = '\0'; return; }
    slurp(fd, buf, sizeof(buf), 150, NULL);
    out[0] = '\0';
    char *save = NULL;
    for (char *t = strtok_r(buf, "\n", &save); t; t = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(t, "LOG ", 4)) continue;
        if (strlen(out) + strlen(t) + 2 >= cap) break;
        strcat(out, t);
        strcat(out, "\n");
    }
}

static void test_server(void)
{
    char out[8192];
    struct ifaddrs *ifa = NULL, *p;
    char remote_ip[64] = "";
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !(p->ifa_flags & 8 /* IFF_LOOPBACK */)) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, remote_ip, sizeof(remote_ip));
                break;
            }
        }
        freeifaddrs(ifa);
    }

    CHECK(control_start() == 0);
    int c = -1;
    for (int i = 0; i < 50 && c < 0; i++) { c = dial("127.0.0.1"); if (c < 0) usleep(100000); }
    CHECK(c >= 0);
    if (c < 0) return;

    /* golden: current read outputs, byte for byte */
    ask(c, "LIST", out, sizeof(out));
    CHECK_STR(out,
        "ae.stable_range=3\nae.hold_range=6\nae.speed=4\nae.exp_max=16384\nae.enabled=1\n"
        "night.mode=auto\nnight.state=day\nnight.trigger_hw_exp=0\nnight.day_hw_exp=0\n"
        "night.confirm_samples=0\nnight.lock_ms=0\n"
        "isp.saturation=0\nisp.contrast=0\nisp.brightness=0\nisp.sharpness=0\n"
        "venc.minqp=28 (read-only)\nvenc.maxqp=43 (read-only)\n"
        "night.stub=1\nled.stub=5\naudio.out_stub=2\nimgq.stub=3\ntalk.stub=4\n"
        "sys.stub=6\nnet.ssid=my \"net\"=x??y\nctl.clients=1\nctl.debug=0\nonvif.state=off\n.\n");
    ask(c, "GET sys.stub", out, sizeof(out));
    CHECK_STR(out, "sys.stub=6\n");
    ask(c, "GET net.ssid", out, sizeof(out));      /* CR/LF in a value never splits the reply */
    CHECK_STR(out, "net.ssid=my \"net\"=x??y\n");
    ask(c, "GET ctl.clients", out, sizeof(out));
    CHECK_STR(out, "ctl.clients=1\n");
    ask(c, "GET ae.speed", out, sizeof(out));
    CHECK_STR(out, "ae.speed=4\n");
    {   /* QUIT: "OK bye", then the server closes the connection */
        int q = dial("127.0.0.1");
        CHECK(q >= 0);
        if (q >= 0) {
            ask(q, "QUIT", out, sizeof(out));
            CHECK_STR(out, "OK bye\n");
            char b;
            CHECK(recv(q, &b, 1, 0) == 0);
            close(q);
        }
    }
    ask(c, "GET night.mode", out, sizeof(out));
    CHECK_STR(out, "night.mode=auto\n");
    ask(c, "GET venc.minqp", out, sizeof(out));
    CHECK_STR(out, "venc.minqp=28 (read-only)\n");
    ask(c, "GET led.stub", out, sizeof(out));
    CHECK_STR(out, "led.stub=5\n");
    ask(c, "GET nope", out, sizeof(out));
    CHECK_STR(out, "ERR unknown param nope\n");
    ask(c, "GET", out, sizeof(out));
    CHECK_STR(out, "ERR GET requires a param name\n");
    ask(c, "SET ae.speed", out, sizeof(out));
    CHECK_STR(out, "ERR SET requires param and value\n");
    ask(c, "FROB", out, sizeof(out));
    CHECK_STR(out, "ERR unknown command FROB\n");

    /* loopback writes */
    ask(c, "SET ae.speed 7", out, sizeof(out));
    CHECK_STR(out, "OK ae.speed=7\n");
    ask(c, "SET led.stub 9", out, sizeof(out));
    CHECK_STR(out, "OK led.stub=9\n");
    /* G16: strict parsing and ranges, a refused value changes nothing */
    {
        static const char *const bad[] = { "SET ae.speed abc", "SET ae.speed 7x", "SET ae.speed -1", "SET ae.speed 101",
            "SET ae.speed 99999999999", "SET ae.stable_range 101", "SET ae.hold_range -3", "SET ae.exp_max 16385",
            "SET ae.enabled 2", "SET ae.enabled yes", "SET night.trigger_hw_exp -1", "SET night.trigger_hw_exp 16385",
            "SET night.day_hw_exp 1e3", "SET night.confirm_samples 0", "SET night.confirm_samples 36001",
            "SET night.lock_ms -5", "SET night.lock_ms 3600001", "SET isp.contrast 1.5", "SET isp.brightness x" };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            ask(c, bad[i], out, sizeof(out));
            CHECK(!strncmp(out, "ERR ", 4) && strstr(out, "range ["));
        }
        ask(c, "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
        ask(c, "SET night.confirm_samples 1", out, sizeof(out));
        CHECK_STR(out, "OK night.confirm_samples=1\n");
        ask(c, "SET night.lock_ms 0", out, sizeof(out));
        CHECK_STR(out, "OK night.lock_ms=0\n");
        ask(c, "SET ae.enabled 1", out, sizeof(out));
        CHECK_STR(out, "OK ae.enabled=1\n");
    }
    ask(c, "SET isp.saturation 99", out, sizeof(out));
    CHECK_STR(out, "ERR isp.saturation range [-50,50]\n");
    /* picture settings are saved after a change (ISP_TUNE_FILE is a temp file here) and removed at all zero */
    ask(c, "SET isp.sharpness 12", out, sizeof(out));
    CHECK_STR(out, "OK isp.sharpness=12\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.sharpness == 12 && t.contrast == 0);
    }
    ask(c, "SET isp.sharpness 0", out, sizeof(out));
    CHECK_STR(out, "OK isp.sharpness=0\n");
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    ask(c, "SET isp.sharpness 99", out, sizeof(out));
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    {
        FILE *f = fopen(ISP_TUNE_FILE, "w");
        CHECK(f != NULL);
        if (f) { fputs("contrast=7\nbrightness=99\nsaturation=x\n", f); fclose(f); }
        control_apply_saved_isp();
        ask(c, "GET isp.contrast", out, sizeof(out));
        CHECK_STR(out, "isp.contrast=7\n");
        ask(c, "GET isp.brightness", out, sizeof(out));
        CHECK_STR(out, "isp.brightness=0\n");
        ask(c, "SET isp.contrast 0", out, sizeof(out));
        CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    }
    /* control_apply_setting is the entry point of the ONVIF Imaging service: same code as SET, persist is explicit */
    {
        char msg[128], val[16];
        isp_tune_t t;
        CHECK(control_apply_setting("isp.contrast", "9", 0, msg, sizeof(msg)) == 0 && !strcmp(msg, "OK isp.contrast=9"));
        CHECK(access(ISP_TUNE_FILE, F_OK) != 0);                      /* persist=0: RAM only */
        CHECK(control_get_setting("isp.contrast", val, sizeof(val)) == 0 && !strcmp(val, "9"));
        ask(c, "GET isp.contrast", out, sizeof(out));
        CHECK_STR(out, "isp.contrast=9\n");                           /* the 8091 GET sees it */
        CHECK(control_persist_picture() == 0 && isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.contrast == 9);
        CHECK(control_apply_setting("isp.contrast", "99", 0, msg, sizeof(msg)) == -1 && !strncmp(msg, "ERR", 3));
        CHECK(control_apply_setting("isp.brightness", "4", 1, msg, sizeof(msg)) == 0);
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.brightness == 4 && t.contrast == 9);   /* persist=1 writes */
        CHECK(control_apply_setting("night.mode", "day", 0, msg, sizeof(msg)) == 0 && g_night_persist == 0);
        CHECK(control_get_setting("night.mode", val, sizeof(val)) == 0 && !strcmp(val, "day"));
        CHECK(control_apply_setting("night.mode", "auto", 1, msg, sizeof(msg)) == 0 && g_night_persist == 1);
        CHECK(control_get_setting("ae.speed", val, sizeof(val)) == -1);
        CHECK(control_apply_setting("isp.contrast", "0", 1, msg, sizeof(msg)) == 0);
        CHECK(control_apply_setting("isp.brightness", "0", 1, msg, sizeof(msg)) == 0);
        CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    }
    /* night.mode / img.wb are saved by their modules: a loopback SET allows it, and a failure is reported */
    ask(c, "SET night.mode day", out, sizeof(out));
    CHECK_STR(out, "OK night.mode=day\n");
    CHECK(g_night_persist == 1 && g_imgq_persist == 1);
    g_night_err = ENOSPC;
    ask(c, "SET night.mode night", out, sizeof(out));
    CHECK(!strncmp(out, "OK night.mode=night (not saved: ", 32));
    g_night_err = 0;
    ask(c, "SET night.mode auto", out, sizeof(out));
    CHECK_STR(out, "OK night.mode=auto\n");
    ask(c, "SET venc.minqp 1", out, sizeof(out));
    CHECK(!strncmp(out, "ERR venc.minqp is read-only in this version", 43));

    ask(c, "GET onvif.state", out, sizeof(out));
    CHECK_STR(out, "onvif.state=off\n");
    ask(c, "ONVIF", out, sizeof(out));
    CHECK(!strncmp(out, "ERR usage", 9));
    ask(c, "ONVIF bogus", out, sizeof(out));
    CHECK(!strncmp(out, "ERR usage", 9) && g_onvif_reloads == 0);
    ask(c, "onvif reload", out, sizeof(out));
    CHECK_STR(out, "OK onvif.state=on:8080\n");
    CHECK(g_onvif_reloads == 1);
    ask(c, "GET onvif.state", out, sizeof(out));
    CHECK_STR(out, "onvif.state=on:8080\n");
    snprintf(g_onvif_state, sizeof(g_onvif_state), "off");
    g_onvif_reloads = 0;

    /* DEBUG from loopback */
    ask(c, "GET ctl.debug", out, sizeof(out));
    CHECK_STR(out, "ctl.debug=0\n");
    ask(c, "GET ctl.debug_remaining", out, sizeof(out));
    CHECK_STR(out, "ctl.debug_remaining=0\n");
    ask(c, "DEBUG status", out, sizeof(out));
    CHECK_STR(out, "ctl.debug=0\n");
    ask(c, "DEBUG on 0", out, sizeof(out));
    CHECK(!strncmp(out, "ERR DEBUG minutes", 17));
    ask(c, "SAVE", out, sizeof(out));       /* a write command that does not exist yet: still "unknown" for loopback */
    CHECK_STR(out, "ERR unknown command SAVE\n");

    /* remote peer */
    if (remote_ip[0]) {
        int r = dial(remote_ip);
        CHECK(r >= 0);
        if (r >= 0) {
            ask(r, "GET ae.speed", out, sizeof(out));
            CHECK_STR(out, "ae.speed=7\n");
            ask(r, "LIST", out, sizeof(out));
            CHECK(strstr(out, "ae.speed=7\n") && strstr(out, "\n.\n"));
            ask(r, "SET ae.speed 9", out, sizeof(out));
            CHECK_STR(out, "ERR read-only mode, see akam_debug.sh\n");
            ask(r, "set ae.speed 9", out, sizeof(out));
            CHECK_STR(out, "ERR read-only mode, see akam_debug.sh\n");
            ask(r, "SAVE", out, sizeof(out));
            CHECK_STR(out, "ERR read-only mode, see akam_debug.sh\n");
            ask(r, "RESET", out, sizeof(out));
            CHECK_STR(out, "ERR read-only mode, see akam_debug.sh\n");
            ask(r, "GET ae.speed", out, sizeof(out));
            CHECK_STR(out, "ae.speed=7\n");
            ask(r, "DEBUG on", out, sizeof(out));
            CHECK(!strncmp(out, "ERR DEBUG is loopback-only", 26));
            ask(r, "ONVIF reload", out, sizeof(out));      /* refused remotely, never reloads */
            CHECK(!strncmp(out, "ERR ", 4) && g_onvif_reloads == 0);
            ask(r, "GET onvif.state", out, sizeof(out));   /* readable by all */
            CHECK_STR(out, "onvif.state=off\n");
            ask(r, "DEBUG status", out, sizeof(out));
            CHECK(!strncmp(out, "ERR ", 4));
            ask(r, "GET ctl.debug", out, sizeof(out));
            CHECK_STR(out, "ctl.debug=0\n");

            /* G05: a remote peer without DEBUG gets no LOG lines and no SSID/MAC; loopback gets both */
            {
                char rb[4096], lb[4096];
                slurp(r, rb, sizeof(rb), 100, NULL);
                slurp(c, lb, sizeof(lb), 100, NULL);
                printf("g05-secret-line\n");
                fflush(stdout);
                slurp(c, lb, sizeof(lb), 1000, "g05-secret-line");
                CHECK(strstr(lb, "LOG g05-secret-line\n") != NULL);
                slurp(r, rb, sizeof(rb), 300, NULL);
                CHECK(strstr(rb, "g05-secret-line") == NULL);
                ask(r, "GET net.ssid", out, sizeof(out));
                CHECK_STR(out, "net.ssid=hidden\n");
                ask(r, "LIST", out, sizeof(out));
                CHECK(strstr(out, "net.ssid=hidden\n") != NULL && strstr(out, "sys.stub=6\n") != NULL);
                ask(c, "GET net.ssid", out, sizeof(out));
                CHECK_STR(out, "net.ssid=my \"net\"=x??y\n");
            }

            /* the flag opens remote writes, and loopback closing it shuts them */
            ask(c, "DEBUG on 5", out, sizeof(out));
            CHECK_STR(out, "OK ctl.debug=1 minutes=5\n");
            ask(r, "GET ctl.debug", out, sizeof(out));
            CHECK_STR(out, "ctl.debug=1\n");
            {   /* DEBUG on: LOG lines and the SSID reach the remote peer */
                char rb[4096];
                slurp(r, rb, sizeof(rb), 100, NULL);
                printf("g05-debug-line\n");
                fflush(stdout);
                slurp(r, rb, sizeof(rb), 1000, "g05-debug-line");
                CHECK(strstr(rb, "LOG g05-debug-line\n") != NULL);
                ask(r, "GET net.ssid", out, sizeof(out));
                CHECK_STR(out, "net.ssid=my \"net\"=x??y\n");
            }
            ask(c, "GET ctl.debug_remaining", out, sizeof(out));
            CHECK(!strcmp(out, "ctl.debug_remaining=300\n") || !strcmp(out, "ctl.debug_remaining=299\n"));
            ask(r, "SET ae.speed 9", out, sizeof(out));
            CHECK_STR(out, "OK ae.speed=9\n");
            ask(c, "GET ae.speed", out, sizeof(out));
            CHECK_STR(out, "ae.speed=9\n");
            /* a picture value set from the network (DEBUG) is applied but never written to flash */
            ask(r, "SET isp.sharpness 5", out, sizeof(out));
            CHECK_STR(out, "OK isp.sharpness=5\n");
            CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
            ask(r, "SET night.mode day", out, sizeof(out));
            CHECK(g_night_persist == 0 && g_imgq_persist == 0);   /* RAM only from the network */
            ask(r, "SET night.mode auto", out, sizeof(out));
            ask(r, "SET isp.sharpness 0", out, sizeof(out));
            ask(c, "SET ae.speed 7", out, sizeof(out));
            ask(r, "DEBUG off", out, sizeof(out));
            CHECK(!strncmp(out, "ERR DEBUG is loopback-only", 26));
            ask(c, "DEBUG off", out, sizeof(out));
            CHECK_STR(out, "OK ctl.debug=0\n");
            ask(r, "SET ae.speed 4", out, sizeof(out));
            CHECK_STR(out, "ERR read-only mode, see akam_debug.sh\n");
            ask(c, "GET ae.speed", out, sizeof(out));
            CHECK_STR(out, "ae.speed=7\n");
            close(r);
        }

        /* an idle remote peer is dropped after CTL_IDLE_REMOTE_S (the test build shortens it);
         * loopback is not, and neither is a remote peer while DEBUG is on */
        int r1 = dial(remote_ip);
        CHECK(r1 >= 0);
        sleep(CTL_IDLE_REMOTE_S + 2);
        {   /* LOG lines may precede the EOF */
            ssize_t rn = 1;
            struct pollfd pp = { r1, POLLIN, 0 };
            while (rn > 0 && poll(&pp, 1, 100) == 1) rn = read(r1, out, sizeof(out));
            CHECK(rn == 0);
        }
        close(r1);
        ask(c, "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
        ask(c, "DEBUG on 5", out, sizeof(out));
        int r2 = dial(remote_ip);
        sleep(CTL_IDLE_REMOTE_S + 2);
        ask(r2, "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
        ask(c, "DEBUG off", out, sizeof(out));
        close(r2);
    } else {
        fprintf(stderr, "skip: no non-loopback IPv4 address on this host, remote gating over TCP not exercised\n");
    }

    /* the multi-call alias against this server */
    {
        char tmpl[] = "/tmp/ctl_test_XXXXXX";
        int tfd = mkstemp(tmpl);
        FILE *f = fdopen(tfd, "w+");
        char *av1[] = { "GET", "ae.speed" };
        CHECK(ctl_client_run(PORT, 2, av1, f) == 0);
        char *av2[] = { "GET", "nope" };
        CHECK(ctl_client_run(PORT, 2, av2, f) == 1);
        char *av3[] = { "DEBUG", "on", "2" };
        CHECK(ctl_client_run(PORT, 3, av3, f) == 0);
        char *av4[] = { "DEBUG", "off" };
        CHECK(ctl_client_run(PORT, 2, av4, f) == 0);
        struct timespec tt;
        clock_gettime(CLOCK_MONOTONIC, &tt);
        char *av5[] = { "GET", "ae.speed" };
        CHECK(ctl_client_run(PORT, 2, av5, f) == 0);
        CHECK(ms_since(&tt) < 700);            /* 250 ms of silence, not 1 s */
        clock_gettime(CLOCK_MONOTONIC, &tt);
        char *av6[] = { "LIST" };
        CHECK(ctl_client_run(PORT, 1, av6, f) == 0);
        CHECK(ms_since(&tt) < 700);            /* ends at the "." line */
        rewind(f);
        char all[512] = "";
        size_t n = fread(all, 1, sizeof(all) - 1, f);
        all[n] = '\0';
        CHECK(!strncmp(all, "ae.speed=7\nERR unknown param nope\nOK ctl.debug=1 minutes=2\nOK ctl.debug=0\nae.speed=7\nae.stable_range=", 99));
        fclose(f);
        unlink(tmpl);
    }

    /* a reply with gaps (150 ms) between lines is delivered whole, LOG lines are dropped, and the
     * alias stops ~250 ms after the last line (well before the old 1 s) */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(PORT + 1);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        CHECK(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(ls, 1) == 0);
        pthread_t th;
        pthread_create(&th, NULL, slow_server, &ls);
        char tmpl[] = "/tmp/ctl_test2_XXXXXX";
        FILE *f = fdopen(mkstemp(tmpl), "w+");
        char *av[] = { "SLOW" };
        struct timespec tt;
        clock_gettime(CLOCK_MONOTONIC, &tt);
        CHECK(ctl_client_run(PORT + 1, 1, av, f) == 0);
        long ms = ms_since(&tt);
        CHECK(ms >= 300 && ms < 900);
        rewind(f);
        char all[128] = "";
        size_t n = fread(all, 1, sizeof(all) - 1, f);
        all[n] = '\0';
        CHECK_STR(all, "line1\nline2\nline3\n");
        fclose(f);
        unlink(tmpl);
        pthread_join(th, NULL);
        close(ls);
    }

    /* slots: 4 clients at once, the 5th is refused, nobody is kicked */
    int cl[CTL_MAX_CLIENTS];
    cl[0] = c;
    for (int i = 1; i < CTL_MAX_CLIENTS; i++) { cl[i] = dial("127.0.0.1"); CHECK(cl[i] >= 0); }
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
        ask(cl[i], "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
    }
    int fifth = dial("127.0.0.1");
    CHECK(fifth >= 0);
    slurp(fifth, out, sizeof(out), 500, "\n");
    CHECK_STR(out, "ERR busy\n");
    CHECK(read(fifth, out, 1) == 0);  /* and closed */
    close(fifth);
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
        ask(cl[i], "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
    }
    close(cl[3]);
    int again = -1;
    for (int i = 0; i < 40; i++) {
        again = dial("127.0.0.1");
        if (again >= 0) {
            ask(again, "GET ae.speed", out, sizeof(out));
            if (!strcmp(out, "ae.speed=7\n")) break;
            close(again);
            again = -1;
        }
        usleep(50000);
    }
    CHECK(again >= 0);

    /* LOG forwarding to every client, and a client that never reads must not block printf */
    slurp(cl[0], out, sizeof(out), 100, NULL);
    slurp(cl[1], out, sizeof(out), 100, NULL);
    printf("hello-log-line\n");
    fflush(stdout);
    char b0[4096], b1[4096];
    slurp(cl[0], b0, sizeof(b0), 1000, "hello-log-line");
    slurp(cl[1], b1, sizeof(b1), 1000, "hello-log-line");
    CHECK(strstr(b0, "LOG hello-log-line\n") != NULL);
    CHECK(strstr(b1, "LOG hello-log-line\n") != NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 30000; i++) printf("flood %05d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i);
    fflush(stdout);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms < 5000);   /* three clients never read: with a blocking forward this would hang */
    /* the server still answers (a reading client drains the backlog of LOG lines first) */
    { char junk[65536]; while (slurp(again, junk, sizeof(junk), 100, NULL) > 0) {} }
    ask(again, "GET ae.speed", out, sizeof(out));
    CHECK_STR(out, "ae.speed=7\n");
    /* G02: lines lost to a full pipe or a stalled console are counted; control_test.sh runs this program a
     * second time with a stdout nobody reads (CTL_TEST_STALLED_STDOUT=1): the flood above must not hang and
     * the counter must show the loss. */
    ask(again, "GET log.dropped", out, sizeof(out));
    CHECK(!strncmp(out, "log.dropped=", 12));
    if (getenv("CTL_TEST_STALLED_STDOUT")) CHECK(atoi(out + 12) > 0);

    for (int i = 0; i < CTL_MAX_CLIENTS - 1; i++) close(cl[i]);
    close(again);

    /* slot reservation: three remote peers fit, a fourth is refused, loopback still gets in */
    if (remote_ip[0]) {
        int rr[CTL_MAX_REMOTE];
        for (int i = 0; i < CTL_MAX_REMOTE; i++) {
            rr[i] = -1;
            for (int t = 0; t < 60 && rr[i] < 0; t++) {   /* the closed loopback clients free their slots asynchronously */
                int fd = dial(remote_ip);
                if (fd >= 0) {
                    ask(fd, "GET ae.speed", out, sizeof(out));
                    if (!strcmp(out, "ae.speed=7\n")) rr[i] = fd; else close(fd);
                }
                if (rr[i] < 0) usleep(50000);
            }
            CHECK(rr[i] >= 0);
        }
        int busy = dial(remote_ip);
        CHECK(busy >= 0);
        slurp(busy, out, sizeof(out), 500, "\n");
        CHECK_STR(out, "ERR busy\n");
        close(busy);
        int lo = dial("127.0.0.1");
        CHECK(lo >= 0);
        ask(lo, "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=7\n");
        ask(lo, "GET ctl.clients", out, sizeof(out));
        CHECK_STR(out, "ctl.clients=4\n");
        close(lo);
        for (int i = 0; i < CTL_MAX_REMOTE; i++) if (rr[i] >= 0) close(rr[i]);
    }
    control_stop();
}

int main(void)
{
    test_pure();
    test_server();
    if (g_failed) { fprintf(stderr, "control_test: %d FAILED\n", g_failed); return 1; }
    fprintf(stderr, "control_test: all passed\n");
    return 0;
}
