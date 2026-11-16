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
static unsigned g_probe_ms = 0;
static unsigned g_feed_ms = A2K_SCENE_FEED_MS, g_settle_ms = A2K_SCENE_SETTLE_MS, g_settle_frames = A2K_SCENE_SETTLE_FRAMES, g_timeout_ms = A2K_SCENE_TIMEOUT_MS;
static sc_thumb_t g_cand; /* the candidate reference (PTZ thread only) */
static int g_cand_mode = -1;
static unsigned g_boots;  /* matched boots since the last full calibration, as of the reference in force (PTZ thread only) */
static int g_boots_known; /* g_boots was read from the file or set by a match / calibration */
static sc_thumb_t g_altc[SC_OFFS]; /* the candidate views at the offsets (multi-view reference; PTZ thread only) */
static int g_altc_mode[SC_OFFS];
static int g_cand_flat;           /* the candidate is a featureless view: the multi-view reference is being built */
static unsigned g_unstamped_rewrites; /* rewrites of a reference with an unknown stamp in this run (at most one) */
static int g_keep_old;            /* a featureless home view while a featureless reference of less than 24 h exists: only the boots counter may be reset */
static unsigned g_writes;         /* flash writes of the reference files by this process (the store skips identical content: counted from its own counter) */
static int64_t g_clock = -1;      /* tests: a fixed wall clock, seconds */
#define CLOCK_SANE 1700000000LL   /* a wall clock below this (Nov 2023) is not set */

static int64_t now_s(void) { return g_clock >= 0 ? g_clock : (int64_t)time(NULL); }

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* the reference files are only ever written through here: the writes are counted */
static int store_w(const char *path, const char *buf, size_t n)
{
    unsigned before = a2k_store_writes();
    int rc = a2k_store_write(path, buf, n);
    g_writes += a2k_store_writes() - before;
    return rc;
}

/* A changed home view rewrites the reference at most once per A2K_PTZ_SCENE_REWRITE_S: the stamp lives in the file (no extra write). An unknown
 * stamp (a file of an older version, written before the clock was set) allows it; a clock that is not set now forbids it (the limit cannot be checked). */
static int rewrite_allowed(const sc_ref_t *old)
{
    int64_t now = now_s();
    if (!old->valid)
        return 1;
    if (!old->stamp) /* the age is unknown (older file, written before the clock was set): once per run, not at every check */
        return g_unstamped_rewrites == 0;
    if (now < CLOCK_SANE)
        return 0;
    return now < (int64_t)old->stamp || now - old->stamp >= A2K_PTZ_SCENE_REWRITE_S;
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

/* the main reference file (alt < 0) or the stored view of offset alt (path + ".a<alt>") */
static void ref_path(char *out, size_t n, int alt)
{
    if (alt < 0)
        snprintf(out, n, "%s", g_path);
    else
        snprintf(out, n, "%s.a%d", g_path, alt);
}

static int write_ref_at(const sc_ref_t *ref, int alt)
{
    static char buf[A2K_STORE_MAX];
    char path[200];
    size_t n = sc_pack(ref, buf, sizeof buf);
    ref_path(path, sizeof path, alt);
    return n && store_w(path, buf, n) == 0 ? 0 : -1;
}

static int write_ref(const sc_ref_t *ref) { return write_ref_at(ref, -1); }

static int env_load_alt(unsigned i, sc_ref_t *ref)
{
    static char buf[A2K_STORE_MAX + 1];
    char path[200];
    int n;
    ref_path(path, sizeof path, (int)i);
    n = a2k_store_read(path, buf, sizeof buf);
    return n > 0 && sc_unpack(buf, (size_t)n, ref) == 0 ? 0 : -1;
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

static int (*g_cancelled)(void);

/* 1 match, 0 calibrate, 2 the saved view is featureless and stored views at nearby offsets exist: the motor layer moves by an offset and calls hook_probe */
static int hook_boot_check(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void))
{
    sc_env_t e = { env_load, env_store, env_frame, env_mode, env_now, env_sleep, cancelled, env_log, env_load_alt };
    sc_cfg_t c = { g_min, g_settle_ms, g_settle_frames, g_timeout_ms, g_probe_ms };
    int v = sc_boot_check(&e, &c, pan, tilt);
    g_cancelled = cancelled;
    if (v == SC_NEED_PROBE) {
        sc_ref_t r;
        if (env_load(&r) == 0) { /* the claimed position, for the probe */
            *pan = r.pan;
            *tilt = r.tilt;
        }
        return 2;
    }
    if (v != SC_MATCH) {
        g_boots = 0; /* a full calibration follows */
        g_boots_known = 1;
    }
    return v == SC_MATCH;
}

static int hook_probe_offset(unsigned i, int *dpan, int *dtilt) { return sc_probe_offset(i, dpan, dtilt); }

/* the head is at offset i: 1 when the live view matches the stored view of that offset (the claimed position is proven) */
static int hook_probe(unsigned i)
{
    sc_env_t e = { env_load, env_store, env_frame, env_mode, env_now, env_sleep, g_cancelled, env_log, env_load_alt };
    sc_cfg_t c = { g_min, g_settle_ms, g_settle_frames, g_timeout_ms, g_probe_ms };
    uint32_t pan, tilt;
    int v = sc_probe_check(&e, &c, i, &pan, &tilt);
    if (v != SC_MATCH) {
        g_boots = 0;
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

static sc_ref_t g_old; /* the reference on the flash, as read by capture / commit (PTZ thread only) */

static int hook_capture(void)
{
    unsigned seq;
    g_cand_mode = env_mode();
    if (g_cand_mode < 0 || env_frame(&g_cand, &seq) != 0)
        return -1;
    g_cand_flat = 0;
    g_keep_old = 0;
    if (!sc_has_detail(&g_cand)) { /* it could prove nothing alone at the next boot */
        sc_ref_t *o = &g_old;
        if (env_load(o) == 0 && !rewrite_allowed(o)) { /* a mode difference is a changed view like any other: same 24 h rule */
            g_keep_old = 1; /* the reference (and its stored offset views) is recent: kept, no excursion, no write but the boots counter */
            return 0;
        }
        a2k_log("ptz: scene: the view is too uniform to be a reference on its own: trying the multi-view reference (nearby offsets)");
        g_cand_flat = 1;
        return 2; /* the controller may store views at offsets; otherwise nothing is saved */
    }
    if (sc_repetitive(&g_cand, g_min)) { /* blinds, tiles, bars: a shift of a period would match */
        a2k_log("ptz: scene: repetitive texture, no reference");
        g_cand_mode = -1;
        return 1;
    }
    return 0;
}

/* The stored reference gets its matched-boots counter back to 0 (after a calibration: the periodic full calibration restarts), nothing else changes. */
static int reset_boots(sc_ref_t *old)
{
    if (!old->valid || !old->boots)
        return 1;
    old->boots = 0;
    if (write_ref(old) != 0)
        return -1;
    g_boots = 0;
    g_boots_known = 1;
    a2k_log("ptz: scene: home reference kept, the matched-boots counter restarts after the calibration");
    return 0;
}

/* The home reference: written only when needed.
 *  - none or invalid on the flash, or taken at another position (home was changed) or another day/night mode is not "changed view": see below;
 *  - the stored one still matches the candidate: nothing (after a calibration only the boots counter restarts);
 *  - the home view changed: rewritten, at most once per 24 h (the stamp is in the file);
 *  - the other day/night mode (a calibration at night, a reference of the day): kept, the mode is not a durable change.
 * 0 written, 1 nothing written, -1 refused. */
static int hook_commit(uint32_t pan, uint32_t tilt, int fresh)
{
    static sc_ref_t ref;
    sc_thumb_t cur;
    unsigned seq;
    int score = -1000, v, have_old;
    sc_ref_t *old = &g_old;
    if (g_keep_old) {
        g_keep_old = 0;
        have_old = env_load(old) == 0 && old->pan == pan && old->tilt == tilt;
        return have_old ? (fresh ? reset_boots(old) : 1) : 1;
    }
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
    have_old = env_load(old) == 0 && old->valid && old->pan == pan && old->tilt == tilt;
    if (have_old) {
        int sc2 = -1000;
        if (old->mode == g_cand_mode && sc_decide(old, &g_cand, g_cand_mode, g_min, &sc2) == SC_MATCH) { /* the home view is unchanged: nothing to write */
            g_cand_mode = -1;
            return fresh ? reset_boots(old) : 1;
        }
        if (!rewrite_allowed(old)) {
            a2k_log("ptz: scene: the home view (or its day/night mode) differs from the reference (score %d), which was written less than 24 h ago (or the clock is not set or the stamp unknown and already used): not rewritten", sc2);
            g_cand_mode = -1;
            return fresh ? reset_boots(old) : 1;
        }
        a2k_log("ptz: scene: the home view changed (score %d against the reference): the reference is rewritten", sc2);
    }
    ref.pan = pan;
    ref.tilt = tilt;
    ref.stamp = now_s() >= CLOCK_SANE ? (uint32_t)now_s() : 0;
    if (!fresh && !g_boots_known)
        load_boots(); /* a new process after an adoption: the count lives in the file (valid or invalidated) */
    ref.boots = fresh ? 0 : g_boots;
    if (write_ref(&ref) != 0)
        return -1;
    if (have_old && !old->stamp)
        g_unstamped_rewrites++;
    g_boots = ref.boots;
    g_boots_known = 1;
    g_cand_mode = -1;
    a2k_log("ptz: scene reference saved: home view written, position %u/%u", (unsigned)pan, (unsigned)tilt);
    return 0;
}

static int hook_offset(unsigned i, int *dpan, int *dtilt) { return sc_offset(i, dpan, dtilt); }

/* the head is at offset i (quiet): keep the view. 0 kept, 1 too uniform too (nothing to prove there), -1 no frame / mode unknown */
static int hook_alt_capture(unsigned i)
{
    static sc_thumb_t t;
    unsigned seq;
    int mode;
    if (i >= SC_OFFS)
        return -1;
    mode = env_mode();
    if (mode < 0 || env_frame(&t, &seq) != 0)
        return -1;
    if (!sc_has_detail(&t)) { /* the candidate of an earlier attempt, if any, is left alone */
        a2k_log("ptz: scene: the view at offset %u is too uniform as well: not stored", i);
        return 1;
    }
    if (sc_repetitive(&t, g_min)) {
        a2k_log("ptz: scene: repetitive texture at offset %u, not stored", i);
        return 1;
    }
    g_altc[i] = t;
    g_altc_mode[i] = mode;
    return 0;
}

/* some seconds later, still at the offset: the live view must still match the candidate (nobody passed in front, the head stayed). 0 yes, -1 no */
static int hook_alt_confirm(unsigned i)
{
    static sc_ref_t r;
    sc_thumb_t cur;
    unsigned seq;
    int score;
    if (i >= SC_OFFS || env_frame(&cur, &seq) != 0)
        return -1;
    r.valid = 1;
    r.mode = g_altc_mode[i];
    r.th = g_altc[i];
    if (sc_decide(&r, &cur, env_mode(), g_min, &score) != SC_MATCH) {
        a2k_log("ptz: scene: the view at offset %u changed during the confirmation (score %d): not stored", i, score);
        return -1;
    }
    return 0;
}

/* The multi-view reference: the stored views of the offsets in `mask` (written first), then the main reference with the featureless view (written
 * LAST: the stored views are only ever used with a valid main reference of the same position). 0 written, -1 not. */
static int hook_commit_multi(uint32_t pan, uint32_t tilt, int fresh, unsigned mask)
{
    static sc_ref_t ref;
    unsigned i;
    if (!g_cand_flat || g_cand_mode < 0 || !mask)
        return -1;
    if (env_load(&g_old) == 0 && g_old.pan == pan && g_old.tilt == tilt && !rewrite_allowed(&g_old)) /* the same 24 h rule as the single view */
        return -1;
    if (env_load(&g_old) == 0 && !g_old.stamp)
        g_unstamped_rewrites++;
    for (i = 0; i < SC_OFFS; i++) {
        int dp, dt;
        if (sc_offset(i, &dp, &dt))
            continue;
        if ((mask >> i) & 1u) {
            ref.valid = 1;
            ref.mode = g_altc_mode[i];
            ref.pan = (uint32_t)((int)pan + dp);
            ref.tilt = (uint32_t)((int)tilt + dt);
            ref.boots = 0;
            ref.th = g_altc[i];
            if (ref.mode != g_cand_mode || write_ref_at(&ref, (int)i) != 0)
                return -1;
        } else {
            char path[200];
            static char buf[A2K_STORE_MAX + 1];
            ref_path(path, sizeof path, (int)i); /* a stale view of an old reference: invalidated (it is never used without the main anyway) */
            if (a2k_store_read(path, buf, sizeof buf) > 0 && store_w(path, sc_invalid_marker, strlen(sc_invalid_marker)) != 0)
                return -1;
        }
    }
    ref.valid = 1;
    ref.mode = g_cand_mode;
    ref.th = g_cand;
    ref.pan = pan;
    ref.tilt = tilt;
    ref.stamp = now_s() >= CLOCK_SANE ? (uint32_t)now_s() : 0;
    if (!fresh && !g_boots_known)
        load_boots();
    ref.boots = fresh ? 0 : g_boots;
    if (write_ref(&ref) != 0)
        return -1;
    g_boots = ref.boots;
    g_boots_known = 1;
    g_cand_mode = -1;
    g_cand_flat = 0;
    a2k_log("ptz: scene multi-view reference saved: %u stored offset view(s) + the featureless rest view, position %u/%u", (unsigned)__builtin_popcount(mask), (unsigned)pan, (unsigned)tilt);
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
        if (store_w(g_path, mark, strlen(mark)) != 0)
            return -1;
        a2k_log("ptz: scene reference invalidated (position no longer trusted)");
    }
    return 0;
}

static unsigned hook_writes(void) { return g_writes; }

static const a2k_ptz_scene_t g_hooks = { hook_boot_check, hook_capture, hook_commit, hook_drop, hook_probe_offset, hook_probe, hook_offset, hook_alt_capture, hook_alt_confirm, hook_commit_multi, hook_writes };
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
    g_cand_flat = 0;
    g_probe_ms = settle_ms ? settle_ms : 1; /* tests: the probe settles as fast as the check */
    g_boots = 0;
    g_boots_known = 0;
    g_keep_old = 0;
    g_unstamped_rewrites = 0;
    g_writes = 0;
    g_clock = -1;
}

void a2k_scene_test_clock(int64_t secs) { g_clock = secs; }
