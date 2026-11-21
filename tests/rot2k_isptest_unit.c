/* Host unit test of the RAM-only ISP test hook (isptest.c) with a fake ISP: the line parser (words, ranges, aliases, rejects), the
 * get commands (they never write), fx / wb / ccm / nr commands (get -> change fields -> set of the same buffer, only the intended bytes
 * differ), the colour matrix blend (row sums kept, range kept), restore, the round trip check and the poll of the command file. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "isptest.h"

volatile sig_atomic_t a2k_stop;
static int fails;
static char lastlog[8][200];
static int nlog;
void a2k_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastlog[nlog % 8], sizeof lastlog[0], fmt, ap);
    va_end(ap);
    nlog++;
}
static int logged(const char *needle)
{
    for (int i = 0; i < 8 && i < nlog; i++)
        if (strstr(lastlog[i], needle))
            return 1;
    return 0;
}
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ---- the fake ISP ---- */
static struct {
    uint8_t attr[A2K_IT_N][A2K_IT_BUF];
    int fx[6];
    int sets, gets, fx_sets;
    int rc_get[A2K_IT_N], rc_set[A2K_IT_N];
    int mutate; /* a set that stores a changed byte: the round trip must notice */
} F;

static void w16(uint8_t *b, size_t o, int v) { b[o] = (uint8_t)(v & 255); b[o + 1] = (uint8_t)((v >> 8) & 255); }
static int r16(const uint8_t *b, size_t o) { return b[o] | (b[o + 1] << 8); }
static int rs16(const uint8_t *b, size_t o) { int v = r16(b, o); return v & 0x8000 ? v - 65536 : v; }

static int f_effect_set(int type, int v) { F.fx_sets++; F.fx[type] = v; return 0; }
static int f_effect_get(int type, int *v) { *v = F.fx[type]; return 0; }
static int f_get(int what, void *buf)
{
    F.gets++;
    if (F.rc_get[what])
        return F.rc_get[what];
    memcpy(buf, F.attr[what], A2K_IT_BUF);
    return 0;
}
static int f_set(int what, const void *buf)
{
    F.sets++;
    if (F.rc_set[what])
        return F.rc_set[what];
    memcpy(F.attr[what], buf, A2K_IT_BUF);
    if (F.mutate)
        F.attr[what][3] ^= 1;
    return 0;
}
static int f_rgb(int *r, int *g, int *b) { *r = 100; *g = 90; *b = 70; return 0; }
static const a2k_isptest_ops_t OPS = { f_effect_set, f_effect_get, f_get, f_set, f_rgb };

static void put_matrix(uint8_t *b, size_t o, const int *m) { for (int i = 0; i < 9; i++) w16(b, o + 2u * (unsigned)i, m[i]); }

static void reset(void)
{
    static const int day3000[9] = { 339, -168, 85, -60, 256, 60, 8, -242, 490 }; /* the day set of the conf file, 3000 K */
    static const int day4200[9] = { 402, -201, 55, -44, 275, 25, 24, -212, 444 };
    memset(&F, 0, sizeof F);
    w16(F.attr[A2K_IT_YUV], 0, 128);
    w16(F.attr[A2K_IT_YUV], 4, 64);
    F.attr[A2K_IT_WBTYPE][0] = 1; /* this fake camera runs its AWB with type 1 */
    w16(F.attr[A2K_IT_MWB], 0, 256), w16(F.attr[A2K_IT_MWB], 2, 256), w16(F.attr[A2K_IT_MWB], 4, 256);
    F.attr[A2K_IT_CCM][0] = 1;
    put_matrix(F.attr[A2K_IT_CCM], A2K_CCM_MAN, day3000);
    w16(F.attr[A2K_IT_CCM], A2K_CCM_NUM, 2);
    w16(F.attr[A2K_IT_CCM], A2K_CCM_AUTO, 3000);
    put_matrix(F.attr[A2K_IT_CCM], A2K_CCM_AUTO + 10, day3000);
    w16(F.attr[A2K_IT_CCM], A2K_CCM_AUTO + A2K_CCM_AUTO_STRIDE, 4200);
    put_matrix(F.attr[A2K_IT_CCM], A2K_CCM_AUTO + A2K_CCM_AUTO_STRIDE + 10, day4200);
    for (int j = 0; j < A2K_REC_N; j++) { /* nr records: k = 5, intensities 3 / 4, uvnr k = 6, tnr enable 1 */
        w16(F.attr[A2K_IT_NR1], 4 + (size_t)A2K_NR1_REC * (size_t)j + 2, 5);
        w16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 4, 5);
        w16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 8, 3);
        w16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 10, 4);
        w16(F.attr[A2K_IT_UVNR], 4 + (size_t)A2K_UVNR_REC * (size_t)j + 4, 6);
        w16(F.attr[A2K_IT_NR3D], 4 + (size_t)A2K_NR3D_REC * (size_t)j, 1);
    }
    F.attr[A2K_IT_NR2][0] = 1; /* mode: linked */
    nlog = 0;
}

static a2k_isptest_cmd_t P(const char *line)
{
    a2k_isptest_cmd_t c;
    memset(&c, 0, sizeof c);
    c.kind = -1;
    (void)a2k_isptest_parse(line, &c);
    return c;
}

static void test_parse(void)
{
    a2k_isptest_cmd_t c;
    printf("== parser\n");
    CHECK(a2k_isptest_parse("get all\n", &c) == 0 && c.kind == A2K_ITC_GET && c.a == A2K_IT_ALL, "get all");
    CHECK(a2k_isptest_parse("get nr3d", &c) == 0 && c.a == A2K_IT_NR3D, "get nr3d");
    CHECK(a2k_isptest_parse("get wb", &c) == 0 && c.a == A2K_IT_WBTYPE, "get wb");
    CHECK(a2k_isptest_parse("get bogus", &c) == -1 && a2k_isptest_parse("get", &c) == -1, "unknown get target and a bare get are refused");
    CHECK(a2k_isptest_parse("fx 2 -30", &c) == 0 && c.kind == A2K_ITC_FX && c.a == 2 && c.b == -30, "fx with a negative value");
    CHECK(a2k_isptest_parse("fx 5 40", &c) == -1, "fx type 5 (WDR) is not reachable: the app owns it");
    CHECK(a2k_isptest_parse("fx 2 51", &c) == -1 && a2k_isptest_parse("fx 2 -51", &c) == -1 && a2k_isptest_parse("fx 2", &c) == -1, "fx range -50..50 and both numbers needed");
    CHECK(a2k_isptest_parse("sat 20", &c) == 0 && c.kind == A2K_ITC_FX && c.a == 2 && c.b == 20, "sat = fx 2");
    CHECK(a2k_isptest_parse("hue -5", &c) == 0 && c.a == 0 && c.b == -5, "hue = fx 0");
    CHECK(a2k_isptest_parse("bright 7", &c) == 0 && c.a == 1, "bright = fx 1");
    CHECK(a2k_isptest_parse("contrast 7", &c) == 0 && c.a == 3, "contrast = fx 3");
    CHECK(a2k_isptest_parse("sharp 10\n", &c) == 0 && c.a == 4 && c.b == 10, "sharp = fx 4, trailing newline");
    CHECK(a2k_isptest_parse("wb auto", &c) == 0 && c.kind == A2K_ITC_WB_AUTO, "wb auto");
    CHECK(a2k_isptest_parse("wb manual 300 256 400", &c) == 0 && c.kind == A2K_ITC_WB_MANUAL && c.a == 300 && c.b == 256 && c.c == 400, "wb manual R G B");
    CHECK(a2k_isptest_parse("wb manual 4096 1 1", &c) == -1 && a2k_isptest_parse("wb manual 1 1", &c) == -1 && a2k_isptest_parse("wb manual -1 1 1", &c) == -1, "wb manual range 0..4095, three numbers");
    CHECK(a2k_isptest_parse("wb type 0", &c) == 0 && c.kind == A2K_ITC_WB_TYPE && c.a == 0 && a2k_isptest_parse("wb type 2", &c) == -1, "wb type 0|1");
    CHECK(a2k_isptest_parse("ccm mode 1", &c) == 0 && c.kind == A2K_ITC_CCM_MODE && c.a == 1 && a2k_isptest_parse("ccm mode 2", &c) == -1, "ccm mode 0|1");
    CHECK(a2k_isptest_parse("ccm blend 70", &c) == 0 && c.kind == A2K_ITC_CCM_BLEND && c.a == 70 && a2k_isptest_parse("ccm blend 101", &c) == -1, "ccm blend 0..100");
    CHECK(a2k_isptest_parse("nr1 15", &c) == 0 && c.kind == A2K_ITC_NR1 && c.a == 15 && a2k_isptest_parse("nr1 16", &c) == -1, "nr1 0..15");
    CHECK(a2k_isptest_parse("nr2d 8", &c) == 0 && c.kind == A2K_ITC_NR2D && c.a == 8 && c.b == -1 && c.c == -1, "nr2d K alone leaves the intensities");
    CHECK(a2k_isptest_parse("nr2d 8 2 3", &c) == 0 && c.b == 2 && c.c == 3 && a2k_isptest_parse("nr2d 8 2", &c) == -1 && a2k_isptest_parse("nr2d 8 2 16", &c) == -1, "nr2d K I1 I2");
    CHECK(a2k_isptest_parse("uvnr 4", &c) == 0 && c.kind == A2K_ITC_UVNR && c.a == 4, "uvnr");
    CHECK(a2k_isptest_parse("nr3d 0", &c) == 0 && c.kind == A2K_ITC_NR3D && c.a == 0 && a2k_isptest_parse("nr3d 2", &c) == -1, "nr3d 0|1");
    CHECK(a2k_isptest_parse("rt ccm", &c) == 0 && c.kind == A2K_ITC_RT && c.a == A2K_IT_CCM, "rt ccm");
    CHECK(a2k_isptest_parse("rt wb", &c) == 0 && c.a == A2K_IT_MWB && a2k_isptest_parse("rt nothing", &c) == -1, "rt wb, unknown name");
    CHECK(a2k_isptest_parse("restore", &c) == 0 && c.kind == A2K_ITC_RESTORE, "restore");
    CHECK(a2k_isptest_parse("sat 20 extra", &c) == -1 && a2k_isptest_parse("restore now", &c) == -1, "extra text is refused");
    CHECK(a2k_isptest_parse("saturate 20", &c) == -1 && a2k_isptest_parse("", &c) == -1 && a2k_isptest_parse(NULL, &c) == -1, "unknown word, empty, NULL");
    CHECK(a2k_isptest_parse("sat 1234567", &c) == -1 && a2k_isptest_parse("sat abc", &c) == -1 && a2k_isptest_parse("sat 5x", &c) == -1, "digits only");
}

static void test_get(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c = P("get all");
    printf("== get never writes\n");
    reset();
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.sets == 0 && F.fx_sets == 0 && F.gets > 8, "get all: only getters");
    CHECK(logged("rgb average = 0: r 100 g 90 b 70"), "the RGB average is logged");
    c = P("get ccm");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.sets == 0 && logged("ccm auto 1 temp 4200"), "get ccm lists the automatic matrices");
    F.rc_get[A2K_IT_NR2] = 0x100;
    c = P("get nr2");
    CHECK(a2k_isptest_exec(&t, &c) == 0x100 && logged("get nr2 = 256"), "a failing get returns the vendor code and is logged");
    a2k_isptest_free(&t);
}

static void test_fx(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c = P("sat 20");
    printf("== fx\n");
    reset();
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.fx[2] == 20 && F.fx_sets == 1 && logged("ak_vpss_effect_set(0, 2, 20) = 0 (effect_get before 0 [rc 0], after 20"), "sat 20 -> effect 2, before and after logged");
    c = P("restore");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.fx[2] == 0, "restore sets the touched effect back to 0");
    a2k_isptest_free(&t);
}

static void test_wb(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c;
    printf("== white balance\n");
    reset();
    a2k_isptest_init(&t, &OPS);
    c = P("wb manual 300 256 400");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_MWB], 0) == 300 && r16(F.attr[A2K_IT_MWB], 2) == 256 && r16(F.attr[A2K_IT_MWB], 4) == 400, "manual gains stored");
    CHECK(F.attr[A2K_IT_MWB][6] == 0 && F.attr[A2K_IT_MWB][11] == 0, "the offsets are left alone (read back unchanged)");
    CHECK(F.attr[A2K_IT_WBTYPE][0] == 0 && t.auto_type == 1, "wb_type goes to the other value than the one read first");
    c = P("wb auto");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.attr[A2K_IT_WBTYPE][0] == 1, "wb auto puts the first-read type back");
    c = P("wb type 0");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.attr[A2K_IT_WBTYPE][0] == 0, "raw wb type");
    c = P("restore");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.attr[A2K_IT_WBTYPE][0] == 1 && r16(F.attr[A2K_IT_MWB], 0) == 256 && r16(F.attr[A2K_IT_MWB], 4) == 256, "restore: type and manual gains back");
    a2k_isptest_free(&t);
    reset();
    F.rc_get[A2K_IT_WBTYPE] = 5;
    a2k_isptest_init(&t, &OPS);
    c = P("wb manual 300 256 400");
    CHECK(a2k_isptest_exec(&t, &c) == -1 && F.sets == 0 && logged("not known"), "the automatic type unknown (first read failed): wb manual refused, nothing written");
    a2k_isptest_free(&t);
}

static void test_ccm(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c;
    uint8_t orig[A2K_IT_BUF];
    printf("== colour matrix\n");
    reset();
    memcpy(orig, F.attr[A2K_IT_CCM], sizeof orig);
    a2k_isptest_init(&t, &OPS);
    c = P("ccm blend 100");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && memcmp(orig, F.attr[A2K_IT_CCM], sizeof orig) == 0, "blend 100 changes nothing");
    c = P("ccm blend 50");
    CHECK(a2k_isptest_exec(&t, &c) == 0, "blend 50 runs");
    {
        const uint8_t *b = F.attr[A2K_IT_CCM];
        int ok = 1, in = 1;
        size_t base[3] = { A2K_CCM_MAN, A2K_CCM_AUTO + 10, A2K_CCM_AUTO + A2K_CCM_AUTO_STRIDE + 10 };
        for (int m = 0; m < 3; m++)
            for (int r = 0; r < 3; r++) {
                int so = 0, sn = 0;
                for (int k = 0; k < 3; k++) {
                    so += rs16(orig, base[m] + 2u * (unsigned)(r * 3 + k));
                    sn += rs16(b, base[m] + 2u * (unsigned)(r * 3 + k));
                    if (rs16(b, base[m] + 2u * (unsigned)(r * 3 + k)) < -2048 || rs16(b, base[m] + 2u * (unsigned)(r * 3 + k)) > 2047)
                        in = 0;
                }
                if (so != sn)
                    ok = 0;
            }
        CHECK(ok, "every row sum (the white point) is unchanged");
        CHECK(in, "every coefficient stays in the vendor range -2048..2047");
        CHECK(rs16(b, A2K_CCM_MAN + 2) == -84 && rs16(b, A2K_CCM_MAN) == 298, "off-diagonals halved (-168 -> -84), diagonal takes the rest");
        CHECK(memcmp(orig + 4, b + 4, 14) == 0 && memcmp(orig + A2K_CCM_NUM, b + A2K_CCM_NUM, 8) == 0 && memcmp(orig + A2K_CCM_AUTO, b + A2K_CCM_AUTO, 10) == 0, "control words and colour temperatures untouched");
    }
    c = P("ccm blend 0");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && rs16(F.attr[A2K_IT_CCM], A2K_CCM_MAN) == 256 && rs16(F.attr[A2K_IT_CCM], A2K_CCM_MAN + 2) == 0, "blend 0 = identity scaled to the row sum (256)");
    c = P("ccm mode 0");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.attr[A2K_IT_CCM][0] == 0, "ccm mode");
    c = P("restore");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && memcmp(orig, F.attr[A2K_IT_CCM], sizeof orig) == 0, "restore puts the first read back byte for byte");
    a2k_isptest_free(&t);
    reset();
    F.rc_set[A2K_IT_CCM] = 256;
    a2k_isptest_init(&t, &OPS);
    c = P("ccm blend 50");
    CHECK(a2k_isptest_exec(&t, &c) == 256, "a refusing set returns the vendor code");
    a2k_isptest_free(&t);
}

static void test_nr(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c;
    int all = 1;
    printf("== noise reduction records\n");
    reset();
    a2k_isptest_init(&t, &OPS);
    c = P("nr2d 9 1 2");
    CHECK(a2k_isptest_exec(&t, &c) == 0, "nr2d 9 1 2 runs");
    for (int j = 0; j < A2K_REC_N; j++)
        if (r16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 4) != 9 || r16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 8) != 1 ||
            r16(F.attr[A2K_IT_NR2], 4 + (size_t)A2K_NR2_REC * (size_t)j + 10) != 2)
            all = 0;
    CHECK(all, "nr2_k and both intensities set in the manual record and the 16 linked ones");
    CHECK(F.attr[A2K_IT_NR2][0] == 1, "the mode word is untouched");
    c = P("nr2d 3");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_NR2], 4 + 4) == 3 && r16(F.attr[A2K_IT_NR2], 4 + 8) == 1, "nr2d K alone keeps the intensities");
    c = P("uvnr 12");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_UVNR], 4 + 4) == 12 && r16(F.attr[A2K_IT_UVNR], 4 + (size_t)A2K_UVNR_REC * 16 + 4) == 12, "uvnr_k, first and last record");
    c = P("nr1 0");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_NR1], 4 + 2) == 0 && r16(F.attr[A2K_IT_NR1], 4 + (size_t)A2K_NR1_REC * 16 + 2) == 0, "nr1_k");
    c = P("nr3d 0");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_NR3D], 4) == 0 && r16(F.attr[A2K_IT_NR3D], 4 + (size_t)A2K_NR3D_REC * 16) == 0, "tnr_enable off everywhere");
    c = P("restore");
    CHECK(a2k_isptest_exec(&t, &c) == 0 && r16(F.attr[A2K_IT_NR2], 4 + 4) == 5 && r16(F.attr[A2K_IT_UVNR], 4 + 4) == 6 && r16(F.attr[A2K_IT_NR3D], 4) == 1 &&
              r16(F.attr[A2K_IT_NR1], 6) == 5 && r16(F.attr[A2K_IT_NR2], 4 + 8) == 3, "restore: everything back to the first read");
    a2k_isptest_free(&t);
}

static void test_rt(void)
{
    a2k_isptest_t t;
    a2k_isptest_cmd_t c = P("rt nr2");
    printf("== round trip\n");
    reset();
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.sets == 1 && logged("identical"), "rt nr2: get / set / get identical");
    reset();
    F.mutate = 1;
    a2k_isptest_free(&t);
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_exec(&t, &c) == 0 && logged("DIFFERS at byte 3"), "a set that changes a byte is reported");
    reset();
    c = P("rt wb");
    a2k_isptest_free(&t);
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_exec(&t, &c) == 0 && F.sets == 2, "rt wb: both the manual gains and the type");
    a2k_isptest_free(&t);
}

static void test_poll(void)
{
    char path[] = "isptest_unit_cmdXXXXXX";
    int fd = mkstemp(path);
    a2k_isptest_t t;
    FILE *f;
    printf("== command file\n");
    if (fd < 0) {
        printf("  FAIL - mkstemp\n");
        fails++;
        return;
    }
    close(fd);
    reset();
    a2k_isptest_init(&t, &OPS);
    CHECK(a2k_isptest_poll(&t, "isptest_unit_absent_file") == 0, "an absent file is nothing");
    f = fopen(path, "w");
    fputs("sat 10\n\nnonsense\nhue -3\n", f);
    fclose(f);
    CHECK(a2k_isptest_poll(&t, path) == 3 && F.fx[2] == 10 && F.fx[0] == -3, "valid lines run, the bad one is skipped, empty lines do not count");
    CHECK(access(path, F_OK) != 0, "the file is deleted once read");
    f = fopen(path, "w");
    for (int i = 0; i < 40; i++)
        fputs("sharp 1\n", f);
    fclose(f);
    F.fx_sets = 0;
    CHECK(a2k_isptest_poll(&t, path) == A2K_IT_MAXLINES && F.fx_sets == A2K_IT_MAXLINES, "at most 16 lines per file");
    unlink(path);
    a2k_isptest_free(&t);
}

int main(void)
{
    test_parse();
    test_get();
    test_fx();
    test_wb();
    test_ccm();
    test_nr();
    test_rt();
    test_poll();
    printf("rot2k isptest unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
