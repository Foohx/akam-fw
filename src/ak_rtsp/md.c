#include "md.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "ak_rtsp.h"
#include "cfg_file.h"
#include "md_logic.h"
#include "night.h"

#define MD_CONF_FILE     "/etc/config/akam/md"
#define MD_PERIOD_US     500000
#define MD_SETTLE_TICKS  6 /* samples skipped after start-up and after a day/night switch (3 s) */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int         g_enabled = 1;
static char        g_sensitivity[8] = "medium";
static md_params_t g_params;
static md_state_t  g_state;
static int64_t     g_last_motion_ms = -1;
static int         g_persist = 1;
static int         g_preroll_s = 10;
static int         g_started;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void load(void)
{
    md_preset(g_sensitivity, &g_params);
    FILE *f = fopen(MD_CONF_FILE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "enabled=", 8)) {
            g_enabled = line[8] != (char)48; /* anything but "0" */
        } else if (!strncmp(line, "preroll=", 8)) {
            int v = atoi(line + 8);
            if (v >= 0 && v <= 15) g_preroll_s = v;
        } else if (!strncmp(line, "sensitivity=", 12)) {
            md_params_t p;
            if (md_preset(line + 12, &p) == 0) {
                snprintf(g_sensitivity, sizeof(g_sensitivity), "%s", line + 12);
                g_params = p;
            }
        }
    }
    fclose(f);
}

static int save(void)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "enabled=%d\nsensitivity=%s\npreroll=%d\n", g_enabled, g_sensitivity,
                     g_preroll_s);
    return cfg_write_atomic(MD_CONF_FILE, buf, (size_t)n);
}

static void ensure_loaded(void);

static void *md_thread(void *arg)
{
    (void)arg;
    /* Detection must never cost the video path anything. */
    setpriority(PRIO_PROCESS, 0, 10);

    static uint8_t prev[VI_TD_BUF_COUNT][MD_BLOCKS], cur[MD_BLOCKS];
    int have[VI_TD_BUF_COUNT] = {0};
    int settle = MD_SETTLE_TICKS;
    int was_night = night_is_night();

    for (;;) {
        usleep(MD_PERIOD_US);

        pthread_mutex_lock(&g_lock);
        int enabled = g_enabled;
        md_params_t params = g_params;
        pthread_mutex_unlock(&g_lock);

        int night = night_is_night();
        if (night != was_night) {
            was_night = night;
            settle = MD_SETTLE_TICKS;
        }
        if (!enabled || settle > 0) {
            if (settle > 0) settle--;
            memset(have, 0, sizeof(have));
            pthread_mutex_lock(&g_lock);
            if (g_state.active) { /* do not leave "motion" standing while we are not looking */
                g_state.active = 0;
                g_state.streak = 0;
            }
            g_state.score = 0;
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        /* The driver alternates between its two buffers, so each is compared with its own previous
         * content and the larger change counts. A frame being written while it is read only adds a
         * partial change, which the two-sample trigger absorbs. */
        unsigned score = 0;
        for (int b = 0; b < VI_TD_BUF_COUNT; b++) {
            const uint8_t *luma = g_vi_td_bufs[b];
            if (!luma || luma == (const uint8_t *)-1) continue;
            md_reduce(luma, cur);
            if (have[b]) {
                unsigned s = md_score(prev[b], cur, params.pixel_threshold);
                if (s > score) score = s;
            }
            memcpy(prev[b], cur, sizeof(cur));
            have[b] = 1;
        }

        int64_t t = now_ms();
        pthread_mutex_lock(&g_lock);
        int tr = md_feed(&g_state, &params, score, t);
        if (score >= params.area_x100) g_last_motion_ms = t;
        unsigned events = g_state.events, peak = g_state.peak;
        pthread_mutex_unlock(&g_lock);

        if (tr > 0) printf("[md] motion started (event %u, %u.%02u%%)\n", events, score / 100, score % 100);
        else if (tr < 0) printf("[md] motion ended (peak %u.%02u%%)\n", peak / 100, peak % 100);
    }
    return NULL;
}

void md_start(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_started = 1;
    ensure_loaded();
    printf("[md] motion detection: enabled=%d sensitivity=%s preroll=%d s\n", g_enabled, g_sensitivity,
           g_preroll_s);
    pthread_mutex_unlock(&g_lock);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    if (pthread_create(&th, &attr, md_thread, NULL) != 0) printf("[md] could not start the detection thread\n");
    pthread_attr_destroy(&attr);
}

/* Read at start-up too, before md_start(): loads the saved settings on first use. */
static void ensure_loaded(void)
{
    static int loaded;
    if (!loaded) {
        loaded = 1;
        load();
    }
}

int md_preroll_seconds(void)
{
    pthread_mutex_lock(&g_lock);
    ensure_loaded();
    int s = g_enabled ? g_preroll_s : 0;
    pthread_mutex_unlock(&g_lock);
    return s;
}

int md_preroll_wanted(void)
{
    if (access("/tmp/akam_md_preroll_always", F_OK) == 0) return 1;
    pthread_mutex_lock(&g_lock);
    int w = g_enabled && (g_state.active || (g_last_motion_ms >= 0 && now_ms() - g_last_motion_ms < 15000));
    pthread_mutex_unlock(&g_lock);
    return w;
}

static int format(const char *key, char *out, size_t cap)
{
    if (!strcmp(key, "md.enabled"))          snprintf(out, cap, "md.enabled=%d", g_enabled);
    else if (!strcmp(key, "md.sensitivity")) snprintf(out, cap, "md.sensitivity=%s", g_sensitivity);
    else if (!strcmp(key, "md.state"))       snprintf(out, cap, "md.state=%d", g_state.active);
    else if (!strcmp(key, "md.events"))      snprintf(out, cap, "md.events=%u", g_state.events);
    else if (!strcmp(key, "md.score"))       snprintf(out, cap, "md.score=%u", g_state.score);
    else if (!strcmp(key, "md.peak"))        snprintf(out, cap, "md.peak=%u", g_state.peak);
    else if (!strcmp(key, "md.preroll"))     snprintf(out, cap, "md.preroll=%d", g_preroll_s);
    else if (!strcmp(key, "md.age"))
        snprintf(out, cap, "md.age=%d", g_last_motion_ms < 0 ? -1 : (int)((now_ms() - g_last_motion_ms) / 1000));
    else return -1;
    return 0;
}

int md_ctl_get(const char *key, char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    int rc = format(key, out, cap);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void md_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = {"md.enabled", "md.sensitivity", "md.state", "md.events", "md.score", "md.peak", "md.age",
                                        "md.preroll"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char line[64];
        if (md_ctl_get(keys[i], line, sizeof(line)) == 0) emit(arg, line);
    }
}

void md_set_persist(int on) { g_persist = on; }

int md_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    if (strncmp(key, "md.", 3) != 0) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    if (!strcmp(key, "md.enabled")) {
        if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
            snprintf(msg, cap, "ERR md.enabled must be 0 or 1");
            rc = 1;
        } else {
            g_enabled = val[0] == (char)49;
        }
    } else if (!strcmp(key, "md.preroll")) {
        char *end = NULL;
        long v = strtol(val, &end, 10);
        if (!*val || *end || v < 0 || v > 15) {
            snprintf(msg, cap, "ERR md.preroll range [0,15]");
            rc = 1;
        } else {
            g_preroll_s = (int)v;
        }
    } else if (!strcmp(key, "md.sensitivity")) {
        md_params_t p;
        if (md_preset(val, &p) != 0) {
            snprintf(msg, cap, "ERR md.sensitivity must be low, medium or high");
            rc = 1;
        } else {
            snprintf(g_sensitivity, sizeof(g_sensitivity), "%s", val);
            g_params = p;
        }
    } else {
        snprintf(msg, cap, "ERR %s is read-only", key);
        rc = 1;
    }
    if (rc == 0) {
        const char *note = "";
        if (!g_persist) note = " (not saved: network peer)";
        else if (save() != 0) note = " (not saved: write failed)";
        snprintf(msg, cap, "OK %s=%s%s", key, val, note);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
