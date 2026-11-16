/* See scene_check.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene_check.h"

const char sc_invalid_marker[] = "invalid\n";

const char *sc_verdict_name(int v)
{
    switch (v) {
    case SC_MATCH: return "match";
    case SC_NO_REF: return "no saved reference";
    case SC_LOW_DETAIL_REF: return "reference has too little detail";
    case SC_LOW_DETAIL_CUR: return "current view has too little detail";
    case SC_MODE_DIFF: return "day/night mode differs";
    case SC_MISMATCH: return "scene differs";
    case SC_MISMATCH_BLOCKS: return "too few image blocks match on their own (a fixed pattern?)";
    case SC_PERIODIC: return "periodic full calibration due";
    case SC_NO_FRAME: return "no usable frame in time";
    case SC_CANCELLED: return "cancelled";
    default: return "?";
    }
}

int sc_downscale_y(const uint8_t *y, unsigned w, unsigned h, unsigned stride, sc_thumb_t *out)
{
    unsigned bx, by, x, yy, ox, oy;
    if (!y || !out || w < SC_W || h < SC_H || w % SC_W || h % SC_H || stride < w)
        return -1;
    bx = w / SC_W;
    by = h / SC_H;
    for (oy = 0; oy < SC_H; oy++)
        for (ox = 0; ox < SC_W; ox++) {
            unsigned long sum = 0;
            for (yy = 0; yy < by; yy++)
                for (x = 0; x < bx; x++)
                    sum += y[(size_t)(oy * by + yy) * stride + ox * bx + x];
            out->px[oy * SC_W + ox] = (uint8_t)((sum + bx * by / 2) / (bx * by));
        }
    return 0;
}

/* detail of the region [x0,x1) x [y0,y1) */
static int region_detail(const sc_thumb_t *t, int x0, int y0, int x1, int y1)
{
    long long n = (long long)(x1 - x0) * (y1 - y0), s = 0, ss = 0, g = 0, ng = 0;
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            int p = t->px[y * SC_W + x];
            s += p;
            ss += (long long)p * p;
        }
    if ((ss * n - s * s) < (long long)SC_MIN_SD * SC_MIN_SD * n * n)
        return 0;
    for (y = y0; y < y1 - 1; y++)
        for (x = x0; x < x1 - 1; x++) {
            int p = t->px[y * SC_W + x];
            g += abs(p - t->px[y * SC_W + x + 1]) + abs(p - t->px[(y + 1) * SC_W + x]);
            ng++;
        }
    return g * 10 >= (long long)SC_MIN_GRAD_X10 * ng;
}

int sc_has_detail(const sc_thumb_t *t) { return region_detail(t, 0, 0, SC_W, SC_H); }

/* square root by Newton (the camera libc needs no libm for this) */
static double dsqrt(double x)
{
    double r = x > 1.0 ? x : 1.0;
    if (x <= 0.0)
        return 0.0;
    for (int i = 0; i < 60; i++)
        r = 0.5 * (r + x / r);
    return r;
}

/* best ZNCC (permille) of the region of ref against cur shifted by -SC_SHIFT..SC_SHIFT; the region is clipped so that every shift stays inside */
static int region_score(const sc_thumb_t *ref, const sc_thumb_t *cur, int x0, int y0, int x1, int y1)
{
    int best = -1000, dx, dy, x, y;
    if (x0 < SC_SHIFT) x0 = SC_SHIFT;
    if (y0 < SC_SHIFT) y0 = SC_SHIFT;
    if (x1 > SC_W - SC_SHIFT) x1 = SC_W - SC_SHIFT;
    if (y1 > SC_H - SC_SHIFT) y1 = SC_H - SC_SHIFT;
    for (dy = -SC_SHIFT; dy <= SC_SHIFT; dy++)
        for (dx = -SC_SHIFT; dx <= SC_SHIFT; dx++) {
            long long n = 0, sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, va, vb, cov;
            int sc;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    int a = ref->px[y * SC_W + x], b = cur->px[(y + dy) * SC_W + x + dx];
                    n++;
                    sa += a;
                    sb += b;
                    saa += a * a;
                    sbb += b * b;
                    sab += a * b;
                }
            va = n * saa - sa * sa;
            vb = n * sbb - sb * sb;
            cov = n * sab - sa * sb;
            if (va <= 0 || vb <= 0)
                continue;
            {
                double r = (double)cov / dsqrt((double)va * (double)vb) * 1000.0;
                sc = (int)(r < 0 ? r - 0.5 : r + 0.5);
            }
            if (sc > best)
                best = sc;
        }
    return best;
}

int sc_score(const sc_thumb_t *ref, const sc_thumb_t *cur) { return region_score(ref, cur, 0, 0, SC_W, SC_H); }

int sc_blocks_ok(const sc_thumb_t *ref, const sc_thumb_t *cur, int min_permille)
{
    int ok = 0, bx, by, bmin = min_permille - SC_BLOCK_MARGIN;
    const int bw = SC_W / SC_BX, bh = SC_H / SC_BY;
    if (bmin < 500)
        bmin = 500;
    for (by = 0; by < SC_BY; by++)
        for (bx = 0; bx < SC_BX; bx++) {
            int x0 = bx * bw, y0 = by * bh, x1 = x0 + bw, y1 = y0 + bh;
            if (region_detail(ref, x0, y0, x1, y1) && region_detail(cur, x0, y0, x1, y1) &&
                region_score(ref, cur, x0, y0, x1, y1) >= bmin)
                ok++;
        }
    return ok;
}

int sc_decide(const sc_ref_t *ref, const sc_thumb_t *cur, int cur_mode, int min_permille, int *score)
{
    int s;
    if (score)
        *score = -1000;
    if (!ref || !ref->valid)
        return SC_NO_REF;
    if (!sc_has_detail(&ref->th))
        return SC_LOW_DETAIL_REF;
    if (!sc_has_detail(cur))
        return SC_LOW_DETAIL_CUR;
    if (cur_mode < 0 || cur_mode != ref->mode)
        return SC_MODE_DIFF;
    s = sc_score(&ref->th, cur);
    if (score)
        *score = s;
    if (s < min_permille)
        return SC_MISMATCH;
    /* a pattern fixed to the camera (lens dirt, an overlay, a sensor artefact) correlates perfectly whatever the scene: most of the 4x3
     * blocks must each have detail AND match on their own */
    return sc_blocks_ok(&ref->th, cur, min_permille) >= SC_BLOCKS_MIN ? SC_MATCH : SC_MISMATCH_BLOCKS;
}

int sc_parse_min(const char *s)
{
    int v = 0, n = 0;
    if (!s)
        return -1;
    if (!strcmp(s, "1") || !strcmp(s, "1.0") || !strcmp(s, "1.00") || !strcmp(s, "1.000"))
        return 1000;
    if (s[0] != '0' || s[1] != '.')
        return -1;
    for (s += 2; *s >= '0' && *s <= '9' && n < 3; s++, n++)
        v = v * 10 + (*s - '0');
    if (*s || n == 0)
        return -1;
    for (int k = n; k < 3; k++)
        v *= 10;
    return v >= SC_MIN_LOWEST ? v : -1;
}

size_t sc_pack(const sc_ref_t *ref, char *out, size_t cap)
{
    int h = snprintf(out, cap, "scene1 %d %u %u %d %d %u\n", ref->mode ? 1 : 0, (unsigned)ref->pan, (unsigned)ref->tilt, SC_W, SC_H,
                    (unsigned)ref->boots);
    if (h < 0 || (size_t)h + SC_N > cap)
        return 0;
    memcpy(out + h, ref->th.px, SC_N);
    return (size_t)h + SC_N;
}

/* one unsigned field of the header, followed by `sep` */
static int field(const char **p, unsigned long max, char sep, unsigned long *v)
{
    char *e;
    if (**p < '0' || **p > '9')
        return -1;
    *v = strtoul(*p, &e, 10);
    if (*e != sep || *v > max)
        return -1;
    *p = e + (sep ? 1 : 0);
    return 0;
}

int sc_unpack(const char *buf, size_t len, sc_ref_t *ref)
{
    char hdr[80];
    const char *nl = memchr(buf, '\n', len < sizeof hdr ? len : sizeof hdr - 1), *p;
    unsigned long mode, pan, tilt, w, h, boots;
    size_t hl;
    memset(ref, 0, sizeof *ref);
    if (!nl)
        return -1;
    hl = (size_t)(nl - buf) + 1;
    if (len != hl + SC_N)
        return -1;
    memcpy(hdr, buf, hl - 1);
    hdr[hl - 1] = '\0';
    if (strncmp(hdr, "scene1 ", 7))
        return -1;
    p = hdr + 7;
    if (field(&p, 1, ' ', &mode) || field(&p, 65535, ' ', &pan) || field(&p, 65535, ' ', &tilt) || field(&p, SC_W, ' ', &w) ||
        field(&p, SC_H, ' ', &h) || field(&p, 1000000, '\0', &boots) || w != SC_W || h != SC_H)
        return -1;
    ref->mode = (int)mode;
    ref->pan = (uint32_t)pan;
    ref->tilt = (uint32_t)tilt;
    ref->boots = (unsigned)boots;
    memcpy(ref->th.px, buf + hl, SC_N);
    ref->valid = 1;
    return 0;
}

static void slog(const sc_env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void slog(const sc_env_t *e, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    if (!e->log)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->log(line);
}

/* "0.934" / "-0.120" */
static const char *fmt3(char *b, size_t n, int permille)
{
    if (permille < 0)
        snprintf(b, n, "-%d.%03d", abs(permille) / 1000, abs(permille) % 1000);
    else
        snprintf(b, n, "%d.%03d", permille / 1000, permille % 1000);
    return b;
}

int sc_boot_check(const sc_env_t *e, const sc_cfg_t *c, uint32_t *pan, uint32_t *tilt)
{
    static sc_ref_t ref; /* 3.9 KB each: not on the (small) thread stack; one boot check per process */
    static sc_thumb_t cur;
    int64_t t0 = e->now_ms(), t_first = -1, t_mode = -1;
    unsigned seq = 0, last_seq = 0, frames = 0;
    int mode, last_mode = -2, v, score = -1000, have = 0;
    char b1[16], b2[16];
    if (e->load(&ref) != 0 || !ref.valid) {
        slog(e, "ptz: scene check: no saved reference: calibrating");
        return SC_NO_REF;
    }
    if (ref.boots >= SC_FULL_EVERY) {
        slog(e, "ptz: scene check: the reference was trusted for %u boots: periodic full calibration", ref.boots);
        return SC_PERIODIC;
    }
    for (;;) {
        int64_t now = e->now_ms();
        if (e->cancelled && e->cancelled()) {
            v = SC_CANCELLED;
            break;
        }
        if (now - t0 > (int64_t)c->timeout_ms) {
            v = SC_NO_FRAME;
            break;
        }
        mode = e->mode();
        if (e->frame(&cur, &seq) == 0) {
            if (!have || seq != last_seq) {
                frames++;
                last_seq = seq;
            }
            have = 1;
            if (t_first < 0)
                t_first = now;
        }
        if (mode != last_mode) { /* the day/night state changed (or became known): the exposure restarts */
            last_mode = mode;
            t_mode = now;
            frames = have ? 1 : 0;
        }
        if (have && mode >= 0 && now - t_first >= (int64_t)c->settle_ms && now - t_mode >= (int64_t)c->settle_ms &&
            frames >= c->settle_frames) {
            v = sc_decide(&ref, &cur, mode, c->min_permille, &score);
            break;
        }
        e->sleep_ms(100);
    }
    if (v == SC_MATCH) {
        ref.boots++;
        if (e->store && e->store(&ref) != 0) { /* the counter cannot advance: never trust a reference that would live forever */
            slog(e, "ptz: scene check: match, but the boot counter cannot be saved: calibrating");
            return SC_STORE_FAILED;
        }
        *pan = ref.pan;
        *tilt = ref.tilt;
        slog(e, "ptz: scene check: MATCH, score %s >= %s (boot %u of %d since the last full calibration): the head did not move, position %u/%u declared, no calibration",
             fmt3(b1, sizeof b1, score), fmt3(b2, sizeof b2, c->min_permille), ref.boots, SC_FULL_EVERY, (unsigned)ref.pan, (unsigned)ref.tilt);
    } else if (score > -1000) {
        slog(e, "ptz: scene check: %s (score %s < %s): calibrating", sc_verdict_name(v), fmt3(b1, sizeof b1, score),
             fmt3(b2, sizeof b2, c->min_permille));
    } else {
        slog(e, "ptz: scene check: %s: calibrating", sc_verdict_name(v));
    }
    return v;
}
