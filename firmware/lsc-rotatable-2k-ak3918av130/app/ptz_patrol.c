/* See ptz_patrol.h and specs/ptz-patrol.md. Pure: no I/O, no clock, no allocation, no hardware. */
#include "ptz_patrol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_none[1] = { 0 }; /* the empty text (the C source scan forbids an empty string literal) */

/* ------------------------------------------------------------------ names, errors */
const char *pp_err_code(int err)
{
    switch (err) {
    case PP_E_SYNTAX: return "bad_json";
    case PP_E_NAME: return "bad_name";
    case PP_E_RANGE: return "range";
    case PP_E_SPEED: return "speed";
    case PP_E_DWELL: return "dwell";
    case PP_E_MODE: return "bad_mode";
    case PP_E_NOPRESET: return "no_preset";
    case PP_E_INUSE: return "in_use";
    case PP_E_NOTFOUND: return "not_found";
    case PP_E_FULL: return "full";
    case PP_E_TOOMANY: return "too_many_steps";
    case PP_E_EMPTY: return "empty";
    case PP_E_PATTERN: return "bad_pattern";
    case PP_E_TOOBIG: return "too_big";
    case PP_E_BUSY: return "busy";
    default: return "error";
    }
}

int pp_name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0, i;
    if (n < 1 || n > PP_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}

const char *pp_mode_name(int m) { return m == PP_ONCE ? "once" : m == PP_PINGPONG ? "pingpong" : "loop"; }

int pp_mode_parse(const char *s)
{
    if (!s)
        return -1;
    return !strcmp(s, "once") ? PP_ONCE : !strcmp(s, "loop") ? PP_LOOP : !strcmp(s, "pingpong") ? PP_PINGPONG : -1;
}

static int dg_set(pp_diag_t *dg, int err, const char *fmt, ...)
{
    if (dg) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(dg->detail, sizeof dg->detail, fmt, ap);
        va_end(ap);
    }
    return err;
}

static void cpy(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > PP_NAME_MAX)
        n = PP_NAME_MAX;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ------------------------------------------------------------------ config */
void pp_cfg_init(pp_cfg_t *c) { memset(c, 0, sizeof *c); }

int pp_find_preset(const pp_cfg_t *c, const char *name)
{
    int i;
    for (i = 0; i < c->npresets; i++)
        if (!strcmp(c->preset[i].name, name))
            return i;
    return -1;
}

int pp_find_route(const pp_cfg_t *c, const char *name)
{
    int i;
    for (i = 0; i < c->nroutes; i++)
        if (!strcmp(c->route[i].name, name))
            return i;
    return -1;
}

static int near_park(const pp_limits_t *lim, int pan, int tilt)
{
    return lim && lim->park_tol > 0 && abs(pan - lim->park_pan) <= lim->park_tol && abs(tilt - lim->park_tilt) <= lim->park_tol;
}

static int pos_ok(const pp_limits_t *lim, int pan, int tilt)
{
    return !lim || (pan >= lim->pan_min && pan <= lim->pan_max && tilt >= lim->tilt_min && tilt <= lim->tilt_max && !near_park(lim, pan, tilt));
}

static const char *why_pos(const pp_limits_t *lim, int pan, int tilt)
{
    return near_park(lim, pan, tilt) ? "near the park position (privacy)" : "outside the soft range";
}

static int speed_ok(const pp_limits_t *lim, int speed)
{
    if (speed == 0)
        return 1;
    return lim ? (speed >= lim->speed_min && speed <= lim->speed_max) : (speed >= 1 && speed <= 99);
}

int pp_route_check(const pp_cfg_t *c, const pp_limits_t *lim, const pp_route_t *r, pp_diag_t *dg)
{
    int i;
    if (!pp_name_ok(r->name))
        return dg_set(dg, PP_E_NAME, "route name: a-z first, then a-z 0-9 _, 1..%d characters", PP_NAME_MAX);
    if (r->mode < PP_ONCE || r->mode > PP_PINGPONG)
        return dg_set(dg, PP_E_MODE, "mode is once, loop or pingpong");
    if (r->nsteps < 1)
        return dg_set(dg, PP_E_EMPTY, "a route needs at least one step");
    if (r->nsteps > PP_STEPS)
        return dg_set(dg, PP_E_TOOMANY, "at most %d steps", PP_STEPS);
    for (i = 0; i < r->nsteps; i++) {
        const pp_step_t *s = &r->step[i];
        if (s->preset[0]) {
            if (!pp_name_ok(s->preset))
                return dg_set(dg, PP_E_NAME, "step %d: bad preset name", i);
            int k = pp_find_preset(c, s->preset);
            if (k < 0)
                return dg_set(dg, PP_E_NOPRESET, "step %d: no preset %s", i, s->preset);
            if (near_park(lim, c->preset[k].pan, c->preset[k].tilt))
                return dg_set(dg, PP_E_RANGE, "step %d: preset %s is near the park position (privacy)", i, s->preset);
        } else if (!pos_ok(lim, s->pan, s->tilt)) {
            return dg_set(dg, PP_E_RANGE, "step %d: %d/%d %s (pan %d..%d tilt %d..%d)", i, s->pan, s->tilt, why_pos(lim, s->pan, s->tilt), lim->pan_min, lim->pan_max,
                          lim->tilt_min, lim->tilt_max);
        }
        if (!speed_ok(lim, s->speed))
            return dg_set(dg, PP_E_SPEED, "step %d: speed %d is not 0 or %d..%d", i, s->speed, lim ? lim->speed_min : 1, lim ? lim->speed_max : 99);
        if (s->dwell_ms > PP_DWELL_MAX_MS)
            return dg_set(dg, PP_E_DWELL, "step %d: dwell_ms over %u", i, PP_DWELL_MAX_MS);
    }
    return PP_OK;
}

/* the bytes of the stored text; -1 over PP_TEXT_MAX */
int pp_cfg_text_size(const pp_cfg_t *c)
{
    long n = 0;
    int i, k;
    for (i = 0; i < c->npresets; i++)
        n += snprintf(NULL, 0, "p %s %d %d\n", c->preset[i].name, c->preset[i].pan, c->preset[i].tilt);
    for (i = 0; i < c->nroutes; i++) {
        const pp_route_t *r = &c->route[i];
        n += snprintf(NULL, 0, "r %s %s\n", r->name, pp_mode_name(r->mode));
        for (k = 0; k < r->nsteps; k++) {
            const pp_step_t *s = &r->step[k];
            if (s->preset[0])
                n += snprintf(NULL, 0, "s %s %d %u\n", s->preset, s->speed, s->dwell_ms);
            else
                n += snprintf(NULL, 0, "s %d:%d %d %u\n", s->pan, s->tilt, s->speed, s->dwell_ms);
        }
    }
    return n > PP_TEXT_MAX ? -1 : (int)n;
}

int pp_preset_set(pp_cfg_t *c, const pp_limits_t *lim, const char *name, int pan, int tilt, pp_diag_t *dg)
{
    int i;
    pp_preset_t old;
    if (!pp_name_ok(name))
        return dg_set(dg, PP_E_NAME, "preset name: a-z first, then a-z 0-9 _, 1..%d characters", PP_NAME_MAX);
    if (!pos_ok(lim, pan, tilt))
        return dg_set(dg, PP_E_RANGE, "%d/%d %s (pan %d..%d tilt %d..%d)", pan, tilt, why_pos(lim, pan, tilt), lim->pan_min, lim->pan_max, lim->tilt_min, lim->tilt_max);
    if (pan < 0 || tilt < 0 || pan > 65535 || tilt > 65535)
        return dg_set(dg, PP_E_RANGE, "pan and tilt are 0..65535");
    i = pp_find_preset(c, name);
    if (i < 0) {
        if (c->npresets >= PP_PRESETS)
            return dg_set(dg, PP_E_FULL, "at most %d presets", PP_PRESETS);
        i = c->npresets++;
        memset(&old, 0, sizeof old);
    } else {
        old = c->preset[i];
    }
    cpy(c->preset[i].name, name);
    c->preset[i].pan = pan;
    c->preset[i].tilt = tilt;
    if (pp_cfg_text_size(c) < 0) {
        if (old.name[0])
            c->preset[i] = old;
        else
            c->npresets--;
        return dg_set(dg, PP_E_TOOBIG, "the configuration would exceed %d bytes", PP_TEXT_MAX);
    }
    return PP_OK;
}

int pp_preset_del(pp_cfg_t *c, const char *name, pp_diag_t *dg)
{
    int i = pp_find_preset(c, name), r, k;
    if (i < 0)
        return dg_set(dg, PP_E_NOTFOUND, "no preset %s", name);
    for (r = 0; r < c->nroutes; r++)
        for (k = 0; k < c->route[r].nsteps; k++)
            if (!strcmp(c->route[r].step[k].preset, name))
                return dg_set(dg, PP_E_INUSE, "route %s step %d uses it", c->route[r].name, k);
    memmove(&c->preset[i], &c->preset[i + 1], sizeof c->preset[0] * (size_t)(c->npresets - i - 1));
    c->npresets--;
    memset(&c->preset[c->npresets], 0, sizeof c->preset[0]);
    return PP_OK;
}

int pp_route_set(pp_cfg_t *c, const pp_limits_t *lim, const pp_route_t *r, pp_diag_t *dg)
{
    int rc = pp_route_check(c, lim, r, dg), i;
    pp_route_t old;
    if (rc != PP_OK)
        return rc;
    i = pp_find_route(c, r->name);
    if (i < 0) {
        if (c->nroutes >= PP_ROUTES)
            return dg_set(dg, PP_E_FULL, "at most %d routes", PP_ROUTES);
        i = c->nroutes++;
        memset(&old, 0, sizeof old);
    } else {
        old = c->route[i];
    }
    c->route[i] = *r;
    if (pp_cfg_text_size(c) < 0) {
        if (old.name[0])
            c->route[i] = old;
        else {
            c->nroutes--;
            memset(&c->route[c->nroutes], 0, sizeof c->route[0]);
        }
        return dg_set(dg, PP_E_TOOBIG, "the configuration would exceed %d bytes", PP_TEXT_MAX);
    }
    return PP_OK;
}

int pp_route_del(pp_cfg_t *c, const char *name, pp_diag_t *dg)
{
    int i = pp_find_route(c, name);
    if (i < 0)
        return dg_set(dg, PP_E_NOTFOUND, "no route %s", name);
    memmove(&c->route[i], &c->route[i + 1], sizeof c->route[0] * (size_t)(c->nroutes - i - 1));
    c->nroutes--;
    memset(&c->route[c->nroutes], 0, sizeof c->route[0]);
    return PP_OK;
}

/* ------------------------------------------------------------------ patterns */
static int lim_pos(const pp_limits_t *lim, int pan, int tilt, pp_diag_t *dg)
{
    if (!lim)
        return dg_set(dg, PP_E_PATTERN, "no limits to expand against");
    if (!pos_ok(lim, pan, tilt))
        return dg_set(dg, PP_E_RANGE, "%d/%d %s (pan %d..%d tilt %d..%d)", pan, tilt, why_pos(lim, pan, tilt), lim->pan_min, lim->pan_max, lim->tilt_min, lim->tilt_max);
    return PP_OK;
}

static int common_ok(const pp_limits_t *lim, int speed, unsigned dwell_ms, pp_diag_t *dg)
{
    if (!speed_ok(lim, speed))
        return dg_set(dg, PP_E_SPEED, "speed %d is not 0 or %d..%d", speed, lim->speed_min, lim->speed_max);
    if (dwell_ms > PP_DWELL_MAX_MS)
        return dg_set(dg, PP_E_DWELL, "dwell_ms over %u", PP_DWELL_MAX_MS);
    return PP_OK;
}

static void put_step(pp_route_t *r, int pan, int tilt, int speed, unsigned dwell_ms)
{
    pp_step_t *s = &r->step[r->nsteps++];
    memset(s, 0, sizeof *s);
    s->pan = pan;
    s->tilt = tilt;
    s->speed = speed;
    s->dwell_ms = dwell_ms;
}

/* the points a0 -> a1 every `step`, the last one exactly a1 (axis 0 = pan moves, 1 = tilt moves) */
static int expand_line(const pp_limits_t *lim, int ax, int a0, int a1, int fixed, int step, int speed, unsigned dwell_ms, pp_route_t *r, pp_diag_t *dg)
{
    long span = a1 > a0 ? (long)a1 - a0 : (long)a0 - a1, n;
    int i, dir = a1 > a0 ? 1 : -1, rc;
    pp_route_t t;
    if (step < 1)
        return dg_set(dg, PP_E_PATTERN, "step is at least 1");
    if (span == 0)
        return dg_set(dg, PP_E_PATTERN, "the range is empty");
    rc = ax == 0 ? lim_pos(lim, a0, fixed, dg) : lim_pos(lim, fixed, a0, dg);
    if (rc == PP_OK)
        rc = ax == 0 ? lim_pos(lim, a1, fixed, dg) : lim_pos(lim, fixed, a1, dg);
    if (rc == PP_OK)
        rc = common_ok(lim, speed, dwell_ms, dg);
    if (rc != PP_OK)
        return rc;
    n = (span + step - 1) / step + 1;
    if (n > PP_STEPS)
        return dg_set(dg, PP_E_TOOMANY, "the sweep needs %ld steps, at most %d (raise step)", n, PP_STEPS);
    memset(&t, 0, sizeof t);
    for (i = 0; i < n; i++) {
        long v = i == n - 1 ? a1 : (long)a0 + (long)dir * i * step;
        if (ax == 0)
            put_step(&t, (int)v, fixed, speed, dwell_ms);
        else
            put_step(&t, fixed, (int)v, speed, dwell_ms);
    }
    for (i = 0; i < t.nsteps; i++) /* every generated step, not only the ends (the park position can lie between them) */
        if (!pos_ok(lim, t.step[i].pan, t.step[i].tilt))
            return dg_set(dg, PP_E_RANGE, "step %d: %d/%d %s", i, t.step[i].pan, t.step[i].tilt, why_pos(lim, t.step[i].pan, t.step[i].tilt));
    memcpy(r->step, t.step, sizeof(pp_step_t) * (size_t)t.nsteps);
    r->nsteps = t.nsteps;
    return t.nsteps;
}

int pp_expand_sweep_h(const pp_limits_t *lim, int pan_from, int pan_to, int tilt, int step, int speed, unsigned dwell_ms, pp_route_t *r, pp_diag_t *dg)
{
    return expand_line(lim, 0, pan_from, pan_to, tilt, step, speed, dwell_ms, r, dg);
}

int pp_expand_sweep_v(const pp_limits_t *lim, int tilt_from, int tilt_to, int pan, int step, int speed, unsigned dwell_ms, pp_route_t *r, pp_diag_t *dg)
{
    return expand_line(lim, 1, tilt_from, tilt_to, pan, step, speed, dwell_ms, r, dg);
}

int pp_expand_raster(const pp_limits_t *lim, int pan_from, int pan_to, int tilt_from, int tilt_to, int rows, int cols, int speed, unsigned dwell_ms,
                     pp_route_t *r, pp_diag_t *dg)
{
    int i, j, rc;
    pp_route_t t;
    if (rows < 1 || cols < 2)
        return dg_set(dg, PP_E_PATTERN, "rows is at least 1 and cols at least 2");
    if (rows > PP_STEPS || cols > PP_STEPS || rows * cols > PP_STEPS)
        return dg_set(dg, PP_E_TOOMANY, "rows x cols is %d, at most %d", rows * cols, PP_STEPS);
    if (pan_from == pan_to || (rows > 1 && tilt_from == tilt_to))
        return dg_set(dg, PP_E_PATTERN, "the range is empty");
    rc = lim_pos(lim, pan_from, tilt_from, dg);
    if (rc == PP_OK)
        rc = lim_pos(lim, pan_to, tilt_to, dg);
    if (rc == PP_OK)
        rc = common_ok(lim, speed, dwell_ms, dg);
    if (rc != PP_OK)
        return rc;
    memset(&t, 0, sizeof t);
    for (i = 0; i < rows; i++) {
        long tl = rows == 1 ? tilt_from : (long)tilt_from + ((long)tilt_to - tilt_from) * i / (rows - 1);
        for (j = 0; j < cols; j++) {
            int jj = i & 1 ? cols - 1 - j : j; /* boustrophedon: the pan direction alternates */
            long pn = (long)pan_from + ((long)pan_to - pan_from) * jj / (cols - 1);
            put_step(&t, (int)pn, (int)tl, speed, dwell_ms);
        }
    }
    for (i = 0; i < t.nsteps; i++) /* every generated step, not only the ends (the park position can lie between them) */
        if (!pos_ok(lim, t.step[i].pan, t.step[i].tilt))
            return dg_set(dg, PP_E_RANGE, "step %d: %d/%d %s", i, t.step[i].pan, t.step[i].tilt, why_pos(lim, t.step[i].pan, t.step[i].tilt));
    memcpy(r->step, t.step, sizeof(pp_step_t) * (size_t)t.nsteps);
    r->nsteps = t.nsteps;
    return t.nsteps;
}

/* ------------------------------------------------------------------ text */
int pp_cfg_format(const pp_cfg_t *c, char *out, size_t cap)
{
    size_t n = 0;
    int i, k;
    if (!cap || pp_cfg_text_size(c) < 0)
        return PP_E_TOOBIG;
#define ADD(...) do { int w_ = snprintf(out + n, cap - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= cap - n) return PP_E_TOOBIG; n += (size_t)w_; } while (0)
    out[0] = '\0';
    for (i = 0; i < c->npresets; i++)
        ADD("p %s %d %d\n", c->preset[i].name, c->preset[i].pan, c->preset[i].tilt);
    for (i = 0; i < c->nroutes; i++) {
        const pp_route_t *r = &c->route[i];
        ADD("r %s %s\n", r->name, pp_mode_name(r->mode));
        for (k = 0; k < r->nsteps; k++) {
            const pp_step_t *s = &r->step[k];
            if (s->preset[0])
                ADD("s %s %d %u\n", s->preset, s->speed, s->dwell_ms);
            else
                ADD("s %d:%d %d %u\n", s->pan, s->tilt, s->speed, s->dwell_ms);
        }
    }
#undef ADD
    return (int)n;
}

/* a plain non-negative decimal of 1..9 digits */
static int dec(const char *s, long *v)
{
    long x = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        x = x * 10 + (*s++ - '0');
        if (++n > 9)
            return -1;
    }
    if (!n)
        return -1;
    *v = x;
    return 0;
}

/* splits a line into up to 4 space separated fields; -1 when there are more or an empty one */
static int fields(char *line, char *f[], int max)
{
    int n = 0;
    char *p = line;
    while (*p) {
        if (n >= max)
            return -1;
        f[n++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p == ' ') {
            *p++ = '\0';
            if (!*p || *p == ' ')
                return -1;
        }
    }
    return n;
}


static int parse_fail(pp_cfg_t *out)
{
    pp_cfg_init(out);
    return PP_E_SYNTAX;
}

int pp_cfg_parse(const char *text, size_t len, pp_cfg_t *out)
{
    size_t i = 0;
    pp_route_t cur;
    int have = 0;
    pp_cfg_init(out);
    if (!text || len > PP_TEXT_MAX)
        return PP_E_SYNTAX;
    memset(&cur, 0, sizeof cur);
    while (i < len) {
        char line[96], *f[4];
        size_t e = i, n;
        int nf;
        while (e < len && text[e] != '\n')
            e++;
        n = e - i;
        if (n == 0 || n >= sizeof line)
            return parse_fail(out);
        memcpy(line, text + i, n);
        line[n] = '\0';
        i = e + 1;
        nf = fields(line, f, 4);
        if (nf < 3 || f[0][1] != '\0')
            return parse_fail(out);
        if (f[0][0] == 'p') {
            long pan, tilt;
            if (nf != 4 || have || dec(f[2], &pan) || dec(f[3], &tilt) || pp_find_preset(out, f[1]) >= 0 ||
                pp_preset_set(out, NULL, f[1], (int)pan, (int)tilt, NULL) != PP_OK)
                return parse_fail(out);
        } else if (f[0][0] == 'r') {
            int m = pp_mode_parse(f[2]);
            if (nf != 3 || m < 0 || !pp_name_ok(f[1]))
                return parse_fail(out);
            if (have && pp_route_set(out, NULL, &cur, NULL) != PP_OK)
                return parse_fail(out);
            if (pp_find_route(out, f[1]) >= 0)
                return parse_fail(out);
            memset(&cur, 0, sizeof cur);
            cpy(cur.name, f[1]);
            cur.mode = m;
            have = 1;
        } else if (f[0][0] == 's') {
            long a, b, sp, dw;
            pp_step_t *s;
            char *colon;
            if (nf != 4 || !have || cur.nsteps >= PP_STEPS || dec(f[2], &sp) || dec(f[3], &dw) || dw > (long)PP_DWELL_MAX_MS)
                return parse_fail(out);
            s = &cur.step[cur.nsteps];
            memset(s, 0, sizeof *s);
            colon = strchr(f[1], ':');
            if (colon) {
                *colon = '\0';
                if (dec(f[1], &a) || dec(colon + 1, &b))
                    return parse_fail(out);
                s->pan = (int)a;
                s->tilt = (int)b;
            } else {
                if (!pp_name_ok(f[1]))
                    return parse_fail(out);
                cpy(s->preset, f[1]);
            }
            s->speed = (int)sp;
            s->dwell_ms = (unsigned)dw;
            cur.nsteps++;
        } else {
            return parse_fail(out);
        }
    }
    if (have && pp_route_set(out, NULL, &cur, NULL) != PP_OK)
        return parse_fail(out);
    return PP_OK;
}

/* ------------------------------------------------------------------ JSON writers */
typedef struct {
    char *b;
    size_t cap, len;
    int err;
} pw_t;

static void pw_f(pw_t *w, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (w->err)
        return;
    va_start(ap, fmt);
    n = vsnprintf(w->b + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len)
        w->err = 1;
    else
        w->len += (size_t)n;
}

static int pw_end(pw_t *w) { return w->err ? -1 : (int)w->len; }

static void pw_init(pw_t *w, char *b, size_t cap)
{
    w->b = b;
    w->cap = cap;
    w->len = 0;
    w->err = cap ? 0 : 1;
    if (cap)
        b[0] = '\0';
}

int pp_preset_json(const pp_preset_t *p, char *out, size_t cap)
{
    pw_t w;
    pw_init(&w, out, cap);
    pw_f(&w, "{\"name\":\"%s\",\"pan\":%d,\"tilt\":%d}", p->name, p->pan, p->tilt);
    return pw_end(&w);
}

int pp_presets_json(const pp_cfg_t *c, char *out, size_t cap)
{
    pw_t w;
    int i;
    pw_init(&w, out, cap);
    pw_f(&w, "{\"presets\":[");
    for (i = 0; i < c->npresets; i++)
        pw_f(&w, "%s{\"name\":\"%s\",\"pan\":%d,\"tilt\":%d}", i ? "," : k_none, c->preset[i].name, c->preset[i].pan, c->preset[i].tilt);
    pw_f(&w, "],\"max\":%d}", PP_PRESETS);
    return pw_end(&w);
}

int pp_route_json(const pp_route_t *r, char *out, size_t cap)
{
    pw_t w;
    int i;
    pw_init(&w, out, cap);
    pw_f(&w, "{\"name\":\"%s\",\"mode\":\"%s\",\"steps\":[", r->name, pp_mode_name(r->mode));
    for (i = 0; i < r->nsteps; i++) {
        const pp_step_t *s = &r->step[i];
        if (s->preset[0])
            pw_f(&w, "%s{\"preset\":\"%s\",\"speed\":%d,\"dwell_ms\":%u}", i ? "," : k_none, s->preset, s->speed, s->dwell_ms);
        else
            pw_f(&w, "%s{\"pan\":%d,\"tilt\":%d,\"speed\":%d,\"dwell_ms\":%u}", i ? "," : k_none, s->pan, s->tilt, s->speed, s->dwell_ms);
    }
    pw_f(&w, "]}");
    return pw_end(&w);
}

int pp_routes_json(const pp_cfg_t *c, int running_idx, const char *def, char *out, size_t cap)
{
    pw_t w;
    int i;
    pw_init(&w, out, cap);
    pw_f(&w, "{\"routes\":[");
    for (i = 0; i < c->nroutes; i++)
        pw_f(&w, "%s{\"name\":\"%s\",\"mode\":\"%s\",\"steps\":%d,\"running\":%s}", i ? "," : k_none, c->route[i].name, pp_mode_name(c->route[i].mode),
             c->route[i].nsteps, i == running_idx ? "true" : "false");
    pw_f(&w, "],\"max\":%d,\"default\":\"%s\"}", PP_ROUTES, def ? def : k_none);
    return pw_end(&w);
}

int pp_error_json(int err, const pp_diag_t *dg, char *out, size_t cap)
{
    pw_t w;
    char d[sizeof dg->detail];
    size_t i, k = 0;
    if (dg)
        for (i = 0; dg->detail[i] && k + 1 < sizeof d; i++) /* the detail is built from validated text and numbers: keep it JSON safe anyway */
            d[k++] = dg->detail[i] == '"' || dg->detail[i] == '\\' || (unsigned char)dg->detail[i] < 0x20 ? ' ' : dg->detail[i];
    d[k] = '\0';
    pw_init(&w, out, cap);
    pw_f(&w, "{\"error\":\"%s\",\"detail\":\"%s\"}", pp_err_code(err), d);
    return pw_end(&w);
}

/* ------------------------------------------------------------------ JSON reader (bounded, no recursion beyond object -> array -> object) */
typedef struct {
    const char *p, *e;
} js_t;

static void sk(js_t *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int lit(js_t *j, char c)
{
    sk(j);
    if (j->p < j->e && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

/* a plain string: printable ASCII, no escape sequence (names, words) */
static int jstr(js_t *j, char *out, size_t cap)
{
    size_t n = 0;
    sk(j);
    if (j->p >= j->e || *j->p != '"')
        return -1;
    j->p++;
    while (j->p < j->e && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c < 0x20 || c > 0x7e || c == '\\' || n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    if (j->p >= j->e)
        return -1;
    j->p++;
    out[n] = '\0';
    return 0;
}

static int jint(js_t *j, long *v)
{
    int neg = 0, n = 0;
    long x = 0;
    sk(j);
    if (j->p < j->e && *j->p == '-') {
        neg = 1;
        j->p++;
    }
    if (j->p < j->e && *j->p == '0' && j->p + 1 < j->e && j->p[1] >= '0' && j->p[1] <= '9')
        return -1;
    while (j->p < j->e && *j->p >= '0' && *j->p <= '9') {
        x = x * 10 + (*j->p++ - '0');
        if (++n > 9)
            return -1;
    }
    if (!n || (j->p < j->e && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')))
        return -1;
    *v = neg ? -x : x;
    return 0;
}

static int jbool(js_t *j, int *v)
{
    sk(j);
    if (j->e - j->p >= 4 && !memcmp(j->p, "true", 4)) {
        j->p += 4;
        *v = 1;
        return 0;
    }
    if (j->e - j->p >= 5 && !memcmp(j->p, "false", 5)) {
        j->p += 5;
        *v = 0;
        return 0;
    }
    return -1;
}

typedef int (*jkey_fn)(js_t *j, const char *key, void *ctx, pp_diag_t *dg);

/* { "key": value, ... }: fn parses each value */
static int jobject(js_t *j, jkey_fn fn, void *ctx, pp_diag_t *dg)
{
    if (!lit(j, '{'))
        return dg_set(dg, PP_E_SYNTAX, "an object was expected");
    if (lit(j, '}'))
        return PP_OK;
    for (;;) {
        char key[24];
        int rc;
        if (jstr(j, key, sizeof key) || !lit(j, ':'))
            return dg_set(dg, PP_E_SYNTAX, "a key was expected");
        rc = fn(j, key, ctx, dg);
        if (rc != PP_OK)
            return rc;
        if (lit(j, ','))
            continue;
        if (lit(j, '}'))
            return PP_OK;
        return dg_set(dg, PP_E_SYNTAX, "a comma or } was expected");
    }
}

enum { F_INT, F_STR, F_BOOL };
typedef struct {
    const char *key;
    int kind;
    long *iv;
    char *sv;
    size_t scap;
    int *bv;
    int seen;
} jf_t;

/* 0 handled, 1 the key is not in the table, < 0 an error */
static int jfield(js_t *j, jf_t *f, size_t nf, const char *key, pp_diag_t *dg)
{
    size_t i;
    for (i = 0; i < nf; i++) {
        if (strcmp(f[i].key, key))
            continue;
        if (f[i].seen)
            return dg_set(dg, PP_E_SYNTAX, "duplicate key %s", key);
        f[i].seen = 1;
        if (f[i].kind == F_INT && jint(j, f[i].iv))
            return dg_set(dg, PP_E_SYNTAX, "%s is an integer", key);
        if (f[i].kind == F_STR && jstr(j, f[i].sv, f[i].scap))
            return dg_set(dg, PP_E_SYNTAX, "%s is a plain string", key);
        if (f[i].kind == F_BOOL && jbool(j, f[i].bv))
            return dg_set(dg, PP_E_SYNTAX, "%s is true or false", key);
        return PP_OK;
    }
    return 1;
}

static int small(long v) { return v >= -1000000 && v <= 1000000; }

/* ---- a step object ---- */
typedef struct {
    jf_t f[5];
    long pan, tilt, speed, dwell;
    char preset[PP_NAME_MAX + 2];
} stepctx_t;

static int step_key(js_t *j, const char *key, void *c, pp_diag_t *dg)
{
    stepctx_t *s = c;
    int rc = jfield(j, s->f, 5, key, dg);
    return rc == 1 ? dg_set(dg, PP_E_SYNTAX, "unknown step key %s", key) : rc;
}

static int parse_step(js_t *j, pp_step_t *out, int idx, pp_diag_t *dg)
{
    stepctx_t s;
    int rc;
    memset(&s, 0, sizeof s);
    s.f[0] = (jf_t){ "preset", F_STR, NULL, s.preset, sizeof s.preset, NULL, 0 };
    s.f[1] = (jf_t){ "pan", F_INT, &s.pan, NULL, 0, NULL, 0 };
    s.f[2] = (jf_t){ "tilt", F_INT, &s.tilt, NULL, 0, NULL, 0 };
    s.f[3] = (jf_t){ "speed", F_INT, &s.speed, NULL, 0, NULL, 0 };
    s.f[4] = (jf_t){ "dwell_ms", F_INT, &s.dwell, NULL, 0, NULL, 0 };
    rc = jobject(j, step_key, &s, dg);
    if (rc != PP_OK)
        return rc;
    if (s.f[0].seen == (s.f[1].seen || s.f[2].seen) || (!s.f[0].seen && !(s.f[1].seen && s.f[2].seen)))
        return dg_set(dg, PP_E_SYNTAX, "step %d: either preset or both pan and tilt", idx);
    if (!small(s.pan) || !small(s.tilt))
        return dg_set(dg, PP_E_RANGE, "step %d: pan / tilt out of bounds", idx);
    if (s.speed < 0 || s.speed > 99)
        return dg_set(dg, PP_E_SPEED, "step %d: speed %ld", idx, s.speed);
    if (s.dwell < 0 || s.dwell > (long)PP_DWELL_MAX_MS)
        return dg_set(dg, PP_E_DWELL, "step %d: dwell_ms is 0..%u", idx, PP_DWELL_MAX_MS);
    memset(out, 0, sizeof *out);
    if (s.f[0].seen)
        cpy(out->preset, s.preset);
    out->pan = (int)s.pan;
    out->tilt = (int)s.tilt;
    out->speed = (int)s.speed;
    out->dwell_ms = (unsigned)s.dwell;
    return PP_OK;
}

/* ---- a pattern object ---- */
enum { PK_TYPE, PK_PAN_FROM, PK_PAN_TO, PK_TILT, PK_STEP, PK_SPEED, PK_DWELL, PK_TILT_FROM, PK_TILT_TO, PK_PAN, PK_ROWS, PK_COLS, PK_N };
typedef struct {
    jf_t f[PK_N];
    long v[PK_N];
    char type[12];
} patctx_t;

static int pat_key(js_t *j, const char *key, void *c, pp_diag_t *dg)
{
    patctx_t *p = c;
    int rc = jfield(j, p->f, PK_N, key, dg);
    return rc == 1 ? dg_set(dg, PP_E_PATTERN, "unknown pattern key %s", key) : rc;
}

#define BIT(k) (1u << (k))

static int parse_pattern(js_t *j, const pp_limits_t *lim, pp_route_t *r, pp_diag_t *dg)
{
    static const char *const names[PK_N] = { "type", "pan_from", "pan_to", "tilt", "step", "speed", "dwell_ms", "tilt_from", "tilt_to", "pan", "rows", "cols" };
    patctx_t p;
    unsigned seen = 0, need, allow;
    int k, rc, type;
    memset(&p, 0, sizeof p);
    for (k = 0; k < PK_N; k++) {
        p.f[k].key = names[k];
        p.f[k].kind = k == PK_TYPE ? F_STR : F_INT;
        p.f[k].iv = &p.v[k];
        p.f[k].sv = p.type;
        p.f[k].scap = sizeof p.type;
    }
    rc = jobject(j, pat_key, &p, dg);
    if (rc != PP_OK)
        return rc;
    for (k = 0; k < PK_N; k++)
        if (p.f[k].seen)
            seen |= BIT(k);
    if (!(seen & BIT(PK_TYPE)))
        return dg_set(dg, PP_E_PATTERN, "pattern.type is sweep_h, sweep_v or raster");
    if (!strcmp(p.type, "sweep_h")) {
        type = 0;
        need = BIT(PK_PAN_FROM) | BIT(PK_PAN_TO) | BIT(PK_TILT) | BIT(PK_STEP);
        allow = need | BIT(PK_SPEED) | BIT(PK_DWELL);
    } else if (!strcmp(p.type, "sweep_v")) {
        type = 1;
        need = BIT(PK_TILT_FROM) | BIT(PK_TILT_TO) | BIT(PK_PAN) | BIT(PK_STEP);
        allow = need | BIT(PK_SPEED) | BIT(PK_DWELL);
    } else if (!strcmp(p.type, "raster")) {
        type = 2;
        need = BIT(PK_PAN_FROM) | BIT(PK_PAN_TO) | BIT(PK_TILT_FROM) | BIT(PK_TILT_TO) | BIT(PK_ROWS);
        allow = need | BIT(PK_COLS) | BIT(PK_SPEED) | BIT(PK_DWELL);
    } else {
        return dg_set(dg, PP_E_PATTERN, "pattern.type is sweep_h, sweep_v or raster");
    }
    allow |= BIT(PK_TYPE);
    if ((seen & need) != need || (seen & ~allow))
        return dg_set(dg, PP_E_PATTERN, "%s takes exactly its own parameters", p.type);
    for (k = 1; k < PK_N; k++)
        if (!small(p.v[k]))
            return dg_set(dg, PP_E_RANGE, "%s is out of bounds", names[k]);
    if (p.v[PK_SPEED] < 0 || p.v[PK_SPEED] > 99)
        return dg_set(dg, PP_E_SPEED, "speed %ld", p.v[PK_SPEED]);
    if (p.v[PK_DWELL] < 0 || p.v[PK_DWELL] > (long)PP_DWELL_MAX_MS)
        return dg_set(dg, PP_E_DWELL, "dwell_ms is 0..%u", PP_DWELL_MAX_MS);
    if (type == 0)
        rc = pp_expand_sweep_h(lim, (int)p.v[PK_PAN_FROM], (int)p.v[PK_PAN_TO], (int)p.v[PK_TILT], (int)p.v[PK_STEP], (int)p.v[PK_SPEED],
                               (unsigned)p.v[PK_DWELL], r, dg);
    else if (type == 1)
        rc = pp_expand_sweep_v(lim, (int)p.v[PK_TILT_FROM], (int)p.v[PK_TILT_TO], (int)p.v[PK_PAN], (int)p.v[PK_STEP], (int)p.v[PK_SPEED],
                               (unsigned)p.v[PK_DWELL], r, dg);
    else
        rc = pp_expand_raster(lim, (int)p.v[PK_PAN_FROM], (int)p.v[PK_PAN_TO], (int)p.v[PK_TILT_FROM], (int)p.v[PK_TILT_TO], (int)p.v[PK_ROWS],
                              seen & BIT(PK_COLS) ? (int)p.v[PK_COLS] : 2, (int)p.v[PK_SPEED], (unsigned)p.v[PK_DWELL], r, dg);
    return rc < 0 ? rc : PP_OK;
}

/* ---- the route object ---- */
typedef struct {
    const pp_limits_t *lim;
    pp_route_t *r;
    char name[PP_NAME_MAX + 2], mode[12];
    int have_name, have_mode, have_steps, have_pattern;
} routectx_t;

static int route_key(js_t *j, const char *key, void *c, pp_diag_t *dg)
{
    routectx_t *x = c;
    if (!strcmp(key, "name")) {
        if (x->have_name || jstr(j, x->name, sizeof x->name))
            return dg_set(dg, PP_E_SYNTAX, "name is a plain string, once");
        x->have_name = 1;
        return PP_OK;
    }
    if (!strcmp(key, "mode")) {
        if (x->have_mode || jstr(j, x->mode, sizeof x->mode))
            return dg_set(dg, PP_E_SYNTAX, "mode is a plain string, once");
        x->have_mode = 1;
        return PP_OK;
    }
    if (!strcmp(key, "steps")) {
        int n = 0, rc;
        if (x->have_steps)
            return dg_set(dg, PP_E_SYNTAX, "duplicate key steps");
        x->have_steps = 1;
        if (!lit(j, '['))
            return dg_set(dg, PP_E_SYNTAX, "steps is an array");
        if (lit(j, ']'))
            return PP_OK;
        for (;;) {
            if (n >= PP_STEPS)
                return dg_set(dg, PP_E_TOOMANY, "at most %d steps", PP_STEPS);
            rc = parse_step(j, &x->r->step[n], n, dg);
            if (rc != PP_OK)
                return rc;
            n++;
            x->r->nsteps = n;
            if (lit(j, ','))
                continue;
            if (lit(j, ']'))
                return PP_OK;
            return dg_set(dg, PP_E_SYNTAX, "a comma or ] was expected");
        }
    }
    if (!strcmp(key, "pattern")) {
        if (x->have_pattern)
            return dg_set(dg, PP_E_SYNTAX, "duplicate key pattern");
        x->have_pattern = 1;
        return parse_pattern(j, x->lim, x->r, dg);
    }
    return dg_set(dg, PP_E_SYNTAX, "unknown key %s", key);
}

int pp_route_from_json(const char *s, size_t len, const pp_limits_t *lim, pp_route_t *out, pp_diag_t *dg)
{
    js_t j;
    routectx_t x;
    int rc;
    memset(out, 0, sizeof *out);
    memset(&x, 0, sizeof x);
    x.lim = lim;
    x.r = out;
    j.p = s;
    j.e = s + len;
    rc = jobject(&j, route_key, &x, dg);
    if (rc != PP_OK) {
        memset(out, 0, sizeof *out);
        return rc;
    }
    sk(&j);
    if (j.p != j.e)
        return dg_set(dg, PP_E_SYNTAX, "text after the object");
    if (!x.have_name || !pp_name_ok(x.name))
        return dg_set(dg, PP_E_NAME, "route name: a-z first, then a-z 0-9 _, 1..%d characters", PP_NAME_MAX);
    if (x.have_steps == x.have_pattern)
        return dg_set(dg, PP_E_SYNTAX, "exactly one of steps or pattern");
    out->mode = x.have_mode ? pp_mode_parse(x.mode) : PP_LOOP;
    if (out->mode < 0)
        return dg_set(dg, PP_E_MODE, "mode is once, loop or pingpong");
    cpy(out->name, x.name);
    if (out->nsteps < 1)
        return dg_set(dg, PP_E_EMPTY, "a route needs at least one step");
    return PP_OK;
}

typedef struct {
    jf_t f[4];
    long pan, tilt;
    int here;
    char name[PP_NAME_MAX + 2];
} presetctx_t;

static int preset_key(js_t *j, const char *key, void *c, pp_diag_t *dg)
{
    presetctx_t *p = c;
    int rc = jfield(j, p->f, 4, key, dg);
    return rc == 1 ? dg_set(dg, PP_E_SYNTAX, "unknown key %s", key) : rc;
}

int pp_preset_from_json(const char *s, size_t len, const char *dflt, char *name, int *pan, int *tilt, int *here, pp_diag_t *dg)
{
    js_t j;
    presetctx_t p;
    int rc;
    memset(&p, 0, sizeof p);
    p.f[0] = (jf_t){ "name", F_STR, NULL, p.name, sizeof p.name, NULL, 0 };
    p.f[1] = (jf_t){ "pan", F_INT, &p.pan, NULL, 0, NULL, 0 };
    p.f[2] = (jf_t){ "tilt", F_INT, &p.tilt, NULL, 0, NULL, 0 };
    p.f[3] = (jf_t){ "here", F_BOOL, NULL, NULL, 0, &p.here, 0 };
    j.p = s;
    j.e = s + len;
    rc = jobject(&j, preset_key, &p, dg);
    if (rc != PP_OK)
        return rc;
    sk(&j);
    if (j.p != j.e)
        return dg_set(dg, PP_E_SYNTAX, "text after the object");
    if (!p.f[0].seen && dflt)
        cpy(p.name, dflt);
    else if (p.f[0].seen && dflt && strcmp(p.name, dflt))
        return dg_set(dg, PP_E_NAME, "the name differs from the one in the path");
    if ((!p.f[0].seen && !dflt) || !pp_name_ok(p.name))
        return dg_set(dg, PP_E_NAME, "preset name: a-z first, then a-z 0-9 _, 1..%d characters", PP_NAME_MAX);
    *here = p.f[3].seen && p.here;
    if (*here) {
        if (p.f[1].seen || p.f[2].seen)
            return dg_set(dg, PP_E_SYNTAX, "here excludes pan and tilt");
        *pan = *tilt = 0;
    } else {
        if (!p.f[1].seen || !p.f[2].seen)
            return dg_set(dg, PP_E_SYNTAX, "pan and tilt (or here:true) are required");
        if (!small(p.pan) || !small(p.tilt))
            return dg_set(dg, PP_E_RANGE, "pan / tilt out of bounds");
        *pan = (int)p.pan;
        *tilt = (int)p.tilt;
    }
    cpy(name, p.name);
    return PP_OK;
}

/* ------------------------------------------------------------------ run */
const char *pp_phase_name(const pp_t *p) { return p->phase == PP_RUNNING ? "running" : p->phase == PP_PAUSED ? "paused" : "idle"; }

const char *pp_event_name(int t)
{
    switch (t) {
    case PP_EV_STARTED: return "started";
    case PP_EV_STEP: return "step";
    case PP_EV_INTERRUPTED: return "interrupted";
    case PP_EV_RESUMED: return "resumed";
    default: return "stopped";
    }
}

void pp_init(pp_t *p) { memset(p, 0, sizeof *p); }

static void out_clear(pp_out_t *o)
{
    o->act = PP_ACT_NONE;
    o->pan = o->tilt = o->speed = 0;
    o->nev = 0;
}

static void ev_add(pp_out_t *o, int type, int step, const char *reason)
{
    if (o->nev < 3) {
        o->ev[o->nev].type = type;
        o->ev[o->nev].step = step;
        o->ev[o->nev].reason = reason;
        o->nev++;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void pp_stop(pp_t *p, const char *reason, pp_out_t *out)
{
    out_clear(out);
    if (p->phase == PP_IDLE)
        return;
    if (p->phase == PP_RUNNING && p->sub == PP_ST_MOVING)
        out->act = PP_ACT_CANCEL;
    ev_add(out, PP_EV_STOPPED, p->step, reason);
    p->phase = PP_IDLE;
    p->have_target = 0;
}

int pp_start(pp_t *p, const pp_cfg_t *c, const pp_limits_t *lim, const char *route, int64_t now_ms, pp_out_t *out)
{
    int idx = route ? pp_find_route(c, route) : -1, i;
    pp_route_t tmp;
    out_clear(out);
    if (idx < 0)
        return PP_E_NOTFOUND;
    tmp = c->route[idx];
    if (tmp.nsteps < 1 || tmp.nsteps > PP_STEPS)
        return PP_E_EMPTY;
    for (i = 0; i < tmp.nsteps; i++) {
        pp_step_t *s = &tmp.step[i];
        if (s->preset[0]) {
            int k = pp_find_preset(c, s->preset);
            if (k < 0)
                return PP_E_NOPRESET;
            s->pan = c->preset[k].pan;
            s->tilt = c->preset[k].tilt;
            s->preset[0] = '\0';
        }
        s->pan = clampi(s->pan, lim->pan_min, lim->pan_max); /* the soft range of the model, whatever was stored */
        s->tilt = clampi(s->tilt, lim->tilt_min, lim->tilt_max);
        if (near_park(lim, s->pan, s->tilt))
            return PP_E_RANGE; /* stored under other limits: never run a step that would park the head */
        s->speed = s->speed == 0 ? clampi(lim->speed_def, lim->speed_min, lim->speed_max) : clampi(s->speed, lim->speed_min, lim->speed_max);
    }
    if (p->phase != PP_IDLE)
        pp_stop(p, "replaced", out);
    p->lim = *lim;
    p->route = tmp;
    p->phase = PP_RUNNING;
    p->sub = PP_ST_PENDING;
    p->step = 0;
    p->dir = 1;
    p->fails = 0;
    p->cycles = 0;
    p->have_target = 0;
    p->dwell_until = p->retry_at = 0;
    p->busy_since = 0;
    p->started_at = now_ms;
    p->last_step_ev = now_ms - PP_STEP_EVENT_GAP_MS;
    ev_add(out, PP_EV_STARTED, 0, NULL);
    return PP_OK;
}

void pp_interrupt(pp_t *p, int64_t now_ms, pp_out_t *out)
{
    out_clear(out);
    if (p->phase != PP_RUNNING)
        return;
    if (p->sub == PP_ST_MOVING)
        out->act = PP_ACT_CANCEL;
    p->phase = PP_PAUSED;
    p->sub = PP_ST_PENDING; /* the resume goes back to the interrupted step */
    p->have_target = 0;
    p->paused_at = now_ms;
    ev_add(out, PP_EV_INTERRUPTED, p->step, "tracking");
}

void pp_resume(pp_t *p, int64_t now_ms, pp_out_t *out)
{
    (void)now_ms;
    out_clear(out);
    if (p->phase != PP_PAUSED)
        return;
    p->phase = PP_RUNNING;
    p->sub = PP_ST_PENDING;
    p->retry_at = 0;
    p->fails = 0;
    p->busy_since = 0;
    ev_add(out, PP_EV_RESUMED, p->step, NULL);
}

static void want_move(pp_t *p, pp_out_t *out)
{
    const pp_step_t *s = &p->route.step[p->step];
    out->act = PP_ACT_MOVE;
    out->pan = s->pan;
    out->tilt = s->tilt;
    out->speed = s->speed;
}

/* after the dwell: the next step by the mode; 0 when the route ended */
static int advance(pp_t *p)
{
    int n = p->route.nsteps;
    switch (p->route.mode) {
    case PP_ONCE:
        if (p->step + 1 >= n)
            return 0;
        p->step++;
        return 1;
    case PP_PINGPONG:
        if (n == 1) {
            p->cycles++;
            return 1;
        }
        if (p->dir > 0 && p->step + 1 >= n) {
            p->dir = -1;
            p->cycles++;
        } else if (p->dir < 0 && p->step == 0) {
            p->dir = 1;
            p->cycles++;
        }
        p->step += p->dir;
        return 1;
    default:
        if (++p->step >= n) {
            p->step = 0;
            p->cycles++;
        }
        return 1;
    }
}

static void stop_into(pp_t *p, const char *reason, pp_out_t *out)
{
    pp_out_t so;
    pp_stop(p, reason, &so);
    out->act = so.act;
    out->nev = so.nev;
    if (so.nev)
        out->ev[0] = so.ev[0];
}

void pp_tick(pp_t *p, const pp_in_t *in, pp_out_t *out)
{
    out_clear(out);
    if (p->phase != PP_RUNNING)
        return;
    if (p->sub == PP_ST_MOVING) {
        if (in->arrived) {
            unsigned dw = p->route.step[p->step].dwell_ms;
            p->have_target = 0;
            p->fails = 0;
            p->sub = PP_ST_DWELL;
            unsigned fl = p->route.mode == PP_ONCE ? PP_MIN_DWELL_MS : PP_LOOP_MIN_DWELL_MS;
            p->dwell_until = in->now_ms + (int64_t)(dw < fl ? fl : dw);
            if (in->now_ms - p->last_step_ev >= PP_STEP_EVENT_GAP_MS) {
                p->last_step_ev = in->now_ms;
                ev_add(out, PP_EV_STEP, p->step, NULL);
            }
        } else if (in->failed) {
            p->have_target = 0;
            if (++p->fails >= PP_MAX_FAILS) {
                stop_into(p, "move_failed", out);
                return;
            }
            p->sub = PP_ST_PENDING;
            p->retry_at = in->now_ms + PP_RETRY_MS;
        }
        return;
    }
    if (p->sub == PP_ST_DWELL) {
        if (in->now_ms < p->dwell_until)
            return;
        if (!advance(p)) {
            stop_into(p, "done", out);
            return;
        }
        p->sub = PP_ST_PENDING;
        p->retry_at = 0;
    }
    if (p->sub == PP_ST_PENDING && in->now_ms >= p->retry_at)
        want_move(p, out);
}

void pp_moved(pp_t *p, int status, int64_t now_ms, pp_out_t *out)
{
    out_clear(out);
    if (p->phase != PP_RUNNING || p->sub != PP_ST_PENDING)
        return;
    if (status == 0) {
        p->sub = PP_ST_MOVING;
        p->have_target = 1;
        p->busy_since = 0;
        return;
    }
    if (status > 0) { /* the controller is busy: retried at the next tick; a controller busy for too long ends the patrol */
        if (!p->busy_since)
            p->busy_since = now_ms;
        else if (now_ms - p->busy_since >= PP_BUSY_MAX_MS)
            pp_stop(p, "busy", out);
        return;
    }
    p->busy_since = 0;
    if (++p->fails >= PP_MAX_FAILS) {
        pp_stop(p, "move_failed", out);
        return;
    }
    p->retry_at = now_ms + PP_RETRY_MS;
}

int pp_idle_due(int idle_start_s, int64_t last_activity_ms, int64_t now_ms)
{
    return idle_start_s > 0 && now_ms - last_activity_ms >= (int64_t)idle_start_s * 1000;
}

int pp_resume_due(const pp_t *p, int64_t last_person_ms, int64_t resume_after_ms, int64_t now_ms)
{
    int64_t since = last_person_ms > p->paused_at ? last_person_ms : p->paused_at;
    return p->phase == PP_PAUSED && now_ms - since >= resume_after_ms;
}
