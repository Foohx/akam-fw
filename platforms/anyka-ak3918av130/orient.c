#include "orient.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define MAX_LISTENERS 4

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_flip, g_mirror;
static unsigned g_gen;
static int64_t g_change_ms = -1, g_test_ms = -1;
static void (*g_cb[MAX_LISTENERS])(int, int);
static int g_ncb;

static int64_t now_ms(void)
{
    struct timespec ts;
    if (g_test_ms >= 0)
        return g_test_ms;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void a2k_orient_get(int *flip, int *mirror)
{
    pthread_mutex_lock(&g_mu);
    if (flip)
        *flip = g_flip;
    if (mirror)
        *mirror = g_mirror;
    pthread_mutex_unlock(&g_mu);
}

void a2k_orient_set(int flip, int mirror)
{
    void (*cb[MAX_LISTENERS])(int, int);
    int n, i, changed;
    flip = flip ? 1 : 0;
    mirror = mirror ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    changed = flip != g_flip || mirror != g_mirror;
    if (changed) {
        g_flip = flip;
        g_mirror = mirror;
        g_gen++;
        g_change_ms = now_ms();
    }
    n = g_ncb;
    memcpy(cb, g_cb, sizeof cb);
    pthread_mutex_unlock(&g_mu);
    if (changed)
        for (i = 0; i < n; i++)
            cb[i](flip, mirror);
}

unsigned a2k_orient_gen(void)
{
    unsigned g;
    pthread_mutex_lock(&g_mu);
    g = g_gen;
    pthread_mutex_unlock(&g_mu);
    return g;
}

int a2k_orient_settling(void)
{
    int r;
    pthread_mutex_lock(&g_mu);
    r = g_change_ms >= 0 && now_ms() - g_change_ms < A2K_ORIENT_SETTLE_MS;
    pthread_mutex_unlock(&g_mu);
    return r;
}

int a2k_orient_listen(void (*fn)(int, int))
{
    int rc = -1;
    pthread_mutex_lock(&g_mu);
    if (fn && g_ncb < MAX_LISTENERS) {
        g_cb[g_ncb++] = fn;
        rc = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void a2k_orient_test_clock(int64_t ms)
{
    pthread_mutex_lock(&g_mu);
    g_test_ms = ms;
    pthread_mutex_unlock(&g_mu);
}

void a2k_orient_test_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_flip = g_mirror = 0;
    g_gen = 0;
    g_change_ms = -1;
    g_test_ms = -1;
    g_ncb = 0;
    memset(g_cb, 0, sizeof g_cb);
    pthread_mutex_unlock(&g_mu);
}

int a2k_orient_pan_sign(int s, int mirror) { return mirror ? -s : s; }
int a2k_orient_tilt_sign(int s, int flip) { return flip ? -s : s; }
int a2k_orient_invert_pan(int base, int mirror) { return (base ? 1 : 0) ^ (mirror ? 1 : 0); }
int a2k_orient_invert_tilt(int base, int flip) { return (base ? 1 : 0) ^ (flip ? 1 : 0); }

void a2k_orient_point(int flip, int mirror, int max_x, int max_y, int *x, int *y)
{
    if (mirror)
        *x = max_x - *x;
    if (flip)
        *y = max_y - *y;
}

void a2k_orient_rect(int flip, int mirror, int max_x, int max_y, int *x1, int *y1, int *x2, int *y2)
{
    int a = *x1, b = *y1, c = *x2, d = *y2;
    if (mirror) {
        *x1 = max_x - c;
        *x2 = max_x - a;
    }
    if (flip) {
        *y1 = max_y - d;
        *y2 = max_y - b;
    }
}

int a2k_orient_plane(int flip, int mirror, const uint8_t *src, unsigned w, unsigned h, unsigned src_stride, uint8_t *dst)
{
    unsigned r, c;
    if (!src || !dst || src == dst || !w || !h || src_stride < w)
        return -1;
    for (r = 0; r < h; r++) {
        const uint8_t *s = src + (size_t)(flip ? h - 1 - r : r) * src_stride;
        uint8_t *d = dst + (size_t)r * w;
        if (!mirror)
            memcpy(d, s, w);
        else
            for (c = 0; c < w; c++)
                d[c] = s[w - 1 - c];
    }
    return 0;
}
