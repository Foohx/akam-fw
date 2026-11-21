/* See scene.h. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "night_ctl.h"
#include "scene.h"
#include "store.h"
#include "drift_match.h"
#include "scene_check.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sc_thumb_t g_th;
static unsigned g_seq;
static int64_t g_th_ms = -1;
static volatile int g_min = SC_MIN_DEFAULT;
static char g_path[160] = A2K_PTZ_SCENE;
static char g_dbg_dir[180] = "/tmp/akam_scene_dbg"; /* diagnosis dumps: RAM only, always on (about 10 KB, overwritten, at most 6 files) */
static unsigned g_probe_ms = 0;
static unsigned g_feed_ms = A2K_SCENE_FEED_MS, g_settle_ms = A2K_SCENE_SETTLE_MS, g_settle_frames = A2K_SCENE_SETTLE_FRAMES, g_timeout_ms = A2K_SCENE_TIMEOUT_MS;
static sc_thumb_t g_cand; /* the candidate reference (PTZ thread only) */
static int g_cand_mode = -1;
static int g_cand_repmin; /* the threshold the candidate was screened for repetitive texture at (the verdict is stored in the reference: no screening at the boot) */
static int g_altc_repmin[SC_OFFS];
static unsigned g_boots;  /* matched boots since the last full calibration, as of the reference in force (PTZ thread only) */
static int g_boots_known; /* g_boots was read from the file or set by a match / calibration */
static sc_thumb_t g_altc[SC_OFFS]; /* the candidate views at the offsets (multi-view reference; PTZ thread only) */
static int g_altc_mode[SC_OFFS];
static int g_cand_rep;            /* the candidate home view is a repetitive / ambiguous texture (the multi-view reference is built, the verdict is stored) */
static int g_cand_flat;           /* the candidate is a featureless view: the multi-view reference is being built */
static unsigned g_unstamped_rewrites; /* rewrites of a reference with an unknown stamp in this run (at most one) */
static int g_keep_old;            /* a featureless home view while a featureless reference of less than 24 h exists: only the boots counter may be reset */
static unsigned g_writes;         /* flash writes of the reference files by this process (the store skips identical content: counted from its own counter) */
static int g_stale_boot;          /* this run's boot check refused the stored reference (mismatch / differs / mode / repetitive) and a calibration follows: one rewrite regardless of the 24 h limit */
/* ---- the drift reference (drift_match.h): the full 320x192 luma of the detection channel, RAM only ---- */
static int g_dm_on = 1;            /* ptz_drift_ref: capture + check; 0 = nothing is captured or judged by this module */
static uint8_t g_full[DM_MAX_W * DM_MAX_H]; /* the newest frame (guarded by g_mu) */
static unsigned g_full_w, g_full_h;
static int64_t g_full_ms = -1;
static int (*volatile g_abort)(void); /* the controller's abandon request: polled while waiting for frames */
static int64_t g_dref_ms[2];       /* CLOCK_MONOTONIC ms when each reference was captured / bridged */
static dm_ref_t g_dref[2];         /* one reference per day / night mode (PTZ thread only): the matcher works across modes, the second one is the bridge */
static dm_work_t g_dwk;            /* PTZ thread only */
static uint8_t g_dcur[DM_MAX_W * DM_MAX_H];
static unsigned g_dm_gap_ms = 2000; /* the second frame of the capture confirmation is taken this long after the first */
static unsigned g_dm_caps, g_dm_bridges;
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

/* rewrite_allowed(), or the one rewrite of this run when the boot check just proved the reference stale (*stale tells which; the caller clears g_stale_boot after the write) */
static int rewrite_ok(const sc_ref_t *old, int *stale)
{
    *stale = 0;
    if (rewrite_allowed(old))
        return 1;
    *stale = g_stale_boot;
    return g_stale_boot;
}

/* the verdicts that say the stored reference no longer describes the home view (not: no reference, not home, no frame, cancelled, periodic, store failed) */
static int verdict_stale(int v)
{
    return v == SC_MISMATCH || v == SC_MISMATCH_BLOCKS || v == SC_LIKELY || v == SC_MODE_DIFF || v == SC_REPETITIVE || v == SC_LOW_DETAIL_CUR || v == SC_LOW_DETAIL_REF;
}

void a2k_scene_set_drift_ref(int on) { g_dm_on = on ? 1 : 0; }

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
    if (g_dm_on && w <= DM_MAX_W && h <= DM_MAX_H && !(w & 3) && !(h & 3)) { /* a copy of the luma plane for the drift matcher (about 60 KB, a few tenths of a ms) */
        unsigned r;
        for (r = 0; r < h; r++)
            memcpy(g_full + (size_t)r * w, y + (size_t)r * stride, w);
        g_full_w = w;
        g_full_h = h;
        g_full_ms = now;
    }
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

static int env_store_alt(unsigned i, const sc_ref_t *ref) { return i < SC_OFFS ? write_ref_at(ref, (int)i) : -1; }

static void env_sleep(unsigned ms) { usleep(ms * 1000u); }
static int64_t env_now(void) { return mono_ms(); }
static void env_log(const char *line) { a2k_log("%s", line); }

/* Diagnosis aid: the thumbnail the check used (80x48 raw gray, 3840 bytes) and one text line next to it, in the RAM directory g_dbg_dir (the camera's
 * /tmp: never persistent storage), the same few names overwritten every time: ref.gray / live.gray / cap.gray (+ .txt). Written in place (no rename: a
 * reader may catch a half file, it is a diagnosis aid). */
static void env_dump(const char *name, const sc_thumb_t *t, const char *text)
{
    char path[260], base[40];
    FILE *f;
    size_t n = strlen(name);
    mkdir(g_dbg_dir, 0755);
    snprintf(base, sizeof base, "%.*s", (int)(n > 5 ? n - 5 : n), name); /* "ref.gray" -> "ref" */
    snprintf(path, sizeof path, "%s/%s", g_dbg_dir, name);
    if (t && (f = fopen(path, "wb")) != NULL) {
        fwrite(t->px, 1, SC_N, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/%s.txt", g_dbg_dir, base);
    if (text && (f = fopen(path, "wb")) != NULL) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}


static int (*g_cancelled)(void);

/* 1 match, 3 very likely home (not proven: the motor layer may run the short calibration and ask again), 0 calibrate, 2 the saved view is featureless and stored views at nearby offsets exist: the motor layer moves by an offset and calls hook_probe */
static int hook_boot_check(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void))
{
    sc_env_t e = { env_load, env_store, env_frame, env_mode, env_now, env_sleep, cancelled, env_log, env_load_alt, env_store_alt, env_dump };
    sc_cfg_t c = { g_min, g_settle_ms, g_settle_frames, g_timeout_ms, g_probe_ms };
    int v;
    g_stale_boot = 0;
    v = sc_boot_check(&e, &c, pan, tilt);
    g_stale_boot = verdict_stale(v);
    g_cancelled = cancelled;
    if (v == SC_NEED_PROBE) {
        sc_ref_t r;
        if (env_load(&r) == 0) { /* the claimed position, for the probe */
            *pan = r.pan;
            *tilt = r.tilt;
        }
        return 2;
    }
    if (v != SC_MATCH && v != SC_LIKELY) { /* a LIKELY head keeps the boots counter: a short calibration that the strict check proves goes on counting toward the periodic full one */
        g_boots = 0; /* a calibration follows */
        g_boots_known = 1;
    }
    return v == SC_MATCH ? 1 : v == SC_LIKELY ? 3 : 0;
}

static int hook_probe_offset(unsigned i, int *dpan, int *dtilt) { return sc_probe_offset(i, dpan, dtilt); }

/* the head is at offset i: 1 when the live view matches the stored view of that offset (the claimed position is proven) */
static int hook_probe(unsigned i)
{
    sc_env_t e = { env_load, env_store, env_frame, env_mode, env_now, env_sleep, g_cancelled, env_log, env_load_alt, env_store_alt, env_dump };
    sc_cfg_t c = { g_min, g_settle_ms, g_settle_frames, g_timeout_ms, g_probe_ms };
    uint32_t pan, tilt;
    int v = sc_probe_check(&e, &c, i, &pan, &tilt);
    if (v != SC_MATCH) {
        g_boots = 0;
        g_boots_known = 1;
        g_stale_boot = verdict_stale(v);
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

static int dm_capture(void);

static int hook_capture(void)
{
    unsigned seq;
    (void)dm_capture();
    g_cand_mode = env_mode();
    if (g_cand_mode < 0 || env_frame(&g_cand, &seq) != 0)
        return -1;
    g_cand_flat = 0;
    g_cand_rep = 0;
    g_keep_old = 0;
    g_cand_repmin = g_min;
    {
        int wdx = 0, wdy = 0, wk = 0;
        char line[520];
        int det = sc_has_detail(&g_cand);
        if (det && (g_cand_rep = sc_repetitive_why(&g_cand, g_min, &wdx, &wdy, &wk)) != 0)
            a2k_log("ptz: scene: repetitive screening triggered at displacement %d,%d px (%s)", wdx, wdy, wk == 1 ? "a separate lobe: a period" : "the main lobe reaches beyond 8 px: invariant along an axis");
        sc_diag_text(&g_cand, NULL, g_min, !det ? "capture: featureless (flat)" : g_cand_rep ? "capture: ambiguous (repetitive)" : "capture: usable", 0, line, sizeof line);
        if (det && !g_cand_rep)
            snprintf(line + strlen(line), sizeof line - strlen(line), " | repetitive 0");
        else if (g_cand_rep)
            snprintf(line + strlen(line), sizeof line - strlen(line), " | repetitive 1 at %d,%d kind %d", wdx, wdy, wk);
        env_dump("cap.gray", &g_cand, line);
    }
    if (!sc_has_detail(&g_cand) || g_cand_rep) { /* it could prove nothing alone at the next boot (featureless, or a shift would match) */
        sc_ref_t *o = &g_old;
        int peek;
        if (env_load(o) == 0 && !rewrite_ok(o, &peek)) { /* a mode difference is a changed view like any other: same 24 h rule */
            g_keep_old = 1; /* the reference (and its stored offset views) is recent: kept, no excursion, no write but the boots counter */
            return 0;
        }
        a2k_log("ptz: scene: the view is %s: not a reference on its own, trying the multi-view reference (nearby offsets)", g_cand_rep ? "ambiguous (repetitive / invariant along an axis)" : "too uniform");
        g_cand_flat = 1;
        return 2; /* the controller may store views at offsets; otherwise nothing is saved */
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
    int score = -1000, v, have_old, stale = 0;
    sc_ref_t *old = &g_old;
    if (g_keep_old) {
        g_keep_old = 0;
        have_old = env_load(old) == 0 && old->pan == pan && old->tilt == tilt;
        return have_old ? (fresh ? reset_boots(old) : 1) : 1;
    }
    if (g_cand_mode < 0 || g_cand_flat || env_frame(&cur, &seq) != 0) /* a featureless / ambiguous candidate is only ever written by commit_multi */
        return -1;
    ref.valid = 1;
    ref.mode = g_cand_mode;
    ref.th = g_cand;
    ref.rep = 0; /* hook_capture screened it (a repetitive candidate never gets here) */
    ref.repmin = g_cand_repmin;
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
        if (!rewrite_ok(old, &stale)) {
            a2k_log("ptz: scene: the home view (or its day/night mode) differs from the reference (score %d), which was written less than 24 h ago (or the clock is not set or the stamp unknown and already used): not rewritten", sc2);
            g_cand_mode = -1;
            return fresh ? reset_boots(old) : 1;
        }
        a2k_log("ptz: scene: the home view changed (score %d against the reference): the reference is rewritten%s", sc2, stale ? " (this boot's check refused the old one: the 24 h limit does not apply, once per run)" : " (allowed by the 24 h rule)");
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
    if (stale)
        g_stale_boot = 0;
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
    g_altc_repmin[i] = g_min;
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
    int stale = 0;
    if (!g_cand_flat || g_cand_mode < 0 || !mask)
        return -1;
    if (env_load(&g_old) == 0 && g_old.pan == pan && g_old.tilt == tilt && !rewrite_ok(&g_old, &stale)) /* the same 24 h rule as the single view */
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
            ref.rep = 0;
            ref.repmin = g_altc_repmin[i];
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
    ref.rep = g_cand_rep; /* 1: the home view is ambiguous, the boot never decides on it (it asks for the offset views); 0: featureless, nothing to alias */
    ref.repmin = g_cand_repmin;
    ref.pan = pan;
    ref.tilt = tilt;
    ref.stamp = now_s() >= CLOCK_SANE ? (uint32_t)now_s() : 0;
    if (!fresh && !g_boots_known)
        load_boots();
    ref.boots = fresh ? 0 : g_boots;
    if (write_ref(&ref) != 0)
        return -1;
    if (stale)
        g_stale_boot = 0;
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
    /* any version of the format ("scene" + a digit: 1, 2, a future one): a reference this build cannot read is still dropped. A downgrade is safe in
     * the other direction too: an older build does not recognise "scene2" at all (no reference: full calibration). */
    if (n >= 6 && !memcmp(buf, "scene", 5) && buf[5] >= '0' && buf[5] <= '9') {
        char mark[32];
        load_boots(); /* the count survives the invalidation (a process restart after an adoption must not forget it) */
        snprintf(mark, sizeof mark, "invalid %u\n", g_boots);
        if (store_w(g_path, mark, strlen(mark)) != 0)
            return -1;
        a2k_log("ptz: scene reference invalidated (position no longer trusted)");
    }
    return 0;
}


/* ---- the drift check (scene_check.h sc_drift): the head is idle at home, are the counters still right? RAM only: the reference is only read, nothing is written. ---- */
_Static_assert((int)A2K_DRIFT_OK == (int)SC_DRIFT_OK && (int)A2K_DRIFT_SHIFT == (int)SC_DRIFT_SHIFT && (int)A2K_DRIFT_RECAL == (int)SC_DRIFT_RECAL && (int)A2K_DRIFT_INCONC == (int)SC_DRIFT_INCONC && (int)A2K_DRIFT_SKIP == (int)SC_DRIFT_SKIP,
               "the controller's drift verdicts are the scene module's");

/* the newest thumbnail taken at or after min_ms (CLOCK_MONOTONIC), waiting up to wait_ms for one; 0 and *got_ms, else -1 */
static int frame_after(sc_thumb_t *t, int64_t min_ms, int64_t *got_ms, unsigned wait_ms)
{
    int64_t end = mono_ms() + (int64_t)wait_ms;
    for (;;) {
        int ok = 0;
        pthread_mutex_lock(&g_mu);
        if (g_th_ms >= 0 && g_th_ms >= min_ms && mono_ms() - g_th_ms <= A2K_SCENE_STALE_MS) {
            *t = g_th;
            *got_ms = g_th_ms;
            ok = 1;
        }
        pthread_mutex_unlock(&g_mu);
        if (ok)
            return 0;
        if (mono_ms() >= end || (g_abort && g_abort())) /* a person arrived / a tracker command: abandoned */
            return -1;
        usleep(50000);
    }
}

/* the newest full frame taken at or after min_ms, waiting up to wait_ms for one: 0 and the copy in dst (w x h), else -1 */
static int full_after(uint8_t *dst, unsigned *w, unsigned *h, int64_t min_ms, int64_t *got_ms, unsigned wait_ms)
{
    int64_t end = mono_ms() + (int64_t)wait_ms;
    for (;;) {
        int ok = 0;
        pthread_mutex_lock(&g_mu);
        if (g_full_ms >= 0 && g_full_ms >= min_ms && mono_ms() - g_full_ms <= A2K_SCENE_STALE_MS) {
            memcpy(dst, g_full, (size_t)g_full_w * g_full_h);
            *w = g_full_w;
            *h = g_full_h;
            *got_ms = g_full_ms;
            ok = 1;
        }
        pthread_mutex_unlock(&g_mu);
        if (ok)
            return 0;
        if (mono_ms() >= end || (g_abort && g_abort())) /* a person arrived / a tracker command: abandoned */
            return -1;
        usleep(50000);
    }
}

/* Captures the drift reference of the current day / night mode in RAM (never written to the flash). Called when the controller captures after a calibration or a proven
 * boot: the head is at home, quiet, trusted. Conditions: no IR-off probe, two frames 2 s apart agree at zero displacement on 90 % of the patches (AE settled, nobody
 * walking through), at least 6 patches on 2 quadrants or more. */
static int dm_capture(void)
{
    static dm_ref_t cand;
    static uint8_t f1[DM_MAX_W * DM_MAX_H];
    unsigned w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    int64_t t1 = 0, t2 = 0;
    int mode = env_mode(), n;
    dm_res_t r;
    if (!g_dm_on || mode < 0)
        return -1;
    if (a2k_night_probe_busy()) {
        a2k_log("ptz: drift reference not captured: day/night IR-off probe or switch in progress");
        return -1;
    }
    if (full_after(f1, &w1, &h1, 0, &t1, 4000) != 0 || full_after(g_dcur, &w2, &h2, t1 + (int64_t)g_dm_gap_ms, &t2, 2500 + g_dm_gap_ms) != 0 || w1 != w2 || h1 != h2) {
        a2k_log("ptz: drift reference not captured: no two fresh frames");
        return -1;
    }
    n = dm_ref_build(f1, w1, h1, w1, &cand, &g_dwk);
    if (n < 6 || cand.quads < 2) {
        a2k_log("ptz: drift reference not captured: the home view has too little structure (%d patches on %d quadrants)", n, cand.quads);
        return -1;
    }
    dm_match(&cand, g_dcur, w2, h2, w2, &g_dwk, &r);
    if (!dm_static_ok(&r)) {
        a2k_log("ptz: drift reference not captured: the view moved between two frames (%s, %d,%d, %d permille agree)", r.why ? r.why : "-", r.dx_q, r.dy_q, r.conf);
        return -1;
    }
    g_dref[mode] = cand;
    g_dref_ms[mode] = mono_ms();
    g_dm_caps++;
    a2k_log("ptz: drift reference captured (%s): %d patches on %d quadrants, held in RAM only", mode ? "night" : "day", n, cand.quads);
    return 0;
}

#define DM_OK_PX_FINE 4     /* quarter px: a fine match inside 1 px is "no displacement" */
#define DM_OK_PX_COARSE 12  /* a half resolution match (day against night) inside 3 px is "no displacement" */
#define DM_DARK_LUMA 8      /* a frame darker than this (mean of 0..255) is no evidence of anything: a flat dark view is a skip, not a slip */
#define DM_MAX_PX 30        /* a displacement beyond this many px (about 72 tilt steps) is not corrected, it recalibrates */

/* The drift check on the patch matcher (units: quarter px of the 320 px frame). One reference serves both modes; a cross-mode match corrects only when the full resolution
 * refinement agrees; a second frame at least 0.5 s later must agree to within 1 px before a shift or a recalibration is returned. */
static int hook_drift4(uint32_t pan, uint32_t tilt, int64_t min_frame_ms, int *dxq, int *dyq)
{
    static dm_res_t r1, r2;
    unsigned w = 0, h = 0, w2 = 0, h2 = 0;
    int64_t t1 = 0, t2 = 0;
    int mode = env_mode(), ri, cross, v;
    char line[200];
    (void)pan;
    (void)tilt;
    *dxq = *dyq = 0;
    if (a2k_night_probe_busy()) {
        a2k_log("ptz: drift check skipped: day/night IR-off probe or switch in progress");
        return A2K_DRIFT_SKIP;
    }
    ri = mode >= 0 && g_dref[mode].valid ? mode : mode >= 0 && g_dref[1 - mode].valid ? 1 - mode : -1;
    if (ri < 0) {
        a2k_log("ptz: drift check skipped: no home reference yet (captured after a calibration, when the view is steady)");
        return A2K_DRIFT_SKIP;
    }
    cross = ri != mode;
    if (full_after(g_dcur, &w, &h, min_frame_ms, &t1, 4000) != 0) {
        a2k_log("ptz: drift check skipped: no fresh frame");
        return A2K_DRIFT_SKIP;
    }
    dm_match(&g_dref[ri], g_dcur, w, h, w, &g_dwk, &r1);
    if (r1.verdict != DM_OK) {
        a2k_log("ptz: drift check: %s (%s; %d of %d patches valid)", dm_verdict_name(r1.verdict), r1.why ? r1.why : "-", r1.nvalid, r1.npatch);
        if (r1.verdict == DM_FLAT) { /* the reference exists (it had detail), the live view has none: a head that slipped to the ceiling, or just a dark / covered lens, or a light switched off a moment ago */
            unsigned long sum = 0, i;
            for (i = 0; i < (unsigned long)w * h; i++)
                sum += g_dcur[i];
            if (w > 0 && h > 0 && sum / ((unsigned long)w * h) < DM_DARK_LUMA) {
                a2k_log("ptz: drift check: the live frame is dark (mean luma %lu): nothing can be judged", sum / ((unsigned long)w * h));
                return A2K_DRIFT_SKIP;
            }
            if (full_after(g_dcur, &w2, &h2, t1 + 500, &t2, 3000) != 0) {
                a2k_log("ptz: drift check: no second frame to confirm the flat view: inconclusive");
                return A2K_DRIFT_INCONC;
            }
            dm_match(&g_dref[ri], g_dcur, w2, h2, w2, &g_dwk, &r2);
            if (r2.verdict != DM_FLAT) { /* the first frame was a flash of light, a passing person or an exposure change */
                a2k_log("ptz: drift check: the flat view was not confirmed by a second frame (%s): inconclusive", dm_verdict_name(r2.verdict));
                return A2K_DRIFT_INCONC;
            }
            return A2K_DRIFT_FLAT;
        }
        return r1.verdict == DM_INCONC ? A2K_DRIFT_INCONC : A2K_DRIFT_SKIP;
    }
    v = abs(r1.dx_q) <= (r1.fine ? DM_OK_PX_FINE : DM_OK_PX_COARSE) && abs(r1.dy_q) <= (r1.fine ? DM_OK_PX_FINE : DM_OK_PX_COARSE) ? A2K_DRIFT_OK : A2K_DRIFT_SHIFT;
    if (v == A2K_DRIFT_SHIFT) {
        if (cross && !r1.fine) { /* day against night, edges only: good to about 3 px, not enough to move the counters */
            a2k_log("ptz: drift check: other-mode reference, coarse displacement %.2f,%.2f px: not corrected (inconclusive)", r1.dx_q / 4.0, r1.dy_q / 4.0);
            return A2K_DRIFT_INCONC;
        }
        if (abs(r1.dx_q) > DM_MAX_PX * 4 || abs(r1.dy_q) > DM_MAX_PX * 4)
            v = A2K_DRIFT_RECAL;
        if (full_after(g_dcur, &w2, &h2, t1 + 500, &t2, 3000) != 0) {
            a2k_log("ptz: drift check: no second frame to confirm: inconclusive");
            return A2K_DRIFT_INCONC;
        }
        dm_match(&g_dref[ri], g_dcur, w2, h2, w2, &g_dwk, &r2);
        if (r2.verdict != DM_OK || abs(r2.dx_q - r1.dx_q) > 4 || abs(r2.dy_q - r1.dy_q) > 4) {
            a2k_log("ptz: drift check: the two frames disagree (%.2f,%.2f then %s %.2f,%.2f): inconclusive", r1.dx_q / 4.0, r1.dy_q / 4.0, dm_verdict_name(r2.verdict), r2.dx_q / 4.0, r2.dy_q / 4.0);
            return A2K_DRIFT_INCONC;
        }
        r1.dx_q = (r1.dx_q + r2.dx_q) / 2;
        r1.dy_q = (r1.dy_q + r2.dy_q) / 2;
    }
    if (a2k_night_probe_busy()) {
        a2k_log("ptz: drift check dropped: day/night probe began during the check");
        return A2K_DRIFT_SKIP;
    }
    snprintf(line, sizeof line, "drift: %s, displacement %.2f,%.2f px, %d of %d patches agree (%d permille), score %d, %s resolution, %s reference",
             v == A2K_DRIFT_OK ? "ok" : v == A2K_DRIFT_SHIFT ? "shift" : "recal", r1.dx_q / 4.0, r1.dy_q / 4.0, r1.ninl, r1.nvalid, r1.conf, r1.score, r1.fine ? "full" : "half", cross ? "other-mode" : "same-mode");
    a2k_log("ptz: drift check: %s", line + 7);
    if (v == A2K_DRIFT_OK && cross && r1.fine && !g_dref[mode].valid && abs(r1.dx_q) <= DM_OK_PX_FINE && abs(r1.dy_q) <= DM_OK_PX_FINE) { /* the mode bridge: the view is proven at home, keep it as this mode's reference */
        static dm_ref_t br;
        if (dm_ref_build(g_dcur, w, h, w, &br, &g_dwk) >= 6 && br.quads >= 2) {
            g_dref[mode] = br;
            g_dref_ms[mode] = mono_ms();
            g_dm_bridges++;
            a2k_log("ptz: drift reference of the %s mode taken from the proven home view (bridge)", mode ? "night" : "day");
        }
    }
    *dxq = r1.dx_q;
    *dyq = r1.dy_q;
    return v;
}

void a2k_scene_drift_ref_info(unsigned *captures, unsigned *bridges, int *have_day, int *have_night)
{
    if (captures) *captures = g_dm_caps;
    if (bridges) *bridges = g_dm_bridges;
    if (have_day) *have_day = g_dref[0].valid;
    if (have_night) *have_night = g_dref[1].valid;
}

/* A shift or recal verdict is only returned when a second, later frame says the same (a frame mid-exposure change, a passing person, must not move the counters or start a
 * calibration); a disagreement is inconclusive (the controller retries later). */
static int hook_drift(uint32_t pan, uint32_t tilt, int64_t min_frame_ms, int *dx, int *dy)
{
    static sc_ref_t ref;
    static sc_thumb_t cur;
    static sc_drift_t d1, d2;
    char line[240];
    int64_t t1 = 0, t2 = 0;
    *dx = *dy = 0;
    if (a2k_night_probe_busy()) { /* the IR LED is off or just came back: dark / jumping frames would read as a displaced view (and two of them agree) */
        a2k_log("ptz: drift check skipped: day/night IR-off probe or switch in progress");
        return A2K_DRIFT_SKIP;
    }
    if (env_load(&ref) != 0 || ref.pan != pan || ref.tilt != tilt) {
        a2k_log("ptz: drift check skipped: no usable home reference at %u/%u", (unsigned)pan, (unsigned)tilt);
        return A2K_DRIFT_SKIP;
    }
    if (frame_after(&cur, min_frame_ms, &t1, 4000) != 0) {
        a2k_log("ptz: drift check skipped: no fresh frame");
        return A2K_DRIFT_SKIP;
    }
    sc_drift(&ref, &cur, env_mode(), g_min, &d1);
    if (d1.verdict == SC_DRIFT_SHIFT || d1.verdict == SC_DRIFT_RECAL) {
        if (frame_after(&cur, t1 + 1, &t2, 3000) != 0) {
            d1.verdict = SC_DRIFT_INCONC;
            d1.why = "no second frame to confirm";
        } else {
            sc_drift(&ref, &cur, env_mode(), g_min, &d2);
            if (d2.verdict != d1.verdict || d2.dx != d1.dx || d2.dy != d1.dy) {
                a2k_log("ptz: drift check: the two frames disagree (%s %d,%d then %s %d,%d): inconclusive", sc_drift_name(d1.verdict), d1.dx, d1.dy, sc_drift_name(d2.verdict), d2.dx, d2.dy);
                d1 = d2;
                d1.verdict = SC_DRIFT_INCONC;
                d1.why = "two frames disagree";
            }
        }
    }
    if (d1.verdict != SC_DRIFT_OK && d1.verdict != SC_DRIFT_SKIP && a2k_night_probe_busy()) { /* a probe began while the frames were read */
        d1.verdict = SC_DRIFT_SKIP;
        d1.why = "day/night probe began during the check";
    }
    snprintf(line, sizeof line, "drift: %s (%s), displacement %d,%d px, SAME %d of %d blocks, moved %d, score %d", sc_drift_name(d1.verdict), d1.why ? d1.why : "-", d1.dx, d1.dy, d1.matched, d1.both, d1.moved, d1.score);
    env_dump("drift.gray", &cur, line);
    a2k_log("ptz: drift check: %s", line + 7);
    *dx = d1.dx;
    *dy = d1.dy;
    return d1.verdict;
}

static unsigned hook_writes(void) { return g_writes; }

static int hook_dref_capture(void) { return dm_capture(); }
static void hook_set_abort(int (*fn)(void)) { g_abort = fn; }
static void hook_dref_drop(void)
{
    if (g_dref[0].valid || g_dref[1].valid)
        a2k_log("ptz: drift reference dropped (a calibration ran: the head is at home again, a fresh one is taken)");
    memset(g_dref, 0, sizeof g_dref);
    g_dref_ms[0] = g_dref_ms[1] = 0;
}
static int hook_dref_info(unsigned long *age_s)
{
    int64_t newest = 0, now = mono_ms();
    int m = (g_dref[0].valid ? 1 : 0) | (g_dref[1].valid ? 2 : 0), k;
    for (k = 0; k < 2; k++)
        if (g_dref[k].valid && g_dref_ms[k] > newest)
            newest = g_dref_ms[k];
    if (age_s)
        *age_s = m && newest ? (unsigned long)((now - newest) / 1000) : 0;
    return m;
}


static const a2k_ptz_scene_t g_hooks = { hook_boot_check, hook_capture, hook_commit, hook_drop, hook_probe_offset, hook_probe, hook_offset, hook_alt_capture, hook_alt_confirm, hook_commit_multi, hook_writes, hook_drift, hook_drift4, hook_dref_capture, hook_dref_drop, hook_dref_info, hook_set_abort };
const a2k_ptz_scene_t *a2k_scene_hooks(void) { return &g_hooks; }

void a2k_scene_test_set(const char *path, unsigned feed_ms, unsigned settle_ms, unsigned settle_frames, unsigned timeout_ms)
{
    snprintf(g_path, sizeof g_path, "%s", path ? path : A2K_PTZ_SCENE);
    g_feed_ms = feed_ms;
    g_settle_ms = settle_ms;
    g_settle_frames = settle_frames;
    g_timeout_ms = timeout_ms;
    g_th_ms = -1;
#ifdef A2K_SCENE_TEST /* host tests only: a production build never derives the dump dir from a path (it could land under /config, flash) */
    snprintf(g_dbg_dir, sizeof g_dbg_dir, "%s.dbg", g_path);
#endif
    g_cand_mode = -1;
    g_cand_flat = 0;
    g_probe_ms = settle_ms ? settle_ms : 1; /* tests: the probe settles as fast as the check */
    g_boots = 0;
    g_boots_known = 0;
    g_keep_old = 0;
    g_unstamped_rewrites = 0;
    g_stale_boot = 0;
    g_writes = 0;
    g_clock = -1;
    g_dm_gap_ms = settle_ms ? settle_ms : 1;
    g_dm_on = 0; /* tests that exercise the drift reference switch it on (a2k_scene_set_drift_ref) */
    g_dm_caps = g_dm_bridges = 0;
    g_full_ms = -1;
    memset(g_dref, 0, sizeof g_dref);
    memset(g_dref_ms, 0, sizeof g_dref_ms);
}

void a2k_scene_test_clock(int64_t secs) { g_clock = secs; }
