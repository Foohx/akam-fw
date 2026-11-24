/* See track.h. */
#include "track.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "store.h"
#include "track_cfgkv.h"

#define PREFIX "track_area_"
#define PREFIX_LEN 11

int a2k_tarea_name_ok(const char *s)
{
    size_t n = strlen(s), i;
    if (n < 1 || n > A2K_TAREA_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}

/* digits (1..5) of v[*i..n) up to a ':' or the end; -1 when none. Advances *i. */
static int num(const char *v, size_t n, size_t *i)
{
    int d = 0, x = 0;
    while (*i < n && v[*i] >= '0' && v[*i] <= '9') {
        if (++d > 5)
            return -1;
        x = x * 10 + (v[*i] - '0');
        (*i)++;
    }
    return d ? x : -1;
}

static int parse_value(const char *v, size_t n, struct a2k_tarea *a)
{
    size_t i = 0;
    int e = 1;
    a->complete = 0;
    if (n >= 4 && !memcmp(v, "none", 4)) {
        i = 4;
    } else {
        int r[4];
        for (int k = 0; k < 4; k++) {
            r[k] = num(v, n, &i);
            if (r[k] < 0)
                return -1;
            if (k < 3) {
                if (i >= n || v[i] != ':')
                    return -1;
                i++;
            }
        }
        if (r[0] >= r[1] || r[2] >= r[3])
            return -1;
        a->complete = 1;
        a->pmin = r[0];
        a->pmax = r[1];
        a->tmin = r[2];
        a->tmax = r[3];
    }
    if (i < n) {
        if (n - i != 2 || v[i] != ':' || (v[i + 1] != '0' && v[i + 1] != '1'))
            return -1;
        e = v[i + 1] - '0';
    }
    a->enabled = e;
    return 0;
}

int a2k_tareas_parse(const char *text, size_t len, a2k_tareas_t *out)
{
    a2k_tareas_t t;
    size_t i = 0;
    memset(&t, 0, sizeof t);
    if (!text || !out || len > A2K_STORE_MAX)
        return -1;
    while (i < len) {
        size_t e = i, n, kl;
        const char *l, *eq;
        struct a2k_tarea *a;
        char name[A2K_TAREA_NAME_MAX + 2];
        while (e < len && text[e] != '\n')
            e++;
        n = e - i;
        if (n && text[i + n - 1] == '\r')
            n--;
        l = text + i;
        i = e + 1;
        if (n == 0)
            continue;
        eq = memchr(l, '=', n);
        if (!eq || n < PREFIX_LEN || memcmp(l, PREFIX, PREFIX_LEN) || t.n >= A2K_TAREA_MAX)
            return -1;
        kl = (size_t)(eq - l) - PREFIX_LEN;
        if (kl < 1 || kl > A2K_TAREA_NAME_MAX)
            return -1;
        memcpy(name, l + PREFIX_LEN, kl);
        name[kl] = '\0';
        if (!a2k_tarea_name_ok(name))
            return -1;
        for (int k = 0; k < t.n; k++)
            if (!strcmp(t.a[k].name, name))
                return -1;
        a = &t.a[t.n];
        memset(a, 0, sizeof *a);
        if (parse_value(eq + 1, n - kl - PREFIX_LEN - 1, a) != 0)
            return -1;
        memcpy(a->name, name, kl + 1);
        t.n++;
    }
    *out = t;
    return 0;
}

int a2k_tareas_format(const a2k_tareas_t *t, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap)
        return -1;
    out[0] = '\0';
    for (int i = 0; i < t->n; i++) {
        const struct a2k_tarea *a = &t->a[i];
        int w;
        if (a->complete)
            w = snprintf(out + n, cap - n, PREFIX "%s=%d:%d:%d:%d:%d\n", a->name, a->pmin, a->pmax, a->tmin, a->tmax, a->enabled);
        else
            w = snprintf(out + n, cap - n, PREFIX "%s=none:%d\n", a->name, a->enabled);
        if (w < 0 || (size_t)w >= cap - n)
            return -1;
        n += (size_t)w;
    }
    return (int)n;
}

static int inside(const struct a2k_tarea *a, int pan, int tilt)
{
    return a->complete && a->enabled && pan >= a->pmin && pan <= a->pmax && tilt >= a->tmin && tilt <= a->tmax;
}

static int any_complete(const a2k_tareas_t *t)
{
    for (int i = 0; i < t->n; i++)
        if (t->a[i].complete)
            return 1;
    return 0;
}

int a2k_tareas_allowed(const a2k_tareas_t *t, int pan, int tilt)
{
    if (!any_complete(t))
        return 1;
    for (int i = 0; i < t->n; i++)
        if (inside(&t->a[i], pan, tilt))
            return 1;
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int iabs(int v) { return v < 0 ? -v : v; }

int a2k_tareas_clip(const a2k_tareas_t *t, int pan, int tilt, int dx, int dy, int *cx, int *cy)
{
    int found = 0, bx = 0, by = 0;
    if (!any_complete(t)) {
        *cx = dx;
        *cy = dy;
        return 1;
    }
    for (int i = 0; i < t->n; i++) {
        const struct a2k_tarea *a = &t->a[i];
        int x, y;
        if (!inside(a, pan, tilt))
            continue;
        x = clampi(pan + dx, a->pmin, a->pmax) - pan;
        y = clampi(tilt + dy, a->tmin, a->tmax) - tilt;
        if (!found || iabs(x) + iabs(y) > iabs(bx) + iabs(by)) {
            bx = x;
            by = y;
        }
        found = 1;
    }
    *cx = bx;
    *cy = by;
    return found;
}

int a2k_tareas_fit(const a2k_tareas_t *t, const int r[4])
{
    for (int i = 0; i < t->n; i++)
        if (t->a[i].complete &&
            (t->a[i].pmin < r[0] || t->a[i].pmax > r[1] || t->a[i].tmin < r[2] || t->a[i].tmax > r[3]))
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------ the wiring core */

void a2k_track_init(a2k_track_t *t, const a2k_track_ops_t *ops, const char *store_path)
{
    memset(t, 0, sizeof *t);
    t->ops = *ops;
    pthread_mutex_init(&t->mu, NULL);
    tl_cfg_defaults(&t->cfg);
    tl_cfg_geometry_2k(&t->cfg);
    (void)tl_init(&t->tl, &t->cfg);
    if (store_path && strlen(store_path) < sizeof t->path)
        strcpy(t->path, store_path);
}

void a2k_track_destroy(a2k_track_t *t) { pthread_mutex_destroy(&t->mu); }

static void persist(a2k_track_t *t)
{
    char buf[A2K_STORE_MAX + 1];
    int n = a2k_tareas_format(&t->areas, buf, sizeof buf);
    if (t->path[0] && (n < 0 || a2k_store_write(t->path, buf, (size_t)n) != 0))
        a2k_log("track: cannot write %s (the areas hold until the next restart)", t->path);
}

int a2k_track_load(a2k_track_t *t, const char *conf)
{
    char buf[A2K_STORE_MAX + 1];
    a2k_tareas_t a;
    int n, r[4], have = 0;
    memset(&a, 0, sizeof a);
    if (conf && a2k_tareas_parse(conf, strlen(conf), &a) != 0) {
        a2k_log("track: the track_area keys of akam.conf are invalid, ignored");
        memset(&a, 0, sizeof a);
    }
    if (t->path[0] && (n = a2k_store_read(t->path, buf, sizeof buf)) >= 0) {
        a2k_tareas_t s;
        if (a2k_tareas_parse(buf, (size_t)n, &s) == 0) {
            a = s;
            have = 1;
        } else {
            a2k_log("track: %s is invalid, ignored", t->path);
        }
    }
    if (t->ops.range && t->ops.range(r) == 0) {
        for (int i = 0; i < a.n;) {
            a2k_tareas_t one = a;
            one.n = 1;
            one.a[0] = a.a[i];
            if (a2k_tareas_fit(&one, r)) {
                i++;
                continue;
            }
            a2k_log("track: area %s is outside the soft range, dropped", a.a[i].name);
            memmove(&a.a[i], &a.a[i + 1], sizeof a.a[0] * (size_t)(a.n - i - 1));
            a.n--;
        }
    }
    pthread_mutex_lock(&t->mu);
    t->areas = a;
    pthread_mutex_unlock(&t->mu);
    a2k_log("track: %d area(s) from %s", a.n, have ? t->path : "akam.conf");
    return a.n;
}

void a2k_track_set_enabled(a2k_track_t *t, int on)
{
    pthread_mutex_lock(&t->mu);
    t->enabled = on ? 1 : 0;
    if (!t->enabled) {
        if (t->active && t->ops.vel)
            (void)t->ops.vel(0, 0, 0, 0); /* a continuous move must not outlive the switch */
        if (t->tl.lost_route_on && t->ops.route_stop)
            (void)t->ops.route_stop();
        t->active = 0;
        (void)tl_init(&t->tl, &t->cfg);
    }
    pthread_mutex_unlock(&t->mu);
}

int a2k_track_enabled(a2k_track_t *t)
{
    int e;
    pthread_mutex_lock(&t->mu);
    e = t->enabled;
    pthread_mutex_unlock(&t->mu);
    return e;
}

void a2k_track_set_return(a2k_track_t *t, int home, int delay_s)
{
    pthread_mutex_lock(&t->mu);
    t->cfg.return_home = home ? 1 : 0;
    t->tl.cfg.return_home = t->cfg.return_home;
    if (delay_s >= 0 && delay_s <= 86400) {
        t->cfg.return_delay_s = delay_s;
        t->tl.cfg.return_delay_s = delay_s;
    }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_zones(a2k_track_t *t, int x_pct, int y_pct, int head_pct)
{
    pthread_mutex_lock(&t->mu);
    if (x_pct >= 0 && x_pct <= 90) { t->cfg.zone_x_pct = x_pct; t->tl.cfg.zone_x_pct = x_pct; }
    if (y_pct >= 0 && y_pct <= 90) { t->cfg.zone_y_pct = y_pct; t->tl.cfg.zone_y_pct = y_pct; }
    if (head_pct >= 0 && head_pct <= 90) { t->cfg.zone_head_pct = head_pct; t->tl.cfg.zone_head_pct = head_pct; }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_get_zones(a2k_track_t *t, int *x_pct, int *y_pct, int *head_pct)
{
    pthread_mutex_lock(&t->mu);
    *x_pct = t->cfg.zone_x_pct;
    *y_pct = t->cfg.zone_y_pct;
    *head_pct = t->cfg.zone_head_pct;
    pthread_mutex_unlock(&t->mu);
}

int a2k_track_set_lost_policy(a2k_track_t *t, const char *text)
{
    struct tl_cfg tmp;
    int rc;
    pthread_mutex_lock(&t->mu);
    tmp = t->cfg;
    rc = tl_lost_parse(text, &tmp);
    if (rc == 0) { /* only the chain changes; the running state is kept, an ongoing chain restarts from its first step */
        t->cfg.lost_n = tmp.lost_n;
        memcpy(t->cfg.lost, tmp.lost, sizeof t->cfg.lost);
        t->tl.cfg.lost_n = tmp.lost_n;
        memcpy(t->tl.cfg.lost, tmp.lost, sizeof t->tl.cfg.lost);
        t->tl.lost_i = -1;
        t->tl.ls_init = 0;
    }
    pthread_mutex_unlock(&t->mu);
    return rc == 0 ? 0 : -1;
}

void a2k_track_get_lost_policy(a2k_track_t *t, char *out, size_t cap)
{
    pthread_mutex_lock(&t->mu);
    tl_lost_format(&t->cfg, out, cap);
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_get_lost_step(a2k_track_t *t, char *out, size_t cap)
{
    pthread_mutex_lock(&t->mu);
    if (!t->tl.lstep || !strcmp(t->tl.lstep, "idle"))
        snprintf(out, cap, "idle");
    else if (!strcmp(t->tl.lstep, "route"))
        snprintf(out, cap, "route:%s", t->tl.lroute);
    else
        snprintf(out, cap, "%s", t->tl.lstep);
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_motion(a2k_track_t *t, int motion)
{
    pthread_mutex_lock(&t->mu);
    if (motion == TL_MOTION_STEPS || motion == TL_MOTION_CONT) {
        if (t->cfg.motion != motion && t->active && t->ops.vel)
            (void)t->ops.vel(0, 0, 0, 0); /* leaving continuous mode: the motor stops now */
        t->cfg.motion = motion;
        t->tl.cfg.motion = motion;
    }
    pthread_mutex_unlock(&t->mu);
}

int a2k_track_get_motion(a2k_track_t *t)
{
    int m;
    pthread_mutex_lock(&t->mu);
    m = t->cfg.motion;
    pthread_mutex_unlock(&t->mu);
    return m;
}

void a2k_track_set_tune(a2k_track_t *t, int band_pct, int head_pct)
{
    pthread_mutex_lock(&t->mu);
    if (band_pct >= 2 && band_pct <= 40) {
        t->cfg.deadband_pct = band_pct;
        t->tl.cfg.deadband_pct = band_pct;
    }
    if (head_pct >= 0 && head_pct <= 50) {
        t->cfg.head_pct = head_pct;
        t->tl.cfg.head_pct = head_pct;
    }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_gains(a2k_track_t *t, int gain_pan, int gain_tilt, int tilt_band_pct)
{
    pthread_mutex_lock(&t->mu);
    if (gain_pan >= 200 && gain_pan <= 4000) {
        t->cfg.gain_pan = gain_pan;
        t->tl.cfg.gain_pan = gain_pan;
    }
    if (gain_tilt >= 200 && gain_tilt <= 4000) {
        t->cfg.gain_tilt = gain_tilt;
        t->tl.cfg.gain_tilt = gain_tilt;
    }
    if (tilt_band_pct >= 2 && tilt_band_pct <= 40) {
        t->cfg.tband_pct = tilt_band_pct;
        t->tl.cfg.tband_pct = tilt_band_pct;
    }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_lag(a2k_track_t *t, int lag_ms)
{
    pthread_mutex_lock(&t->mu);
    if (lag_ms >= -150 && lag_ms <= 300) {
        t->cfg.lag_ms = lag_ms;
        t->tl.cfg.lag_ms = lag_ms;
    }
    pthread_mutex_unlock(&t->mu);
}

int a2k_track_get_lag(a2k_track_t *t)
{
    int v;
    pthread_mutex_lock(&t->mu);
    v = t->cfg.lag_ms;
    pthread_mutex_unlock(&t->mu);
    return v;
}

void a2k_track_get_gains(a2k_track_t *t, int *gain_pan, int *gain_tilt, int *tilt_band_pct)
{
    pthread_mutex_lock(&t->mu);
    *gain_pan = t->cfg.gain_pan;
    *gain_tilt = t->cfg.gain_tilt;
    *tilt_band_pct = t->cfg.tband_pct;
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_get_tune(a2k_track_t *t, int *band_pct, int *head_pct)
{
    pthread_mutex_lock(&t->mu);
    *band_pct = t->cfg.deadband_pct;
    *head_pct = t->cfg.head_pct;
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_tilt_return(a2k_track_t *t, int s)
{
    pthread_mutex_lock(&t->mu);
    if (s >= 0 && s <= 3600) {
        t->cfg.tilt_return_s = s;
        t->tl.cfg.tilt_return_s = s;
    }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_set_tilt_back(a2k_track_t *t, int s)
{
    pthread_mutex_lock(&t->mu);
    if (s >= 0 && s <= 600) {
        t->cfg.tilt_back_s = s;
        t->tl.cfg.tilt_back_s = s;
    }
    pthread_mutex_unlock(&t->mu);
}

/* track.tilt (0 = pan only: the policy never commands the tilt: no follow, no close scan, no tilt-back, no vertical search), live. */
int a2k_track_set_tilt(a2k_track_t *t, int on)
{
    if (on != 0 && on != 1)
        return -1;
    pthread_mutex_lock(&t->mu);
    t->cfg.tilt_enable = on;
    t->tl.cfg.tilt_enable = on;
    pthread_mutex_unlock(&t->mu);
    return 0;
}

/* The motor direction flags of the policy (live): the 2K binding's base combined with the image orientation (track_run.c, orient.h). 0, or -1 for a value other than 0 / 1. */
int a2k_track_set_invert(a2k_track_t *t, int invert_pan, int invert_tilt)
{
    if ((invert_pan & ~1) || (invert_tilt & ~1))
        return -1;
    pthread_mutex_lock(&t->mu);
    if (t->cfg.invert_pan != invert_pan || t->cfg.invert_tilt != invert_tilt) {
        /* The policy memory holds world positions built with the old signs (m * pan, kt * tilt, tilt_ref, exit_tilt0): read with the new ones they point to the mirrored
         * side. A real change forgets the target as the disable path does (a moving command and a lost-chain route stop, the next target starts clean). */
        if (t->active) {
            if (t->ops.vel)
                (void)t->ops.vel(0, 0, 0, 0);
            if (t->tl.lost_route_on && t->ops.route_stop)
                (void)t->ops.route_stop();
        }
        t->cfg.invert_pan = invert_pan;
        t->cfg.invert_tilt = invert_tilt;
        (void)tl_init(&t->tl, &t->cfg);
    }
    pthread_mutex_unlock(&t->mu);
    return 0;
}

int a2k_track_get_tilt(a2k_track_t *t)
{
    int v;
    pthread_mutex_lock(&t->mu);
    v = t->cfg.tilt_enable;
    pthread_mutex_unlock(&t->mu);
    return v;
}

int a2k_track_set_close_scan(a2k_track_t *t, int which, int v)
{
    int rc = 0;
    pthread_mutex_lock(&t->mu);
    if (which == 0 && (v == 0 || v == 1)) {
        t->cfg.close_scan = v;
        t->tl.cfg.close_scan = v;
    } else if (which == 1 && v >= 0 && v <= 600) {
        t->cfg.close_scan_up = v;
        t->tl.cfg.close_scan_up = v;
    } else if (which == 2 && v >= 0 && v <= 600) {
        t->cfg.close_scan_down = v;
        t->tl.cfg.close_scan_down = v;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

int a2k_track_get_close_scan(a2k_track_t *t, int which)
{
    int v;
    pthread_mutex_lock(&t->mu);
    v = which == 0 ? t->cfg.close_scan : which == 1 ? t->cfg.close_scan_up : t->cfg.close_scan_down;
    pthread_mutex_unlock(&t->mu);
    return v;
}

void a2k_track_get_tilt_back(a2k_track_t *t, int *s)
{
    pthread_mutex_lock(&t->mu);
    *s = t->cfg.tilt_back_s;
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_get_tilt_return(a2k_track_t *t, int *s)
{
    pthread_mutex_lock(&t->mu);
    *s = t->cfg.tilt_return_s;
    pthread_mutex_unlock(&t->mu);
}

static void rec_enable(int on);
void a2k_track_set_debug(a2k_track_t *t, int on)
{
    pthread_mutex_lock(&t->mu);
    t->debug = on < 0 ? 0 : on > 2 ? 2 : on;
    rec_enable(t->debug >= 2);
    pthread_mutex_unlock(&t->mu);
}

/* ---- the raw recorder (track_debug=2): RAM only (/tmp), rolling like the decision log. Lines: C = the policy config (first, and again after every roll), I = every tl_update input,
 * R = every position-ring push during a continuous session (a2k_track_rec_ring, wired to the ptz layer by the main), D = the decision line. tests/rot2k_track_replay.c replays I lines,
 * tools/track_lagfit reads R + I lines. ---- */
static const char REC_PATH[] = "/tmp/akam2k_track.raw";
#define REC_MAX 1048576
static pthread_mutex_t g_rec_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_rec_f;
static long g_rec_sz;
static volatile int g_rec_on;
static char g_rec_cfg[1300]; /* the last C line (with its newline) */
static char g_rec_keep[REC_MAX / 2 + 1];

static void rec_roll_locked(void)
{
    size_t got = 0, skip = 0;
    FILE *f = fopen(REC_PATH, "r");
    if (g_rec_f) {
        fclose(g_rec_f);
        g_rec_f = NULL;
    }
    if (f) {
        if (fseek(f, g_rec_sz - REC_MAX / 2, SEEK_SET) == 0)
            got = fread(g_rec_keep, 1, REC_MAX / 2, f);
        fclose(f);
    }
    while (skip < got && g_rec_keep[skip] != '\n')
        skip++;
    skip = skip < got ? skip + 1 : got; /* the first line is partial: dropped */
    g_rec_f = fopen(REC_PATH, "w");
    g_rec_sz = 0;
    if (!g_rec_f)
        return;
    if (g_rec_cfg[0]) {
        fputs(g_rec_cfg, g_rec_f);
        g_rec_sz += (long)strlen(g_rec_cfg);
    }
    if (got > skip) {
        fwrite(g_rec_keep + skip, 1, got - skip, g_rec_f);
        g_rec_sz += (long)(got - skip);
    }
}

/* One line (with its newline) into the raw file. */
static void rec_put(const char *line)
{
    size_t n = strlen(line);
    pthread_mutex_lock(&g_rec_mu);
    if (g_rec_on) {
        if (!g_rec_f) {
            g_rec_f = fopen(REC_PATH, "a");
            g_rec_sz = 0;
            if (g_rec_f && fseek(g_rec_f, 0, SEEK_END) == 0)
                g_rec_sz = ftell(g_rec_f);
        }
        if (g_rec_f && g_rec_sz + (long)n > REC_MAX)
            rec_roll_locked();
        if (g_rec_f) {
            fputs(line, g_rec_f);
            fflush(g_rec_f);
            g_rec_sz += (long)n;
        }
    }
    pthread_mutex_unlock(&g_rec_mu);
}

static void rec_enable(int on)
{
    pthread_mutex_lock(&g_rec_mu);
    if (on && !g_rec_on) { /* a fresh recording */
        FILE *f = fopen(REC_PATH, "w");
        if (f)
            fclose(f);
        g_rec_cfg[0] = 0;
    }
    g_rec_on = on;
    if (!on && g_rec_f) {
        fclose(g_rec_f);
        g_rec_f = NULL;
    }
    pthread_mutex_unlock(&g_rec_mu);
}

void a2k_track_rec_ring(int64_t t_ms, int pan, int tilt)
{
    char l[64];
    if (!g_rec_on)
        return;
    snprintf(l, sizeof l, "R %lld %d %d\n", (long long)t_ms, pan, tilt);
    rec_put(l);
}

static void rec_input(a2k_track_t *t, int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, const struct tl_ptz *p)
{
    char l[1200], cfg[1100];
    size_t k;
    int i, w;
    if (n > TL_MAX_ALL)
        n = TL_MAX_ALL;
    if (n < 0 || !boxes)
        n = 0;
    {   /* the config line when it changed (first line, after a live tuning change) */
        char lost[300];
        size_t m = tl_cfg_kv_dump(&t->tl.cfg, cfg, sizeof cfg);
        tl_lost_format(&t->tl.cfg, lost, sizeof lost);
        snprintf(cfg + m, sizeof cfg - m, "lost=%s", lost);
        snprintf(l, sizeof l, "C %s\n", cfg);
        if (strcmp(l, g_rec_cfg) != 0) { /* g_rec_cfg is only touched under t->mu (this caller) and g_rec_mu in roll: copy under it */
            pthread_mutex_lock(&g_rec_mu);
            snprintf(g_rec_cfg, sizeof g_rec_cfg, "%s", l);
            pthread_mutex_unlock(&g_rec_mu);
            rec_put(l);
        }
    }
    k = (size_t)snprintf(l, sizeof l, "I %lld %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d", (long long)now_ms, (long long)cap_ms, p->pan, p->tilt, p->has_cap, p->cap_pan, p->cap_tilt,
                         p->cap_rate_pan, p->cap_rate_tilt, p->moving, p->tilt_home, p->pan_min, p->pan_max, p->tilt_min, p->tilt_max, n);
    for (i = 0; i < n && k + 64 < sizeof l; i++) {
        w = snprintf(l + k, sizeof l - k, " {%d,%d,%d,%d,%d,%d}", boxes[i].cls, boxes[i].score, boxes[i].x1, boxes[i].y1, boxes[i].x2, boxes[i].y2);
        if (w < 0)
            break;
        k += (size_t)w;
    }
    snprintf(l + k, sizeof l - k, " me=%lld cr=%d ob=%d rb=%d ct=%d\n", (long long)p->motion_end_ms, p->cap_rate_ok, p->over_budget, p->route_busy, p->cont);
    rec_put(l);
}

static const char DBG_PATH[] = "/tmp/akam2k_track.log"; /* RAM (tmpfs), never the card */
#define DBG_MAX 65536
#define DBG_EVERY_MS 500
/* One decision line into the RAM file (never the card), at most one per DBG_EVERY_MS except when a nudge is sent; above DBG_MAX the newest half is kept (rolling, never truncated to empty). */
static void debug_line(a2k_track_t *t, int64_t now_ms, int n, int known, int tilt, int pan, int over_budget, const struct tl_out *o)
{
    const struct tl_dbg *d = &t->tl.dbg;
    static const char *const rule[] = { "centre", "head-clip", "near", "far", "body", "ambiguous", "head", "low-crop" };
    FILE *f;
    long sz;
    int cont = t->tl.cfg.motion == TL_MOTION_CONT && t->tl.cfg.zone_x_pct > 0 && t->tl.cfg.settle_ms > 0;
    if (cont) { /* the continuous decision log: every tick that has a box or a command, an idle tick only when the reason changed or every second */
        int quiet = !d->has_box && o->kind == TL_NONE && d->why == t->dbg_why && now_ms - t->dbg_last_ms < 1000;
        if (quiet)
            return;
    } else if (o->kind != TL_NUDGE && now_ms - t->dbg_last_ms < DBG_EVERY_MS)
        return;
    t->dbg_last_ms = now_ms;
    t->dbg_why = d->why;
    f = fopen(DBG_PATH, "a");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz > DBG_MAX) { /* rolling window: keep the newest half (from a line start), so an episode survives */
        static char keep[DBG_MAX / 2 + 1]; /* the caller holds t->mu */
        size_t got = 0, skip = 0;
        if (fseek(f, sz - DBG_MAX / 2, SEEK_SET) == 0)
            got = fread(keep, 1, DBG_MAX / 2, f);
        fclose(f);
        while (skip < got && keep[skip] != '\n')
            skip++;
        if (skip < got)
            skip++; /* the first line is partial: drop it */
        else
            skip = got;
        f = fopen(DBG_PATH, "w");
        if (!f)
            return;
        if (got > skip)
            fwrite(keep + skip, 1, got - skip, f);
    }
    {
        char pos[48];
        if (known)
            snprintf(pos, sizeof pos, "pan=%d tilt=%d", pan, tilt);
        else
            snprintf(pos, sizeof pos, "pan=unknown tilt=unknown"); /* a motion-time read is 0/0: never used, never shown as a position */
        if (cont) {
            /* T n box rule ref(col,row) pose | 3D state: distance, angular rates, filter gate | comfort zone: predicted offset / half (steps), moving axes | want (steps/s), command (dir/div), lease | hold / search / sweep | why */
            static const char *const kfs[] = { "ok", "held", "reinit" };
            const struct tl_tracker_view { int col, row; } ref = { (d->box.x1 + d->box.x2) / 2, d->rule == 2 || d->rule == 1 || d->rule == 7 ? d->box.y1 : (d->box.y1 + d->box.y2) / 2 };
            char dl[1024];
            snprintf(dl, sizeof dl, "%lld n=%d CONT %s %s %s ref=%d,%d %s | dist=%dmm v=%d/%d st/s kf=%s | zone ex=%d/%d ey=%d/%d mv=%d%d | want=%d/%d cmd=%s%d/%d,%s%d/%d lease=%d | hold=%d search=%d sweep=%d exit=%d | why=%s"
                    " | rule=%s dcue=%s inn=%d/%d rs=%d/%d st=near%d,kf%d,vact%d,lost:%s bud=ob%d,rej%d,age=%lld\n",
                    (long long)now_ms, n, d->has_box ? "box" : "nobox", d->has_box ? rule[d->rule % 8] : "-", d->upl ? "(head-out-from-ambiguous)" : "-", d->has_box ? ref.col : -1, d->has_box ? ref.row : -1, pos,
                    t->tl.dist_mm, t->tl.vx, t->tl.vy, kfs[d->kf % 3], d->ex, d->hx, d->ey, d->hy, d->mvx, d->mvy, d->vdx, d->vdy,
                    o->kind == TL_VEL ? (o->pan_dir > 0 ? "+" : o->pan_dir < 0 ? "-" : "0") : "-", o->kind == TL_VEL ? o->pan_dir : 0, o->kind == TL_VEL ? o->pan_div : 0,
                    o->kind == TL_VEL ? (o->tilt_dir > 0 ? "d" : o->tilt_dir < 0 ? "u" : "0") : "-", o->kind == TL_VEL ? o->tilt_dir : 0, o->kind == TL_VEL ? o->tilt_div : 0,
                    o->kind == TL_VEL ? t->cfg.lease_ms : o->kind == TL_STOP ? -1 : 0, d->hold, t->tl.search_until > 0, t->tl.sweeping, d->exit, d->why ? d->why : "?",
                    d->has_box ? rule[d->rule % 8] : "-", d->dcue ? d->dcue : "-", d->innx, d->inny, d->rsx, d->rsy, t->tl.near_mode, d->kf, t->tl.vact, t->tl.lstep ? t->tl.lstep : "-", over_budget, t->tl.kf_rej,
                    (long long)(t->tl.lastcap_ms ? now_ms - t->tl.lastcap_ms : 0));
            fputs(dl, f);
            if (t->debug >= 2) {
                char dr[1030];
                snprintf(dr, sizeof dr, "D %s", dl);
                rec_put(dr);
            }
        } else if (d->has_box)
            fprintf(f, "%lld n=%d box=%d,%d,%d,%d rule=%s target_row=%d off_px=%d %s want=%d/%d nudge=%d/%d vel=%d/%d dist_mm=%d v=%d/%d why=%s\n", (long long)now_ms, n, d->box.x1,
                    d->box.y1, d->box.x2, d->box.y2, rule[d->rule % 8], d->ty, d->d2y / 2, pos, d->dp, d->dt, o->kind == TL_NUDGE ? o->dpan : 0,
                    o->kind == TL_NUDGE ? o->dtilt : 0, o->kind == TL_VEL ? o->pan_dir * 1000 / (o->pan_div ? o->pan_div : 1000) : 0, o->kind == TL_VEL ? -o->tilt_dir * 1000 / (o->tilt_div ? o->tilt_div : 1000) : 0,
                    t->tl.dist_mm, t->tl.vx, t->tl.vy, d->why ? d->why : "?");
        else
            fprintf(f, "%lld n=%d no box %s exit=%d nudge=%d/%d why=%s\n", (long long)now_ms, n, pos, d->exit, o->kind == TL_NUDGE ? o->dpan : 0,
                    o->kind == TL_NUDGE ? o->dtilt : 0, d->why ? d->why : "?");
    }
    fclose(f);
}

void a2k_track_get_return(a2k_track_t *t, int *home, int *delay_s)
{
    pthread_mutex_lock(&t->mu);
    *home = t->cfg.return_home;
    *delay_s = t->cfg.return_delay_s;
    pthread_mutex_unlock(&t->mu);
}

int a2k_track_set_cfg(a2k_track_t *t, const struct tl_cfg *c)
{
    int rc = tl_cfg_validate(c);
    if (rc != 0)
        return -1;
    pthread_mutex_lock(&t->mu);
    t->cfg = *c;
    (void)tl_init(&t->tl, &t->cfg);
    pthread_mutex_unlock(&t->mu);
    return 0;
}

void a2k_track_tick(a2k_track_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    a2k_track_tick_ts(t, now_ms, 0, boxes, n, fw, fh);
}

/* The area (enabled, complete) that contains the position, as a range r = {pan_min, pan_max, tilt_min, tilt_max}: the largest when several overlap. 1 when one was found. */
static int area_range(const a2k_tareas_t *t, int pan, int tilt, int r[4])
{
    int found = 0, best = -1;
    for (int i = 0; i < t->n; i++) {
        const struct a2k_tarea *a = &t->a[i];
        int sz;
        if (!a->complete || !a->enabled || pan < a->pmin || pan > a->pmax || tilt < a->tmin || tilt > a->tmax)
            continue;
        sz = (a->pmax - a->pmin) * (a->tmax - a->tmin);
        if (sz > best) {
            best = sz;
            r[0] = a->pmin; r[1] = a->pmax; r[2] = a->tmin; r[3] = a->tmax;
            found = 1;
        }
    }
    return found;
}

static int any_area(const a2k_tareas_t *t)
{
    for (int i = 0; i < t->n; i++)
        if (t->a[i].complete)
            return 1;
    return 0;
}

void a2k_track_tick_ts(a2k_track_t *t, int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    a2k_ptz_info_t i;
    struct tl_ptz p;
    struct tl_out o;
    int r[4], act, mdx = 0, mdy = 0, ok = 1, moving, cont, have_pos = 0, cp, ct, np = 0, bi;
    for (bi = 0; boxes && bi < n; bi++)
        np += boxes[bi].cls != TL_CLS_HEAD; /* a head box alone is not a person for the patrol / tracker */
    pthread_mutex_lock(&t->mu);
    t->ops.info(&i);
    cont = t->cfg.motion == TL_MOTION_CONT && t->ops.vel && t->ops.pos_at;
    t->tl.cfg.motion = cont ? TL_MOTION_CONT : TL_MOTION_STEPS;
    moving = i.state == HAL_PTZ_MOVED && !i.known;
    if (cont && t->ops.pos_at(now_ms, &cp, &ct) == 0) { /* the driver position now, also while a continuous move runs */
        i.pan = (uint32_t)cp;
        i.tilt = (uint32_t)ct;
        have_pos = 1;
        moving = 0;
    }
    act = t->enabled && (i.state == HAL_PTZ_HOME || i.state == HAL_PTZ_MOVED) && !(t->ops.privacy && t->ops.privacy()) &&
          t->ops.range && t->ops.range(r) == 0 && (moving || a2k_tareas_allowed(&t->areas, (int)i.pan, (int)i.tilt));
    if (!act) {
        if (t->active) {
            if (t->ops.vel)
                (void)t->ops.vel(0, 0, 0, 0); /* parked / calibrating / privacy / outside the areas: a continuous move stops at once */
            if (t->tl.lost_route_on && t->ops.route_stop)
                (void)t->ops.route_stop(); /* a route the lost chain started does not outlive the tracking */
            (void)tl_init(&t->tl, &t->cfg); /* forget the target: the next one starts with a clean streak */
        }
        t->active = 0;
        if (t->ops.person && np > 0 && boxes) /* somebody is about (the patrol's idle clock), but the tracker does not act on it */
            t->ops.person(0, 0);
        pthread_mutex_unlock(&t->mu);
        return;
    }
    t->active = 1;
    memset(&p, 0, sizeof p);
    p.moving = moving;
    p.cap_ms = cap_ms;
    p.motion_end_ms = i.motion_end_ms;
    p.pan = (int)i.pan;
    p.tilt = (int)i.tilt;
    if (cont && have_pos && any_area(&t->areas)) { /* the move is bounded by the area that contains the head: it REPLACES the soft range handed to the policy */
        int ar[4];
        if (area_range(&t->areas, p.pan, p.tilt, ar))
            memcpy(r, ar, sizeof r);
    }
    p.pan_min = r[0];
    p.pan_max = r[1];
    p.tilt_min = r[2];
    p.tilt_max = r[3];
    p.tilt_home = t->ops.tilt_home ? t->ops.tilt_home() : 0;
    p.cont = cont && have_pos;
    p.route_busy = t->ops.route_busy ? t->ops.route_busy() : 0;
    p.over_budget = t->ops.over_budget ? t->ops.over_budget() : 0;
    if (cont && have_pos) { /* the pose at the capture time of the frame: a frame taken while the head moved is still usable */
        int lag = t->cfg.lag_ms, qp, qt;
        if (cap_ms && t->ops.pos_at(cap_ms - lag, &qp, &qt) == 0) {
            int ap, at, bp, bt;
            p.has_cap = 1;
            p.cap_pan = qp;
            p.cap_tilt = qt;
            if (t->ops.pos_at(cap_ms - lag + 25, &ap, &at) == 0 && t->ops.pos_at(cap_ms - lag - 25, &bp, &bt) == 0) { /* the camera speed at the capture instant (steps/s) */
                p.cap_rate_ok = 1;
                p.cap_rate_pan = (ap - bp) * 20;
                p.cap_rate_tilt = (at - bt) * 20;
            }
        } else if (!cap_ms) {
            p.has_cap = 1;
            p.cap_pan = p.pan;
            p.cap_tilt = p.tilt;
        }
    }
    if (t->debug >= 2)
        rec_input(t, now_ms, cap_ms, boxes, n, &p); /* the exact input of the tick, before the policy sees it */
    tl_update(&t->tl, now_ms, boxes, n, fw, fh, &p, &o);
    if (t->debug)
        debug_line(t, now_ms, n, !moving, p.tilt, p.pan, p.over_budget, &o);
    if (o.route_op == 1) {
        if (!t->ops.route_start || t->ops.route_start(o.route_name) != 0)
            t->tl.lost_route_on = 0; /* not started: never stop a route (the user's patrol) that is not ours; the chain moves on after 3 s */
    }
    else if (o.route_op == 2 && t->ops.route_stop)
        (void)t->ops.route_stop();
    if (t->tl.vs_ev) {   /* the close scan: logged at its start and at its end (once per loss episode, so never a flood) */
        int ev = t->tl.vs_ev, tl_at = t->tl.vs_ev_tilt;
        t->tl.vs_ev = 0;
        if (ev == VSEV_UP || ev == VSEV_DOWN)
            a2k_log("track: close scan %s from tilt %d", ev == VSEV_UP ? "up" : "down", tl_at);
        else
            a2k_log("track: close scan done, %s (tilt %d)", ev == VSEV_FOUND ? "found" : "nothing", tl_at);
    }
    {   /* the current lost step: announced when it changes */
        char cur[40];
        if (!t->tl.lstep || !strcmp(t->tl.lstep, "idle")) cur[0] = 0;
        else if (!strcmp(t->tl.lstep, "route")) snprintf(cur, sizeof cur, "route:%s", t->tl.lroute);
        else snprintf(cur, sizeof cur, "%s", t->tl.lstep);
        if (strcmp(cur, t->lost_last) != 0) {
            snprintf(t->lost_last, sizeof t->lost_last, "%s", cur);
            if (t->ops.lost_event)
                t->ops.lost_event(cur);
            if (cur[0])
                a2k_log("track: lost target, step %s", cur);
        }
    }
    if (t->ops.person && np > 0 && boxes) {
        /* the patrol pauses only for a CONFIRMED person the tracker acts on (a fresh, non-stale frame with a target held for the confirm streak); any other
         * sighting only tells it that somebody is about */
        int confirmed = act && t->tl.has_target && t->tl.streak >= t->cfg.confirm_ticks && !(t->tl.dbg.why && !strcmp(t->tl.dbg.why, "stale"));
        t->ops.person(confirmed, (t->cfg.lost_grace_s + t->cfg.return_delay_s) * 1000);
    }
    if (o.kind == TL_VEL && t->ops.vel) {
        /* motor signs: pan_dir right = +; the tilt direction is in image sign (below = +): the motor tilt + is up */
        int sx = o.pan_dir, sy = -o.tilt_dir, dx = o.pan_div, dy = o.tilt_div;
        if (any_area(&t->areas)) { /* defence in depth: never toward the edge of the area that holds the head */
            int q[4];
            if (!area_range(&t->areas, p.pan, p.tilt, q)) {
                sx = sy = 0;
            } else {
                /* the margin is what the head can travel before the next refresh can stop it: speed x (a tick + the lease) + the soft-range style 30 steps */
                int mx = 30 + (dx ? 1100 / dx : 0) * (200 + t->cfg.lease_ms) / 1000, my = 30 + (dy ? 1100 / dy : 0) * (200 + t->cfg.lease_ms) / 1000;
                if ((sx > 0 && q[1] - p.pan < mx) || (sx < 0 && p.pan - q[0] < mx)) sx = 0;
                if ((sy > 0 && q[3] - p.tilt < my) || (sy < 0 && p.tilt - q[2] < my)) sy = 0;
            }
        }
        if (t->ops.vel(sx, sy, sx ? dx : 0, sy ? dy : 0) < 0)
            t->tl.vact = 0; /* refused (another command runs): the policy must not believe a move is on */
        t->vels++;
    } else if (o.kind == TL_STOP && t->ops.vel) {
        (void)t->ops.vel(0, 0, 0, 0);
        t->tl.vact = 0;
    } else if (o.kind == TL_NUDGE) {
        /* image axes -> motor: right = pan +, below the centre = tilt DOWN (tilt + is up) */
        mdx = o.dpan;
        mdy = -o.dtilt;
        ok = a2k_tareas_clip(&t->areas, p.pan, p.tilt, mdx, mdy, &mdx, &mdy);
        if (ok && (mdx || mdy)) {
            (void)t->ops.nudge(mdx, mdy, o.speed);
            t->nudges++;
        }
    } else if (o.kind == TL_HOME) {
        if (!(t->ops.home_hold && t->ops.home_hold())) { /* a patrol resumes at the interrupted step instead of the head going home */
            if (t->ops.home() < 0)
                tl_home_refused(&t->tl, now_ms); /* refused (a manual hold, the drift window, busy): asked again, bounded */
            else
                t->homes++;
        }
    }
    pthread_mutex_unlock(&t->mu);
}

void a2k_track_areas(a2k_track_t *t, a2k_tareas_t *out)
{
    pthread_mutex_lock(&t->mu);
    *out = t->areas;
    pthread_mutex_unlock(&t->mu);
}

static void changed(a2k_track_t *t)
{
    persist(t);
    if (t->notify)
        t->notify();
}

int a2k_track_area_new(a2k_track_t *t, const char *name)
{
    int rc = -1;
    pthread_mutex_lock(&t->mu);
    if (a2k_tarea_name_ok(name) && t->areas.n < A2K_TAREA_MAX) {
        int dup = 0;
        for (int i = 0; i < t->areas.n; i++)
            dup |= !strcmp(t->areas.a[i].name, name);
        if (!dup) {
            struct a2k_tarea *a = &t->areas.a[t->areas.n++];
            memset(a, 0, sizeof *a);
            snprintf(a->name, sizeof a->name, "%s", name);
            a->enabled = 1;
            rc = 0;
            changed(t);
        }
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

int a2k_track_area_corner(a2k_track_t *t, int slot, int corner)
{
    a2k_ptz_info_t i;
    int rc = -1;
    t->ops.info(&i);
    pthread_mutex_lock(&t->mu);
    if (slot >= 0 && slot < t->areas.n && i.known && (corner == 0 || corner == 1)) {
        struct a2k_tarea *a = &t->areas.a[slot];
        if (corner == 0) {
            a->has_a = 1;
            a->a_pan = (int)i.pan;
            a->a_tilt = (int)i.tilt;
        } else {
            a->has_b = 1;
            a->b_pan = (int)i.pan;
            a->b_tilt = (int)i.tilt;
        }
        rc = 0;
        if (a->has_a && a->has_b) {
            int p0 = a->a_pan < a->b_pan ? a->a_pan : a->b_pan, p1 = a->a_pan < a->b_pan ? a->b_pan : a->a_pan;
            int t0 = a->a_tilt < a->b_tilt ? a->a_tilt : a->b_tilt, t1 = a->a_tilt < a->b_tilt ? a->b_tilt : a->a_tilt;
            if (p0 < p1 && t0 < t1) { /* a degenerate rectangle (the two corners on a line) keeps the old area */
                a->complete = 1;
                a->pmin = p0;
                a->pmax = p1;
                a->tmin = t0;
                a->tmax = t1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0)
            changed(t);
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

int a2k_track_area_enable(a2k_track_t *t, int slot, int on)
{
    int rc = -1;
    pthread_mutex_lock(&t->mu);
    if (slot >= 0 && slot < t->areas.n) {
        t->areas.a[slot].enabled = on ? 1 : 0;
        rc = 0;
        changed(t);
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

int a2k_track_area_delete(a2k_track_t *t, int slot)
{
    int rc = -1;
    pthread_mutex_lock(&t->mu);
    if (slot >= 0 && slot < t->areas.n) {
        memmove(&t->areas.a[slot], &t->areas.a[slot + 1], sizeof t->areas.a[0] * (size_t)(t->areas.n - slot - 1));
        t->areas.n--;
        memset(&t->areas.a[t->areas.n], 0, sizeof t->areas.a[0]);
        rc = 0;
        changed(t);
    }
    pthread_mutex_unlock(&t->mu);
    return rc;
}

void a2k_track_areas_text(a2k_track_t *t, char *out, size_t cap)
{
    a2k_tareas_t a;
    size_t n = 0;
    a2k_track_areas(t, &a);
    snprintf(out, cap, "none");
    for (int i = 0; i < a.n; i++) {
        const struct a2k_tarea *x = &a.a[i];
        int w;
        if (n + 1 >= cap)
            break;
        if (i)
            out[n++] = ';', out[n] = 0;
        if (n + 1 >= cap)
            break;
        if (x->complete)
            w = snprintf(out + n, cap - n, "%s=%d:%d:%d:%d", x->name, x->pmin, x->pmax, x->tmin, x->tmax);
        else
            w = snprintf(out + n, cap - n, "%s=incomplete", x->name);
        if (w >= 0 && (size_t)w < cap - n && x->complete && !x->enabled) {
            n += (size_t)w;
            w = snprintf(out + n, cap - n, ":off");
        }
        if (w < 0 || (size_t)w >= cap - n)
            break;
        n += (size_t)w;
    }
}
