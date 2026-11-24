/* Host test of the NR / sharpening knobs of the 1080p ISP (isp_nr.c composers, isp_nr_ctl.c senders, isp.c SHARP order).
 * The ISP driver is a fake ioctl() that keeps the five live blocks (vendor-like tables built here from the formula of
 * specs/video-and-streaming.md 5.5.2) and counts the SETs. Cases (a)..(h) of 5.5.7 plus the reload and the SHARP order.
 * No camera. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include "platform.h"
#include "ak_ioctls.h"
#include "isp.h"
#include "isp_nr.h"
#include "isp_nr_layout.h"

int g_isp_fd = 3;
int g_subdev;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---------------------------------------------------------------- the fake driver */
#define BUF 65536
static uint8_t g_live[ISP_NR_BLOCKS][BUF];
static int g_sets[ISP_NR_BLOCKS], g_gets[ISP_NR_BLOCKS], g_other;
static uint8_t g_expo[0x720], g_run[36];

static int blk_of(uint32_t cmd, int get)
{
    for (int b = 0; b < ISP_NR_BLOCKS; b++)
        if (cmd == (get ? isp_nr_get_cmd(b) : isp_nr_set_cmd(b))) return b;
    return -1;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    struct isp_cmd_wrapper *w = va_arg(ap, struct isp_cmd_wrapper *);
    va_end(ap);
    (void)fd; (void)req;
    uint32_t cmd = (uint32_t)w->inner_cmd;
    int b;
    if ((b = blk_of(cmd, 1)) >= 0) { g_gets[b]++; memcpy(w->payload, g_live[b], isp_nr_block_len(b)); return 0; }
    if ((b = blk_of(cmd, 0)) >= 0) { g_sets[b]++; memcpy(g_live[b], w->payload, isp_nr_block_len(b)); return 0; }
    if (cmd == (uint32_t)ISP_INNER_GET_EXPOSURE) { memcpy(w->payload, g_expo, sizeof(g_expo)); return 0; }
    if (cmd == (uint32_t)ISP_INNER_GET_AE_RUN_INFO) { memcpy(w->payload, g_run, sizeof(g_run)); return 0; }
    g_other++;
    return 0;
}

static int total_sets(void) { int n = 0; for (int b = 0; b < ISP_NR_BLOCKS; b++) n += g_sets[b]; return n; }
static void clear_counts(void) { memset(g_sets, 0, sizeof(g_sets)); memset(g_gets, 0, sizeof(g_gets)); g_other = 0; }

/* ---------------------------------------------------------------- vendor-like blocks */
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned r16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned r32(const uint8_t *p) { return r16(p) | (r16(p + 2) << 16); }
static void w32(uint8_t *p, unsigned v) { w16(p, v & 0xffff); w16(p + 2, v >> 16); }
static size_t eoff(int b, int k) { return 4 + (size_t)k * (b == ISP_NR_SHARP ? SHARP_ENTRY_SIZE : b == ISP_NR_3DNR ? 416 : b == ISP_NR_NR1 ? 180 : b == ISP_NR_NR2 ? 58 : 42); }

static void vendor_table(uint8_t *tbl, unsigned s, unsigned floor_v, int jitter)
{
    w16(tbl, 1023);
    for (int i = 1; i <= 15; i++) {
        long v = lround(1023.0 * exp(-256.0 * i * i / s));
        if (v < (long)floor_v) v = (long)floor_v;
        if (jitter && (i % 3 == 0) && v > (long)floor_v + 1) v += (jitter > 0) ? 1 : -1; /* the vendor's rounding is +-1 */
        w16(tbl + 2 * i, (unsigned)v);
    }
    w16(tbl + 32, floor_v);
}

static unsigned g_lcg = 12345;
static void fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { g_lcg = g_lcg * 1103515245u + 12345u; p[i] = (uint8_t)(g_lcg >> 16); } }

static void build_live(void)
{
    for (int b = 0; b < ISP_NR_BLOCKS; b++) { memset(g_live[b], 0, BUF); fill(g_live[b], isp_nr_block_len(b)); w32(g_live[b], 1); }
    for (int L = 0; L < 16; L++) {
        int k = 1 + L;
        uint8_t *p = g_live[ISP_NR_NR1] + eoff(ISP_NR_NR1, k);
        unsigned s1 = 100 + 800u * L, fl = (L % 2) ? 10 : 1;
        w16(p, 1); w16(p + 6, s1); w16(p + 8, s1); w16(p + 10, s1);
        for (int t = 0; t < 3; t++) vendor_table(p + 0x4e + 34 * t, s1, fl, L % 4 == 1 ? 1 : 0);
        if (L == 3) { for (int t = 0; t < 3; t++) { w16(p + 6 + 2 * t, 0); for (int i = 0; i < 17; i++) w16(p + 0x4e + 34 * t + 2 * i, 1); } } /* s = 0 entry: skipped */
        p = g_live[ISP_NR_NR2] + eoff(ISP_NR_NR2, k);
        unsigned s2 = 200 + 380u * L;
        w16(p, 1); w16(p + 0x0c, s2); vendor_table(p + 0x0e, s2, fl * 5, L % 4 == 2 ? -1 : 0);
        p = g_live[ISP_NR_UVNR] + eoff(ISP_NR_UVNR, k);
        unsigned s3 = 100 + 580u * L;
        w16(p, 1); w16(p + 6, s3); vendor_table(p + 8, s3, fl * 2, 0);
        p = g_live[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, k);
        w16(p, 1); w16(p + 6, 800 + 400 * L); w16(p + 8, 1800 + 400 * L); w16(p + 0x130, 50 + 1300 * L);
        vendor_table(p + 0x132, 50 + 1300 * L, 1, 0);
        p = g_live[ISP_NR_SHARP] + eoff(ISP_NR_SHARP, k);
        w16(p, 1);
        static const unsigned cor[8] = SHARP_E_CORING_OFFS_INIT;
        static const unsigned gn[8] = SHARP_E_GAIN_OFFS_INIT;
        for (int i = 0; i < 8; i++) { w16(p + cor[i], 4 + L * 11 / 10 + (i & 1)); w16(p + gn[i], 30 - L); }
    }
    memset(g_live[ISP_NR_3DNR] + 4 + 17 * 416, 0, 7180 - 4 - 17 * 416);      /* the rc tail: zero, as the start-up sends it */
    w16(g_live[ISP_NR_NR2] + 990, 0);
    w16(g_live[ISP_NR_UVNR] + 718, 0);
}

static uint8_t g_pristine[ISP_NR_BLOCKS][BUF];
static void snapshot(void) { memcpy(g_pristine, g_live, sizeof(g_pristine)); }
static void restore_vendor(void) { memcpy(g_live, g_pristine, sizeof(g_live)); }
static int live_is_pristine(int b) { return memcmp(g_live[b], g_pristine[b], isp_nr_block_len(b)) == 0; }

/* ---------------------------------------------------------------- what a knob may touch */
/* 1 when byte `off` of block b may differ because of the knob */
static int allowed(int knob, int b, size_t off)
{
    size_t es = eoff(b, 1) - eoff(b, 0);
    size_t first = eoff(b, 1), last = eoff(b, 17);          /* entries 1..16 only */
    if (off < first || off >= last) return 0;
    size_t rel = (off - 4) % es;
    switch (knob) {
    case ISP_KNOB_NR2D_BIAS: case ISP_KNOB_UVNR_BIAS: case ISP_KNOB_NR3D_BIAS: case ISP_KNOB_SHARP_BIAS: return 1;
    case ISP_KNOB_NR2D:
        if (b == ISP_NR_NR1) return (rel >= 6 && rel < 12) || (rel >= 0x4e + 2 && rel < 0x4e + 3 * 34 && (rel - 0x4e) % 34 >= 2 && (rel - 0x4e) % 34 < 32);
        return rel == 0x0c || rel == 0x0d || (rel >= 0x0e + 2 && rel < 0x0e + 32);
    case ISP_KNOB_UVNR: return rel == 6 || rel == 7 || (rel >= 8 + 2 && rel < 8 + 32);
    case ISP_KNOB_NR3D_MOTION: return rel >= 6 && rel < 10;
    case ISP_KNOB_CORING: {
        static const unsigned cor[8] = SHARP_E_CORING_OFFS_INIT;
        for (int i = 0; i < 8; i++) if (rel == cor[i] || rel == cor[i] + 1u) return 1;
        return 0;
    }
    }
    return 0;
}


/* ---------------------------------------------------------------- the live dump of the two 1080p cameras */
/* Builds the NR1, NR2, UVNR and 3D_NR blocks from the decoded fields of a `SET img.dump nr` capture (tests/fixtures/
 * isp_nr_dump_akam-0N.txt: mode word, enable and strength per entry; the dump holds no table bytes, so the weight tables
 * are generated from the strength, floor 1). 0 on success. */
static int load_dump(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[16];
        unsigned a, b, c, d, e, g;
        int b_ix = -1;
        if (strncmp(line, "[isp.nr] ", 9)) continue;
        if (!strncmp(line + 9, "NR1 ", 4)) b_ix = ISP_NR_NR1;
        else if (!strncmp(line + 9, "NR2 ", 4)) b_ix = ISP_NR_NR2;
        else if (!strncmp(line + 9, "UVNR ", 5)) b_ix = ISP_NR_UVNR;
        else if (!strncmp(line + 9, "3D_NR ", 6)) b_ix = ISP_NR_3DNR;
        if (b_ix < 0) continue;
        if (sscanf(line + 9, "%15s mode=%u len=%u", name, &a, &b) == 3) { memset(g_live[b_ix], 0, BUF); w32(g_live[b_ix], a); continue; }
        char *eq = strstr(line, " e");
        if (!eq || sscanf(eq, " e%u", &a) != 1 || a > 16) continue;
        char *en = strstr(line, "en=");
        if (!en || sscanf(en, "en=%u", &b) != 1) continue;
        uint8_t *p = g_live[b_ix] + eoff(b_ix, (int)a);
        w16(p, b);
        if (b_ix == ISP_NR_NR1 && sscanf(strstr(line, "s=") + 2, "%u/%u/%u", &c, &d, &e) == 3) {
            w16(p + 6, c); w16(p + 8, d); w16(p + 10, e);
            if (c) for (int t = 0; t < 3; t++) vendor_table(p + 0x4e + 34 * t, c, 1, 0);
        } else if (b_ix == ISP_NR_NR2 && sscanf(strstr(line, "s=") + 2, "%u", &c) == 1) {
            w16(p + 0x0c, c); vendor_table(p + 0x0e, c, 1, 0);
        } else if (b_ix == ISP_NR_UVNR && sscanf(strstr(line, "s=") + 2, "%u", &c) == 1) {
            w16(p + 6, c); vendor_table(p + 8, c, 1, 0);
        } else if (b_ix == ISP_NR_3DNR && sscanf(strstr(line, "lo="), "lo=%u hi=%u s=%u", &c, &d, &g) == 3) {
            w16(p + 6, c); w16(p + 8, d); w16(p + 0x130, g); vendor_table(p + 0x132, g, 1, 0);
        } else continue;
        n++;
    }
    fclose(f);
    return n == 4 * 17 ? 0 : -2;
}

static int knob_block(int knob)
{
    switch (knob) {
    case ISP_KNOB_NR2D_BIAS: case ISP_KNOB_NR2D: return ISP_NR_NR1;
    case ISP_KNOB_UVNR_BIAS: case ISP_KNOB_UVNR: return ISP_NR_UVNR;
    case ISP_KNOB_NR3D_BIAS: case ISP_KNOB_NR3D_MOTION: return ISP_NR_3DNR;
    }
    return ISP_NR_SHARP;
}

static void level_expo(void)
{
    memset(g_expo, 0, sizeof(g_expo));
    for (int i = 0; i < 16; i++) { w32(g_expo + 0x98 + 8 * i, 512u * i); w32(g_expo + 0x98 + 8 * i + 4, 512u * (i + 1)); }
}

int main(void)
{
    build_live();
    snapshot();

    /* ---- (a) defaults: composer output identical to the baseline, and the senders see zero SETs */
    isp_nr_knobs_t kd;
    isp_nr_knobs_default(&kd);
    for (int k = 0; k < ISP_KNOB_COUNT; k++) CHECK(kd.v[k] == isp_nr_knob_info(k)->def && isp_nr_knob_get(k) == kd.v[k]);
    for (int b = 0; b < ISP_NR_SHARP; b++) {
        uint8_t *out = malloc(isp_nr_block_len(b));
        CHECK(isp_nr_compose(b, g_live[b], isp_nr_block_len(b), &kd, out) == 0);
        CHECK(memcmp(out, g_live[b], isp_nr_block_len(b)) == 0);
        free(out);
    }
    clear_counts();
    for (int k = 0; k < ISP_KNOB_COUNT; k++) CHECK(isp_nr_knob_set(k, kd.v[k]) == 0);
    isp_reset_picture_baselines();
    CHECK(total_sets() == 0 && g_gets[0] + g_gets[1] + g_gets[2] + g_gets[3] + g_gets[4] == 0 && g_other == 2); /* the two other calls are the read-only GETs of the WDR and Y_GAMMA blocks for the [isp.wdr] load line, never a SET */
    for (int b = 0; b < ISP_NR_BLOCKS; b++) CHECK(live_is_pristine(b));

    /* ---- (b) each knob changes only its declared offsets, and does change something */
    static const int test_v[ISP_KNOB_COUNT] = { 2, 3, 1, 4, 200, 150, 250, 150 };
    for (int k = 0; k < ISP_KNOB_COUNT; k++) {
        int b = knob_block(k);
        isp_nr_knobs_t kn = kd;
        kn.v[k] = test_v[k];
        size_t len = isp_nr_block_len(b);
        uint8_t *out = malloc(len);
        int rc = (b == ISP_NR_SHARP) ? 0 : isp_nr_compose(b, g_live[b], len, &kn, out);
        if (b == ISP_NR_SHARP) {
            if (k == ISP_KNOB_SHARP_BIAS) isp_nr_sharp_bias(g_live[b], test_v[k], out);
            else { memcpy(out, g_live[b], len); isp_nr_sharp_coring(out, test_v[k]); }
        }
        CHECK(rc == 0);
        size_t ndiff = 0, bad = 0;
        for (size_t i = 0; i < len; i++)
            if (out[i] != g_live[b][i]) { ndiff++; if (!allowed(k, b, i)) bad++; }
        CHECK(ndiff > 0 && bad == 0);
        if (bad) printf("  knob %s: %zu bytes outside the declared offsets\n", isp_nr_knob_info(k)->key, bad);
        if (b == ISP_NR_NR1) { /* NR2 follows the same knob */
            len = isp_nr_block_len(ISP_NR_NR2);
            uint8_t *o2 = malloc(len);
            CHECK(isp_nr_compose(ISP_NR_NR2, g_live[ISP_NR_NR2], len, &kn, o2) == 0);
            ndiff = bad = 0;
            for (size_t i = 0; i < len; i++) if (o2[i] != g_live[ISP_NR_NR2][i]) { ndiff++; if (!allowed(k, ISP_NR_NR2, i)) bad++; }
            CHECK(ndiff > 0 && bad == 0);
            free(o2);
        }
        free(out);
    }

    /* ---- (c) bias n copies vendor entry L+n byte for byte, clamped at level 15 */
    for (int b = 0; b < ISP_NR_SHARP; b++) {
        size_t len = isp_nr_block_len(b), es = eoff(b, 1) - eoff(b, 0);
        uint8_t *out = malloc(len);
        for (int n = 1; n <= 4; n++) {
            isp_nr_knobs_t kn = kd;
            kn.v[b == ISP_NR_NR1 || b == ISP_NR_NR2 ? ISP_KNOB_NR2D_BIAS : b == ISP_NR_UVNR ? ISP_KNOB_UVNR_BIAS : ISP_KNOB_NR3D_BIAS] = n;
            CHECK(isp_nr_compose(b, g_live[b], len, &kn, out) == 0);
            for (int L = 0; L < 16; L++) {
                int src = L + n > 15 ? 15 : L + n;
                CHECK(memcmp(out + eoff(b, 1 + L), g_live[b] + eoff(b, 1 + src), es) == 0);
            }
            CHECK(memcmp(out, g_live[b], eoff(b, 1)) == 0);                          /* mode word and entry 0 */
            CHECK(memcmp(out + eoff(b, 17), g_live[b] + eoff(b, 17), len - eoff(b, 17)) == 0); /* pad / rc tail */
        }
        free(out);
    }
    {
        uint8_t *out = malloc(SHARP_BLOCK_SIZE);
        isp_nr_sharp_bias(g_live[ISP_NR_SHARP], 3, out);
        for (int L = 0; L < 16; L++)
            CHECK(memcmp(out + SHARP_ENTRY_OFF(1 + L), g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(1 + (L + 3 > 15 ? 15 : L + 3)), SHARP_ENTRY_SIZE) == 0);
        CHECK(memcmp(out, g_live[ISP_NR_SHARP], SHARP_ENTRY_OFF(1)) == 0);
        free(out);
    }

    /* ---- (d) regenerated tables: 100 % reproduces the vendor tables within +-1, 50 and 400 % stay in 0..1023;
     *      the integer formula agrees with libm everywhere on a grid of strengths */
    {
        for (int b = 0; b < ISP_NR_UVNR + 1; b++) {
            isp_nr_knobs_t kn = kd;
            size_t len = isp_nr_block_len(b);
            uint8_t *out = malloc(len);
            for (int pct = 50; pct <= 400; pct += 350) {
                kn.v[ISP_KNOB_NR2D] = kn.v[ISP_KNOB_UVNR] = pct;
                CHECK(isp_nr_compose(b, g_live[b], len, &kn, out) == 0);
                for (int k = 1; k <= 16; k++) {
                    const uint8_t *p = out + eoff(b, k), *v = g_live[b] + eoff(b, k);
                    int tb = b == ISP_NR_NR1 ? 0x4e : b == ISP_NR_NR2 ? 0x0e : 0x08;
                    int nt = b == ISP_NR_NR1 ? 3 : 1;
                    for (int t = 0; t < nt; t++)
                        for (int i = 0; i < 17; i++) {
                            CHECK(r16(p + tb + 34 * t + 2 * i) <= 1023);
                            if (i == 0 || i == 16) CHECK(r16(p + tb + 34 * t + 2 * i) == r16(v + tb + 34 * t + 2 * i)); /* ends kept */
                        }
                    if (b == ISP_NR_NR1 && k == 4) CHECK(memcmp(p, v, 180) == 0);   /* the s = 0 entry is skipped */
                    else if (b == ISP_NR_NR1) CHECK(r16(p + 6) == r16(v + 6) * pct / 100);
                }
            }
            free(out);
        }
        for (unsigned s = 50; s <= 20000; s += (s < 1000 ? 37 : 411)) {
            uint16_t lut[17] = { 0 };
            isp_nr_gauss(lut, s, 0);
            for (int i = 1; i <= 15; i++) {
                long e = lround(1023.0 * exp(-256.0 * i * i / s));
                if (labs((long)lut[i] - e) > 1) { printf("  gauss s=%u i=%d: %u vs %ld\n", s, i, lut[i], e); fails++; }
            }
        }
        /* the vendor tables of the test (jitter +-1) regenerate within +-1 at 100 % */
        for (int L = 0; L < 16; L++) {
            const uint8_t *p = g_live[ISP_NR_NR2] + eoff(ISP_NR_NR2, 1 + L);
            uint16_t lut[17];
            for (int i = 0; i < 17; i++) lut[i] = (uint16_t)r16(p + 0x0e + 2 * i);
            uint16_t fl = lut[15];
            for (int i = 1; i <= 15; i++) if (lut[i] < fl) fl = lut[i];
            isp_nr_gauss(lut, r16(p + 0x0c), fl);
            for (int i = 1; i <= 15; i++) CHECK(abs((int)lut[i] - (int)r16(p + 0x0e + 2 * i)) <= 1);
        }
    }

    /* ---- (e) coring clamps at 255, motion at 8191 with hi > lo */
    {
        uint8_t *sh = malloc(SHARP_BLOCK_SIZE);
        memcpy(sh, g_live[ISP_NR_SHARP], SHARP_BLOCK_SIZE);
        static const unsigned cor[8] = SHARP_E_CORING_OFFS_INIT;
        w16(sh + SHARP_ENTRY_OFF(16) + cor[0], 200);
        w16(sh + SHARP_ENTRY_OFF(1) + cor[1], 255);
        isp_nr_sharp_coring(sh, 300);
        CHECK(r16(sh + SHARP_ENTRY_OFF(16) + cor[0]) == 255 && r16(sh + SHARP_ENTRY_OFF(1) + cor[1]) == 255);
        CHECK(r16(sh + SHARP_ENTRY_OFF(2) + cor[2]) == r16(g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(2) + cor[2]) * 3);
        CHECK(memcmp(sh + SHARP_ENTRY_OFF(0), g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(0), SHARP_ENTRY_SIZE) == 0);
        free(sh);
        isp_nr_knobs_t kn = kd;
        kn.v[ISP_KNOB_NR3D_MOTION] = 200;
        uint8_t *out = malloc(7180);
        CHECK(isp_nr_compose(ISP_NR_3DNR, g_live[ISP_NR_3DNR], 7180, &kn, out) == 0);
        for (int k = 1; k <= 16; k++) {
            unsigned lo = r16(out + eoff(ISP_NR_3DNR, k) + 6), hi = r16(out + eoff(ISP_NR_3DNR, k) + 8);
            CHECK(lo <= 8191 && hi <= 8191 && hi > lo);
            unsigned vlo = r16(g_live[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, k) + 6);
            if (vlo * 2 <= 8191) CHECK(lo == vlo * 2);
        }
        CHECK(r16(out + eoff(ISP_NR_3DNR, 16) + 6) == 8190 && r16(out + eoff(ISP_NR_3DNR, 16) + 8) == 8191);
        kn.v[ISP_KNOB_NR3D_MOTION] = 50;
        CHECK(isp_nr_compose(ISP_NR_3DNR, g_live[ISP_NR_3DNR], 7180, &kn, out) == 0);
        for (int k = 1; k <= 16; k++) CHECK(r16(out + eoff(ISP_NR_3DNR, k) + 8) > r16(out + eoff(ISP_NR_3DNR, k) + 6));
        free(out);
    }

    /* ---- (f) refusal on a baseline that does not match; bias alone needs no table decode */
    {
        isp_nr_knobs_t kn = kd;
        kn.v[ISP_KNOB_NR2D] = 150;
        uint8_t *bad = malloc(3064), *out = malloc(3064);
        memcpy(bad, g_live[ISP_NR_NR1], 3064);
        w32(bad, 0);
        CHECK(isp_nr_compose(ISP_NR_NR1, bad, 3064, &kn, out) == ISP_NR_REFUSED);        /* mode word */
        memcpy(bad, g_live[ISP_NR_NR1], 3064);
        w16(bad + eoff(ISP_NR_NR1, 9) + 0x4e + 2 * 5, r16(bad + eoff(ISP_NR_NR1, 9) + 0x4e + 2 * 5) + 6);
        CHECK(isp_nr_compose(ISP_NR_NR1, bad, 3064, &kn, out) == ISP_NR_REFUSED);        /* table off the formula */
        kn.v[ISP_KNOB_NR2D] = 100; kn.v[ISP_KNOB_NR2D_BIAS] = 1;
        CHECK(isp_nr_compose(ISP_NR_NR1, bad, 3064, &kn, out) == 0);                     /* bias only: no regeneration */
        CHECK(isp_nr_compose(ISP_NR_NR1, bad, 3000, &kn, out) == -1);                    /* wrong length */
        kn.v[ISP_KNOB_NR2D_BIAS] = 0; kn.v[ISP_KNOB_NR2D] = 150;
        CHECK(isp_nr_compose(ISP_NR_NR1, bad, 3000, &kn, out) == -1);
        uint8_t *d3 = malloc(7180), *o3 = malloc(7180);
        memcpy(d3, g_live[ISP_NR_3DNR], 7180);
        kn = kd; kn.v[ISP_KNOB_NR3D_MOTION] = 120;
        CHECK(isp_nr_compose(ISP_NR_3DNR, d3, 7180, &kn, o3) == 0);
        CHECK(isp_nr_compose(ISP_NR_3DNR, d3, 7076, &kn, o3) == -1);                     /* the 7076-byte conf block is refused */
        w16(d3 + eoff(ISP_NR_3DNR, 5) + 6, 9000);                                        /* 13-bit field out of range */
        CHECK(isp_nr_compose(ISP_NR_3DNR, d3, 7180, &kn, o3) == ISP_NR_REFUSED);
        free(bad); free(out); free(d3); free(o3);
        uint8_t *sb = malloc(SHARP_BLOCK_SIZE);
        memcpy(sb, g_live[ISP_NR_SHARP], SHARP_BLOCK_SIZE);
        CHECK(isp_nr_sharp_check(sb, SHARP_BLOCK_SIZE) == 0);
        w32(sb, 0);
        CHECK(isp_nr_sharp_check(sb, SHARP_BLOCK_SIZE) == ISP_NR_REFUSED);
        CHECK(isp_nr_sharp_check(g_live[ISP_NR_SHARP], 1000) == ISP_NR_REFUSED);
        free(sb);

        /* through the senders: nothing sent, knob unchanged, live untouched */
        w16(g_live[ISP_NR_UVNR] + eoff(ISP_NR_UVNR, 7) + 8 + 2 * 4, r16(g_live[ISP_NR_UVNR] + eoff(ISP_NR_UVNR, 7) + 8 + 2 * 4) + 9);
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_nr_knob_set(ISP_KNOB_UVNR, 200) == ISP_NR_REFUSED);
        CHECK(isp_nr_knob_get(ISP_KNOB_UVNR) == 100 && total_sets() == 0);
        CHECK(isp_nr_knob_set(ISP_KNOB_UVNR_BIAS, 1) >= 0);                              /* bias still works */
        CHECK(isp_nr_knob_set(ISP_KNOB_UVNR_BIAS, 0) >= 0);
        memcpy(g_live[ISP_NR_UVNR], g_pristine[ISP_NR_UVNR], 720);
        isp_reset_picture_baselines();
        clear_counts();
    }

    /* ---- (g)(h) through the senders: set, then back to defaults re-sends the original bytes exactly */
    {
        restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D_BIAS, 2) == 2);                              /* NR1 + NR2 */
        CHECK(g_sets[ISP_NR_NR1] == 1 && g_sets[ISP_NR_NR2] == 1 && total_sets() == 2);
        CHECK(!live_is_pristine(ISP_NR_NR1) && live_is_pristine(ISP_NR_UVNR) && live_is_pristine(ISP_NR_3DNR));
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 200) == 2);                                 /* on top of the bias */
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_MOTION, 150) == 1 && g_sets[ISP_NR_3DNR] == 1);
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 1) == 1);
        CHECK(memcmp(g_live[ISP_NR_3DNR] + 4 + 17 * 416, g_pristine[ISP_NR_3DNR] + 4 + 17 * 416, 7180 - 4 - 17 * 416) == 0); /* tail */
        CHECK(isp_nr_knob_set(ISP_KNOB_UVNR, 300) == 1);
        clear_counts();
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 100) == 2);                                 /* still biased: composed, not baseline */
        CHECK(!live_is_pristine(ISP_NR_NR1));
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D_BIAS, 0) == 2);                              /* default: the original bytes */
        CHECK(live_is_pristine(ISP_NR_NR1) && live_is_pristine(ISP_NR_NR2));
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D_BIAS, 0) == 0);                              /* already original: nothing sent */
        CHECK(g_sets[ISP_NR_NR1] == 2 && g_sets[ISP_NR_NR2] == 2);
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_MOTION, 100) == 1);                          /* bias 1 still on */
        CHECK(!live_is_pristine(ISP_NR_3DNR));
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 0) == 1 && live_is_pristine(ISP_NR_3DNR));
        CHECK(isp_nr_knob_set(ISP_KNOB_UVNR, 100) == 1 && live_is_pristine(ISP_NR_UVNR));
        int before = total_sets();
        for (int k = 0; k < ISP_KNOB_COUNT; k++) CHECK(isp_nr_knob_set(k, kd.v[k]) == 0);
        CHECK(total_sets() == before);

        /* (h) a baseline whose rc tail is not zero keeps it; with the zero tail the SET is zero there too */
        memset(g_live[ISP_NR_3DNR] + 4 + 17 * 416, 0xa5, 104);
        memcpy(g_pristine[ISP_NR_3DNR], g_live[ISP_NR_3DNR], 7180);
        isp_reset_picture_baselines();
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_MOTION, 120) == 1);
        for (size_t i = 4 + 17 * 416; i < 7180; i++) CHECK(g_live[ISP_NR_3DNR][i] == 0xa5);
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_MOTION, 100) == 1 && live_is_pristine(ISP_NR_3DNR));
        build_live(); snapshot();                                                       /* zero tail again */
        isp_reset_picture_baselines();
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 2) == 1);
        for (size_t i = 4 + 17 * 416; i < 7180; i++) CHECK(g_live[ISP_NR_3DNR][i] == 0);
        CHECK(isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 0) == 1);
        CHECK(ISP_3DNR_IOCTL_BYTES == 7180 && isp_nr_block_len(ISP_NR_3DNR) == 7180);
    }

    /* ---- a day/night reload: the vendor bytes come back, the non-default knobs are applied again on them */
    {
        restore_vendor();
        isp_reset_picture_baselines();
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 200) == 2 && isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 1) == 1);
        uint8_t ex1[3064], ex3[7180];
        memcpy(ex1, g_live[ISP_NR_NR1], 3064);
        memcpy(ex3, g_live[ISP_NR_3DNR], 7180);
        /* the night subfile: other vendor tables */
        for (int b = 0; b < ISP_NR_BLOCKS; b++) { g_lcg += 77; }
        build_live();
        for (int k = 1; k <= 16; k++) w16(g_live[ISP_NR_UVNR] + eoff(ISP_NR_UVNR, k) + 6 + 0, r16(g_live[ISP_NR_UVNR] + eoff(ISP_NR_UVNR, k) + 6)); /* unchanged */
        snapshot();
        clear_counts();
        isp_reset_picture_baselines();
        CHECK(g_sets[ISP_NR_NR1] == 1 && g_sets[ISP_NR_NR2] == 1 && g_sets[ISP_NR_3DNR] == 1 && g_sets[ISP_NR_UVNR] == 0);
        isp_nr_knobs_t kn = kd;
        kn.v[ISP_KNOB_NR2D] = 200;
        uint8_t *o = malloc(3064);
        CHECK(isp_nr_compose(ISP_NR_NR1, g_pristine[ISP_NR_NR1], 3064, &kn, o) == 0 && memcmp(o, g_live[ISP_NR_NR1], 3064) == 0);
        free(o);
        (void)ex3;
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 100) == 2 && isp_nr_knob_set(ISP_KNOB_NR3D_BIAS, 0) == 1);
        CHECK(live_is_pristine(ISP_NR_NR1) && live_is_pristine(ISP_NR_3DNR));
    }

    /* ---- SHARP: bias -> slider -> coring, one SET; defaults send nothing; back to defaults */
    {
        restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 0) == 0 && isp_nr_knob_set(ISP_KNOB_CORING, 100) == 0 && g_sets[ISP_NR_SHARP] == 0);
        CHECK(isp_set_sharpness(-15) == 0 && g_sets[ISP_NR_SHARP] == 1);
        uint8_t slider_only[SHARP_BLOCK_SIZE];
        memcpy(slider_only, g_live[ISP_NR_SHARP], SHARP_BLOCK_SIZE);
        static const unsigned cor[8] = SHARP_E_CORING_OFFS_INIT;
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 1) == 1 && g_sets[ISP_NR_SHARP] == 2);
        CHECK(isp_nr_knob_set(ISP_KNOB_CORING, 200) == 1 && g_sets[ISP_NR_SHARP] == 3);
        for (int L = 0; L < 16; L += 5) {
            int src = L + 1 > 15 ? 15 : L + 1;
            for (int i = 0; i < 8; i++) {
                unsigned v = r16(g_pristine[ISP_NR_SHARP] + SHARP_ENTRY_OFF(1 + src) + cor[i]);
                v = (v * 331u) >> 8;                    /* slider -15: inv_gain_q8 = 65 * 255 / 50 = 331, on the biased value */
                if (v > 255) v = 255;
                v = v * 200 / 100;
                if (v > 255) v = 255;
                CHECK(r16(g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(1 + L) + cor[i]) == v);
            }
        }
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 0) == 1 && isp_nr_knob_set(ISP_KNOB_CORING, 100) == 1);
        CHECK(memcmp(g_live[ISP_NR_SHARP], slider_only, SHARP_BLOCK_SIZE) == 0);        /* knobs off = the slider alone, byte for byte */
        /* knobs alone (no slider applied) and back to the pristine block */
        CHECK(isp_set_sharpness(0) == 0);
        isp_reset_picture_baselines();                                                   /* slider 0 is not re-applied */
        restore_vendor();
        clear_counts();
        CHECK(isp_nr_knob_set(ISP_KNOB_CORING, 150) == 1 && g_sets[ISP_NR_SHARP] == 1);
        CHECK(r16(g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(3) + cor[0]) == r16(g_pristine[ISP_NR_SHARP] + SHARP_ENTRY_OFF(3) + cor[0]) * 150 / 100);
        CHECK(memcmp(g_live[ISP_NR_SHARP] + SHARP_ENTRY_OFF(1) + 0x674, g_pristine[ISP_NR_SHARP] + SHARP_ENTRY_OFF(1) + 0x674, 512) == 0); /* no slider: curves untouched */
        CHECK(isp_nr_knob_set(ISP_KNOB_CORING, 100) == 1 && live_is_pristine(ISP_NR_SHARP));
        CHECK(isp_nr_knob_set(ISP_KNOB_CORING, 100) == 0 && g_sets[ISP_NR_SHARP] == 2);
        /* a reload re-applies the knob on the fresh block */
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 2) == 1);
        restore_vendor();
        clear_counts();
        isp_reset_picture_baselines();
        CHECK(g_sets[ISP_NR_SHARP] == 1 && !live_is_pristine(ISP_NR_SHARP));
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 0) == 1 && live_is_pristine(ISP_NR_SHARP));
        /* refusal */
        w32(g_live[ISP_NR_SHARP], 0);
        isp_reset_picture_baselines();
        CHECK(isp_nr_knob_set(ISP_KNOB_SHARP_BIAS, 1) == ISP_NR_REFUSED && isp_nr_knob_get(ISP_KNOB_SHARP_BIAS) == 0);
        restore_vendor();
        isp_reset_picture_baselines();
    }

    /* ---- the NR1 / NR2 live dump of akam-01 and akam-02: NR1 mode word 192, every linkage entry zero (s = 0), s = 64 in
     *      the manual entry. These used to be refused ("live table not as decoded", "NR1 unreadable"): now accepted */
    {
        static const char *const dumps[] = { FIXTURE_DIR "/isp_nr_dump_akam-01.txt", FIXTURE_DIR "/isp_nr_dump_akam-02.txt" };
        for (int di = 0; di < 2; di++) {
            build_live();
            CHECK(load_dump(dumps[di]) == 0);
            CHECK(r32(g_live[ISP_NR_NR1]) == 192 && r16(g_live[ISP_NR_NR1] + eoff(ISP_NR_NR1, 0) + 6) == 64);
            snapshot();
            restore_vendor();
            isp_reset_picture_baselines();
            clear_counts();
            isp_nr_knobs_t kn = kd;
            kn.v[ISP_KNOB_NR2D] = 150; kn.v[ISP_KNOB_NR2D_BIAS] = 2;
            uint8_t *o1 = malloc(3064), *o2 = malloc(992);
            CHECK(isp_nr_compose(ISP_NR_NR1, g_live[ISP_NR_NR1], 3064, &kn, o1) == 0);
            CHECK(isp_nr_compose(ISP_NR_NR2, g_live[ISP_NR_NR2], 992, &kn, o2) == 0);
            CHECK(memcmp(o1, g_live[ISP_NR_NR1], 3064) == 0);                         /* all entries are s = 0: nothing to change */
            CHECK(r16(o2 + eoff(ISP_NR_NR2, 3) + 0x0c) * 100 / 150 >= 700 && r16(o2 + eoff(ISP_NR_NR2, 3) + 0x0c) <= 6000 * 150 / 100);
            CHECK(r16(o2 + eoff(ISP_NR_NR2, 1) + 0x0c) == 800 * 150 / 100);          /* level 0 took vendor entry 3 (s 800), x 1.5 */
            free(o1); free(o2);
            CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 150) == 2);                          /* both blocks accepted by the senders */
            CHECK(g_sets[ISP_NR_NR1] == 1 && g_sets[ISP_NR_NR2] == 1);
            CHECK(r32(g_live[ISP_NR_NR1]) == 192 && r16(g_live[ISP_NR_NR2] + eoff(ISP_NR_NR2, 3) + 0x0c) == 1200);
            CHECK(memcmp(g_live[ISP_NR_NR1], g_pristine[ISP_NR_NR1], 3064) == 0);
            CHECK(isp_nr_knob_set(ISP_KNOB_NR2D_BIAS, 2) == 2);
            CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 100) == 2 && isp_nr_knob_set(ISP_KNOB_NR2D_BIAS, 0) == 2);
            CHECK(live_is_pristine(ISP_NR_NR1) && live_is_pristine(ISP_NR_NR2));
            /* GET img.nr no longer says "NR1 unreadable" */
            level_expo();
            memset(g_run, 0, sizeof(g_run));
            w32(g_run + 4, 600); w32(g_run + 8, 256); w32(g_run + 12, 256);
            char line[512];
            CHECK(isp_nr_status(line, sizeof(line)) == 0 && !strstr(line, "unreadable") && strstr(line, "NR1.s=0/0/0"));
            /* a mode word that is neither 1 nor 192 is still refused, and so is NR2 with mode 192 */
            w32(g_live[ISP_NR_NR1], 3);
            isp_reset_picture_baselines();
            clear_counts();
            CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 150) == ISP_NR_REFUSED && total_sets() == 0 && isp_nr_knob_get(ISP_KNOB_NR2D) == 100);
            w32(g_live[ISP_NR_NR1], 192);
            w32(g_live[ISP_NR_NR2], 192);
            isp_reset_picture_baselines();
            CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 150) == ISP_NR_REFUSED && total_sets() == 0);
            /* the 3D_NR, UVNR of the dump take the bias (the picture default) */
            restore_vendor();
            isp_reset_picture_baselines();
            CHECK(isp_set_denoise(2) == 0 && g_sets[ISP_NR_3DNR] == 1 && g_sets[ISP_NR_NR1] == 0);
            CHECK(isp_set_denoise(0) == 0 && live_is_pristine(ISP_NR_3DNR));
        }
        build_live(); snapshot(); restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
    }

    /* ---- isp.denoise = img.nr3d_bias 2 (the 1080p default): exactly the vendor entries min(L+2,15) of 3D_NR, one SET, nothing
     *      else; 0 (stock) sends nothing and puts the original bytes back; a refusal keeps the stock tables running */
    {
        restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_set_denoise(0) == 0 && total_sets() == 0 && g_gets[ISP_NR_3DNR] == 0);          /* stock path: no GET, no SET */
        CHECK(isp_set_sharpness(0) == 0 && g_sets[ISP_NR_SHARP] == 1);                           /* slider 0 on its own sends the block, as before */
        restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_set_denoise(2) == 0);
        CHECK(total_sets() == 1 && g_sets[ISP_NR_3DNR] == 1);
        for (int b = 0; b < ISP_NR_BLOCKS; b++) if (b != ISP_NR_3DNR) CHECK(live_is_pristine(b));
        CHECK(memcmp(g_live[ISP_NR_3DNR], g_pristine[ISP_NR_3DNR], eoff(ISP_NR_3DNR, 1)) == 0);   /* mode word + manual entry */
        for (int L = 0; L < 16; L++) {
            int src = L + 2 > 15 ? 15 : L + 2;
            CHECK(memcmp(g_live[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, 1 + L), g_pristine[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, 1 + src), 416) == 0);
        }
        CHECK(memcmp(g_live[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, 17), g_pristine[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, 17), 7180 - eoff(ISP_NR_3DNR, 17)) == 0);
        /* a day/night reload: the vendor block is back, the default is applied again on it */
        uint8_t first[7180];
        memcpy(first, g_live[ISP_NR_3DNR], 7180);
        restore_vendor();
        clear_counts();
        isp_reset_picture_baselines();
        CHECK(g_sets[ISP_NR_3DNR] == 1 && total_sets() == 1 && memcmp(g_live[ISP_NR_3DNR], first, 7180) == 0);
        /* back to stock: the original bytes */
        CHECK(isp_set_denoise(0) == 0 && live_is_pristine(ISP_NR_3DNR));
        clear_counts();
        isp_reset_picture_baselines();
        CHECK(total_sets() == 0);                                                                 /* stock after a reload: nothing sent */
        /* refusal: a live block that is not as decoded gets nothing, the knob stays 0, and a reload does not touch it */
        w32(g_live[ISP_NR_3DNR], 0);
        isp_reset_picture_baselines();
        CHECK(isp_set_denoise(2) == ISP_NR_REFUSED && total_sets() == 0 && isp_nr_knob_get(ISP_KNOB_NR3D_BIAS) == 0);
        restore_vendor();
        w16(g_live[ISP_NR_3DNR] + eoff(ISP_NR_3DNR, 4), 0x1234);                                  /* enable is a bit, not 0x1234 */
        isp_reset_picture_baselines();
        CHECK(isp_set_denoise(2) == ISP_NR_REFUSED && total_sets() == 0);
        restore_vendor();
        isp_reset_picture_baselines();
        clear_counts();
    }

    /* ---- isp.nr2 (isp.c / isp_nr_ctl.c): N steps of 50 % on the NR2 strength (the 2K's 0..4), 0 = the camera's own table (nothing sent) */
    {
        restore_vendor();
        for (int k = 0; k < ISP_KNOB_COUNT; k++) isp_nr_knob_set(k, isp_nr_knob_info(k)->def);
        isp_reset_picture_baselines();
        restore_vendor();
        clear_counts();
        CHECK(isp_get_nr2() == 0);
        CHECK(isp_set_nr2(0) == 0 && total_sets() == 0 && g_gets[ISP_NR_NR2] == 0);              /* the default sends and reads nothing */
        CHECK(isp_set_nr2(-1) == -1 && isp_set_nr2(5) == -1 && total_sets() == 0 && isp_get_nr2() == 0);
        CHECK(isp_set_nr2(4) == 0 && isp_get_nr2() == 4 && isp_nr_knob_get(ISP_KNOB_NR2D) == 300 && g_sets[ISP_NR_NR2] == 1);
        CHECK(!live_is_pristine(ISP_NR_NR2) && live_is_pristine(ISP_NR_UVNR) && live_is_pristine(ISP_NR_3DNR) && live_is_pristine(ISP_NR_SHARP));
        {   /* the same bytes as the img.nr2d 300 knob */
            isp_nr_knobs_t kn = kd;
            uint8_t *want = malloc(isp_nr_block_len(ISP_NR_NR2));
            kn.v[ISP_KNOB_NR2D] = 300;
            CHECK(isp_nr_compose(ISP_NR_NR2, g_pristine[ISP_NR_NR2], isp_nr_block_len(ISP_NR_NR2), &kn, want) == 0);
            CHECK(memcmp(want, g_live[ISP_NR_NR2], isp_nr_block_len(ISP_NR_NR2)) == 0);
            free(want);
        }
        /* a reload puts the vendor tables back; the level is applied again on the fresh baselines */
        restore_vendor();
        clear_counts();
        isp_reset_picture_baselines();
        CHECK(isp_get_nr2() == 4 && g_sets[ISP_NR_NR2] == 1 && !live_is_pristine(ISP_NR_NR2));
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 162) >= 0 && isp_get_nr2() == 1);                   /* a raw percent reads as the nearest step */
        CHECK(isp_nr_knob_set(ISP_KNOB_NR2D, 80) >= 0 && isp_get_nr2() == 0);
        CHECK(isp_set_nr2(2) == 0 && isp_nr_knob_get(ISP_KNOB_NR2D) == 200 && isp_get_nr2() == 2);
        clear_counts();
        CHECK(isp_set_nr2(0) == 0 && isp_get_nr2() == 0 && g_sets[ISP_NR_NR2] == 1 && live_is_pristine(ISP_NR_NR2));  /* back: the original bytes, once */
        CHECK(isp_set_nr2(0) == 0 && g_sets[ISP_NR_NR2] == 1);
        /* a live NR2 block that is not as decoded: refused, nothing sent, the level stays */
        w32(g_live[ISP_NR_NR2], 7);
        isp_reset_picture_baselines();
        clear_counts();
        CHECK(isp_set_nr2(3) == ISP_NR_REFUSED && total_sets() == 0 && isp_get_nr2() == 0);
        restore_vendor();
        isp_reset_picture_baselines();
    }

    /* ---- ISO level and GET img.nr */
    {
        level_expo();
        memset(g_run, 0, sizeof(g_run));
        w32(g_run + 4, 600); w32(g_run + 8, 256); w32(g_run + 12, 256);              /* G = 600 */
        uint32_t g;
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 1 && g == 600);
        w32(g_run + 4, 100);
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 0 && g == 100);
        w32(g_run + 4, 16384); w32(g_run + 8, 1024); w32(g_run + 12, 512);          /* G = 16384 * 4 * 2 = 131072 */
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 15 && g == 131072);
        w32(g_expo + 8, 1); w32(g_expo + 0x3c, 342); w32(g_run + 16, 100);           /* the special case: level 0 */
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 0 && g == 0);
        w32(g_run + 16, 400);
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 15);
        w32(g_expo + 0x98 + 8 * 4, 5000); w32(g_expo + 0x98 + 8 * 4 + 4, 5000);     /* an empty pair is skipped */
        w32(g_run + 4, 600); w32(g_run + 8, 256); w32(g_run + 12, 256);
        CHECK(isp_nr_level(g_expo, sizeof(g_expo), g_run, sizeof(g_run), &g) == 1);

        w32(g_expo + 8, 0);
        char line[512];
        clear_counts();
        CHECK(isp_nr_status(line, sizeof(line)) == 0);
        CHECK(strstr(line, "img.nr=G=600 level=1 knobs=0,0,0,0,100,100,100,100"));
        unsigned nr2s = r16(g_live[ISP_NR_NR2] + eoff(ISP_NR_NR2, 2) + 0x0c);
        char want[32];
        snprintf(want, sizeof(want), "NR2.s=%u", nr2s);
        CHECK(strstr(line, want) && strstr(line, "SHARP.coring=") && strstr(line, "q8="));
        CHECK(total_sets() == 0);                                                        /* read-only */
    }

    /* ---- SET img.dump nr: one header and 17 entry lines per block */
    {
        extern int isp_nr_dump_live(void);
        clear_counts();
        CHECK(isp_nr_dump_live() == 5 && total_sets() == 0);
        CHECK(g_gets[0] == 1 && g_gets[3] == 1 && g_gets[4] == 1);
    }

    if (fails) { printf("isp_nr_test: %d failure(s)\n", fails); return 1; }
    puts("isp_nr_test: OK");
    return 0;
}
