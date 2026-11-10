#include <stdlib.h>
#include <string.h>

#include "isp_blocks.h"

#define CONF_HEADER_SIZE   512
#define CONF_SENSOR_ID     0x1c
#define CONF_MODE_OFFSET   31
#define CONF_MAIN_VERSION  7
#define DUMP_BUF_SIZE      (64 * 1024)

/* Offsets and sizes: module layout of isp.c (isp_send_modules) and the payload sizes seen in the stock
 * capture. A block's size plus its 4-byte offset always fits its module (checked at load time).
 * skip_words: SET_EXPOSURE starts with 7 words of runtime AE state (priv[]) that the driver rewrites. */
static const isp_block_t g_blocks[] = {
    { "BLC",            0x40044902, 0x80044903, 0,  0,      4,   312, 0 },
    { "LSC",            0x40044904, 0x80044905, 0,  1,      4,   208, 0 },
    { "RAW_LUT",        0x4004490a, 0x8004490b, 0,  2,      4,   884, 0 },
    { "NR1",            0x4004490c, 0x8004490d, 0,  3,      4,  3064, 0 },
    { "NR2",            0x4004492b, 0x8004492c, 0,  3, 0xbfc,   992, 0 },
    { "UVNR",           0x4004497d, 0x8004497e, 0,  3, 0xfdc,   720, 0 },
    { "3D_NR",          0x4004492f, 0x80044930, 0,  4,      4,  7076, 0 },
    { "GB",             0x40044906, 0x80044907, 0,  5,      4,   140, 0 },
    { "DEMO",           0x4004490e, 0x8004490f, 0,  6,      4,    18, 0 },
    { "RGB_GAMMA",      0x4004491a, 0x8004491b, 0,  7,      4,  1746, 0 },
    { "CCM",            0x40044912, 0x80044913, 0,  8,      4,   324, 0 },
    { "FCS",            0x40044936, 0x80044937, 0,  9,      4,   176, 0 },
    { "WDR",            0x4004491b, 0x8004491c, 0, 10,      4, 16936, 0 },
    { "SHARP",          0x40044925, 0x80044926, 1, 11,      4, 46584, 0 },
    { "SHARP_EX",       0x40044927, 0x80044928, 0, 11, 0xb5fc,    20, 0 },
    { "SATURATION",     0x4004493b, 0x8004493c, 0, 12,      4,   344, 0 },
    { "CONTRAST",       0x40044939, 0x8004493a, 0, 13,      4,   104, 0 },
    { "RGB2YUV",        0x4004493f, 0x80044940, 0, 14,      4,     4, 0 },
    { "EFFECT",         0x40044941, 0x80044942, 1, 15,      4,    10, 0 },
    { "DPC",            0x40044910, 0x80044911, 0, 16,      4,   208, 0 },
    { "LCE",            0x8004498e, 0,          0, 17,      4,  1500, 0 },
    { "AF",             0x40044960, 0x80044961, 0, 18,      4,    42, 0 },
    { "SET_WB",         0x80044986, 0x80044987, 1, 19,      4,  2016, 0 },
    { "AWB_EX",         0x40044973, 0x80044974, 0, 19,  0x7e4,  5124, 0 },
    { "AWB_CALIB",      0x80044988, 0,          0, 19, 0x1be8,    64, 0 },
    { "SET_EXPOSURE",   0x8004498a, 0x8004498b, 1, 20,      4,  1824, 7 },
    { "SET_FRAME_RATE", 0x4004494e, 0x8004494f, 1, 20,  0x724,    24, 0 },
    { "MISC",           0x4004496e, 0x8004496f, 0, 21,      4,    38, 0 },
    { "Y_GAMMA",        0x40044975, 0x80044976, 0, 22,      4,   302, 0 },
    { "HUE",            0x40044977, 0x80044978, 0, 23,      4,  2204, 0 },
};

const isp_block_t *isp_blocks_table(size_t *n)
{
    *n = sizeof(g_blocks) / sizeof(g_blocks[0]);
    return g_blocks;
}

static uint32_t rd_le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Walks one subfile starting at `off`: fills the module pointers when c != NULL and returns the offset
 * just after its sensor-register section, or 0 when the layout does not validate. */
static size_t walk_subfile(const uint8_t *d, size_t len, size_t off, isp_conf_t *c)
{
    if (off + CONF_HEADER_SIZE > len) return 0;
    size_t p = off + CONF_HEADER_SIZE;
    for (int i = 0; i < ISP_BLOCKS_MODULES; i++) {
        if (p + 4 > len) return 0;
        uint32_t id = rd_le16(d + p), sz = rd_le16(d + p + 2);
        if (id != (uint32_t)i || sz < 4 || p + sz > len) return 0;
        if (c) { c->mod[i] = d + p; c->len[i] = sz; }
        p += sz;
    }
    if (p + 4 > len || rd_le16(d + p) != CONF_SENSOR_ID) return 0;
    p += 4 + rd_le16(d + p + 2);
    return p <= len ? p : 0;
}

int isp_conf_load(const char *path, int mode, isp_conf_t *c)
{
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1u << 16, len = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (len < cap) break;
        cap *= 2;
        if (cap > (1u << 24)) { free(buf); buf = NULL; break; }
        uint8_t *nb = realloc(buf, cap);
        if (!nb) { free(buf); buf = NULL; break; }
        buf = nb;
    }
    fclose(f);
    if (!buf) return -1;

    size_t off = 0;
    for (int m = 0; m <= mode; m++) {
        size_t next = walk_subfile(buf, len, off, m == mode ? c : NULL);
        if (!next) { free(buf); memset(c, 0, sizeof(*c)); return -1; }
        if (m == mode) break;
        off = next;
    }
    if (rd_le32(buf + off) != CONF_MAIN_VERSION || buf[off + CONF_MODE_OFFSET] != (uint8_t)mode) {
        free(buf);
        memset(c, 0, sizeof(*c));
        return -1;
    }
    c->data = buf;
    return 0;
}

void isp_conf_free(isp_conf_t *c)
{
    free(c->data);
    memset(c, 0, sizeof(*c));
}

const uint8_t *isp_conf_payload(const isp_conf_t *c, const isp_block_t *b)
{
    if (!c->data || b->module < 0 || b->module >= ISP_BLOCKS_MODULES || !c->mod[b->module]) return NULL;
    if ((uint64_t)b->off + b->size > c->len[b->module]) return NULL;
    return c->mod[b->module] + b->off;
}

size_t isp_blocks_diff_words(const uint8_t *a, const uint8_t *b, size_t size, size_t skip_words)
{
    size_t n = 0;
    for (size_t w = skip_words * 4; w < size; w += 4) {
        size_t chunk = size - w < 4 ? size - w : 4;
        if (memcmp(a + w, b + w, chunk) != 0) n++;
    }
    return n;
}

uint32_t isp_blocks_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void write_record(FILE *bin, uint32_t t_ms, uint32_t cmd, const uint8_t *data, uint32_t size)
{
    uint8_t h[24];
    put_le32(h + 0, 0x42505349u);                 /* "ISPB" */
    put_le32(h + 4, t_ms);
    put_le32(h + 8, cmd);
    put_le32(h + 12, size);
    put_le32(h + 16, isp_blocks_hash(data, size));
    put_le32(h + 20, 0);                          /* tid */
    fwrite(h, 1, sizeof(h), bin);
    fwrite(data, 1, size, bin);
}

static const char *verdict(size_t diff, int have)
{
    if (!have) return "n/a";
    return diff == 0 ? "eq" : "diff";
}

int isp_blocks_dump(isp_get_fn get, void *gctx, const isp_dump_opts_t *o, isp_emit_fn emit, void *ectx)
{
    isp_conf_t day, night;
    int have_day = 0, have_night = 0;
    memset(&day, 0, sizeof(day));
    memset(&night, 0, sizeof(night));
    if (o->conf_path) {
        have_day = isp_conf_load(o->conf_path, 0, &day) == 0;
        have_night = isp_conf_load(o->conf_path, 1, &night) == 0;
        if (!have_day) emit(ectx, "[isp.dump] day subfile of the conf file not readable: no comparison for day");
        if (!have_night) emit(ectx, "[isp.dump] night subfile of the conf file not readable: no comparison for night");
    }

    uint8_t *buf = malloc(DUMP_BUF_SIZE);
    if (!buf) { isp_conf_free(&day); isp_conf_free(&night); return -1; }

    size_t n;
    const isp_block_t *t = isp_blocks_table(&n);
    int read_ok = 0, unreadable = 0, day_eq = 0, night_eq = 0, compared = 0;
    char line[256];

    for (size_t i = 0; i < n; i++) {
        const isp_block_t *b = &t[i];
        if (!b->get_cmd) {
            snprintf(line, sizeof(line), "[isp.dump] %-14s no getter known, skipped", b->name);
            emit(ectx, line);
            continue;
        }
        if (!b->get_confirmed && !o->all) {
            snprintf(line, sizeof(line), "[isp.dump] %-14s getter 0x%08x only guessed, skipped (img.dump all to try)",
                     b->name, b->get_cmd);
            emit(ectx, line);
            continue;
        }
        memset(buf, 0, DUMP_BUF_SIZE);
        int rc = get(gctx, b->get_cmd, buf);
        if (rc != 0) {
            unreadable++;
            snprintf(line, sizeof(line), "[isp.dump] %-14s get 0x%08x failed (%d)", b->name, b->get_cmd, rc);
            emit(ectx, line);
            continue;
        }
        read_ok++;
        if (o->bin) write_record(o->bin, o->t_ms, b->set_cmd, buf, b->size);

        const uint8_t *pd = have_day ? isp_conf_payload(&day, b) : NULL;
        const uint8_t *pn = have_night ? isp_conf_payload(&night, b) : NULL;
        size_t dd = pd ? isp_blocks_diff_words(buf, pd, b->size, b->skip_words) : 0;
        size_t dn = pn ? isp_blocks_diff_words(buf, pn, b->size, b->skip_words) : 0;
        if (pd && pn) {
            compared++;
            if (dd == 0) day_eq++;
            if (dn == 0) night_eq++;
        }
        snprintf(line, sizeof(line), "[isp.dump] %-14s size=%-5u hash=%08x day=%s(%zuw) night=%s(%zuw)%s",
                 b->name, b->size, isp_blocks_hash(buf, b->size), verdict(dd, pd != NULL), dd,
                 verdict(dn, pn != NULL), dn, b->skip_words ? " [runtime words ignored]" : "");
        emit(ectx, line);
    }

    const char *state = "unknown";
    if (compared) {
        if (day_eq == compared && night_eq < compared) state = "day";
        else if (night_eq == compared && day_eq < compared) state = "night";
        else if (day_eq == compared && night_eq == compared) state = "day=night (identical files)";
        else state = day_eq >= night_eq ? "day-like, not exact" : "night-like, not exact";
    }
    snprintf(line, sizeof(line),
             "[isp.dump] summary: read=%d unreadable=%d compared=%d equal_to_day=%d equal_to_night=%d live state: %s",
             read_ok, unreadable, compared, day_eq, night_eq, state);
    emit(ectx, line);

    free(buf);
    isp_conf_free(&day);
    isp_conf_free(&night);
    return read_ok > 0 ? read_ok : -1;
}
