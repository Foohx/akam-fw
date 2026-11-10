/* Host test of src/ak_rtsp/isp_tune.c: saved picture settings (load, validation, atomic save, removal at all zero). */
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
    CHECK(!t.brightness && !t.contrast && !t.saturation && !t.sharpness);
    CHECK(access(dir, F_OK) != 0);

    /* all zero never creates a file */
    CHECK(isp_tune_save(path, &t) == 0);
    CHECK(access(path, F_OK) != 0);

    /* save creates the directory (0700) and the file (0600), round trip, no temp left */
    isp_tune_t w = { -50, 50, -1, 7 };
    CHECK(isp_tune_save(path, &w) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(access(tmp, F_OK) != 0);
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(t.brightness == -50 && t.contrast == 50 && t.saturation == -1 && t.sharpness == 7);

    /* overwrite */
    w.brightness = 3;
    CHECK(isp_tune_save(path, &w) == 0);
    CHECK(isp_tune_load(path, &t) == 1 && t.brightness == 3);

    /* validation: out of range, junk, trailing text, unknown key, CRLF, missing '=' */
    put(path, "brightness=51\ncontrast=-51\nsaturation=abc\nsharpness=5x\nfoo=3\nnoequals\n");
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(!t.brightness && !t.contrast && !t.saturation && !t.sharpness);
    put(path, "brightness=10\r\ncontrast=-20\r\nsaturation=\nsharpness= 4\n");
    CHECK(isp_tune_load(path, &t) == 1);
    CHECK(t.brightness == 10 && t.contrast == -20 && t.saturation == 0);
    put(path, "");
    CHECK(isp_tune_load(path, &t) == 1 && !t.brightness);

    /* all zero removes the file; a second removal is fine */
    isp_tune_t z = { 0, 0, 0, 0 };
    CHECK(isp_tune_save(path, &z) == 0 && access(path, F_OK) != 0);
    CHECK(isp_tune_save(path, &z) == 0);

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
