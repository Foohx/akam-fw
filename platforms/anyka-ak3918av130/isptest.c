#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "isptest.h"
#include "log.h"

extern volatile sig_atomic_t a2k_stop;

/* ---- parsing ---- */

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int word(const char **p, const char *w)
{
    size_t n = strlen(w);
    const char *s = skip(*p);
    if (strncmp(s, w, n) || (s[n] && s[n] != ' ' && s[n] != '\t' && s[n] != '\n' && s[n] != '\r'))
        return 0;
    *p = s + n;
    return 1;
}

/* A signed decimal of at most 6 digits. */
static int snum(const char **p, int lo, int hi, int *out)
{
    const char *s = skip(*p);
    long n = 0;
    int d = 0, neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9' && d < 6) {
        n = n * 10 + (*s++ - '0');
        d++;
    }
    if (!d || (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r'))
        return -1;
    if (neg)
        n = -n;
    if (n < lo || n > hi)
        return -1;
    *out = (int)n;
    *p = s;
    return 0;
}

static int at_end(const char *p)
{
    p = skip(p);
    while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
        p++;
    return *p == 0;
}

static const struct { const char *name; int what; } GET_NAMES[] = {
    { "all", A2K_IT_ALL }, { "fx", A2K_IT_FX }, { "yuv", A2K_IT_YUV }, { "wb", A2K_IT_WBTYPE }, { "ccm", A2K_IT_CCM }, { "nr1", A2K_IT_NR1 },
    { "nr2", A2K_IT_NR2 }, { "uvnr", A2K_IT_UVNR }, { "nr3d", A2K_IT_NR3D }, { "awb", A2K_IT_AWB }, { "rgb", A2K_IT_RGB }, { NULL, 0 },
};
static const struct { const char *name; int type; } FX_ALIASES[] = {
    { "hue", 0 }, { "bright", 1 }, { "sat", 2 }, { "contrast", 3 }, { "sharp", 4 }, { NULL, 0 },
};

int a2k_isptest_parse(const char *line, a2k_isptest_cmd_t *out)
{
    const char *p = line;
    a2k_isptest_cmd_t c;
    int i;
    if (!line)
        return -1;
    memset(&c, 0, sizeof c);
    if (word(&p, "get")) {
        c.kind = A2K_ITC_GET;
        for (i = 0; GET_NAMES[i].name; i++)
            if (word(&p, GET_NAMES[i].name)) {
                c.a = GET_NAMES[i].what;
                break;
            }
        if (!GET_NAMES[i].name)
            return -1;
    } else if (word(&p, "fx")) {
        c.kind = A2K_ITC_FX;
        if (snum(&p, 0, 4, &c.a) || snum(&p, -50, 50, &c.b)) /* type 5 (WDR) belongs to the app (a2k_isp_wdr) */
            return -1;
    } else if (word(&p, "wb")) {
        if (word(&p, "auto")) {
            c.kind = A2K_ITC_WB_AUTO;
        } else if (word(&p, "manual")) {
            c.kind = A2K_ITC_WB_MANUAL;
            if (snum(&p, 0, 4095, &c.a) || snum(&p, 0, 4095, &c.b) || snum(&p, 0, 4095, &c.c))
                return -1;
        } else if (word(&p, "type")) {
            c.kind = A2K_ITC_WB_TYPE;
            if (snum(&p, 0, 1, &c.a))
                return -1;
        } else {
            return -1;
        }
    } else if (word(&p, "ccm")) {
        if (word(&p, "mode")) {
            c.kind = A2K_ITC_CCM_MODE;
            if (snum(&p, 0, 1, &c.a))
                return -1;
        } else if (word(&p, "blend")) {
            c.kind = A2K_ITC_CCM_BLEND;
            if (snum(&p, 0, 100, &c.a))
                return -1;
        } else {
            return -1;
        }
    } else if (word(&p, "nr1")) {
        c.kind = A2K_ITC_NR1;
        if (snum(&p, 0, 15, &c.a))
            return -1;
    } else if (word(&p, "nr2d")) {
        c.kind = A2K_ITC_NR2D;
        c.b = c.c = -1;
        if (snum(&p, 0, 15, &c.a))
            return -1;
        if (!at_end(p) && (snum(&p, 0, 15, &c.b) || snum(&p, 0, 15, &c.c)))
            return -1;
    } else if (word(&p, "uvnr")) {
        c.kind = A2K_ITC_UVNR;
        if (snum(&p, 0, 15, &c.a))
            return -1;
    } else if (word(&p, "nr3d")) {
        c.kind = A2K_ITC_NR3D;
        if (snum(&p, 0, 1, &c.a))
            return -1;
    } else if (word(&p, "rt")) {
        c.kind = A2K_ITC_RT;
        if (word(&p, "yuv"))
            c.a = A2K_IT_YUV;
        else if (word(&p, "wb"))
            c.a = A2K_IT_MWB;
        else if (word(&p, "ccm"))
            c.a = A2K_IT_CCM;
        else if (word(&p, "nr1"))
            c.a = A2K_IT_NR1;
        else if (word(&p, "nr2"))
            c.a = A2K_IT_NR2;
        else if (word(&p, "uvnr"))
            c.a = A2K_IT_UVNR;
        else if (word(&p, "nr3d"))
            c.a = A2K_IT_NR3D;
        else
            return -1;
    } else if (word(&p, "restore")) {
        c.kind = A2K_ITC_RESTORE;
    } else {
        for (i = 0; FX_ALIASES[i].name; i++)
            if (word(&p, FX_ALIASES[i].name)) {
                c.kind = A2K_ITC_FX;
                c.a = FX_ALIASES[i].type;
                if (snum(&p, -50, 50, &c.b))
                    return -1;
                break;
            }
        if (!FX_ALIASES[i].name)
            return -1;
    }
    if (!at_end(p))
        return -1;
    *out = c;
    return 0;
}

/* ---- little endian field access on the byte buffers ---- */

static unsigned rd16(const uint8_t *b, size_t off)
{
    return (unsigned)b[off] | ((unsigned)b[off + 1] << 8);
}
static int rds16(const uint8_t *b, size_t off)
{
    unsigned v = rd16(b, off);
    return v & 0x8000u ? (int)v - 65536 : (int)v;
}
static void wr16(uint8_t *b, size_t off, int v)
{
    b[off] = (uint8_t)(v & 0xff);
    b[off + 1] = (uint8_t)((v >> 8) & 0xff);
}
static unsigned rd32(const uint8_t *b, size_t off)
{
    return rd16(b, off) | (rd16(b, off + 2) << 16);
}
static void wr32(uint8_t *b, size_t off, unsigned v)
{
    wr16(b, off, (int)(v & 0xffff));
    wr16(b, off + 2, (int)(v >> 16));
}

/* ---- state ---- */

void a2k_isptest_init(a2k_isptest_t *t, const a2k_isptest_ops_t *ops)
{
    memset(t, 0, sizeof *t);
    t->ops = ops;
    t->auto_type = -1;
}

void a2k_isptest_free(a2k_isptest_t *t)
{
    for (int i = 0; i < A2K_IT_N; i++) {
        free(t->orig[i]);
        t->orig[i] = NULL;
    }
}

static const char *const WHAT[A2K_IT_N] = { "yuv_effect", "wb_type", "mwb", "ccm", "nr1", "nr2", "uvnr", "3d_nr", "awb_stat" };

static int get_buf(a2k_isptest_t *t, int what, uint8_t *buf)
{
    int rc;
    memset(buf, 0, A2K_IT_BUF);
    rc = t->ops->get(what, buf);
    if (rc)
        a2k_log("isp-test: get %s = %d", WHAT[what], rc);
    return rc;
}

/* Keeps the first read of an attribute before the hook changes it. */
static void remember(a2k_isptest_t *t, int what, const uint8_t *buf)
{
    if (t->orig[what])
        return;
    t->orig[what] = malloc(A2K_IT_BUF);
    if (t->orig[what])
        memcpy(t->orig[what], buf, A2K_IT_BUF);
}

static int put_buf(a2k_isptest_t *t, int what, const uint8_t *buf)
{
    int rc = t->ops->set(what, buf);
    a2k_log("isp-test: set %s = %d", WHAT[what], rc);
    return rc;
}

/* ---- dumps ---- */

static void dump_fx(a2k_isptest_t *t)
{
    static const char *const nm[5] = { "hue", "brightness", "saturation", "contrast", "sharpness" };
    for (int i = 0; i < 5; i++) {
        int v = 0, rc = t->ops->effect_get(i, &v);
        a2k_log("isp-test: effect %d (%s) get = %d value %d", i, nm[i], rc, v);
    }
}

static int dump_yuv(a2k_isptest_t *t, uint8_t *b)
{
    int rc = get_buf(t, A2K_IT_YUV, b);
    if (!rc)
        a2k_log("isp-test: yuv_effect y_a %u y_b %d uv_a %d uv_b %d dark_margin_en %u", rd16(b, 0), rds16(b, 2), rds16(b, 4), rds16(b, 6), rd16(b, 8));
    return rc;
}

static int dump_wb(a2k_isptest_t *t, uint8_t *b)
{
    int rc = get_buf(t, A2K_IT_WBTYPE, b);
    if (!rc)
        a2k_log("isp-test: wb_type %u (auto type of this run: %d)", rd32(b, 0), t->auto_type);
    int rc2 = get_buf(t, A2K_IT_MWB, b);
    if (!rc2)
        a2k_log("isp-test: mwb r %u g %u b %u offsets %d %d %d", rd16(b, 0), rd16(b, 2), rd16(b, 4), rds16(b, 6), rds16(b, 8), rds16(b, 10));
    return rc ? rc : rc2;
}

static int dump_ccm(a2k_isptest_t *t, uint8_t *b)
{
    int rc = get_buf(t, A2K_IT_CCM, b), n;
    if (rc)
        return rc;
    n = (int)rd16(b, A2K_CCM_NUM);
    a2k_log("isp-test: ccm mode %u (1 = automatic) manual ctrl %u %u %u enable %u manual [%d %d %d | %d %d %d | %d %d %d]", rd32(b, A2K_CCM_MODE), rd16(b, 4), rd16(b, 6),
            rd16(b, 8), rd16(b, 10), rds16(b, 18), rds16(b, 20), rds16(b, 22), rds16(b, 24), rds16(b, 26), rds16(b, 28), rds16(b, 30), rds16(b, 32), rds16(b, 34));
    a2k_log("isp-test: ccm auto count %d ctrl %u %u %u", n, rd16(b, 38), rd16(b, 40), rd16(b, 42));
    if (n > A2K_CCM_AUTO_N)
        n = A2K_CCM_AUTO_N;
    for (int i = 0; i < n; i++) {
        size_t o = A2K_CCM_AUTO + (size_t)A2K_CCM_AUTO_STRIDE * i;
        a2k_log("isp-test: ccm auto %d temp %u en %u [%d %d %d | %d %d %d | %d %d %d]", i, rd16(b, o), rd16(b, o + 2), rds16(b, o + 10), rds16(b, o + 12), rds16(b, o + 14),
                rds16(b, o + 16), rds16(b, o + 18), rds16(b, o + 20), rds16(b, o + 22), rds16(b, o + 24), rds16(b, o + 26));
    }
    return 0;
}

/* The mode word and one u16 field (a second one when off2 is non-zero) of each of the 17 records (manual first). */
static void rec_row(const uint8_t *b, const char *name, size_t off)
{
    char line[160];
    size_t n = 0;
    for (int j = 0; j < A2K_REC_N; j++) {
        size_t o = 4 + (size_t)j * ((strcmp(name, "nr1") == 0) ? A2K_NR1_REC : strcmp(name, "nr2") == 0 ? A2K_NR2_REC : strcmp(name, "uvnr") == 0 ? A2K_UVNR_REC : A2K_NR3D_REC) + off;
        if (o + 2 > A2K_IT_BUF || n + 8 > sizeof line)
            break;
        n += (size_t)snprintf(line + n, sizeof line - n, "%u ", rd16(b, o));
    }
    a2k_log("isp-test: %s mode %u, field +%zu of the manual record then the 16 linked ones: %s", name, rd32(b, 0), off, line);
}

static int dump_recs(a2k_isptest_t *t, uint8_t *b, int what, const char *name, size_t off, size_t off2)
{
    int rc = get_buf(t, what, b);
    if (rc)
        return rc;
    rec_row(b, name, off);
    if (off2)
        rec_row(b, name, off2);
    return 0;
}

static int dump_awb(a2k_isptest_t *t, uint8_t *b)
{
    char line[200];
    size_t n = 0;
    int rc = get_buf(t, A2K_IT_AWB, b);
    if (rc)
        return rc;
    for (int i = 0; i < A2K_AWB_RUN_LEN / 2; i++)
        n += (size_t)snprintf(line + n, sizeof line - n, "%u ", rd16(b, A2K_AWB_RUN_OFF + 2u * i));
    a2k_log("isp-test: awb run info (u16 x19, meaning UNVERIFIED: expect the current r/g/b gains and the colour temperature among them): %s", line);
    return 0;
}

static int dump_rgb(a2k_isptest_t *t)
{
    int r = 0, g = 0, bl = 0, rc = t->ops->rgb_avg(&r, &g, &bl);
    a2k_log("isp-test: rgb average = %d: r %d g %d b %d", rc, r, g, bl);
    return rc;
}

static int dump_all(a2k_isptest_t *t, uint8_t *b)
{
    int rc = 0, r;
    dump_fx(t);
    if ((r = dump_yuv(t, b)) != 0 && !rc)
        rc = r;
    if ((r = dump_wb(t, b)) != 0 && !rc)
        rc = r;
    if ((r = dump_ccm(t, b)) != 0 && !rc)
        rc = r;
    if ((r = dump_recs(t, b, A2K_IT_NR1, "nr1", 2, 0)) != 0 && !rc)
        rc = r;
    if ((r = dump_recs(t, b, A2K_IT_NR2, "nr2", 4, 8)) != 0 && !rc)
        rc = r;
    if ((r = dump_recs(t, b, A2K_IT_UVNR, "uvnr", 4, 0)) != 0 && !rc)
        rc = r;
    if ((r = dump_recs(t, b, A2K_IT_NR3D, "3d_nr", 0, 0)) != 0 && !rc)
        rc = r;
    if ((r = dump_awb(t, b)) != 0 && !rc)
        rc = r;
    if ((r = dump_rgb(t)) != 0 && !rc)
        rc = r;
    return rc;
}

/* ---- commands ---- */

/* Sets one u16 field in every record (manual + 16 linked). */
static void set_recs(uint8_t *b, size_t rec, size_t off, int v)
{
    for (int j = 0; j < A2K_REC_N; j++) {
        size_t o = 4 + rec * (size_t)j + off;
        if (o + 2 <= A2K_IT_BUF)
            wr16(b, o, v);
    }
}

/* Get, change, set of a record based attribute; logs the field before and after. */
static int rec_cmd(a2k_isptest_t *t, uint8_t *b, int what, const char *name, size_t rec, size_t off, int v, int off_b, int vb, int off_c, int vc)
{
    int rc = get_buf(t, what, b);
    if (rc)
        return rc;
    remember(t, what, b);
    a2k_log("isp-test: %s before: mode %u, manual field +%zu = %u", name, rd32(b, 0), off, rd16(b, 4 + off));
    set_recs(b, rec, off, v);
    if (off_b >= 0 && vb >= 0)
        set_recs(b, rec, (size_t)off_b, vb);
    if (off_c >= 0 && vc >= 0)
        set_recs(b, rec, (size_t)off_c, vc);
    rc = put_buf(t, what, b);
    if (!rc) {
        rc = get_buf(t, what, b);
        if (!rc)
            a2k_log("isp-test: %s after: manual field +%zu = %u", name, off, rd16(b, 4 + off));
    }
    return rc;
}

static void blend_matrix(uint8_t *b, size_t off, int p)
{
    int v[9], sum[3] = { 0, 0, 0 };
    for (int i = 0; i < 9; i++) {
        v[i] = rds16(b, off + 2u * (unsigned)i);
        sum[i / 3] += v[i];
    }
    for (int r = 0; r < 3; r++) {
        int off_sum = 0;
        for (int c = 0; c < 3; c++) {
            int x = v[r * 3 + c];
            if (c != r) {
                x = (x * p) / 100; /* identity has 0 off the diagonal */
                off_sum += x;
                wr16(b, off + 2u * (unsigned)(r * 3 + c), x);
            }
        }
        wr16(b, off + 2u * (unsigned)(r * 3 + r), sum[r] - off_sum); /* the row sum (white point) is kept exactly */
    }
}

static int cmd_ccm(a2k_isptest_t *t, uint8_t *b, const a2k_isptest_cmd_t *c)
{
    int rc = get_buf(t, A2K_IT_CCM, b), n;
    if (rc)
        return rc;
    remember(t, A2K_IT_CCM, b);
    if (c->kind == A2K_ITC_CCM_MODE) {
        a2k_log("isp-test: ccm mode %u -> %d", rd32(b, A2K_CCM_MODE), c->a);
        wr32(b, A2K_CCM_MODE, (unsigned)c->a);
    } else {
        n = (int)rd16(b, A2K_CCM_NUM);
        if (n > A2K_CCM_AUTO_N)
            n = A2K_CCM_AUTO_N;
        a2k_log("isp-test: ccm blend %d %% toward identity (manual + %d automatic matrices)", c->a, n);
        blend_matrix(b, A2K_CCM_MAN, c->a);
        for (int i = 0; i < n; i++)
            blend_matrix(b, A2K_CCM_AUTO + (size_t)A2K_CCM_AUTO_STRIDE * (size_t)i + 10, c->a);
    }
    rc = put_buf(t, A2K_IT_CCM, b);
    if (!rc)
        (void)dump_ccm(t, b);
    return rc;
}

static int cmd_wb(a2k_isptest_t *t, uint8_t *b, const a2k_isptest_cmd_t *c)
{
    int rc;
    unsigned want;
    if (c->kind == A2K_ITC_WB_MANUAL) {
        rc = get_buf(t, A2K_IT_MWB, b);
        if (rc)
            return rc;
        remember(t, A2K_IT_MWB, b);
        wr16(b, 0, c->a);
        wr16(b, 2, c->b);
        wr16(b, 4, c->c);
        rc = put_buf(t, A2K_IT_MWB, b);
        if (rc)
            return rc;
        want = (unsigned)(1 - t->auto_type);
    } else if (c->kind == A2K_ITC_WB_AUTO) {
        want = (unsigned)t->auto_type;
    } else {
        want = (unsigned)c->a;
    }
    rc = get_buf(t, A2K_IT_WBTYPE, b);
    if (rc)
        return rc;
    remember(t, A2K_IT_WBTYPE, b);
    a2k_log("isp-test: wb_type %u -> %u", rd32(b, 0), want);
    wr32(b, 0, want);
    rc = put_buf(t, A2K_IT_WBTYPE, b);
    if (!rc)
        (void)dump_wb(t, b);
    return rc;
}

static int cmd_rt(a2k_isptest_t *t, uint8_t *b, int what)
{
    uint8_t *a = malloc(A2K_IT_BUF);
    int rc;
    size_t i;
    if (!a)
        return -1;
    rc = get_buf(t, what, a);
    if (!rc) {
        rc = put_buf(t, what, a);
        if (!rc)
            rc = get_buf(t, what, b);
    }
    if (!rc) {
        for (i = 0; i < A2K_IT_BUF && a[i] == b[i]; i++) {
        }
        if (i == A2K_IT_BUF)
            a2k_log("isp-test: rt %s: get / set / get identical (the set path works and changes nothing)", WHAT[what]);
        else
            a2k_log("isp-test: rt %s: DIFFERS at byte %zu (%02x then %02x): the layout or the set path is not as assumed", WHAT[what], i, a[i], b[i]);
    }
    free(a);
    return rc;
}

static int cmd_restore(a2k_isptest_t *t)
{
    int rc = 0, r;
    for (int w = 0; w < A2K_IT_N; w++) {
        if (!t->orig[w] || w == A2K_IT_AWB)
            continue;
        r = put_buf(t, w, t->orig[w]);
        if (r && !rc)
            rc = r;
        free(t->orig[w]);
        t->orig[w] = NULL;
    }
    if (t->fx_touched) {
        for (int ty = 0; ty < 5; ty++)
            if (t->fx_touched & (1 << ty)) {
                r = t->ops->effect_set(ty, 0);
                a2k_log("isp-test: restore effect %d to 0 (the conf file value) = %d", ty, r);
                if (r && !rc)
                    rc = r;
            }
        t->fx_touched = 0;
    }
    return rc;
}

int a2k_isptest_exec(a2k_isptest_t *t, const a2k_isptest_cmd_t *c)
{
    uint8_t *b = malloc(A2K_IT_BUF);
    int rc = 0;
    if (!b)
        return -1;
    if (t->auto_type < 0) { /* the first look: the ISP runs its own AWB now, whatever wb_type says is "automatic" */
        if (get_buf(t, A2K_IT_WBTYPE, b) == 0) {
            t->auto_type = (int)rd32(b, 0) & 1;
            a2k_log("isp-test: first look: wb_type %u is taken as the automatic type", rd32(b, 0));
        }
    }
    switch (c->kind) {
    case A2K_ITC_GET:
        if (c->a == A2K_IT_ALL)
            rc = dump_all(t, b);
        else if (c->a == A2K_IT_FX)
            dump_fx(t);
        else if (c->a == A2K_IT_YUV)
            rc = dump_yuv(t, b);
        else if (c->a == A2K_IT_WBTYPE)
            rc = dump_wb(t, b);
        else if (c->a == A2K_IT_CCM)
            rc = dump_ccm(t, b);
        else if (c->a == A2K_IT_NR1)
            rc = dump_recs(t, b, A2K_IT_NR1, "nr1", 2, 0);
        else if (c->a == A2K_IT_NR2)
            rc = dump_recs(t, b, A2K_IT_NR2, "nr2", 4, 8);
        else if (c->a == A2K_IT_UVNR)
            rc = dump_recs(t, b, A2K_IT_UVNR, "uvnr", 4, 0);
        else if (c->a == A2K_IT_NR3D)
            rc = dump_recs(t, b, A2K_IT_NR3D, "3d_nr", 0, 0);
        else if (c->a == A2K_IT_AWB)
            rc = dump_awb(t, b);
        else if (c->a == A2K_IT_RGB)
            rc = dump_rgb(t);
        break;
    case A2K_ITC_FX: {
        int before = 0, after = 0, r0 = t->ops->effect_get(c->a, &before), r1;
        rc = t->ops->effect_set(c->a, c->b);
        t->fx_touched |= 1 << c->a;
        r1 = t->ops->effect_get(c->a, &after);
        a2k_log("isp-test: ak_vpss_effect_set(0, %d, %d) = %d (effect_get before %d [rc %d], after %d [rc %d])", c->a, c->b, rc, before, r0, after, r1);
        break;
    }
    case A2K_ITC_WB_AUTO:
    case A2K_ITC_WB_MANUAL:
    case A2K_ITC_WB_TYPE:
        if (t->auto_type < 0 && c->kind != A2K_ITC_WB_TYPE) {
            a2k_log("isp-test: wb: the automatic wb_type is not known (the first read failed), refused");
            rc = -1;
        } else {
            rc = cmd_wb(t, b, c);
        }
        break;
    case A2K_ITC_CCM_MODE:
    case A2K_ITC_CCM_BLEND:
        rc = cmd_ccm(t, b, c);
        break;
    case A2K_ITC_NR1:
        rc = rec_cmd(t, b, A2K_IT_NR1, "nr1", A2K_NR1_REC, 2, c->a, -1, -1, -1, -1);
        break;
    case A2K_ITC_NR2D:
        rc = rec_cmd(t, b, A2K_IT_NR2, "nr2", A2K_NR2_REC, 4, c->a, 8, c->b, 10, c->c);
        break;
    case A2K_ITC_UVNR:
        rc = rec_cmd(t, b, A2K_IT_UVNR, "uvnr", A2K_UVNR_REC, 4, c->a, -1, -1, -1, -1);
        break;
    case A2K_ITC_NR3D:
        rc = rec_cmd(t, b, A2K_IT_NR3D, "3d_nr", A2K_NR3D_REC, 0, c->a, -1, -1, -1, -1);
        break;
    case A2K_ITC_RT:
        rc = cmd_rt(t, b, c->a);
        if (c->a == A2K_IT_MWB && rc == 0)
            rc = cmd_rt(t, b, A2K_IT_WBTYPE);
        break;
    case A2K_ITC_RESTORE:
        rc = cmd_restore(t);
        break;
    default:
        rc = -1;
    }
    free(b);
    return rc;
}

int a2k_isptest_poll(a2k_isptest_t *t, const char *file)
{
    char buf[512];
    FILE *f = fopen(file, "r");
    size_t n;
    int lines = 0;
    char *s, *next;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    (void)unlink(file);
    buf[n] = 0;
    for (s = buf; *s && lines < A2K_IT_MAXLINES; s = next) {
        a2k_isptest_cmd_t c;
        int rc;
        next = strchr(s, '\n');
        if (next)
            *next++ = 0;
        else
            next = s + strlen(s);
        if (at_end(s))
            continue;
        lines++;
        if (a2k_isptest_parse(s, &c)) {
            a2k_log("isp-test: bad command \"%s\", ignored", s);
            continue;
        }
        rc = a2k_isptest_exec(t, &c);
        a2k_log("isp-test: command \"%s\" done, rc %d", s, rc);
    }
    return lines;
}

/* ---- the thread ---- */

static pthread_t g_th;
static int g_started;
static a2k_isptest_t g_t;

static void *poll_thread(void *arg)
{
    (void)arg;
    while (!a2k_stop) {
        struct timespec ts = { 0, 500 * 1000000L };
        (void)a2k_isptest_poll(&g_t, A2K_ISP_TEST_FILE);
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !a2k_stop) {
        }
    }
    return NULL;
}

int a2k_isptest_start(const a2k_isptest_ops_t *ops)
{
    if (g_started)
        return 0;
    a2k_isptest_init(&g_t, ops);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int rc = pthread_create(&g_th, &at, poll_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0)
        return -1;
    g_started = 1;
    a2k_log("isp-test: hook ON, polling %s (RAM only, nothing persistent)", A2K_ISP_TEST_FILE);
    return 0;
}

void a2k_isptest_stop(void)
{
    if (!g_started)
        return;
    pthread_join(g_th, NULL);
    g_started = 0;
    a2k_isptest_free(&g_t);
}
