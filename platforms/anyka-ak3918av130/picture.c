/* See picture.h. */
#include "picture.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static const a2k_isptest_ops_t *g_ops;
static int g_sat;                 /* user's values (RAM) */
static int g_denoise;
static int g_wb = A2K_PIC_WB_CONF;
static int g_auto_type = -1;      /* wb_type read before the first change: the ISP's own automatic white balance */
static uint8_t *g_pristine;       /* the 3D NR attribute as the conf set loaded it (A2K_PIC_NR3D_SIZE bytes), kept only while a denoise bias is active */
static int g_pristine_ok;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void a2k_pic_init(const a2k_isptest_ops_t *ops) { g_ops = ops; }

void a2k_pic_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_sat = g_denoise = 0;
    g_wb = A2K_PIC_WB_CONF;
    g_auto_type = -1;
    free(g_pristine);
    g_pristine = NULL;
    g_pristine_ok = 0;
    pthread_mutex_unlock(&g_mu);
}

int a2k_pic_nr3d_shift(const uint8_t *in, uint8_t *out, int bias)
{
    int i;
    uint32_t mode = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (mode > 1)
        return 1;
    for (i = 0; i < A2K_REC_N; i++) {
        const uint8_t *r = in + 4 + i * A2K_NR3D_REC;
        if (rd16(r) > 1 || rd16(r + 2) > 1) /* tnr_enable, tnr_refFrame_format */
            return 1;
    }
    memcpy(out, in, A2K_PIC_NR3D_SIZE);
    if (bias <= 0)
        return 0;
    /* Record 0 is the manual one, 1..16 the gain-linked ones (low gain first): linked record i takes the record `bias` steps higher, the last ones
     * stay. tnr_enable is never lowered by the borrowed record (a level that had the temporal filter on keeps it). */
    for (i = 1; i < A2K_REC_N; i++) {
        int src = i + bias > A2K_REC_N - 1 ? A2K_REC_N - 1 : i + bias;
        uint8_t *dst = out + 4 + i * A2K_NR3D_REC;
        uint16_t keep = rd16(in + 4 + i * A2K_NR3D_REC);
        memcpy(dst, in + 4 + src * A2K_NR3D_REC, A2K_NR3D_REC);
        if (keep > rd16(dst)) {
            dst[0] = (uint8_t)(keep & 0xff);
            dst[1] = (uint8_t)(keep >> 8);
        }
    }
    return 0;
}

/* ---- the three applications (mutex held) ---- */

static int apply_sat(int v)
{
    if (!g_ops || !g_ops->effect_set)
        return A2K_PIC_FAIL;
    return g_ops->effect_set(A2K_FX_SATURATION, v) == 0 ? A2K_PIC_OK : A2K_PIC_FAIL;
}

/* fresh: the conf set was just (re)loaded, so what the ISP holds is the original. */
static int apply_denoise(int bias, int fresh)
{
    uint8_t *buf;
    int rc = A2K_PIC_OK;
    if (!g_ops || !g_ops->get || !g_ops->set)
        return A2K_PIC_FAIL;
    if (fresh)
        g_pristine_ok = 0;
    if (bias == 0 && !g_pristine_ok)
        return A2K_PIC_OK; /* never changed since the conf was loaded: the camera's own tables are in place */
    buf = calloc(1, A2K_IT_BUF);
    if (!buf)
        return A2K_PIC_FAIL;
    if (!g_pristine_ok) {
        if (!g_pristine)
            g_pristine = malloc(A2K_PIC_NR3D_SIZE);
        if (!g_pristine || g_ops->get(A2K_IT_NR3D, buf) != 0) {
            rc = A2K_PIC_FAIL;
            goto out;
        }
        if (a2k_pic_nr3d_shift(buf, g_pristine, 0) != 0) { /* not as decoded: refuse before anything is sent */
            rc = A2K_PIC_REFUSED;
            goto out;
        }
        g_pristine_ok = 1;
    }
    if (a2k_pic_nr3d_shift(g_pristine, buf, bias) != 0) {
        rc = A2K_PIC_REFUSED;
        goto out;
    }
    if (g_ops->set(A2K_IT_NR3D, buf) != 0)
        rc = A2K_PIC_FAIL;
out:
    free(buf);
    if (bias == 0 && rc == A2K_PIC_OK) { /* back on the original: nothing to keep */
        free(g_pristine);
        g_pristine = NULL;
        g_pristine_ok = 0;
    }
    return rc;
}

static int read_type(int *t)
{
    uint8_t *buf = calloc(1, A2K_IT_BUF);
    int rc;
    if (!buf)
        return -1;
    rc = g_ops->get(A2K_IT_WBTYPE, buf);
    if (rc == 0) {
        int v = (int)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((unsigned)buf[3] << 24));
        if (v == 0 || v == 1)
            *t = v;
        else
            rc = -1;
    }
    free(buf);
    return rc;
}

static int apply_wb(int mode, int night)
{
    int t;
    if (!g_ops || !g_ops->get || !g_ops->set)
        return A2K_PIC_FAIL;
    if (g_auto_type < 0 && mode == A2K_PIC_WB_CONF)
        return A2K_PIC_OK; /* never left the automatic one */
    if (g_auto_type < 0) {
        if (read_type(&g_auto_type) != 0) { /* the type read now, before our first change, is the automatic one (AWB running) */
            g_auto_type = -1;
            return A2K_PIC_FAIL;
        }
    }
    if (mode == A2K_PIC_WB_MANUAL && night)
        mode = A2K_PIC_WB_CONF; /* the night set is mono: the manual white balance is a day setting (the 1080p does the same) */
    t = mode == A2K_PIC_WB_MANUAL ? 1 - g_auto_type : g_auto_type;
    {
        int32_t v = t;
        return g_ops->set(A2K_IT_WBTYPE, &v) == 0 ? A2K_PIC_OK : A2K_PIC_FAIL;
    }
}

/* ---- public ---- */

int a2k_pic_set_saturation(int v)
{
    int rc;
    if (v < A2K_PIC_SAT_MIN || v > A2K_PIC_SAT_MAX)
        return A2K_PIC_ARG;
    pthread_mutex_lock(&g_mu);
    rc = apply_sat(v);
    if (rc == A2K_PIC_OK)
        g_sat = v;
    pthread_mutex_unlock(&g_mu);
    a2k_log("picture: saturation %d = %d", v, rc);
    return rc;
}

int a2k_pic_get_saturation(void) { return g_sat; }

int a2k_pic_set_denoise(int bias)
{
    int rc;
    if (bias < 0 || bias > A2K_PIC_DENOISE_MAX)
        return A2K_PIC_ARG;
    pthread_mutex_lock(&g_mu);
    rc = apply_denoise(bias, 0);
    if (rc == A2K_PIC_OK)
        g_denoise = bias;
    pthread_mutex_unlock(&g_mu);
    a2k_log("picture: denoise %d = %d", bias, rc);
    return rc;
}

int a2k_pic_get_denoise(void) { return g_denoise; }

int a2k_pic_set_wb(int mode)
{
    int rc;
    if (mode != A2K_PIC_WB_CONF && mode != A2K_PIC_WB_MANUAL)
        return A2K_PIC_ARG;
    pthread_mutex_lock(&g_mu);
    rc = apply_wb(mode, 0);
    if (rc == A2K_PIC_OK)
        g_wb = mode;
    pthread_mutex_unlock(&g_mu);
    a2k_log("picture: white balance %s = %d", mode == A2K_PIC_WB_MANUAL ? "manual" : "conf", rc);
    return rc;
}

int a2k_pic_get_wb(void) { return g_wb; }

int a2k_pic_replay(int night)
{
    int fails = 0;
    pthread_mutex_lock(&g_mu);
    if (g_sat != 0 && apply_sat(g_sat) != A2K_PIC_OK)
        fails++;
    if (g_denoise != 0 && apply_denoise(g_denoise, 1) != A2K_PIC_OK)
        fails++;
    else if (g_denoise == 0)
        g_pristine_ok = 0;
    if (g_wb == A2K_PIC_WB_MANUAL && apply_wb(g_wb, night) != A2K_PIC_OK)
        fails++;
    pthread_mutex_unlock(&g_mu);
    if (g_sat != 0 || g_denoise != 0 || g_wb != A2K_PIC_WB_CONF)
        a2k_log("picture: replayed after the %s switch (%d failure(s))", night ? "night" : "day", fails);
    return fails;
}
