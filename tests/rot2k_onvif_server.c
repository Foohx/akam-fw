/* Host harness of the 2K ONVIF stack (firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_onvif*.c over the shared core in src/net): the REAL http_srv, soap, onvif, wsd,
 * cred and rtsp_auth code on loopback, with a FAKE motor. tests/rot2k_onvif_test.py is the client (python stdlib: Digest, SOAP, golden XML, WS-Discovery).
 *
 *   rot2k_onvif_server HTDIGEST_FILE STATE_FILE
 *
 * The htdigest file is the one-line "user:akam:HA1" file of rtsp_auth.h. STATE_FILE holds one word the fake motor reads at every call: ok | parked | calibrating | notready | down.
 * Stdout (flushed): "READY http=<port> wsd=<port>" once up, then one line per motor call ("VEL sx sy dx dy lease", "STOP", "HOME", "GOTO name") and "STATE <onvif state>" after a "state"
 * line on stdin. The fake motor keeps the lease with the real dead-man helper (ptz_lease.c) so that GetStatus reports MOVING until it runs out. It exits when stdin closes. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "onvif_services.h"
#include "video.h"
#include "cred.h"
#include "onvif.h"
#include "ptz_lease.h"

static const char *g_state_file;
static ptzl_t g_dm;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rtsp_auth_db_t g_db;

void a2k_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[log] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void a2k_vid_get(int chn, a2k_vid_t *out)
{
    memset(out, 0, sizeof *out);
    out->v[A2K_VP_FPS] = 15;
    out->v[A2K_VP_GOP] = 30;
    out->v[A2K_VP_KBPS] = chn ? 512 : 2000;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int gate_now(void)
{
    char w[32] = "ok";
    FILE *f = fopen(g_state_file, "r");
    if (f) {
        if (fscanf(f, "%31s", w) != 1)
            strcpy(w, "ok");
        fclose(f);
    }
    if (!strcmp(w, "parked")) return A2K_PTZ_E_PARKED;
    if (!strcmp(w, "calibrating") || !strcmp(w, "notready")) return A2K_PTZ_E_NOTREADY;
    if (!strcmp(w, "down")) return A2K_PTZ_E_REFUSED;
    return 0;
}

static int f_vel(int sx, int sy, int dx, int dy, int lease)
{
    int rc = gate_now();
    if (rc)
        return rc;
    pthread_mutex_lock(&g_mu);
    ptzl_arm(&g_dm, mono_ms(), lease);
    pthread_mutex_unlock(&g_mu);
    say("VEL %d %d %d %d %d", sx, sy, dx, dy, lease);
    return 0;
}

static int f_stop(void)
{
    pthread_mutex_lock(&g_mu);
    ptzl_clear(&g_dm);
    pthread_mutex_unlock(&g_mu);
    say("STOP");
    return 0;
}

static int f_gate(void) { return gate_now(); }

static int f_home(void)
{
    say("HOME");
    return 0;
}

static const struct { const char *name; int pan, tilt; } PRESETS[] = { { "door", 600, 900 }, { "window", 2100, 1300 }, { "desk_2", 1275, 700 } };

static int f_goto(const char *name)
{
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++)
        if (!strcmp(PRESETS[i].name, name)) {
            say("GOTO %s", name);
            return PP_OK;
        }
    return PP_E_NOTFOUND;
}

static int f_preset_at(unsigned i, char name[PP_NAME_MAX + 1], int *pan, int *tilt)
{
    if (i >= sizeof PRESETS / sizeof PRESETS[0])
        return -1;
    snprintf(name, PP_NAME_MAX + 1, "%s", PRESETS[i].name);
    *pan = PRESETS[i].pan;
    *tilt = PRESETS[i].tilt;
    return 0;
}

static void f_info(a2k_ptz_info_t *out)
{
    int moving;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_mu);
    moving = ptzl_live(&g_dm, mono_ms());
    pthread_mutex_unlock(&g_mu);
    out->state = moving ? HAL_PTZ_MOVED : HAL_PTZ_HOME;
    out->known = !moving;
    out->pan = 1275;
    out->tilt = 1300;
}

static const a2k_onvif_ptz_ops_t FAKE = { f_vel, f_stop, f_gate, f_home, f_goto, f_preset_at, f_info };

static long long fake_time(void) { return 1767225600LL; } /* 2026-01-01T00:00:00Z: golden XML does not depend on the day */

int main(int argc, char **argv)
{
    static wsd_test_t wt; /* ephemeral WS-Discovery port; the group is the loopback address (not multicast: never joined), Hello / Bye go to a port nobody listens on */
    a2k_onvif_cfg_t oc;
    char line[64];
    if (argc != 3) {
        fprintf(stderr, "usage: rot2k_onvif_server HTDIGEST_FILE STATE_FILE\n");
        return 2;
    }
    g_state_file = argv[2];
    wt.port = 0;
    wt.notify_port = 41999;
    wt.group = htonl(0x7f000001u);
    rtsp_auth_db_init(&g_db, argv[1]);
    a2k_onvif_set_ptz_ops(&FAKE);
    a2k_onvif_test_set_wsd(&wt);
    a2k_onvif_time_fn = fake_time;
    onvif_ifname = "lo"; /* MAC 00:00:00:00:00:00, the identity of the golden files */
    memset(&oc, 0, sizeof oc);
    oc.enabled = 1;
    oc.port = 0; /* patched below: any free port */
    oc.rtsp_port = 554;
    oc.api_port = 8092;
    oc.auth = &g_db;
    /* pick a free port the way a client would: bind 0 is not allowed by the port rule (>= 1024), so scan a few */
    for (int p = 18080; p < 18200; p++) {
        char st[A2K_ONVIF_STATE_MAX];
        oc.port = p;
        (void)a2k_onvif_start(&oc);
        a2k_onvif_state(st, sizeof st);
        if (!strncmp(st, "on:", 3))
            break;
        if (strcmp(st, "error:port_in_use") != 0)
            break;
    }
    {
        char st[A2K_ONVIF_STATE_MAX];
        a2k_onvif_state(st, sizeof st);
        say("READY http=%d wsd=%d state=%s", http_srv_port(), wsd_port(), st);
    }
    for (;;) { /* commands on stdin: "state" prints the ONVIF state; EOF ends */
        struct pollfd p = { 0, POLLIN, 0 };
        if (poll(&p, 1, 1000) > 0) {
            if (!fgets(line, sizeof line, stdin))
                break;
            if (!strncmp(line, "state", 5)) {
                char st[A2K_ONVIF_STATE_MAX];
                a2k_onvif_state(st, sizeof st);
                say("STATE %s", st);
            }
        }
    }
    a2k_onvif_stop();
    return 0;
}
