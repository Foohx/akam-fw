/* Motion detection wiring, see motion.h. */
#include "motion.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#include "motion_grid.h"
#include "orient.h"
#include "ptz.h"

#define GAP_RESET_MS 2000    /* no frame this long: a stream restart, the reference is stale */
#define ORIENT_MASK_MS 3000  /* an orientation change: the frames of both orientations are in flight (A2K_ORIENT_SETTLE_MS + margin) */
#define MD_RETRY_MS 30000    /* a vendor source found unusable is tried again this often (one frame) */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static mg_t g_mg;
static int g_enabled = 1, g_source = A2K_MOTION_DIFF, g_up, g_md_fell;
static int g_md_frame = MG_FRAME_PICTURE; /* where the vendor grid lives; undecoded, see docs: the sensor readout is flipped before the VPSS, so "picture" until a camera says otherwise */
static void (*g_sink)(const struct tl_mot *m);
static void (*g_notify)(const struct a2k_motion_info *i);
static void (*g_chain)(const uint8_t *y, unsigned w, unsigned h, unsigned stride);
static int (*g_md_rd)(void *buf, unsigned n);
static struct a2k_motion_info g_info;
static int64_t g_fake = -1, g_last_frame, g_last_md_try;
static unsigned g_orient_gen_seen;
static int g_last_pan = -1, g_last_tilt = -1;
static int64_t g_orient_mask_at;

static int64_t now_ms(void)
{
    struct timespec ts;
    if (g_fake >= 0) return g_fake;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void a2k_motion_init(int enabled, int sensitivity, int source)
{
    struct mg_cfg c;
    pthread_mutex_lock(&g_mu);
    mg_defaults(&c);
    c.sensitivity = sensitivity < 1 ? 1 : sensitivity > 10 ? 10 : sensitivity;
    mg_init(&g_mg, &c);
    g_enabled = enabled ? 1 : 0;
    g_source = source == A2K_MOTION_MD ? A2K_MOTION_MD : A2K_MOTION_DIFF;
    g_md_fell = 0;
    g_last_frame = 0;
    g_last_md_try = 0;
    g_last_pan = g_last_tilt = -1;
    g_orient_gen_seen = a2k_orient_gen();
    memset(&g_info, 0, sizeof g_info);
    g_info.enabled = g_enabled;
    g_info.x = g_info.y = -1;
    g_up = 1;
    pthread_mutex_unlock(&g_mu);
}

void a2k_motion_set_md_frame(int picture) { g_md_frame = picture ? MG_FRAME_PICTURE : MG_FRAME_SENSOR; }
void a2k_motion_set_sink(void (*sink)(const struct tl_mot *m)) { g_sink = sink; }
void a2k_motion_set_notify(void (*fn)(const struct a2k_motion_info *i)) { g_notify = fn; }
void a2k_motion_set_chain(void (*cb)(const uint8_t *y, unsigned w, unsigned h, unsigned stride)) { g_chain = cb; }
void a2k_motion_set_md_reader(int (*rd)(void *buf, unsigned n)) { g_md_rd = rd; }

static void publish(const struct mg_state *st, int src, int64_t now)
{
    struct tl_mot mo;
    struct a2k_motion_info ni, old;
    int changed;
    memset(&mo, 0, sizeof mo);
    mo.valid = st->valid;
    mo.masked = st->masked;
    mo.active = st->active;
    mo.cx = st->cx;
    mo.cy = st->cy;
    mo.last_dir = st->last_dir;
    mo.last_active_ms = st->last_active_ms;
    mo.dir_ms = st->dir_ms;
    memset(&ni, 0, sizeof ni);
    ni.enabled = 1;
    ni.valid = st->valid;
    ni.active = st->active;
    ni.dir = st->dir;
    ni.x = st->active ? st->cx / 10 : -1;
    ni.y = st->active ? st->cy / 10 : -1;
    ni.cells = st->cells;
    ni.source = src;
    pthread_mutex_lock(&g_mu);
    old = g_info;
    g_info = ni;
    pthread_mutex_unlock(&g_mu);
    changed = old.active != ni.active || old.dir != ni.dir;
    (void)now;
    if (g_sink) g_sink(&mo);
    if (changed && g_notify) g_notify(&ni);
}

void a2k_motion_frame(const uint8_t *y, unsigned w, unsigned h, unsigned stride)
{
    int64_t now = now_ms();
    int enabled, src, flip, mirror, moved = 0;
    a2k_ptz_info_t pi;
    if (g_chain) g_chain(y, w, h, stride);
    pthread_mutex_lock(&g_mu);
    enabled = g_enabled && g_up;
    src = g_source;
    pthread_mutex_unlock(&g_mu);
    if (!enabled) {
        if (g_sink) { struct tl_mot none; memset(&none, 0, sizeof none); g_sink(&none); }
        return;
    }
    if (g_last_frame && now - g_last_frame > GAP_RESET_MS) mg_reset(&g_mg); /* a stream restart / a stall: the reference is stale */
    g_last_frame = now;
    /* the masks: the head moves (the position counters change, or it is not known / running), an orientation change, a day / night switch (the orientation hold) */
    a2k_ptz_info(&pi);
    if (!pi.known || (g_last_pan >= 0 && ((int)pi.pan != g_last_pan || (int)pi.tilt != g_last_tilt)) || (pi.motion_end_ms && now - pi.motion_end_ms < MG_PTZ_AFTER_MS)) moved = 1;
    g_last_pan = pi.known ? (int)pi.pan : -1;
    g_last_tilt = pi.known ? (int)pi.tilt : -1;
    if (moved) mg_mask(&g_mg, now, MG_PTZ_AFTER_MS);
    if (a2k_orient_gen() != g_orient_gen_seen || g_orient_mask_at) {
        g_orient_gen_seen = a2k_orient_gen();
        g_orient_mask_at = 0;
        mg_mask(&g_mg, now, ORIENT_MASK_MS);
        g_mg.prev_ok = 0; /* the picture is another one: no difference with the old orientation */
        g_mg.base_ok = 0;
    }
    if (a2k_orient_settling()) mg_mask(&g_mg, now, MG_PTZ_AFTER_MS);
    a2k_orient_get(&flip, &mirror);
    if (src == A2K_MOTION_MD && g_md_rd && (mg_md_usable(&g_mg) || now - g_last_md_try >= MD_RETRY_MS)) {
        uint32_t buf[MG_MD_BYTES / 4];
        int rc;
        memset(buf, 0, sizeof buf);
        rc = g_md_rd(buf, sizeof buf);
        if (!mg_md_usable(&g_mg)) g_last_md_try = now;
        mg_feed_md(&g_mg, now, rc, buf, sizeof buf, g_md_frame, flip, mirror);
        g_md_fell = !mg_md_usable(&g_mg);
        if (!g_md_fell) {
            publish(mg_get(&g_mg), A2K_MOTION_MD, now);
            return;
        }
    } else if (src == A2K_MOTION_MD) {
        g_md_fell = 1;
    }
    mg_feed_diff(&g_mg, now, y, w, h, stride);
    publish(mg_get(&g_mg), A2K_MOTION_DIFF, now);
}

void a2k_motion_set_enabled(int on)
{
    int was;
    pthread_mutex_lock(&g_mu);
    g_enabled = on ? 1 : 0;
    g_info.enabled = g_enabled;
    was = g_info.active || g_info.dir;
    if (!g_enabled) { g_info.active = 0; g_info.dir = 0; g_info.valid = 0; g_info.x = g_info.y = -1; }
    pthread_mutex_unlock(&g_mu);
    if (!g_enabled && was && g_notify) { struct a2k_motion_info z = g_info; g_notify(&z); } /* the sensors fall back to OFF / none */
}

int a2k_motion_get_enabled(void) { return g_enabled; }

void a2k_motion_set_sensitivity(int s)
{
    pthread_mutex_lock(&g_mu);
    mg_set_sensitivity(&g_mg, s);
    pthread_mutex_unlock(&g_mu);
}

int a2k_motion_get_sensitivity(void)
{
    int s;
    pthread_mutex_lock(&g_mu);
    s = g_mg.cfg.sensitivity ? g_mg.cfg.sensitivity : 5;
    pthread_mutex_unlock(&g_mu);
    return s;
}

void a2k_motion_info(struct a2k_motion_info *out)
{
    pthread_mutex_lock(&g_mu);
    *out = g_info;
    out->enabled = g_enabled;
    pthread_mutex_unlock(&g_mu);
}

const char *a2k_motion_source_name(void)
{
    if (!g_enabled) return "off";
    if (g_source == A2K_MOTION_MD) return g_md_fell ? "md>diff" : "md";
    return "diff";
}

void a2k_motion_test_clock(int64_t ms) { g_fake = ms; }

void a2k_motion_test_reset(void)
{
    pthread_mutex_lock(&g_mu);
    memset(&g_mg, 0, sizeof g_mg);
    memset(&g_info, 0, sizeof g_info);
    g_up = 0;
    g_enabled = 1;
    g_source = A2K_MOTION_DIFF;
    g_md_fell = 0;
    g_sink = 0;
    g_notify = 0;
    g_chain = 0;
    g_md_rd = 0;
    g_fake = -1;
    g_last_frame = g_last_md_try = 0;
    g_orient_mask_at = 0;
    pthread_mutex_unlock(&g_mu);
}
