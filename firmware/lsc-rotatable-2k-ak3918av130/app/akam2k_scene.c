/* See akam2k_scene.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akam2k_log.h"
#include "akam2k_night.h"
#include "akam2k_scene.h"
#include "akam2k_store.h"
#include "scene_check.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sc_thumb_t g_th;
static unsigned g_seq;
static int64_t g_th_ms = -1;
static volatile int g_min = SC_MIN_DEFAULT;
static char g_path[160] = A2K_PTZ_SCENE;
static unsigned g_feed_ms = A2K_SCENE_FEED_MS, g_settle_ms = A2K_SCENE_SETTLE_MS, g_settle_frames = A2K_SCENE_SETTLE_FRAMES, g_timeout_ms = A2K_SCENE_TIMEOUT_MS;
static sc_thumb_t g_cand; /* the candidate reference (PTZ thread only) */
static int g_cand_mode = -1;
static unsigned g_boots;  /* matched boots since the last full calibration, as of the reference in force (PTZ thread only) */
static int g_boots_known; /* g_boots was read from the file or set by a match / calibration */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void a2k_scene_set_min(int permille) { g_min = permille >= SC_MIN_LOWEST && permille <= 1000 ? permille : SC_MIN_DEFAULT; }

void a2k_scene_feed(const uint8_t *y, unsigned w, unsigned h, unsigned stride)
{
    static sc_thumb_t tmp; /* the detection thread only */
    int64_t now = mono_ms();
    if (g_th_ms >= 0 && now - g_th_ms < g_feed_ms)
        return;
    if (sc_downscale_y(y, w, h, stride, &tmp) != 0)
        return;
    pthread_mutex_lock(&g_mu);
    g_th = tmp;
    g_seq++;
    g_th_ms = now;
    pthread_mutex_unlock(&g_mu);
}

static int env_frame(sc_thumb_t *t, unsigned *seq)
{
    int rc = -1;
    pthread_mutex_lock(&g_mu);
    if (g_th_ms >= 0 && mono_ms() - g_th_ms <= A2K_SCENE_STALE_MS) {
        *t = g_th;
        *seq = g_seq;
        rc = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

static int env_mode(void)
{
    a2k_night_info_t i;
    a2k_night_get(&i);
    return i.valid ? (i.night ? 1 : 0) : -1;
}

static int env_load(sc_ref_t *ref)
{
    static char buf[A2K_STORE_MAX + 1];
    int n = a2k_store_read(g_path, buf, sizeof buf);
    return n > 0 && sc_unpack(buf, (size_t)n, ref) == 0 ? 0 : -1;
}

static int write_ref(const sc_ref_t *ref)
{
    static char buf[A2K_STORE_MAX];
    size_t n = sc_pack(ref, buf, sizeof buf);
    return n && a2k_store_write(g_path, buf, n) == 0 ? 0 : -1;
}

/* a match: the matched-boots counter is rewritten (and remembered: the next reference carries it on) */
static int env_store(const sc_ref_t *ref)
{
    g_boots = ref->boots;
    g_boots_known = 1;
    return write_ref(ref);
}

static void env_sleep(unsigned ms) { usleep(ms * 1000u); }
static int64_t env_now(void) { return mono_ms(); }
static void env_log(const char *line) { a2k_log("%s", line); }

static int hook_boot_check(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void))
{
    sc_env_t e = { env_load, env_store, env_frame, env_mode, env_now, env_sleep, cancelled, env_log };
    sc_cfg_t c = { g_min, g_settle_ms, g_settle_frames, g_timeout_ms };
    int v = sc_boot_check(&e, &c, pan, tilt);
    if (v != SC_MATCH) {
        g_boots = 0; /* a full calibration follows */
        g_boots_known = 1;
    }
    return v == SC_MATCH;
}

/* the matched-boots count of the file: a valid reference, or "invalid <boots>" (written by hook_drop); 0 for anything else */
static void load_boots(void)
{
    static char buf[A2K_STORE_MAX + 1];
    sc_ref_t r;
    int n = a2k_store_read(g_path, buf, sizeof buf);
    unsigned b = 0;
    if (n > 0 && sc_unpack(buf, (size_t)n, &r) == 0)
        b = r.boots;
    else if (n > 8 && !strncmp(buf, "invalid ", 8))
        b = (unsigned)strtoul(buf + 8, NULL, 10);
    g_boots = b > SC_FULL_EVERY ? SC_FULL_EVERY : b;
    g_boots_known = 1;
}

static int hook_capture(void)
{
    unsigned seq;
    g_cand_mode = env_mode();
    if (g_cand_mode < 0 || env_frame(&g_cand, &seq) != 0)
        return -1;
    if (!sc_has_detail(&g_cand)) { /* it could prove nothing at the next boot: no flash write for it (the old reference was dropped at the motion) */
        a2k_log("ptz: scene: the view has too little detail to be a reference, none saved");
        g_cand_mode = -1;
        return 1;
    }
    return 0;
}

static int hook_commit(uint32_t pan, uint32_t tilt, int fresh)
{
    static sc_ref_t ref;
    sc_thumb_t cur;
    unsigned seq;
    int score = -1000, v;
    if (g_cand_mode < 0 || env_frame(&cur, &seq) != 0)
        return -1;
    ref.valid = 1;
    ref.mode = g_cand_mode;
    ref.th = g_cand;
    v = sc_decide(&ref, &cur, env_mode(), g_min, &score);
    if (v != SC_MATCH) { /* the scene changed during the window (a person, a lamp, a moved head): the candidate proves nothing */
        a2k_log("ptz: scene reference not saved: the view changed since it was captured (%s, score %d)", sc_verdict_name(v), score);
        return -1;
    }
    ref.pan = pan;
    ref.tilt = tilt;
    if (!fresh && !g_boots_known)
        load_boots(); /* a new process after an adoption: the count lives in the file (valid or invalidated) */
    ref.boots = fresh ? 0 : g_boots;
    if (write_ref(&ref) != 0)
        return -1;
    g_boots = ref.boots;
    g_boots_known = 1;
    g_cand_mode = -1;
    return 0;
}

/* 0 ok (or nothing valid to invalidate), -1 the marker could not be written */
static int hook_drop(void)
{
    static char buf[A2K_STORE_MAX + 1];
    int n = a2k_store_read(g_path, buf, sizeof buf);
    if (n >= 6 && !memcmp(buf, "scene1", 6)) {
        char mark[32];
        load_boots(); /* the count survives the invalidation (a process restart after an adoption must not forget it) */
        snprintf(mark, sizeof mark, "invalid %u\n", g_boots);
        if (a2k_store_write(g_path, mark, strlen(mark)) != 0)
            return -1;
        a2k_log("ptz: scene reference invalidated (position no longer trusted)");
    }
    return 0;
}

static const a2k_ptz_scene_t g_hooks = { hook_boot_check, hook_capture, hook_commit, hook_drop };
const a2k_ptz_scene_t *a2k_scene_hooks(void) { return &g_hooks; }

void a2k_scene_test_set(const char *path, unsigned feed_ms, unsigned settle_ms, unsigned settle_frames, unsigned timeout_ms)
{
    snprintf(g_path, sizeof g_path, "%s", path ? path : A2K_PTZ_SCENE);
    g_feed_ms = feed_ms;
    g_settle_ms = settle_ms;
    g_settle_frames = settle_frames;
    g_timeout_ms = timeout_ms;
    g_th_ms = -1;
    g_cand_mode = -1;
    g_boots = 0;
    g_boots_known = 0;
}
