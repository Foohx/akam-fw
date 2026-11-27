/* See motion_grid.h. Integer arithmetic only, no allocation. */
#include "motion_grid.h"

#include <string.h>

#define NOISE_CLEAR_MS 600   /* no motion this long: the centroid history is dropped */
#define ENTER_X10 10         /* a direction is set after a net centroid travel of this many cells x 10 over the history (1.0 cell) */
#define KEEP_X10 5           /* ... and kept while it is above this (0.5 cell): the hysteresis */
#define AXIS_RATIO_X10 13    /* the dominant axis must win by this factor x 10 */
#define MD_ZERO_MAX 100      /* the statistics all zero this many frames in a row: not usable */

void mg_defaults(struct mg_cfg *c) {
    c->sensitivity = 5;
    c->min_cells = 2;
}

void mg_set_sensitivity(mg_t *m, int s) { m->cfg.sensitivity = s < 1 ? 1 : s > 10 ? 10 : s; }

void mg_reset(mg_t *m) {
    memset(&m->st, 0, sizeof m->st);
    m->hn = 0;
    m->dir_cur = MG_DIR_NONE;
    m->prev_ok = 0;
    m->base_ok = 0;
    m->mask_until = 0;
}

void mg_init(mg_t *m, const struct mg_cfg *c) {
    memset(m, 0, sizeof *m);
    if (c) m->cfg = *c; else mg_defaults(&m->cfg);
    m->usable = 1;
}

void mg_mask(mg_t *m, int64_t now_ms, int ms) {
    int64_t u = now_ms + ms;
    if (u > m->mask_until) m->mask_until = u;
    m->hn = 0;
    m->dir_cur = MG_DIR_NONE;
    m->st.active = 0;
    m->st.cells = 0;
    m->st.dir = MG_DIR_NONE;
}

const struct mg_state *mg_get(const mg_t *m) { return &m->st; }

const char *mg_dir_name(int d) {
    switch (d) {
    case MG_DIR_LEFT: return "left";
    case MG_DIR_RIGHT: return "right";
    case MG_DIR_UP: return "up";
    case MG_DIR_DOWN: return "down";
    default: return "none";
    }
}

void mg_grid_orient(int flip, int mirror, const uint8_t *in, uint8_t *out) {
    int x, y;
    for (y = 0; y < MG_H; y++)
        for (x = 0; x < MG_W; x++)
            out[y * MG_W + x] = in[(flip ? MG_H - 1 - y : y) * MG_W + (mirror ? MG_W - 1 - x : x)];
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* the direction from the centroid history, with hysteresis; MG_DIR_NONE when there is none */
static int direction(mg_t *m) {
    int n = m->hn, dx, dy, ax, ay, d = MG_DIR_NONE;
    if (n < 3) return m->dir_cur;
    dx = (m->hx[n - 1] - m->hx[0]) * MG_W / 100; /* permille -> cells x 10 */
    dy = (m->hy[n - 1] - m->hy[0]) * MG_H / 100;
    ax = iabs(dx);
    ay = iabs(dy);
    if (ax * 10 >= ay * AXIS_RATIO_X10 && ax >= ENTER_X10) d = dx < 0 ? MG_DIR_LEFT : MG_DIR_RIGHT;
    else if (ay * 10 >= ax * AXIS_RATIO_X10 && ay >= ENTER_X10) d = dy < 0 ? MG_DIR_UP : MG_DIR_DOWN;
    if (d != MG_DIR_NONE) return d;
    /* below the enter limit: the current direction survives while its axis still travels above the keep limit */
    if (m->dir_cur == MG_DIR_LEFT && dx <= -KEEP_X10 && ax >= ay) return m->dir_cur;
    if (m->dir_cur == MG_DIR_RIGHT && dx >= KEEP_X10 && ax >= ay) return m->dir_cur;
    if (m->dir_cur == MG_DIR_UP && dy <= -KEEP_X10 && ay >= ax) return m->dir_cur;
    if (m->dir_cur == MG_DIR_DOWN && dy >= KEEP_X10 && ay >= ax) return m->dir_cur;
    return MG_DIR_NONE;
}

static void drop_state(mg_t *m, int64_t now_ms, int masked) {
    m->st.masked = masked;
    m->st.active = 0;
    m->st.cells = 0;
    if (now_ms - m->st.last_active_ms > NOISE_CLEAR_MS || masked) { m->hn = 0; m->dir_cur = MG_DIR_NONE; }
    if (masked || now_ms - m->st.dir_ms > MG_DIR_HOLD_MS) m->st.dir = MG_DIR_NONE;
}

void mg_feed_cells(mg_t *m, int64_t now_ms, const uint8_t act[MG_N]) {
    int i, n = 0, sx = 0, sy = 0, x0 = MG_W, y0 = MG_H, x1 = -1, y1 = -1;
    if (now_ms < m->mask_until) { drop_state(m, now_ms, 1); m->st.valid = 1; return; }
    for (i = 0; i < MG_N; i++) {
        if (act[i] >= 128) {
            int x = i % MG_W, y = i / MG_W;
            n++; sx += x; sy += y;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    m->st.valid = 1;
    if (n * 100 > MG_N * MG_GLOBAL_PCT) { /* a global change: the light, the exposure, the IR cut, a shaken camera */
        mg_mask(m, now_ms, MG_GLOBAL_MASK_MS);
        drop_state(m, now_ms, 1);
        return;
    }
    if (n < m->cfg.min_cells) { drop_state(m, now_ms, 0); return; }
    m->st.masked = 0;
    m->st.active = 1;
    m->st.cells = n;
    m->st.x0 = x0; m->st.y0 = y0; m->st.x1 = x1; m->st.y1 = y1;
    m->st.cx = (sx * 1000 + n * 500) / (n * MG_W); /* the cell centre */
    m->st.cy = (sy * 1000 + n * 500) / (n * MG_H);
    m->st.last_active_ms = now_ms;
    if (m->hn >= MG_HIST) {
        memmove(m->hx, m->hx + 1, (MG_HIST - 1) * sizeof m->hx[0]);
        memmove(m->hy, m->hy + 1, (MG_HIST - 1) * sizeof m->hy[0]);
        m->hn = MG_HIST - 1;
    }
    m->hx[m->hn] = m->st.cx;
    m->hy[m->hn] = m->st.cy;
    m->hn++;
    m->dir_cur = direction(m);
    m->st.dir = m->dir_cur;
    if (m->dir_cur != MG_DIR_NONE) { m->st.dir_ms = now_ms; m->st.last_dir = m->dir_cur; }
}

void mg_feed_diff(mg_t *m, int64_t now_ms, const uint8_t *y, unsigned w, unsigned h, unsigned stride) {
    uint16_t cur[MG_N];
    uint8_t act[MG_N];
    int cx, cy, i, g = 0, thr4;
    if (!y || w < MG_W * 4 || h < MG_H * 4 || stride < w) return;
    for (cy = 0; cy < MG_H; cy++) {
        unsigned ya = cy * h / MG_H, yb = (cy + 1) * h / MG_H;
        for (cx = 0; cx < MG_W; cx++) {
            unsigned xa = cx * w / MG_W, xb = (cx + 1) * w / MG_W, xx, yy, s = 0, k = 0;
            for (yy = ya; yy < yb; yy += 2)
                for (xx = xa; xx < xb; xx += 2) { s += y[yy * stride + xx]; k++; }
            cur[cy * MG_W + cx] = (uint16_t)(k ? s * 4 / k : 0);
        }
    }
    if (!m->prev_ok || m->pw != w || m->ph != h) { /* the first frame, a stream restart, a size change: only a reference */
        memcpy(m->prev, cur, sizeof cur);
        m->prev_ok = 1;
        m->pw = w;
        m->ph = h;
        m->hn = 0;
        m->st.valid = 0;
        return;
    }
    for (i = 0; i < MG_N; i++) g += (int)cur[i] - (int)m->prev[i];
    g /= MG_N; /* the global shift: auto exposure, flicker */
    if (iabs(g) > 12 * 4) mg_mask(m, now_ms, MG_GLOBAL_MASK_MS); /* a day / night switch, a light */
    thr4 = (14 - m->cfg.sensitivity) * 4;
    for (i = 0; i < MG_N; i++) {
        int d = iabs((int)cur[i] - (int)m->prev[i] - g), a = d * 128 / thr4;
        act[i] = (uint8_t)(a > 255 ? 255 : a);
    }
    memcpy(m->prev, cur, sizeof cur);
    mg_feed_cells(m, now_ms, act);
}

int mg_md_usable(const mg_t *m) { return m->usable; }

void mg_feed_md(mg_t *m, int64_t now_ms, int rc, const void *buf, unsigned n, int frame, int flip, int mirror) {
    uint8_t act[MG_N], pic[MG_N];
    uint32_t w[MG_N];
    int i, nz = 0, masked = now_ms < m->mask_until;
    if (rc != 0 || !buf || n < MG_MD_BYTES) {
        if (++m->md_bad_n > 20) m->usable = 0;
        return;
    }
    m->md_bad_n = 0;
    memcpy(w, buf, sizeof w);
    for (i = 0; i < MG_N; i++) if (w[i]) nz++;
    if (!nz) {
        if (++m->md_zero_n >= MD_ZERO_MAX) m->usable = 0;
        return;
    }
    m->md_zero_n = 0;
    m->usable = 1;
    if (!m->base_ok || masked) { /* the reference follows the picture while masked: no burst of motion when the mask ends */
        for (i = 0; i < MG_N; i++) {
            if (!m->base_ok) m->dev[i] = 0;
            m->base[i] = w[i];
        }
        m->base_ok = 1;
        if (masked) {
            memset(act, 0, sizeof act);
            mg_feed_cells(m, now_ms, act);
        }
        return;
    }
    {
        uint32_t mult = (uint32_t)(11 - m->cfg.sensitivity); /* sensitivity 5: 6 deviations */
        for (i = 0; i < MG_N; i++) {
            uint32_t v = w[i], b = m->base[i], d = v > b ? v - b : b - v, lim = m->dev[i] * mult + b / 8 + 2;
            act[i] = d > lim ? 255 : 0;
            m->dev[i] = (uint32_t)((int32_t)m->dev[i] + ((int32_t)d - (int32_t)m->dev[i]) / 16);
            m->base[i] = (uint32_t)((int32_t)b + ((int32_t)v - (int32_t)b) / 16);
        }
    }
    if (frame == MG_FRAME_SENSOR) {
        mg_grid_orient(flip, mirror, act, pic);
        mg_feed_cells(m, now_ms, pic);
    } else {
        mg_feed_cells(m, now_ms, act);
    }
}
