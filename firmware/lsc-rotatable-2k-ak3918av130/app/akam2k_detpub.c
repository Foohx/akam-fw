/* See akam2k_detpub.h. */
#include "akam2k_detpub.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "akam2k_detect.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_hz = A2K_DP_HZ_DEFAULT;
static char g_latest[A2K_DP_JSON];

int a2k_dp_hz(void)
{
    int h;
    pthread_mutex_lock(&g_mu);
    h = g_hz;
    pthread_mutex_unlock(&g_mu);
    return h;
}

void a2k_dp_set_hz(int hz)
{
    pthread_mutex_lock(&g_mu);
    g_hz = hz < 0 ? 0 : hz > A2K_DP_HZ_MAX ? A2K_DP_HZ_MAX : hz;
    pthread_mutex_unlock(&g_mu);
}

void a2k_dp_set_latest(const char *json)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_latest, sizeof g_latest, "%s", json);
    pthread_mutex_unlock(&g_mu);
}

void a2k_dp_clear_latest(void)
{
    pthread_mutex_lock(&g_mu);
    g_latest[0] = '\0';
    pthread_mutex_unlock(&g_mu);
}

int a2k_dp_latest(char *out, size_t cap)
{
    int n;
    pthread_mutex_lock(&g_mu);
    n = snprintf(out, cap, "%s", g_latest[0] ? g_latest : "{\"ts_ms\":0,\"boxes\":[]}");
    pthread_mutex_unlock(&g_mu);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

void a2k_dp_init(a2k_dp_t *p)
{
    memset(p, 0, sizeof *p);
    p->last_ms = -1;
}

static int absi(int v) { return v < 0 ? -v : v; }

/* The per-mille box of a detector pixel box (x2 / y2 inclusive indexes become edges, so a one pixel box is not empty). */
static void to_pm(const struct a2k_dp_in *b, int pm[4])
{
    pm[0] = b->x1 * 1000 / (int)A2K_DET_W;
    pm[1] = b->y1 * 1000 / (int)A2K_DET_H;
    pm[2] = (b->x2 + 1 - b->x1) * 1000 / (int)A2K_DET_W;
    pm[3] = (b->y2 + 1 - b->y1) * 1000 / (int)A2K_DET_H;
}

/* Stable ids: a box takes the id of the nearest unclaimed box of the previous list whose centre is within a quarter of the frame, else -1
 * (a new id is drawn when the list is published). */
static void match_ids(const a2k_dp_t *p, int cur[][4], int n, int *id)
{
    int used[A2K_DP_MAX] = { 0 };
    int i, j;
    for (i = 0; i < n; i++) {
        int best = -1, bd = 250 * 250 + 1;
        int cx = cur[i][0] + cur[i][2] / 2, cy = cur[i][1] + cur[i][3] / 2;
        for (j = 0; j < p->n; j++) {
            int dx = cx - (p->pm[j][0] + p->pm[j][2] / 2), dy = cy - (p->pm[j][1] + p->pm[j][3] / 2), d = dx * dx + dy * dy;
            if (!used[j] && d < bd)
                bd = d, best = j;
        }
        id[i] = best >= 0 ? p->id[best] : -1;
        if (best >= 0)
            used[best] = 1;
    }
}

int a2k_dp_update(a2k_dp_t *p, int64_t now_ms, int64_t ts_ms, int hz, int blocked, const struct a2k_dp_in *b, int n, int pan_known, int pan,
                  int tilt, char *json, size_t cap)
{
    int cur[A2K_DP_MAX][4], id[A2K_DP_MAX], i, changed = 0, due, w, min_ms, next_id = p->next_id;
    size_t len;
    if (hz <= 0 || blocked) {
        a2k_dp_init(p);
        p->next_id = next_id;
        return 0;
    }
    if (hz > A2K_DP_HZ_MAX)
        hz = A2K_DP_HZ_MAX;
    min_ms = 1000 / hz < A2K_DP_MIN_MS ? A2K_DP_MIN_MS : 1000 / hz;
    if (n < 0)
        n = 0;
    if (n > A2K_DP_MAX)
        n = A2K_DP_MAX;
    due = p->last_ms < 0 || now_ms - p->last_ms >= min_ms;
    if (n == 0) {
        if (p->n == 0 || !due)
            return 0; /* nobody was published, or the one empty list waits for the rate limit */
    } else {
        for (i = 0; i < n; i++)
            to_pm(&b[i], cur[i]);
        match_ids(p, cur, n, id);
        if (n != p->n)
            changed = 1;
        for (i = 0; i < n && !changed; i++) {
            int j, k;
            for (j = 0; j < p->n && id[i] != p->id[j]; j++)
                ;
            if (j == p->n || b[i].ignored != p->ign[j] || b[i].target != p->tgt[j])
                changed = 1;
            for (k = 0; k < 4 && !changed; k++)
                if (absi(cur[i][k] - p->pm[j][k]) > A2K_DP_MOVE_PM)
                    changed = 1;
        }
        if (!due || (!changed && now_ms - p->last_ms < A2K_DP_KEEPALIVE_MS))
            return 0;
        for (i = 0; i < n; i++)
            if (id[i] < 0)
                id[i] = next_id, next_id = (next_id + 1) % 1000;
    }
    w = snprintf(json, cap, "{\"ts_ms\":%lld,\"w\":%u,\"h\":%u", (long long)ts_ms, A2K_DET_W, A2K_DET_H);
    if (w < 0 || (size_t)w >= cap)
        return 0;
    len = (size_t)w;
    if (pan_known) {
        w = snprintf(json + len, cap - len, ",\"pan\":%d,\"tilt\":%d", pan, tilt);
        if (w < 0 || (size_t)w >= cap - len)
            return 0;
        len += (size_t)w;
    }
    w = snprintf(json + len, cap - len, ",\"boxes\":[");
    if (w < 0 || (size_t)w >= cap - len)
        return 0;
    len += (size_t)w;
    for (i = 0; i < n; i++) {
        if (i && len + 1 < cap)
            json[len++] = ',';
        w = snprintf(json + len, cap - len, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"score\":%d,\"id\":%d,\"target\":%s,\"ignored\":%s}",
                     cur[i][0], cur[i][1], cur[i][2], cur[i][3], b[i].score, id[i], b[i].target ? "true" : "false",
                     b[i].ignored ? "true" : "false");
        if (w < 0 || (size_t)w >= cap - len)
            return 0;
        len += (size_t)w;
    }
    if (len + 3 > cap || len + 2 > 800)
        return 0;
    memcpy(json + len, "]}", 3);
    len += 2;
    /* remember what was published */
    p->n = n;
    p->next_id = next_id;
    for (i = 0; i < n; i++) {
        memcpy(p->pm[i], cur[i], sizeof cur[i]);
        p->id[i] = id[i];
        p->ign[i] = b[i].ignored;
        p->tgt[i] = b[i].target;
    }
    p->last_ms = now_ms;
    return (int)len;
}
