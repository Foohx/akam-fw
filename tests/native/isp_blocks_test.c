/* Host test of src/ak_rtsp/isp_blocks.c: the block table against a synthetic two-subfile
 * sensor_isp.conf, the conf loader, the comparison and the dump (fake ISP driver, no camera).
 * With AKAM_ISP_CONF_REAL=/path/to/isp_gc20c3.conf it also checks that every table entry fits the real
 * file (the file is the vendor's: it is never part of the repository). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "isp_blocks.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* A synthetic conf: `subfiles` subfiles; the payload byte of block b in subfile m is (m*37 + module + 1). */
static uint8_t *make_conf(int subfiles, size_t *len_out)
{
    size_t n;
    const isp_block_t *t = isp_blocks_table(&n);
    uint32_t msize[ISP_BLOCKS_MODULES] = { 0 };
    for (size_t i = 0; i < n; i++) {
        uint32_t end = t[i].off + t[i].size;
        if (end > msize[t[i].module]) msize[t[i].module] = end;
    }
    size_t one = 512;
    for (int m = 0; m < ISP_BLOCKS_MODULES; m++) one += msize[m];
    one += 4 + 8;
    uint8_t *buf = calloc(1, one * (size_t)subfiles);
    for (int s = 0; s < subfiles; s++) {
        uint8_t *p = buf + one * (size_t)s;
        p[0] = 7;
        p[31] = (uint8_t)s;
        p += 512;
        for (int m = 0; m < ISP_BLOCKS_MODULES; m++) {
            put16(p, (unsigned)m);
            put16(p + 2, msize[m]);
            memset(p + 4, s * 37 + m + 1, msize[m] - 4);
            p += msize[m];
        }
        put16(p, 0x1c);
        put16(p + 2, 8);
    }
    *len_out = one * (size_t)subfiles;
    return buf;
}

static void write_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(d, 1, n, f);
    fclose(f);
}

/* Fake ISP driver: returns the payload of the subfile it is set to, optionally altered. */
typedef struct {
    isp_conf_t conf;
    int fail_cmd_count;
    uint32_t fail_cmd[4];
    int alter_module;          /* -1 = none: flip one byte of the blocks of this module */
    int calls;
} fake_t;

static int fake_get(void *ctx, uint32_t cmd, void *buf)
{
    fake_t *f = ctx;
    size_t n;
    const isp_block_t *t = isp_blocks_table(&n);
    f->calls++;
    for (int i = 0; i < f->fail_cmd_count; i++)
        if (f->fail_cmd[i] == cmd) return -1;
    for (size_t i = 0; i < n; i++) {
        if (t[i].get_cmd != cmd) continue;
        const uint8_t *p = isp_conf_payload(&f->conf, &t[i]);
        if (!p) return -1;
        memcpy(buf, p, t[i].size);
        if (f->alter_module == t[i].module)
            ((uint8_t *)buf)[t[i].skip_words * 4 + 1] ^= 0x55;
        return 0;
    }
    return -1;
}

#define MAXL 128
typedef struct { char line[MAXL][256]; int n; } log_t;
static void emit(void *ctx, const char *line)
{
    log_t *l = ctx;
    if (l->n < MAXL) snprintf(l->line[l->n++], 256, "%s", line);
}
static int has(const log_t *l, const char *needle)
{
    for (int i = 0; i < l->n; i++) if (strstr(l->line[i], needle)) return 1;
    return 0;
}
static const char *find(const log_t *l, const char *needle)
{
    for (int i = 0; i < l->n; i++) if (strstr(l->line[i], needle)) return l->line[i];
    return "";
}

int main(void)
{
    size_t n;
    const isp_block_t *t = isp_blocks_table(&n);
    CHECK(n == 30);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            CHECK(strcmp(t[i].name, t[j].name) != 0);
            CHECK(t[i].set_cmd != t[j].set_cmd);
        }

    size_t len;
    uint8_t *conf = make_conf(2, &len);
    char path[] = "/tmp/isp_blocks_test_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    write_file(path, conf, len);

    isp_conf_t day, night, bad;
    CHECK(isp_conf_load(path, 0, &day) == 0);
    CHECK(isp_conf_load(path, 1, &night) == 0);
    CHECK(isp_conf_load(path, 2, &bad) == -1);
    CHECK(isp_conf_load("/nonexistent/conf", 0, &bad) == -1);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *pd = isp_conf_payload(&day, &t[i]);
        const uint8_t *pn = isp_conf_payload(&night, &t[i]);
        CHECK(pd && pn);
        if (pd && pn) {
            CHECK(pd[0] == (uint8_t)(0 * 37 + t[i].module + 1));
            CHECK(pn[0] == (uint8_t)(1 * 37 + t[i].module + 1));
        }
    }

    /* a damaged file: mode byte of the night subfile says day, a truncated module, a wrong version */
    size_t one = len / 2;
    conf[one + 31] = 0;
    write_file(path, conf, len);
    CHECK(isp_conf_load(path, 1, &bad) == -1);
    conf[one + 31] = 1;
    write_file(path, conf, len - 100);
    CHECK(isp_conf_load(path, 1, &bad) == -1);
    CHECK(isp_conf_load(path, 0, &bad) == 0);
    isp_conf_free(&bad);
    conf[0] = 6;
    write_file(path, conf, len);
    CHECK(isp_conf_load(path, 0, &bad) == -1);
    conf[0] = 7;
    write_file(path, conf, len);

    /* comparison */
    uint8_t a[11] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, b[11];
    memcpy(b, a, sizeof(a));
    CHECK(isp_blocks_diff_words(a, b, 11, 0) == 0);
    b[10] ^= 1;
    CHECK(isp_blocks_diff_words(a, b, 11, 0) == 1);   /* trailing partial word counts */
    b[0] ^= 1;
    CHECK(isp_blocks_diff_words(a, b, 11, 0) == 2);
    CHECK(isp_blocks_diff_words(a, b, 11, 1) == 1);   /* first word skipped */
    CHECK(isp_blocks_hash(a, 11) != isp_blocks_hash(b, 11));
    CHECK(isp_blocks_hash((const uint8_t *)"", 0) == 2166136261u);

    /* dump: the driver is in day state, only confirmed getters */
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.alter_module = -1;
    CHECK(isp_conf_load(path, 0, &f.conf) == 0);
    log_t log;
    memset(&log, 0, sizeof(log));
    isp_dump_opts_t o = { path, 0, NULL, 0 };
    int rc = isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(rc == 5);                                   /* SHARP, EFFECT, SET_WB, SET_EXPOSURE, SET_FRAME_RATE */
    CHECK(has(&log, "summary: read=5 unreadable=0 compared=5 equal_to_day=5 equal_to_night=0 live state: day"));
    CHECK(has(&log, "NR1            getter 0x8004490d only guessed, skipped"));
    CHECK(has(&log, "LCE            no getter known"));
    CHECK(strstr(find(&log, "SET_EXPOSURE"), "[runtime words ignored]") != NULL);

    /* all getters, night state */
    isp_conf_free(&f.conf);
    CHECK(isp_conf_load(path, 1, &f.conf) == 0);
    memset(&log, 0, sizeof(log));
    o.all = 1;
    rc = isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(rc == 28);                                  /* 30 minus LCE and AWB_CALIB */
    CHECK(has(&log, "equal_to_day=0 equal_to_night=28 live state: night"));
    CHECK(strstr(find(&log, "NR1 "), "day=diff(") != NULL);
    CHECK(strstr(find(&log, "NR1 "), "night=eq(0w)") != NULL);

    /* a block that differs from the file, runtime words of SET_EXPOSURE ignored */
    f.alter_module = 3;                               /* NR1, NR2, UVNR */
    memset(&log, 0, sizeof(log));
    isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(strstr(find(&log, "NR2 "), "night=diff(1w)") != NULL);
    CHECK(has(&log, "live state: night-like, not exact"));
    f.alter_module = 20;                              /* flips word 8 of SET_EXPOSURE (past the 7 runtime words) */
    memset(&log, 0, sizeof(log));
    isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(strstr(find(&log, "SET_EXPOSURE"), "night=diff(1w)") != NULL);

    /* a failing getter is reported and counted, the others are still read */
    f.alter_module = -1;
    f.fail_cmd_count = 1;
    f.fail_cmd[0] = 0x80044926;
    memset(&log, 0, sizeof(log));
    rc = isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(rc == 27);
    CHECK(has(&log, "SHARP          get 0x80044926 failed"));
    CHECK(has(&log, "unreadable=1"));

    /* nothing readable: -1 */
    f.fail_cmd_count = 0;
    isp_conf_free(&f.conf);
    memset(&f.conf, 0, sizeof(f.conf));
    memset(&log, 0, sizeof(log));
    rc = isp_blocks_dump(fake_get, &f, &o, emit, &log);
    CHECK(rc == -1);

    /* no conf file: still dumps, no comparison */
    isp_conf_load(path, 0, &f.conf);
    memset(&log, 0, sizeof(log));
    isp_dump_opts_t o2 = { NULL, 0, NULL, 0 };
    rc = isp_blocks_dump(fake_get, &f, &o2, emit, &log);
    CHECK(rc == 5);
    CHECK(has(&log, "day=n/a(0w) night=n/a(0w)"));
    CHECK(has(&log, "compared=0"));

    /* ISPB records */
    char bpath[] = "/tmp/isp_blocks_test_bin_XXXXXX";
    int bfd = mkstemp(bpath);
    assert(bfd >= 0);
    FILE *bin = fdopen(bfd, "wb+");
    isp_dump_opts_t o3 = { path, 0, bin, 1234 };
    memset(&log, 0, sizeof(log));
    rc = isp_blocks_dump(fake_get, &f, &o3, emit, &log);
    CHECK(rc == 5);
    fflush(bin);
    rewind(bin);
    int records = 0;
    for (;;) {
        uint8_t h[24];
        if (fread(h, 1, 24, bin) != 24) break;
        CHECK(h[0] == 'I' && h[1] == 'S' && h[2] == 'P' && h[3] == 'B');
        uint32_t tms = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
        uint32_t sz = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
        CHECK(tms == 1234);
        CHECK(sz > 0 && sz <= 46584);
        fseek(bin, (long)sz, SEEK_CUR);
        records++;
    }
    CHECK(records == 5);
    fclose(bin);
    unlink(bpath);

    isp_conf_free(&f.conf);
    isp_conf_free(&day);
    isp_conf_free(&night);

    /* the real file: every table entry must fit its module, both modes */
    const char *real = getenv("AKAM_ISP_CONF_REAL");
    if (real && *real) {
        for (int mode = 0; mode < 2; mode++) {
            isp_conf_t rc2;
            CHECK(isp_conf_load(real, mode, &rc2) == 0);
            for (size_t i = 0; i < n; i++) CHECK(isp_conf_payload(&rc2, &t[i]) != NULL);
            isp_conf_free(&rc2);
        }
        printf("real conf checked\n");
    }

    unlink(path);
    free(conf);
    if (g_failed) { printf("isp_blocks_test: %d failure(s)\n", g_failed); return 1; }
    printf("isp_blocks_test: OK\n");
    return 0;
}
