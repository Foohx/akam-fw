#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "isp_nr.h"
#include "isp_nr_layout.h"

#define SCRATCH 65536

static const isp_nr_knob_info_t g_info[ISP_KNOB_COUNT] = {
    { "img.nr2d_bias",    0,   4,   0,   "NR1+NR2", "0..4" },
    { "img.uvnr_bias",    0,   4,   0,   "UVNR",    "0..4" },
    { "img.nr3d_bias",    0,   4,   0,   "3D_NR",   "0..4" },
    { "img.sharp_bias",   0,   4,   0,   "SHARP",   "0..4" },
    { "img.nr2d",         50,  400, 100, "NR1+NR2", "50..400 (percent)" },
    { "img.uvnr",         50,  400, 100, "UVNR",    "50..400 (percent)" },
    { "img.sharp_coring", 100, 300, 100, "SHARP",   "100..300 (percent)" },
    { "img.nr3d_motion",  50,  200, 100, "3D_NR",   "50..200 (percent)" },
};

const isp_nr_knob_info_t *isp_nr_knob_info(int k) { return (k >= 0 && k < ISP_KNOB_COUNT) ? &g_info[k] : NULL; }

int isp_nr_knob_find(const char *key)
{
    for (int i = 0; i < ISP_KNOB_COUNT; i++)
        if (!strcmp(key, g_info[i].key)) return i;
    return -1;
}

void isp_nr_knobs_default(isp_nr_knobs_t *k)
{
    for (int i = 0; i < ISP_KNOB_COUNT; i++) k->v[i] = g_info[i].def;
}

int isp_nr_block_default(int block, const isp_nr_knobs_t *k)
{
    int a, b;
    switch (block) {
    case ISP_NR_NR1: case ISP_NR_NR2: a = ISP_KNOB_NR2D_BIAS; b = ISP_KNOB_NR2D; break;
    case ISP_NR_UVNR:  a = ISP_KNOB_UVNR_BIAS;  b = ISP_KNOB_UVNR; break;
    case ISP_NR_3DNR:  a = ISP_KNOB_NR3D_BIAS;  b = ISP_KNOB_NR3D_MOTION; break;
    case ISP_NR_SHARP: a = ISP_KNOB_SHARP_BIAS; b = ISP_KNOB_CORING; break;
    default: return 1;
    }
    return k->v[a] == g_info[a].def && k->v[b] == g_info[b].def;
}

typedef struct blk { const char *name; uint32_t set, get; size_t len, esize; } blk_t;
static const blk_t g_blk[ISP_NR_BLOCKS] = {
    { "NR1",   0x4004490c, 0x8004490d, 3064,  180 },
    { "NR2",   0x4004492b, 0x8004492c, 992,   58 },
    { "UVNR",  0x4004497d, 0x8004497e, 720,   42 },
    { "3D_NR", 0x4004492f, 0x80044930, ISP_3DNR_IOCTL_BYTES, 416 },
    { "SHARP", 0x40044925, 0x80044926, SHARP_BLOCK_SIZE, SHARP_ENTRY_SIZE },
};
size_t isp_nr_block_len(int b)       { return g_blk[b].len; }
uint32_t isp_nr_get_cmd(int b)       { return g_blk[b].get; }
uint32_t isp_nr_set_cmd(int b)       { return g_blk[b].set; }
const char *isp_nr_block_name(int b) { return g_blk[b].name; }

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
/* start of entry k (0 = manual, 1 + L = ISO level L) of a block */
static size_t ent(int block, int k) { return 4 + (size_t)k * g_blk[block].esize; }

/* ---------------------------------------------------------------- Gaussian tables, integer only */
/* e^-x for x = x24 / 2^24, result in Q28. 2^-(x log2 e) = 2^-n * e^-(f ln 2); the 7-term series on f ln 2 < 0.7 is good
 * to 1e-6, far below the rounding of the 10-bit table. */
static uint32_t exp_neg_q28(uint64_t x24)
{
    uint64_t y = (x24 * 24204406ull) >> 24;                 /* x * log2(e), Q24 */
    uint64_t n = y >> 24;
    if (n >= 16) return 0;
    uint64_t t = ((y & 0xffffffu) * 11629080ull) >> 20;     /* frac * ln2, Q28 */
    int64_t term = 1ll << 28, sum = term;
    for (int k = 1; k <= 7; k++) {
        term = (int64_t)(((uint64_t)term * t) >> 28) / k;
        sum += (k & 1) ? -term : term;
    }
    return (uint32_t)((uint64_t)sum >> n);
}

static uint16_t gauss_at(int i, uint32_t s)
{
    uint64_t x24 = (((uint64_t)256 * (uint64_t)i * (uint64_t)i) << 24) / s;
    return (uint16_t)((1023ull * exp_neg_q28(x24) + (1ull << 27)) >> 28);
}

void isp_nr_gauss(uint16_t lut[17], uint32_t s, uint16_t floor_v)
{
    if (s == 0) return;
    for (int i = 1; i <= 15; i++) {
        uint16_t v = gauss_at(i, s);
        lut[i] = v < floor_v ? floor_v : v;
    }
}

static uint16_t tbl_floor(const uint8_t *tbl)
{
    uint32_t m = 0xffff;
    for (int i = 1; i <= 15; i++) { uint32_t v = rd16(tbl + 2 * i); if (v < m) m = v; }
    return (uint16_t)m;
}

/* 1 when the 17-point table at tbl is the formula of s within +-1 (lut[0] and lut[16] are not read) */
static int gauss_ok(const uint8_t *tbl, uint32_t s)
{
    uint16_t fl = tbl_floor(tbl);
    for (int i = 1; i <= 15; i++) {
        int v = (int)rd16(tbl + 2 * i), e = gauss_at(i, s);
        if (e < fl) e = fl;
        if (v > 1023 || v < e - 1 || v > e + 1) return 0;
    }
    return 1;
}

/* regenerates the table at tbl from s' = s * pct / 100 and writes s' at sp; skipped when s = 0 */
static void gauss_scale(uint8_t *tbl, uint8_t *sp, int pct)
{
    uint32_t s = rd16(sp);
    if (!s) return;
    uint32_t s2 = s * (uint32_t)pct / 100;
    if (s2 < 1) s2 = 1;
    if (s2 > 0xffff) s2 = 0xffff;
    uint16_t lut[17];
    for (int i = 0; i < 17; i++) lut[i] = (uint16_t)rd16(tbl + 2 * i);
    isp_nr_gauss(lut, s2, tbl_floor(tbl));
    for (int i = 1; i <= 15; i++) wr16(tbl + 2 * i, lut[i]);
    wr16(sp, s2);
}

/* (offset of s, offset of table) pairs of one entry */
static int gauss_pairs(int block, int off[3][2])
{
    switch (block) {
    case ISP_NR_NR1:
        off[0][0] = 0x06; off[0][1] = 0x4e; off[1][0] = 0x08; off[1][1] = 0x70; off[2][0] = 0x0a; off[2][1] = 0x92;
        return 3;
    case ISP_NR_NR2:  off[0][0] = 0x0c; off[0][1] = 0x0e; return 1;
    case ISP_NR_UVNR: off[0][0] = 0x06; off[0][1] = 0x08; return 1;
    }
    return 0;
}

static void bias_copy(int block, uint8_t *out, const uint8_t *base, int n)
{
    for (int l = 0; l < ISP_LINK_LEVELS; l++) {
        int src = l + n > 15 ? 15 : l + n;
        memcpy(out + ent(block, 1 + l), base + ent(block, 1 + src), g_blk[block].esize);
    }
}

int isp_nr_compose(int block, const uint8_t *base, size_t len, const isp_nr_knobs_t *k, uint8_t *out)
{
    if (block < 0 || block >= ISP_NR_SHARP || len != g_blk[block].len) return -1;
    if (isp_nr_block_default(block, k)) { memcpy(out, base, len); return 0; }
    if (rd32(base) != 1) return ISP_NR_REFUSED;
    int bias, pct, off[3][2], np = gauss_pairs(block, off);
    switch (block) {
    case ISP_NR_NR1: case ISP_NR_NR2: bias = k->v[ISP_KNOB_NR2D_BIAS]; pct = k->v[ISP_KNOB_NR2D]; break;
    case ISP_NR_UVNR:                 bias = k->v[ISP_KNOB_UVNR_BIAS]; pct = k->v[ISP_KNOB_UVNR]; break;
    default:                          bias = k->v[ISP_KNOB_NR3D_BIAS]; pct = k->v[ISP_KNOB_NR3D_MOTION]; break;
    }
    /* validate, on the baseline, every entry that is going to be rewritten */
    if (pct != 100) {
        for (int e = 1; e <= ISP_LINK_LEVELS; e++) {
            const uint8_t *p = base + ent(block, e);
            for (int j = 0; j < np; j++) {
                uint32_t s = rd16(p + off[j][0]);
                if (s && !gauss_ok(p + off[j][1], s)) return ISP_NR_REFUSED;
            }
            if (block == ISP_NR_3DNR && (rd16(p + 6) > 8191 || rd16(p + 8) > 8191)) return ISP_NR_REFUSED;
        }
    }
    memcpy(out, base, len);
    if (bias) bias_copy(block, out, base, bias + 0 * 1);
    if (pct == 100) return 0;
    for (int e = 1; e <= ISP_LINK_LEVELS; e++) {
        uint8_t *p = out + ent(block, e);
        if (np) {
            for (int j = 0; j < np; j++) gauss_scale(p + off[j][1], p + off[j][0], pct);
        } else { /* 3D_NR motion ramp */
            uint32_t lo = rd16(p + 6) * (uint32_t)pct / 100, hi = rd16(p + 8) * (uint32_t)pct / 100;
            if (lo > 8191) lo = 8191;
            if (hi > 8191) hi = 8191;
            if (hi <= lo) { if (lo >= 8191) lo = 8190; hi = lo + 1; }
            wr16(p + 6, lo);
            wr16(p + 8, hi);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- SHARP */
static const uint16_t g_coring[8] = SHARP_E_CORING_OFFS_INIT;
static const uint16_t g_gain8[8] = SHARP_E_GAIN_OFFS_INIT;

int isp_nr_sharp_check(const uint8_t *base, size_t len)
{
    if (len != SHARP_BLOCK_SIZE || rd32(base) != 1) return ISP_NR_REFUSED;
    for (int e = 1; e <= ISP_LINK_LEVELS; e++)
        for (int i = 0; i < 8; i++)
            if (rd16(base + SHARP_ENTRY_OFF(e) + g_coring[i]) > 255) return ISP_NR_REFUSED;
    return 0;
}

void isp_nr_sharp_bias(const uint8_t *base, int n, uint8_t *out)
{
    memcpy(out, base, SHARP_BLOCK_SIZE);
    if (n) bias_copy(ISP_NR_SHARP, out, base, n);
}

void isp_nr_sharp_coring(uint8_t *buf, int pct)
{
    if (pct == 100) return;
    for (int e = 1; e <= ISP_LINK_LEVELS; e++)
        for (int i = 0; i < 8; i++) {
            uint8_t *p = buf + SHARP_ENTRY_OFF(e) + g_coring[i];
            uint32_t v = rd16(p) * (uint32_t)pct / 100;
            wr16(p, v > 255 ? 255 : v);
        }
}

/* ---------------------------------------------------------------- ISO level */
int isp_nr_level(const uint8_t *expo, size_t expo_len, const uint8_t *run, size_t run_len, uint32_t *g)
{
    *g = 0;
    if (expo_len < EXPO_ISO_PAIRS_OFF + 16 * 8 || run_len < 20) return 0;
    uint32_t G = (((rd32(run + 4) * rd32(run + 8)) >> 8) * rd32(run + 12)) >> 8; /* 32-bit wrap, as the kernel */
    if (rd32(expo + EXPO_ISO_FLAG_OFF) != 0 && rd32(run + 16) < rd32(expo + EXPO_ISO_MIN_OFF)) G = 0;
    *g = G;
    for (int i = 0; i < 16; i++) {
        uint32_t lo = rd32(expo + EXPO_ISO_PAIRS_OFF + 8 * i), hi = rd32(expo + EXPO_ISO_PAIRS_OFF + 8 * i + 4);
        if (lo <= G && G < hi) return i;
    }
    return 15;
}

/* ---------------------------------------------------------------- live decode */
static int get_block(isp_get_fn get, void *gctx, int b, uint8_t *buf)
{
    memset(buf, 0, SCRATCH);
    return get(gctx, g_blk[b].get, buf) == 0 ? 0 : -1;
}

void isp_nr_live_level(isp_get_fn get, void *gctx, int level, isp_nr_live_t *o)
{
    memset(o, 0, sizeof(*o));
    uint8_t *buf = malloc(SCRATCH);
    if (!buf) return;
    for (int b = 0; b < ISP_NR_BLOCKS; b++) {
        if (get_block(get, gctx, b, buf) != 0 || rd32(buf) != 1) continue;
        const uint8_t *p = buf + ent(b, 1 + level);
        o->ok[b] = 1;
        switch (b) {
        case ISP_NR_NR1:
            o->nr1_s[0] = (uint16_t)rd16(p + 6); o->nr1_s[1] = (uint16_t)rd16(p + 8); o->nr1_s[2] = (uint16_t)rd16(p + 10);
            break;
        case ISP_NR_NR2:  o->nr2_s = (uint16_t)rd16(p + 0x0c); break;
        case ISP_NR_UVNR: o->uvnr_s = (uint16_t)rd16(p + 6); break;
        case ISP_NR_3DNR: o->d3_lo = (uint16_t)rd16(p + 6); o->d3_hi = (uint16_t)rd16(p + 8); o->d3_s = (uint16_t)rd16(p + 0x130); break;
        default:
            for (int i = 0; i < 8; i++) {
                o->coring[i] = (uint16_t)rd16(p + g_coring[i]);
                o->gain[i] = (uint16_t)rd16(p + g_gain8[i]);
            }
            break;
        }
    }
    free(buf);
}

int isp_nr_dump(isp_get_fn get, void *gctx, isp_emit_fn emit, void *ectx)
{
    uint8_t *buf = malloc(SCRATCH);
    if (!buf) return -1;
    int n = 0;
    char line[256];
    for (int b = 0; b < ISP_NR_BLOCKS; b++) {
        if (get_block(get, gctx, b, buf) != 0) {
            snprintf(line, sizeof(line), "[isp.nr] %-5s get 0x%08x failed", g_blk[b].name, g_blk[b].get);
            emit(ectx, line);
            continue;
        }
        n++;
        snprintf(line, sizeof(line), "[isp.nr] %-5s mode=%u len=%zu hash=%08x", g_blk[b].name, (unsigned)rd32(buf),
                 g_blk[b].len, (unsigned)isp_blocks_hash(buf, g_blk[b].len));
        emit(ectx, line);
        for (int e = 0; e < ISP_LINK_ENTRIES; e++) {
            const uint8_t *p = buf + ent(b, e);
            size_t w = (size_t)snprintf(line, sizeof(line), "[isp.nr] %-5s e%-2d%s en=%u ", g_blk[b].name, e,
                                        e ? "" : " (manual)", (unsigned)rd16(p));
            char *q = line + w;
            size_t room = sizeof(line) - w;
            switch (b) {
            case ISP_NR_NR1:  snprintf(q, room, "s=%u/%u/%u", (unsigned)rd16(p + 6), (unsigned)rd16(p + 8), (unsigned)rd16(p + 10)); break;
            case ISP_NR_NR2:  snprintf(q, room, "s=%u", (unsigned)rd16(p + 0x0c)); break;
            case ISP_NR_UVNR: snprintf(q, room, "s=%u", (unsigned)rd16(p + 6)); break;
            case ISP_NR_3DNR: snprintf(q, room, "lo=%u hi=%u s=%u", (unsigned)rd16(p + 6), (unsigned)rd16(p + 8), (unsigned)rd16(p + 0x130)); break;
            default: {
                size_t o = 0;
                o += (size_t)snprintf(q + o, room - o, "coring=");
                for (int i = 0; i < 8; i++) o += (size_t)snprintf(q + o, room - o, "%u%c", (unsigned)rd16(p + g_coring[i]), i < 7 ? ',' : ' ');
                o += (size_t)snprintf(q + o, room - o, "q8=");
                for (int i = 0; i < 8; i++) o += (size_t)snprintf(q + o, room - o, "%u%c", (unsigned)rd16(p + g_gain8[i]), i < 7 ? ',' : '\0');
                break;
            }
            }
            emit(ectx, line);
        }
    }
    free(buf);
    return n;
}
