/* isp.wdr of the 1080p: see isp_wdr.h and specs/video-and-streaming.md 5.8. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "isp_nr.h"
#include "isp_wdr.h"

#define ENTRIES    17
#define ENTRY_LEN  996u
#define CURVES     6
#define CURVE_N    65
#define CURVE_OFF  0x10u
#define CURVE_STEP 130u
#define SCRATCH    65536

uint32_t isp_wdr_get_cmd(void) { return 0x8004491c; }
uint32_t isp_wdr_set_cmd(void) { return ISP_INNER_WDR; }

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static const uint8_t *entry(const uint8_t *b, int e) { return b + 4 + (size_t)e * ENTRY_LEN; }

/* a switched-on entry: the six curves are programmed (+4) and the WDR switch is set (+0x3dc bit 0) */
static int entry_on(const uint8_t *ent) { return rd16(ent + 4) != 0 && (rd16(ent + 0x3dc) & 1); }

int isp_wdr_check(const uint8_t *base, size_t len)
{
    if (!base || len != ISP_WDR_BLOCK_LEN) return ISP_NR_REFUSED;
    if (rd32(base) == 0) return ISP_NR_REFUSED; /* manual mode: the kernel would use entry 0 only */
    for (int e = 0; e < ENTRIES; e++) {
        const uint8_t *ent = entry(base, e);
        if (rd16(ent + 4) > 1 || rd16(ent + 0x3dc) > 1 || rd16(ent + 0x3e2) >= 1024) return ISP_NR_REFUSED;
        for (int c = 0; c < CURVES; c++) {
            const uint8_t *cv = ent + CURVE_OFF + (size_t)c * CURVE_STEP;
            for (int i = 0; i < CURVE_N; i++)
                if (rd16(cv + 2 * i) > 1023) return ISP_NR_REFUSED;
            if (entry_on(ent) && rd16(cv) != 0) return ISP_NR_REFUSED; /* a tone curve starts at black */
        }
    }
    return 0;
}

int isp_wdr_compose(const uint8_t *base, size_t len, int s, uint8_t *out)
{
    if (s < 0 || s > ISP_WDR_MAX || !out) return -1;
    int rc = isp_wdr_check(base, len);
    if (rc != 0) return rc;
    memcpy(out, base, len);
    if (s == ISP_WDR_STOCK) return 0;
    for (int e = 1; e < ENTRIES; e++) {
        const uint8_t *src = entry(base, e);
        uint8_t *dst = out + 4 + (size_t)e * ENTRY_LEN;
        if (!entry_on(src)) continue;
        for (int c = 0; c < CURVES; c++) {
            const uint8_t *sc = src + CURVE_OFF + (size_t)c * CURVE_STEP;
            uint8_t *dc = dst + CURVE_OFF + (size_t)c * CURVE_STEP;
            int prev = 0;
            for (int i = 0; i < CURVE_N; i++) {
                int id = 16 * i > 1023 ? 1023 : 16 * i;
                int d = ((int)rd16(sc + 2 * i) - id) * s; /* |d| <= 1023 * 50 */
                int v = id + (d >= 0 ? d + ISP_WDR_STOCK / 2 : d - ISP_WDR_STOCK / 2) / ISP_WDR_STOCK;
                if (v < 0) v = 0;
                if (v > 1023) v = 1023;
                if (i && v < prev) v = prev;
                prev = v;
                wr16(dc + 2 * i, (unsigned)v);
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- Y_GAMMA toe */
uint32_t isp_ygamma_get_cmd(void) { return 0x80044976; }
uint32_t isp_ygamma_set_cmd(void) { return ISP_INNER_Y_GAMMA; }

static unsigned yg_id(int i) { return 8 * i > 1023 ? 1023u : (unsigned)(8 * i); }

int isp_ygamma_check(const uint8_t *base, size_t len)
{
    if (!base || len != ISP_YGAMMA_BLOCK_LEN) return ISP_NR_REFUSED;
    unsigned prev = 0;
    for (int i = 0; i < ISP_YGAMMA_POINTS; i++) {
        unsigned v = rd16(base + 2 * i);
        if (v > 1024 || v < prev) return ISP_NR_REFUSED; /* a gamma curve is a non-decreasing 10 bit table (1024: the night ramp's last point) */
        prev = v;
    }
    return 0;
}

int isp_ygamma_compose(const uint8_t *base, size_t len, int user, uint8_t *out)
{
    if (user < 0 || user > ISP_WDR_USER_MAX || !out) return -1;
    int rc = isp_ygamma_check(base, len);
    if (rc != 0) return rc;
    memcpy(out, base, len);
    if (user >= ISP_WDR_USER_STOCK) return 0;
    unsigned prev = 0;
    for (int i = 0; i < ISP_YGAMMA_POINTS; i++) {
        unsigned c = rd16(base + 2 * i), id = yg_id(i), v = c;
        if (c > id) {
            v = id + ((c - id) * (unsigned)user + ISP_WDR_USER_STOCK / 2) / ISP_WDR_USER_STOCK;
            if (v > 1023) v = 1023;
        }
        if (v < prev) v = prev;
        prev = v;
        wr16(out + 2 * i, v);
    }
    return 0;
}

/* ---------------------------------------------------------------- self-diagnosis */
/* One line per isp.wdr change and per ISP (re)load, to stdout like the other [isp] lines, and kept for the WDRDUMP command of port 8091.
 * It answers the questions a hardware run needs: did the GET work, what did it return (mode word, which entries have the switch and the
 * lift), what did the rescale change, did the SET go through, and does a GET after it read the composed block back. */
#define DIAG_MAX 768
static pthread_mutex_t g_diag_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_diag[DIAG_MAX]; /* "" until the first event */

typedef struct {
    const char *ev;       /* "set" | "load" */
    int         user, s;
    int         get_err;  /* 0 ok, else errno, -1 not read */
    int         have;     /* the live block was read: the fields below are valid */
    unsigned    mode, sw, prog, lift;
    uint32_t    csum_before, csum_after;
    int         changed;  /* bytes that differ between the live block and the composed one */
    const char *note;     /* why nothing was sent, or NULL */
    int         set_err;  /* 0 ok, else errno, -1 not sent */
    int         rb_err;   /* read-back after the SET: 0 matches, -2 differs, -1 not done, else errno */
    /* the Y_GAMMA part (same meanings): */
    int         gget_err, ghave, gset_err, grb_err, gchanged, glift, gsent;
    uint32_t    gcsum_before, gcsum_after;
    const char *gnote;
} diag_t;

static void diag_init(diag_t *d, const char *ev)
{
    memset(d, 0, sizeof(*d));
    d->ev = ev;
    d->get_err = d->set_err = d->rb_err = -1;
    d->gget_err = d->gset_err = d->grb_err = -1;
}

/* FNV-1a over the six tone curves of every entry (the part the rescale touches) */
static uint32_t curves_csum(const uint8_t *b)
{
    uint32_t h = 2166136261u;
    for (int e = 0; e < ENTRIES; e++)
        for (size_t k = 0; k < (size_t)CURVES * CURVE_STEP; k++) {
            h ^= entry(b, e)[CURVE_OFF + k];
            h *= 16777619u;
        }
    return h;
}

static void block_fields(const uint8_t *b, diag_t *d)
{
    d->have = 1;
    d->mode = rd32(b);
    d->csum_before = curves_csum(b);
    d->sw = d->prog = d->lift = 0;
    for (int e = 0; e < ENTRIES; e++) {
        const uint8_t *ent = entry(b, e);
        if (rd16(ent + 0x3dc) & 1) d->sw |= 1u << e;
        if (rd16(ent + 4)) d->prog |= 1u << e;
        if (!entry_on(ent)) continue;
        for (int c = 0; c < CURVES && !(d->lift & (1u << e)); c++)
            for (int i = 0; i < CURVE_N; i++)
                if (rd16(ent + CURVE_OFF + (size_t)c * CURVE_STEP + 2 * i) != (unsigned)(16 * i > 1023 ? 1023 : 16 * i)) { d->lift |= 1u << e; break; }
    }
}

static void diag_log(const diag_t *d)
{
    char line[DIAG_MAX], t[48];
    size_t n = (size_t)snprintf(line, sizeof(line), "[isp.wdr] ev=%s user=%d s=%d", d->ev, d->user, d->s);
#define ADD(...) do { if (n < sizeof(line)) n += (size_t)snprintf(line + n, sizeof(line) - n, __VA_ARGS__); } while (0)
    if (d->get_err == 0) ADD(" get=ok");
    else if (d->get_err > 0) ADD(" get=errno%d", d->get_err);
    else ADD(" get=none");
    if (d->have)
        ADD(" mode=%u sw=0x%05x prog=0x%05x lift=0x%05x csum_before=%08x csum_after=%08x changed=%d",
            d->mode, d->sw, d->prog, d->lift, d->csum_before, d->csum_after, d->changed);
    if (d->set_err == 0) snprintf(t, sizeof(t), "ok");
    else if (d->set_err > 0) snprintf(t, sizeof(t), "errno%d", d->set_err);
    else snprintf(t, sizeof(t), "none");
    ADD(" set=%s", t);
    if (d->rb_err == 0) snprintf(t, sizeof(t), "match");
    else if (d->rb_err == -2) snprintf(t, sizeof(t), "DIFFERS");
    else if (d->rb_err > 0) snprintf(t, sizeof(t), "errno%d", d->rb_err);
    else snprintf(t, sizeof(t), "none");
    ADD(" readback=%s", t);
    if (d->note) ADD(" note=%s", d->note);
    if (d->gget_err == 0) ADD(" gamma=ok");
    else if (d->gget_err > 0) ADD(" gamma=errno%d", d->gget_err);
    else ADD(" gamma=none");
    if (d->ghave)
        ADD(" glift=%d gcsum_before=%08x gcsum_after=%08x gchanged=%d", d->glift, d->gcsum_before, d->gcsum_after, d->gchanged);
    if (d->gset_err == 0) snprintf(t, sizeof(t), "ok");
    else if (d->gset_err > 0) snprintf(t, sizeof(t), "errno%d", d->gset_err);
    else snprintf(t, sizeof(t), "none");
    ADD(" gset=%s", t);
    if (d->grb_err == 0) snprintf(t, sizeof(t), "match");
    else if (d->grb_err == -2) snprintf(t, sizeof(t), "DIFFERS");
    else if (d->grb_err > 0) snprintf(t, sizeof(t), "errno%d", d->grb_err);
    else snprintf(t, sizeof(t), "none");
    ADD(" greadback=%s", t);
    if (d->gnote) ADD(" gnote=%s", d->gnote);
#undef ADD
    pthread_mutex_lock(&g_diag_lock);
    snprintf(g_diag, sizeof(g_diag), "%s", line);
    pthread_mutex_unlock(&g_diag_lock);
    printf("%s\n", line);
    fflush(stdout);
}

int isp_wdr_dump(char *out, size_t cap)
{
    pthread_mutex_lock(&g_diag_lock);
    int n = snprintf(out, cap, "%s", g_diag[0] ? g_diag : "[isp.wdr] no event yet");
    pthread_mutex_unlock(&g_diag_lock);
    return n;
}

/* ---------------------------------------------------------------- sender */
static int      g_s = ISP_WDR_USER_STOCK; /* the user value */
static uint8_t *g_base;  /* the live block as the conf loaded it, read at the first use */
static int      g_dirty; /* the live block holds bytes other than g_base */

int isp_wdr_get(void) { return g_s; }

/* GET of cmd into dst (len bytes): 0 or the errno */
static int read_cmd(uint32_t cmd, uint8_t *dst, size_t len)
{
    uint8_t *scratch = calloc(1, SCRATCH);
    if (!scratch) return ENOMEM;
    errno = 0;
    int rc = isp_tunnel(cmd, scratch);
    int err = rc == 0 ? 0 : (errno ? errno : EIO);
    if (rc == 0) memcpy(dst, scratch, len);
    free(scratch);
    return err;
}
static int read_live(uint8_t *dst) { return read_cmd(isp_wdr_get_cmd(), dst, ISP_WDR_BLOCK_LEN); }

/* ---- the Y_GAMMA sender: its own pristine baseline and "changed" mark, same rules as the WDR block */
static uint8_t *g_gbase;
static int      g_gdirty;

static uint32_t yg_csum(const uint8_t *b)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < 2u * ISP_YGAMMA_POINTS; k++) { h ^= b[k]; h *= 16777619u; }
    return h;
}

static void yg_fields(const uint8_t *b, diag_t *d)
{
    d->ghave = 1;
    d->gcsum_before = yg_csum(b);
    for (int i = 0; i < ISP_YGAMMA_POINTS; i++) d->glift += rd16(b + 2 * i) > yg_id(i);
}

/* Applies the toe for the user value: 0 done (also "nothing to send"), -1 failed, ISP_NR_REFUSED. d->gsent = a SET went through. */
static int gamma_apply(int user, diag_t *d)
{
    if (user >= ISP_WDR_USER_STOCK && !g_gdirty) { d->gnote = "stock-nothing-to-send"; return 0; }
    if (!g_gbase) {
        uint8_t *keep = malloc(ISP_YGAMMA_BLOCK_LEN);
        if (!keep) { d->gget_err = ENOMEM; d->gnote = "get-failed"; return -1; }
        d->gget_err = read_cmd(isp_ygamma_get_cmd(), keep, ISP_YGAMMA_BLOCK_LEN);
        if (d->gget_err != 0) { free(keep); d->gnote = "get-failed"; return -1; }
        g_gbase = keep;
    } else d->gget_err = -1;
    yg_fields(g_gbase, d);
    uint8_t *out = malloc(ISP_YGAMMA_BLOCK_LEN);
    if (!out) return -1;
    int rc = isp_ygamma_compose(g_gbase, ISP_YGAMMA_BLOCK_LEN, user, out);
    if (rc != 0) {
        free(out);
        d->gnote = rc == ISP_NR_REFUSED ? "live-block-not-as-decoded" : "compose-failed";
        return rc == ISP_NR_REFUSED ? ISP_NR_REFUSED : -1;
    }
    d->gcsum_after = yg_csum(out);
    for (size_t i = 0; i < ISP_YGAMMA_BLOCK_LEN; i++) d->gchanged += out[i] != g_gbase[i];
    if (!g_gdirty && d->gchanged == 0) { free(out); d->gnote = "composed-equals-live-nothing-to-send"; return 0; } /* the night ramp */
    errno = 0;
    rc = isp_tunnel(isp_ygamma_set_cmd(), out) == 0 ? 0 : -1;
    d->gset_err = rc == 0 ? 0 : (errno ? errno : EIO);
    if (rc == 0) {
        d->gsent = 1;
        g_gdirty = d->gchanged != 0;
        uint8_t *back = malloc(ISP_YGAMMA_BLOCK_LEN);
        if (back) {
            int e = read_cmd(isp_ygamma_get_cmd(), back, ISP_YGAMMA_BLOCK_LEN);
            d->grb_err = e ? e : (memcmp(back, out, ISP_YGAMMA_BLOCK_LEN) == 0 ? 0 : -2);
            free(back);
        }
    }
    free(out);
    return rc;
}

static int fetch_base(diag_t *d)
{
    if (g_base) return 0;
    uint8_t *keep = malloc(ISP_WDR_BLOCK_LEN);
    if (!keep) { d->get_err = ENOMEM; return -1; }
    d->get_err = read_live(keep);
    if (d->get_err != 0) { free(keep); return -1; }
    g_base = keep;
    return 0;
}

int isp_wdr_strength(int user)
{
    if (user < 0 || user > ISP_WDR_USER_MAX) return -1;
    return user <= ISP_WDR_USER_STOCK ? (user * ISP_WDR_STOCK + ISP_WDR_USER_STOCK / 2) / ISP_WDR_USER_STOCK : user - 50;
}

static int apply_diag(int user, diag_t *d)
{
    int s = isp_wdr_strength(user);
    d->user = user;
    d->s = s;
    if (s < 0) return -1;
    if (s == ISP_WDR_STOCK && !g_dirty) { d->note = "stock-nothing-to-send"; return 0; } /* the camera's own block is live */
    int had_base = g_base != NULL;
    if (fetch_base(d) != 0) { d->note = "get-failed"; return -1; }
    if (had_base) d->get_err = -1; /* no GET this time: the baseline is the one read before */
    block_fields(g_base, d);
    uint8_t *out = malloc(ISP_WDR_BLOCK_LEN);
    if (!out) return -1;
    int rc = isp_wdr_compose(g_base, ISP_WDR_BLOCK_LEN, s, out);
    if (rc != 0) {
        free(out);
        d->note = rc == ISP_NR_REFUSED ? "live-block-not-as-decoded" : "compose-failed";
        return rc == ISP_NR_REFUSED ? ISP_NR_REFUSED : -1;
    }
    d->csum_after = curves_csum(out);
    for (size_t i = 0; i < ISP_WDR_BLOCK_LEN; i++) d->changed += out[i] != g_base[i];
    if (!g_dirty && d->changed == 0) { free(out); d->note = "composed-equals-live-nothing-to-send"; return 0; } /* nothing to change in this table (night) */
    errno = 0;
    rc = isp_tunnel(isp_wdr_set_cmd(), out) == 0 ? 0 : -1;
    d->set_err = rc == 0 ? 0 : (errno ? errno : EIO);
    if (rc == 0) {
        g_dirty = d->changed != 0;
        uint8_t *back = malloc(ISP_WDR_BLOCK_LEN);
        if (back) {
            int e = read_live(back);
            d->rb_err = e ? e : (memcmp(back, out, ISP_WDR_BLOCK_LEN) == 0 ? 0 : -2);
            free(back);
        }
    }
    free(out);
    return rc;
}

static int apply(int user, const char *ev)
{
    diag_t d;
    diag_init(&d, ev);
    int rc = apply_diag(user, &d);
    if (d.s >= 0 && user >= 0) { (void)gamma_apply(user, &d); }
    if (rc != 0 && d.gsent) rc = 0; /* the toe went through: the strength is in force */
    diag_log(&d);
    return rc;
}

int isp_wdr_set(int s)
{
    if (s < 0 || s > ISP_WDR_USER_MAX) return -1;
    int old = g_s;
    g_s = s;
    int rc = apply(s, "set");
    if (rc != 0) g_s = old;
    return rc;
}

void isp_wdr_reset(void)
{
    free(g_base);
    g_base = NULL;
    g_dirty = 0;
    free(g_gbase);
    g_gbase = NULL;
    g_gdirty = 0;
}

/* After isp_wdr_reset(): re-applies a non-stock strength; at the stock one only reads the block (no write) so that every ISP load leaves one
 * [isp.wdr] line with what the camera loaded. */
void isp_wdr_reapply(void)
{
    if (g_s == ISP_WDR_USER_STOCK) {
        diag_t d;
        diag_init(&d, "load");
        d.user = g_s;
        d.s = ISP_WDR_STOCK;
        uint8_t *b = malloc(ISP_WDR_BLOCK_LEN);
        d.get_err = b ? read_live(b) : ENOMEM;
        if (d.get_err == 0) { block_fields(b, &d); d.csum_after = d.csum_before; d.note = "stock-nothing-sent"; }
        free(b);
        uint8_t *g = malloc(ISP_YGAMMA_BLOCK_LEN);
        d.gget_err = g ? read_cmd(isp_ygamma_get_cmd(), g, ISP_YGAMMA_BLOCK_LEN) : ENOMEM;
        if (d.gget_err == 0) { yg_fields(g, &d); d.gcsum_after = d.gcsum_before; d.gnote = "stock-nothing-sent"; }
        free(g);
        diag_log(&d);
        return;
    }
    int rc = apply(g_s, "load");
    if (rc != 0) fprintf(stderr, "[isp.wdr] strength %d not re-applied after the reload (%d)\n", g_s, rc);
}
