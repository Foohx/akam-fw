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
static int g_nr2;
static int g_wdr = A2K_PIC_WDR_DEFAULT;
static int g_wdr_touched;         /* the user changed isp.wdr at least once: it is replayed (the stock's own replay covers the untouched case) */
static uint8_t *g_pristine2;      /* the nr2 attribute as the conf set loaded it, kept only while a level is active */
static int g_pristine2_ok;
static uint8_t *g_pristine;       /* the 3D NR attribute as the conf set loaded it (A2K_PIC_NR3D_SIZE bytes), kept only while a denoise bias is active */
static int g_pristine_ok;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void a2k_pic_init(const a2k_isptest_ops_t *ops) { g_ops = ops; }

void a2k_pic_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_sat = g_denoise = g_nr2 = 0;
    g_wdr = A2K_PIC_WDR_DEFAULT;
    g_wdr_touched = 0;
    free(g_pristine2);
    g_pristine2 = NULL;
    g_pristine2_ok = 0;
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

static int nr2_decodes(const uint8_t *in)
{
    uint32_t mode = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    int i;
    if (mode > 1)
        return 0;
    for (i = 0; i < A2K_REC_N; i++) {
        const uint8_t *r = in + 4 + i * A2K_NR2_REC;
        if (rd16(r) > 1 || rd16(r + 2) > 1 || rd16(r + 4) > 15 || rd16(r + 6) > 31 || rd16(r + 8) > 15 || rd16(r + 10) > 15)
            return 0;
    }
    return 1;
}

int a2k_pic_nr2_scale(const uint8_t *in, uint8_t *out, int level)
{
    int i, f;
    if (!nr2_decodes(in))
        return 1;
    memcpy(out, in, A2K_PIC_NR2_SIZE);
    if (level <= 0)
        return 0;
    /* Record 0 is the manual one (a plain +level), records 1..16 the gain-linked ones, low gain first: the step grows from about level * 5/8 to
     * level * 20/8, rounded to nearest, so the high-gain records, where a backlit shadow lives, get the strongest filter. */
    for (i = 0; i < A2K_REC_N; i++) {
        uint8_t *r = out + 4 + i * A2K_NR2_REC;
        int add = i == 0 ? level : (level * (i + 4) + 4) / 8;
        static const int FIELD[3] = { 4, 8, 10 }; /* nr2_k, ynr_intensity1, ynr_intensity2 */
        for (f = 0; f < 3; f++) {
            int v = rd16(r + FIELD[f]) + add;
            r[FIELD[f]] = (uint8_t)(v > 15 ? 15 : v);
            r[FIELD[f] + 1] = 0;
        }
    }
    return 0;
}

/* ---- the applications (mutex held) ---- */

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

/* Same shape as apply_denoise: fresh = the conf set was just reloaded. */
static int apply_nr2(int level, int fresh)
{
    uint8_t *buf;
    int rc = A2K_PIC_OK;
    if (!g_ops || !g_ops->get || !g_ops->set)
        return A2K_PIC_FAIL;
    if (fresh)
        g_pristine2_ok = 0;
    if (level == 0 && !g_pristine2_ok)
        return A2K_PIC_OK; /* never changed since the conf was loaded: the camera's own table is in place */
    buf = calloc(1, A2K_IT_BUF);
    if (!buf)
        return A2K_PIC_FAIL;
    if (!g_pristine2_ok) {
        if (!g_pristine2)
            g_pristine2 = malloc(A2K_PIC_NR2_SIZE);
        if (!g_pristine2 || g_ops->get(A2K_IT_NR2, buf) != 0) {
            rc = A2K_PIC_FAIL;
            goto out;
        }
        if (a2k_pic_nr2_scale(buf, g_pristine2, 0) != 0) { /* not as decoded: refuse before anything is sent */
            rc = A2K_PIC_REFUSED;
            goto out;
        }
        g_pristine2_ok = 1;
    }
    memset(buf, 0, A2K_IT_BUF);
    if (a2k_pic_nr2_scale(g_pristine2, buf, level) != 0) {
        rc = A2K_PIC_REFUSED;
        goto out;
    }
    if (g_ops->set(A2K_IT_NR2, buf) != 0)
        rc = A2K_PIC_FAIL;
out:
    free(buf);
    if (level == 0 && rc == A2K_PIC_OK) {
        free(g_pristine2);
        g_pristine2 = NULL;
        g_pristine2_ok = 0;
    }
    return rc;
}

static int apply_wdr(int strength)
{
    if (!g_ops || !g_ops->effect_set)
        return A2K_PIC_FAIL;
    return g_ops->effect_set(A2K_FX_WDR, strength - 50) == 0 ? A2K_PIC_OK : A2K_PIC_FAIL;
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

int a2k_pic_set_nr2(int level)
{
    int rc;
    if (level < 0 || level > A2K_PIC_NR2_MAX)
        return A2K_PIC_ARG;
    pthread_mutex_lock(&g_mu);
    rc = apply_nr2(level, 0);
    if (rc == A2K_PIC_OK)
        g_nr2 = level;
    pthread_mutex_unlock(&g_mu);
    a2k_log("picture: nr2 %d = %d", level, rc);
    return rc;
}

int a2k_pic_get_nr2(void) { return g_nr2; }

int a2k_pic_set_wdr(int strength)
{
    int rc = A2K_PIC_OK;
    if (strength < 0 || strength > A2K_PIC_WDR_MAX)
        return A2K_PIC_ARG;
    pthread_mutex_lock(&g_mu);
    if (g_wdr_touched || strength != A2K_PIC_WDR_DEFAULT) { /* the default on an untouched camera: the stock value is already in place, nothing is sent */
        rc = apply_wdr(strength);
        if (rc == A2K_PIC_OK)
            g_wdr_touched = 1;
    }
    if (rc == A2K_PIC_OK)
        g_wdr = strength;
    pthread_mutex_unlock(&g_mu);
    a2k_log("picture: wdr %d (effect %d) = %d", strength, strength - 50, rc);
    return rc;
}

int a2k_pic_get_wdr(void) { return g_wdr; }

int a2k_pic_wdr_effect(void) { return g_wdr - 50; }

/* A fixed white balance is refused on the 2K: the vendor API has no named "current AWB gains" to freeze (the AWB run info is unnamed u16 fields, the manual
 * gain attribute holds the conf's own unrelated values and its unit is unverified), and switching wb_type to manual with those gave a strong blue cast on a
 * camera (2026-11-24). Nothing is sent to the ISP. conf is always the state (the automatic white balance is never left). */
int a2k_pic_set_wb(int mode)
{
    if (mode == A2K_PIC_WB_CONF)
        return A2K_PIC_OK;
    if (mode == A2K_PIC_WB_MANUAL) {
        a2k_log("picture: white balance manual refused (no way to freeze the automatic gains, see isp-controls.md)");
        return A2K_PIC_REFUSED;
    }
    return A2K_PIC_ARG;
}

int a2k_pic_get_wb(void) { return A2K_PIC_WB_CONF; }

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
    if (g_nr2 != 0 && apply_nr2(g_nr2, 1) != A2K_PIC_OK)
        fails++;
    else if (g_nr2 == 0)
        g_pristine2_ok = 0;
    if (g_wdr_touched && apply_wdr(g_wdr) != A2K_PIC_OK) /* the stock's own replay (night_ctl) follows 100 ms later with the same value; this one covers --wdr 0 */
        fails++;
    pthread_mutex_unlock(&g_mu);
    if (g_sat != 0 || g_denoise != 0 || g_nr2 != 0 || g_wdr_touched)
        a2k_log("picture: replayed after the %s switch (%d failure(s))", night ? "night" : "day", fails);
    return fails;
}
