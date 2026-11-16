/* See akam2k_track.h. */
#include "akam2k_track.h"

#include <stdio.h>
#include <string.h>

#include "akam2k_log.h"
#include "akam2k_store.h"

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
        a2k_log("track: the track_area keys of akam2k.conf are invalid, ignored");
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
    a2k_log("track: %d area(s) from %s", a.n, have ? t->path : "akam2k.conf");
    return a.n;
}

void a2k_track_set_enabled(a2k_track_t *t, int on)
{
    pthread_mutex_lock(&t->mu);
    t->enabled = on ? 1 : 0;
    if (!t->enabled)
        (void)tl_init(&t->tl, &t->cfg);
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

void a2k_track_tick_ts(a2k_track_t *t, int64_t now_ms, int64_t cap_ms, const struct tl_box *boxes, int n, int fw, int fh)
{
    a2k_ptz_info_t i;
    struct tl_ptz p;
    struct tl_out o;
    int r[4], act, mdx = 0, mdy = 0, ok = 1, moving;
    pthread_mutex_lock(&t->mu);
    t->ops.info(&i);
    moving = i.state == HAL_PTZ_MOVED && !i.known;
    act = t->enabled && (i.state == HAL_PTZ_HOME || i.state == HAL_PTZ_MOVED) && !(t->ops.privacy && t->ops.privacy()) &&
          t->ops.range && t->ops.range(r) == 0 && (moving || a2k_tareas_allowed(&t->areas, (int)i.pan, (int)i.tilt));
    if (!act) {
        if (t->active)
            (void)tl_init(&t->tl, &t->cfg); /* forget the target: the next one starts with a clean streak */
        t->active = 0;
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
    p.pan_min = r[0];
    p.pan_max = r[1];
    p.tilt_min = r[2];
    p.tilt_max = r[3];
    tl_update(&t->tl, now_ms, boxes, n, fw, fh, &p, &o);
    if (o.kind == TL_NUDGE) {
        /* image axes -> motor: right = pan +, below the centre = tilt DOWN (tilt + is up) */
        mdx = o.dpan;
        mdy = -o.dtilt;
        ok = a2k_tareas_clip(&t->areas, p.pan, p.tilt, mdx, mdy, &mdx, &mdy);
        if (ok && (mdx || mdy)) {
            (void)t->ops.nudge(mdx, mdy, o.speed);
            t->nudges++;
        }
    } else if (o.kind == TL_HOME) {
        (void)t->ops.home();
        t->homes++;
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
