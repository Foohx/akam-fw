/* Host test of platforms/anyka-ak3918/isp_tune.c: saved picture settings (load, validation, atomic save, removal at all zero). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "isp_tune.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen"); exit(2); }
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char dir[512], path[600], tmp[620];
    snprintf(dir, sizeof(dir), "%s/akam", argv[1]);
    snprintf(path, sizeof(path), "%s/picture", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    isp_tune_t t;

    /* absent file: defaults, nothing created */
    CHECK(isp_tune_load(path, &t) == 0);
    CHECK(!t.brightness && !t.contrast && !t.saturation && t.sharpness == ISP_TUNE_DEFAULT_SHARPNESS);
    CHECK(ISP_TUNE_DEFAULT_SHARPNESS == -15);
    CHECK(access(dir, F_OK) != 0);

    /* the defaults never create a file */
    CHECK(isp_tune_save(path, &t) == 0);
    CHECK(access(path, F_OK) != 0);

    /* save creates the directory (0700) and the file (0600), round trip, no temp left */
    isp_tune_t w = { -50, 50, -1, 7, 4, ISP_TUNE_DEFAULT_WDR, 0 };
    CHECK(isp_tune_save(path, &w) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(access(tmp, F_OK) != 0);
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(t.brightness == -50 && t.contrast == 50 && t.saturation == -1 && t.sharpness == 7 && t.denoise == 4);

    /* same values again: no write (an atomic write makes a new inode) */
    {
        struct stat s1, s2;
        stat(path, &s1);
        CHECK(isp_tune_save(path, &w) == 0);
        stat(path, &s2);
        CHECK(s1.st_ino == s2.st_ino);
    }

    /* overwrite */
    w.brightness = 3;
    CHECK(isp_tune_save(path, &w) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.brightness == 3);

    /* validation: out of range, junk, trailing text, unknown key, CRLF, missing '=' */
    put(path, "brightness=51\ncontrast=-51\nsaturation=abc\nsharpness=5x\nfoo=3\nnoequals\n");
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(!t.brightness && !t.contrast && !t.saturation && t.sharpness == ISP_TUNE_DEFAULT_SHARPNESS);   /* junk: defaults */
    put(path, "brightness=10\r\ncontrast=-20\r\nsaturation=\nsharpness= 4\n");
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(t.brightness == 10 && t.contrast == -20 && t.saturation == 0);
    put(path, "");
    CHECK(isp_tune_load(path, &t) == 1 && !t.brightness);

    /* an explicitly saved sharpness 0 (stock) wins over the default: it is a file, and it loads back as 0 */
    isp_tune_t z = { 0, 0, 0, 0, ISP_TUNE_DEFAULT_DENOISE, ISP_TUNE_DEFAULT_WDR, 0 };
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.sharpness == 0);
    put(path, "contrast=4\n");                                                   /* no sharpness line: the default */
    CHECK(isp_tune_load(path, &t) == 1 && t.contrast == 4 && t.sharpness == ISP_TUNE_DEFAULT_SHARPNESS);

    /* denoise: default 2 (a file without the line gets it), range 0..4, an explicit 0 (stock) is a saved line */
    CHECK(ISP_TUNE_DEFAULT_DENOISE == 2);
    put(path, "denoise=5\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.denoise == ISP_TUNE_DEFAULT_DENOISE);
    put(path, "denoise=-1\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.denoise == ISP_TUNE_DEFAULT_DENOISE);
    put(path, "denoise=0\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.denoise == 0);
    z.denoise = 0;
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.denoise == 0 && t.sharpness == 0);
    z.denoise = ISP_TUNE_DEFAULT_DENOISE;

    /* the defaults remove the file; a second removal is fine */
    z.sharpness = ISP_TUNE_DEFAULT_SHARPNESS;
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) != 0);
    CHECK(isp_tune_save(path, &z) == 0);

    /* wdr: default 90 (the stock curves, a file without the line gets it), range 0..100 (the 2K's scale); an explicit 0 and an explicit 45 are saved lines;
     * nr2: default 0 (the camera's own table), range 0..4; the defaults of both remove the file */
    CHECK(ISP_TUNE_DEFAULT_WDR == 90 && ISP_TUNE_WDR_MAX == 100 && ISP_TUNE_NR2_MAX == 4);
    put(path, "contrast=4\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == ISP_TUNE_DEFAULT_WDR && t.nr2 == 0);
    put(path, "wdr=101\nnr2=5\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == ISP_TUNE_DEFAULT_WDR && t.nr2 == 0);
    put(path, "wdr=-1\nnr2=-1\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == ISP_TUNE_DEFAULT_WDR && t.nr2 == 0);
    put(path, "wdr=0\nnr2=4\n");
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == 0 && t.nr2 == 4);
    z.wdr = 0;
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == 0 && t.nr2 == 0);
    z.wdr = 45; z.nr2 = 3;
    CHECK(isp_tune_save(path, &z) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.wdr == 45 && t.nr2 == 3 && t.denoise == ISP_TUNE_DEFAULT_DENOISE);
    z.wdr = ISP_TUNE_DEFAULT_WDR; z.nr2 = 1;
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) == 0);   /* nr2 alone keeps the file */
    z.nr2 = 0;
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) != 0);   /* both at the defaults: the file goes */

    /* parse */
    int v = 99;
    CHECK(isp_tune_parse("0", &v) == 0 && v == 0);
    CHECK(isp_tune_parse("-50", &v) == 0 && v == -50);
    CHECK(isp_tune_parse("51", &v) < 0 && isp_tune_parse("", &v) < 0 && isp_tune_parse("1.5", &v) < 0);
    CHECK(isp_tune_parse(NULL, &v) < 0);

    /* a directory that cannot be created: save fails with -1 */
    CHECK(isp_tune_save("/nonexistent_dir_for_test/x/picture", &w) < 0);

    printf("isp_tune_test: %s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
