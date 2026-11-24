/* Host test of the control port (8091): the read-only gate, DEBUG, client slots, golden read output.
 * Part 1 tests the pure logic of src/core/control/ctl_gate.c (no sockets, injected clock).
 * Part 2 links the REAL platforms/anyka-ak3918/control.c with stub modules (built with -DCONTROL_PORT=<free port>) and talks to it
 * over TCP: loopback clients, the 4-client limit, LOG forwarding that never blocks, `akamd ctl`. A "remote" client
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
#include "mqtt_svc.h"

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
/* isp_cfg_lock stubs: the real setters read-modify-write baselines the night thread frees under this lock, so
 * a setter called without it is the H1 race. Each stub counts such a call. */
static pthread_mutex_t g_stub_isp_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_isp_held, g_isp_unlocked_calls;
void isp_cfg_lock(void)   { pthread_mutex_lock(&g_stub_isp_mu); g_isp_held = 1; }
void isp_cfg_unlock(void) { g_isp_held = 0; pthread_mutex_unlock(&g_stub_isp_mu); }
static int stub_isp_set(int v) { (void)v; if (!g_isp_held) g_isp_unlocked_calls++; return 0; }
int  isp_set_saturation(int v) { return stub_isp_set(v); }
int  isp_set_contrast(int v) { return stub_isp_set(v); }
int  isp_set_brightness(int v) { return stub_isp_set(v); }
int  isp_set_sharpness(int v) { return stub_isp_set(v); }
static int g_stub_denoise, g_stub_denoise_rc, g_stub_ready = 1;   /* the running 3D-NR bias; what the setter answers */
int  isp_pipeline_ready(void) { return g_stub_ready; }
int  isp_nr_knob_get(int k) { (void)k; return g_stub_denoise; }
int  isp_set_denoise(int v) { int rc = stub_isp_set(v); if (rc == 0 && g_stub_denoise_rc == 0) g_stub_denoise = v; return rc ? rc : g_stub_denoise_rc; }
/* isp.wdr / isp.nr2: the running values and what the setters answer (0, -1 refused by the ISP, -2 = ISP_NR_REFUSED) */
static int g_stub_wdr = 90, g_stub_wdr_rc, g_stub_nr2, g_stub_nr2_rc;
int  isp_wdr_get(void) { return g_stub_wdr; }
int  isp_wdr_set(int v) { int rc = stub_isp_set(v); if (rc == 0 && g_stub_wdr_rc == 0) g_stub_wdr = v; return rc ? rc : g_stub_wdr_rc; }
static int g_stub_wdr_loads;
void isp_wdr_reapply(void) { g_stub_wdr_loads++; }
int  isp_wdr_dump(char *out, size_t cap) { return snprintf(out, cap, "[isp.wdr] stub line"); }
static int g_stub_ae_rc;
int  ae_get_last_stats(int *lum, uint32_t *e, uint32_t *s, uint32_t *i) { *lum = 47; *e = 512; *s = 512; *i = 256; return g_stub_ae_rc; }
int  isp_get_nr2(void) { return g_stub_nr2; }
int  isp_set_nr2(int v) { int rc = stub_isp_set(v); if (rc == 0 && g_stub_nr2_rc == 0) g_stub_nr2 = v; return rc ? rc : g_stub_nr2_rc; }
int  night_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  night_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void night_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "night.stub=1"); }
int  audio_out_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  audio_out_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void audio_out_set_persist(int p) { (void)p; }
void audio_out_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "audio.out_stub=2"); }
int  imgq_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
char g_imgq_last_val[64];
int  imgq_ctl_set(const char *k, const char *v, char *m, size_t c)
{
    if (strcmp(k, "img.qp")) return -1;
    snprintf(g_imgq_last_val, sizeof(g_imgq_last_val), "%s", v);   /* what the control parser hands to imgq.c */
    snprintf(m, c, "OK img.qp=%s", v);
    return 0;
}
void imgq_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "imgq.stub=3"); }
int  talk_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  talk_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
void talk_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "talk.stub=4"); }
int  video_ctl_get(const char *k, char *o, size_t c) { (void)k; (void)o; (void)c; return -1; }
int  video_ctl_set(const char *k, const char *v, char *m, size_t c) { (void)k; (void)v; (void)m; (void)c; return -1; }
int  video_minqp(void) { return 28; }
int  video_maxqp(void) { return 43; }
void video_ctl_list(void (*e)(void *, const char *), void *a) { e(a, "video.stub=5"); }
void video_ctl_set_persist(int on) { (void)on; }
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
/* mqtt_svc stub: the real service is tested in mqtt_svc_test.c */
static char g_mqtt_state[MQTT_STATE_MAX] = "off";
static int  g_mqtt_reloads, g_mqtt_tests, g_mqtt_fail;
int  mqtt_svc_reload(char *s, size_t c)
{
    g_mqtt_reloads++;
    snprintf(g_mqtt_state, sizeof(g_mqtt_state), "%s", g_mqtt_fail ? "error:thread" : "connecting");
    snprintf(s, c, "%s", g_mqtt_state);
    return g_mqtt_fail ? -1 : 0;
}
void mqtt_svc_state(char *s, size_t c) { snprintf(s, c, "%s", g_mqtt_state); }
void mqtt_svc_stats(char *s, size_t c) { snprintf(s, c, "published=1 received=2 applied=3 rejected=4 connects=5"); }
void mqtt_svc_test_result(char *s, size_t c) { snprintf(s, c, "%s", g_mqtt_tests ? "running" : "idle"); }
int  mqtt_svc_test_start(void) { g_mqtt_tests++; return 0; }
void mqtt_svc_stop(void) {}
const char *firmware_version(void) { return "b1-test"; }
void onvif_hostname_get(char *out) { strcpy(out, "akam-test"); }

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

#ifdef CTL_TEST_REGISTRY
/* The settings registry behind the port (control.h control_set_registry, convergence P7): two fake rows. Built only with
 * -DCTL_TEST_REGISTRY (tests/control_test.sh does): everything above and below is the unchanged contract test. */
static int g_reg_val = 7, g_reg_last_persist = -1;
static int reg_get(const char *k, char *line, size_t cap)
{
    if (strcmp(k, "reg.stub") && strcmp(k, "ae.speed")) return -1; /* "ae.speed" must never reach us: the old code answers first */
    snprintf(line, cap, "%s=%d", k, g_reg_val);
    return 0;
}
static int reg_set(const char *k, const char *v, int persist, char *msg, size_t cap)
{
    g_reg_last_persist = persist;
    if (!strcmp(k, "reg.bad")) { snprintf(msg, cap, "ERR invalid reg.bad value %s (range)", v); return 1; }
    if (strcmp(k, "reg.stub")) return -1;
    g_reg_val = atoi(v);
    snprintf(msg, cap, "OK reg.stub=%d", g_reg_val);
    return 0;
}
static const control_reg_ops_t g_reg_ops = { reg_get, reg_set };
#endif

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
    CHECK(ctl_session_limit_s(1, 0) == 0 && ctl_session_limit_s(0, 1) == 0);
    CHECK(ctl_session_limit_s(0, 0) == CTL_SESSION_REMOTE_S && CTL_SESSION_REMOTE_S >= CTL_IDLE_REMOTE_S);
    CHECK(CTL_IDLE_REMOTE_S < CTL_IDLE_LOCAL_S);
}

/* ------------------------------------------------------------------ part 2: the real server */
#ifndef CONTROL_PORT
#define CONTROL_PORT 18091 /* tests/control_test.sh passes a free port (PORT and PORT + 1 are used) */
#endif
#define PORT CONTROL_PORT

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

#ifdef CTL_TEST_REGISTRY
    control_set_registry(&g_reg_ops);
#endif
    /* the port is closed to the network by default (loopback only); the remote-peer checks below need the saved LAN opt-in */
    if (!getenv("CTL_TEST_NO_LAN")) CHECK(control_lan_set(1) == 0);
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
        "isp.saturation=0\nisp.contrast=0\nisp.brightness=0\nisp.sharpness=-15\nisp.denoise=0\nisp.wdr=90\nisp.nr2=0\n"
        "venc.minqp=28 (read-only)\nvenc.maxqp=43 (read-only)\n"
        "night.stub=1\nled.stub=5\naudio.out_stub=2\nimgq.stub=3\ntalk.stub=4\nvideo.stub=5\n"
        "sys.stub=6\nnet.ssid=my \"net\"=x??y\nctl.clients=1\nctl.debug=0\nfw.version=b1-test\nsys.hostname=akam-test\nonvif.state=off\nmqtt.state=off\n.\n");
    ask(c, "GET sys.stub", out, sizeof(out));
    CHECK_STR(out, "sys.stub=6\n");
    ask(c, "GET net.ssid", out, sizeof(out));      /* CR/LF in a value never splits the reply */
    CHECK_STR(out, "net.ssid=my \"net\"=x??y\n");
    ask(c, "GET ctl.clients", out, sizeof(out));
    CHECK_STR(out, "ctl.clients=1\n");
    ask(c, "GET fw.version", out, sizeof(out));
    CHECK_STR(out, "fw.version=b1-test\n");
    ask(c, "GET sys.hostname", out, sizeof(out));
    CHECK_STR(out, "sys.hostname=akam-test\n");
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
#ifdef CTL_TEST_REGISTRY
    ask(c, "GET reg.stub", out, sizeof(out));        /* a key the old code does not know: the registry answers */
    CHECK_STR(out, "reg.stub=7\n");
    ask(c, "GET ae.speed", out, sizeof(out));        /* a key it knows: still the old code, never the registry */
    CHECK_STR(out, "ae.speed=4\n");
    ask(c, "SET reg.stub 9", out, sizeof(out));
    CHECK_STR(out, "OK reg.stub=9\n");
    CHECK(g_reg_val == 9 && g_reg_last_persist == 1);   /* a loopback write is saved */
    ask(c, "SET reg.bad 1", out, sizeof(out));
    CHECK_STR(out, "ERR invalid reg.bad value 1 (range)\n");
    ask(c, "SET nope 1", out, sizeof(out));          /* the registry does not know it either */
    CHECK_STR(out, "ERR unknown param nope\n");
    ask(c, "LIST", out, sizeof(out));                /* LIST is unchanged: no registry row in it */
    CHECK(!strstr(out, "reg.stub"));
    {
        char v[40];
        int n = control_apply_setting("reg.stub", "11", 0, v, sizeof(v));   /* the entry point of ONVIF Imaging and of the row setters */
        CHECK(n == 0 && g_reg_val == 11 && g_reg_last_persist == 0);
    }
#endif
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
            "SET ae.speed 99999999999", "SET ae.stable_range 101", "SET ae.hold_range -3", "SET ae.exp_max 32768",
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
        ask(c, "SET ae.exp_max 32767", out, sizeof(out));
        CHECK_STR(out, "OK ae.exp_max=32767\n");
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
    /* sharpness 0 (stock) is an explicit choice: saved as a line; the default -15 removes the file */
    ask(c, "SET isp.sharpness 0", out, sizeof(out));
    CHECK_STR(out, "OK isp.sharpness=0\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.sharpness == 0);
    }
    ask(c, "SET isp.sharpness -15", out, sizeof(out));
    CHECK_STR(out, "OK isp.sharpness=-15\n");
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    /* isp.denoise (3D-NR bias 0..4): range, refusal keeps the old value and the file, saved as a line, default removes it */
    ask(c, "SET isp.denoise 9", out, sizeof(out));
    CHECK_STR(out, "ERR isp.denoise range [0,4]\n");
    ask(c, "SET isp.denoise 3", out, sizeof(out));
    CHECK_STR(out, "OK isp.denoise=3\n");
    ask(c, "GET isp.denoise", out, sizeof(out));
    CHECK_STR(out, "isp.denoise=3\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.denoise == 3 && t.sharpness == -15);
    }
    g_stub_denoise_rc = -2;                                                         /* ISP_NR_REFUSED: nothing sent */
    ask(c, "SET isp.denoise 1", out, sizeof(out));
    CHECK(!strncmp(out, "ERR isp.denoise: live table not as decoded", 41));
    g_stub_denoise_rc = 0;
    ask(c, "GET isp.denoise", out, sizeof(out));
    CHECK_STR(out, "isp.denoise=3\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.denoise == 3);              /* the refused value was not saved */
    }
    ask(c, "SET isp.denoise 2", out, sizeof(out));                                  /* the default 2: file removed */
    CHECK_STR(out, "OK isp.denoise=2\n");
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    ask(c, "SET isp.denoise 0", out, sizeof(out));                                  /* 0 = stock is an explicit choice */
    CHECK_STR(out, "OK isp.denoise=0\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.denoise == 0 && t.sharpness == -15);
    }
    ask(c, "SET isp.denoise 2", out, sizeof(out));
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);
    /* isp.wdr (0..100, default 90 = the camera's own curves) and isp.nr2 (0..4, default 0 = the camera's own table): range, refusal keeps the old
     * value and the file, saved as a line, the defaults remove the file, applied at start-up */
    /* WDRDUMP: read-only, the diagnostic line of isp_wdr.c plus the AE product that picks the ISO level */
    ask(c, "WDRDUMP", out, sizeof(out));
    CHECK_STR(out, "[isp.wdr] stub line ae=ok lum=47 exp=512 sgain=512 ispgain=256 iso_prod=1024\n");
    g_stub_ae_rc = -1;
    ask(c, "WDRDUMP", out, sizeof(out));
    CHECK(!strncmp(out, "[isp.wdr] stub line ae=none", 27));
    g_stub_ae_rc = 0;
    ask(c, "SET isp.wdr 101", out, sizeof(out));
    CHECK_STR(out, "ERR isp.wdr range [0,100]\n");
    ask(c, "SET isp.wdr -1", out, sizeof(out));
    CHECK_STR(out, "ERR isp.wdr range [0,100]\n");
    ask(c, "SET isp.nr2 5", out, sizeof(out));
    CHECK_STR(out, "ERR isp.nr2 range [0,4]\n");
    ask(c, "GET isp.wdr", out, sizeof(out));
    CHECK_STR(out, "isp.wdr=90\n");
    ask(c, "GET isp.nr2", out, sizeof(out));
    CHECK_STR(out, "isp.nr2=0\n");
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);                                        /* a refused range saved nothing */
    ask(c, "SET isp.wdr 0", out, sizeof(out));
    CHECK_STR(out, "OK isp.wdr=0\n");
    ask(c, "GET isp.wdr", out, sizeof(out));
    CHECK_STR(out, "isp.wdr=0\n");
    ask(c, "SET isp.nr2 3", out, sizeof(out));
    CHECK_STR(out, "OK isp.nr2=3\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.wdr == 0 && t.nr2 == 3 && t.sharpness == -15 && t.denoise == 2);
    }
    g_stub_wdr_rc = -2;                                                             /* ISP_NR_REFUSED: nothing sent */
    ask(c, "SET isp.wdr 25", out, sizeof(out));
    CHECK(!strncmp(out, "ERR isp.wdr: live table not as decoded", 38));
    g_stub_wdr_rc = -1;
    ask(c, "SET isp.wdr 25", out, sizeof(out));
    CHECK_STR(out, "ERR isp.wdr: the ISP refused the block\n");
    g_stub_wdr_rc = 0;
    g_stub_nr2_rc = -2;
    ask(c, "SET isp.nr2 4", out, sizeof(out));
    CHECK(!strncmp(out, "ERR isp.nr2: live table not as decoded", 38));
    g_stub_nr2_rc = 0;
    ask(c, "GET isp.wdr", out, sizeof(out));
    CHECK_STR(out, "isp.wdr=0\n");
    {
        isp_tune_t t;
        CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.wdr == 0 && t.nr2 == 3);   /* the refused values were not saved */
    }
    ask(c, "SET isp.wdr 90", out, sizeof(out));
    ask(c, "SET isp.nr2 0", out, sizeof(out));
    CHECK_STR(out, "OK isp.nr2=0\n");
    CHECK(access(ISP_TUNE_FILE, F_OK) != 0);                                        /* both at the defaults: no file */
    {   /* start-up: nothing saved = nothing sent; a saved line is applied; a refusal is logged and the wish is kept */
        FILE *f;
        g_stub_wdr = 90; g_stub_nr2 = 0;
        control_apply_saved_isp();
        CHECK(g_stub_wdr == 90 && g_stub_nr2 == 0);
        CHECK(g_stub_wdr_loads == 1);                                                /* the default still leaves the [isp.wdr] ev=load line at the start (a read) */
        if ((f = fopen(ISP_TUNE_FILE, "w"))) { fputs("wdr=12\nnr2=2\n", f); fclose(f); }
        control_apply_saved_isp();
        CHECK(g_stub_wdr_loads == 1);                                                /* a saved value goes through isp_wdr_set, which logs its own line */
        ask(c, "GET isp.wdr", out, sizeof(out));
        CHECK_STR(out, "isp.wdr=12\n");
        ask(c, "GET isp.nr2", out, sizeof(out));
        CHECK_STR(out, "isp.nr2=2\n");
        g_stub_wdr = 90; g_stub_nr2 = 0; g_stub_wdr_rc = -2; g_stub_nr2_rc = -1;
        control_apply_saved_isp();
        ask(c, "GET isp.wdr", out, sizeof(out));
        CHECK_STR(out, "isp.wdr=90\n");                                            /* refused: the camera's own curves keep running */
        ask(c, "SET isp.contrast 5", out, sizeof(out));                              /* saving another value ... */
        { isp_tune_t t; CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.wdr == 12 && t.nr2 == 2 && t.contrast == 5); } /* ... keeps the wish */
        g_stub_wdr_rc = 0; g_stub_nr2_rc = 0;
        ask(c, "SET isp.contrast 0", out, sizeof(out));
        unlink(ISP_TUNE_FILE);
        g_stub_wdr = 90; g_stub_nr2 = 0;
        control_apply_saved_isp();
    }
    /* img.qp takes two values: both reach imgq.c, as "MIN MAX" (also from the "MIN,MAX" token) */
    ask(c, "SET img.qp 28 43", out, sizeof(out));
    CHECK_STR(out, "OK img.qp=28 43\n");
    CHECK(!strcmp(g_imgq_last_val, "28 43"));
    ask(c, "SET img.qp 22,40", out, sizeof(out));
    CHECK_STR(out, "OK img.qp=22,40\n");
    CHECK(!strcmp(g_imgq_last_val, "22,40"));
    ask(c, "SET img.qp 20", out, sizeof(out));
    CHECK_STR(out, "OK img.qp=20\n");
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
        /* H1: every isp.* setter ran under isp_cfg_lock (8091 SET, ONVIF path and the saved-picture apply) */
        CHECK(g_isp_unlocked_calls == 0);
    }
    /* start-up denoise: no saved value = the default 2 applied; a saved 0 = stock, nothing applied; a saved 3 wins;
     * a refusal is not fatal: the stock tables keep running and the wanted value is not lost */
    {
        FILE *f;
        g_stub_denoise = 0;
        unlink(ISP_TUNE_FILE);
        control_apply_saved_isp();
        ask(c, "GET isp.denoise", out, sizeof(out));
        CHECK_STR(out, "isp.denoise=2\n");
        g_stub_denoise = 0;
        if ((f = fopen(ISP_TUNE_FILE, "w"))) { fputs("denoise=0\n", f); fclose(f); }
        control_apply_saved_isp();
        ask(c, "GET isp.denoise", out, sizeof(out));
        CHECK_STR(out, "isp.denoise=0\n");
        if ((f = fopen(ISP_TUNE_FILE, "w"))) { fputs("denoise=3\n", f); fclose(f); }
        control_apply_saved_isp();
        ask(c, "GET isp.denoise", out, sizeof(out));
        CHECK_STR(out, "isp.denoise=3\n");
        g_stub_denoise = 0;
        g_stub_denoise_rc = -2;
        control_apply_saved_isp();
        ask(c, "GET isp.denoise", out, sizeof(out));
        CHECK_STR(out, "isp.denoise=0\n");                                          /* refused: stock keeps running */
        ask(c, "SET isp.contrast 5", out, sizeof(out));                              /* saving another value ... */
        { isp_tune_t t; CHECK(isp_tune_load(ISP_TUNE_FILE, &t) == 1 && t.denoise == 3 && t.contrast == 5); } /* ... keeps the wish */
        g_stub_denoise_rc = 0;
        ask(c, "SET isp.contrast 0", out, sizeof(out));
        ask(c, "SET isp.denoise 2", out, sizeof(out));
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

    /* MQTT: reload and test from loopback only, state readable by all */
    ask(c, "GET mqtt.state", out, sizeof(out));
    CHECK_STR(out, "mqtt.state=off\n");
    ask(c, "MQTT", out, sizeof(out));
    CHECK(!strncmp(out, "ERR usage", 9));
    ask(c, "MQTT bogus", out, sizeof(out));
    CHECK(!strncmp(out, "ERR usage", 9) && g_mqtt_reloads == 0 && g_mqtt_tests == 0);
    ask(c, "mqtt reload", out, sizeof(out));
    CHECK_STR(out, "OK mqtt.state=connecting\n");
    CHECK(g_mqtt_reloads == 1);
    g_mqtt_fail = 1;                                         /* a failed start is an ERR, not a blind OK */
    ask(c, "mqtt reload", out, sizeof(out));
    CHECK_STR(out, "ERR mqtt.state=error:thread\n");
    g_mqtt_fail = 0;
    ask(c, "mqtt reload", out, sizeof(out));
    g_mqtt_reloads = 1;
    ask(c, "GET mqtt.state", out, sizeof(out));
    CHECK_STR(out, "mqtt.state=connecting\n");
    ask(c, "GET mqtt.stats", out, sizeof(out));
    CHECK_STR(out, "mqtt.stats=published=1 received=2 applied=3 rejected=4 connects=5\n");
    ask(c, "GET mqtt.test", out, sizeof(out));
    CHECK_STR(out, "mqtt.test=idle\n");
    ask(c, "MQTT test", out, sizeof(out));
    CHECK_STR(out, "OK mqtt.test=running\n");
    ask(c, "GET mqtt.test", out, sizeof(out));
    CHECK_STR(out, "mqtt.test=running\n");
    CHECK(g_mqtt_tests == 1);
    snprintf(g_mqtt_state, sizeof(g_mqtt_state), "off");
    g_mqtt_reloads = g_mqtt_tests = 0;
    /* control_get_value: the one reader behind the MQTT module */
    {
        char val[32];
        CHECK(control_get_value("night.state", val, sizeof(val)) == 0 && !strcmp(val, "day"));
        CHECK(control_get_value("isp.contrast", val, sizeof(val)) == 0);
        CHECK(control_get_value("night.mode", val, sizeof(val)) == 0 && !strcmp(val, "auto"));
        CHECK(control_get_value("sys.stub", val, sizeof(val)) == 0 && !strcmp(val, "6"));
        char want[16];
        snprintf(want, sizeof(want), "%d", g_stub_val);
        CHECK(control_get_value("led.stub", val, sizeof(val)) == 0 && !strcmp(val, want));        /* "key=value" line, value only */
        CHECK(control_get_value("nope.nothing", val, sizeof(val)) == -1);
    }

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
            ask(r, "MQTT reload", out, sizeof(out));       /* refused remotely, never reloads */
            CHECK(!strncmp(out, "ERR ", 4) && g_mqtt_reloads == 0);
            ask(r, "MQTT test", out, sizeof(out));
            CHECK(!strncmp(out, "ERR ", 4) && g_mqtt_tests == 0);
            ask(r, "GET mqtt.state", out, sizeof(out));   /* readable by all */
            CHECK_STR(out, "mqtt.state=off\n");
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
            ask(r, "SET isp.sharpness -15", out, sizeof(out));
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
    /* Earlier clients (the LOG section above) are closed by the test, but their slots are released later by
     * their own server threads: under load the 4th dial below could still find the table full and get
     * "ERR busy". Wait until only this connection is counted. */
    for (int i = 0; i < 200; i++) {
        ask(c, "GET ctl.clients", out, sizeof(out));
        if (!strcmp(out, "ctl.clients=1\n")) break;
        usleep(25000);
    }
    CHECK_STR(out, "ctl.clients=1\n");
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

/* Default: the text port listens on 127.0.0.1 only. A host address of this machine must be refused, the loopback must work,
 * and the opt-in file is written only on a change and removed again (no stale file, no write for an equal value). */
static void test_closed_by_default(void)
{
    char out[256], remote_ip[64] = "";
    struct ifaddrs *ifa = NULL, *p;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next)
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !(p->ifa_flags & 8 /* IFF_LOOPBACK */)) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, remote_ip, sizeof(remote_ip));
                break;
            }
        freeifaddrs(ifa);
    }
    unlink(CONTROL_LAN_FILE);
    CHECK(control_lan_get() == 0);
    CHECK(control_start() == 0);
    int c = -1;
    for (int i = 0; i < 50 && c < 0; i++) { c = dial("127.0.0.1"); if (c < 0) usleep(100000); }
    CHECK(c >= 0);
    if (c >= 0) {
        ask(c, "GET ae.speed", out, sizeof(out));
        CHECK_STR(out, "ae.speed=4\n");
        close(c);
    }
    if (remote_ip[0]) {
        int r = dial(remote_ip);
        CHECK(r < 0);   /* connection refused: nothing listens on the LAN address */
        if (r >= 0) close(r);
    } else {
        printf("skip: no non-loopback address on this host\n");
    }
    control_stop();
    CHECK(control_lan_set(0) == 0);       /* equal value: nothing to do, no file */
    CHECK(access(CONTROL_LAN_FILE, F_OK) != 0);
    CHECK(control_lan_set(1) == 0 && control_lan_get() == 1 && access(CONTROL_LAN_FILE, F_OK) == 0);
    CHECK(control_lan_set(0) == 0 && control_lan_get() == 0 && access(CONTROL_LAN_FILE, F_OK) != 0);
}

int main(void)
{
    test_pure();
    if (getenv("CTL_TEST_NO_LAN")) test_closed_by_default();
    else test_server();
    if (g_failed) { fprintf(stderr, "control_test: %d FAILED\n", g_failed); return 1; }
    fprintf(stderr, "control_test: all passed\n");
    return 0;
}
