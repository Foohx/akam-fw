/* Host test of the RAM log ring (platforms/anyka-ak3918av130/ramlog.h, used by main.c a2k_log): rotation, the size cap, the segment headers, and the order surviving an app restart
 * (a new process continues on the NEWER segment: it used to restart on segment 0 whichever was newer, and the two files could no longer be put in order). Prints "rot2k ramlog unit: N failed". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../platforms/anyka-ak3918av130/ramlog.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static long fsize(const char *p)
{
    FILE *f = fopen(p, "r");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static void put_n(struct a2k_ramlog *r, int from, int to, const char *tag)
{
    char line[100];
    for (int i = from; i < to; i++) {
        int n = snprintf(line, sizeof line, "[%7dms] %s line %04d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i, tag, i);
        a2k_ramlog_put(r, line, (size_t)n);
    }
}

/* the lowest and highest line number found in a file (the "line %04d" field), -1 when none */
static void range(const char *p, int *lo, int *hi)
{
    char l[200];
    FILE *f = fopen(p, "r");
    *lo = *hi = -1;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        char *q = strstr(l, "line ");
        if (q) { int v = atoi(q + 5); if (*lo < 0 || v < *lo) *lo = v; if (v > *hi) *hi = v; }
    }
    fclose(f);
}

static int has_line(const char *p, const char *needle)
{
    char l[200];
    int found = 0;
    FILE *f = fopen(p, "r");
    while (f && fgets(l, sizeof l, f)) if (strstr(l, needle)) found = 1;
    if (f) fclose(f);
    return found;
}

int main(void)
{
    struct a2k_ramlog r = A2K_RAMLOG_INIT;
    char base[300], p0[320], p1[320], dir[256];
    int lo0, hi0, lo1, hi1;
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/ramlog_unit_XXXXXX", tmp && *tmp ? tmp : ".");
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 2; }
    snprintf(base, sizeof base, "%s/log", dir);
    snprintf(p0, sizeof p0, "%s", base);
    snprintf(p1, sizeof p1, "%s.1", base);
    printf("== RAM log ring: rotation, cap, headers, order across an app restart\n");

    a2k_ramlog_set(&r, base, 1000); /* 1000 bytes per segment: about 8 lines */
    put_n(&r, 0, 6, "A");
    CHECK(fsize(p0) > 0 && fsize(p1) < 0, "the first lines go to segment 0 only");
    put_n(&r, 6, 30, "A");
    CHECK(fsize(p0) <= 1000 && fsize(p1) <= 1000 && fsize(p0) > 0 && fsize(p1) > 0, "both segments exist and neither passes the cap");
    range(p0, &lo0, &hi0);
    range(p1, &lo1, &hi1);
    CHECK((lo1 > hi0 && lo0 >= 0) || (lo0 > hi1 && lo1 >= 0), "the segments hold disjoint, ordered ranges of lines (one older, one newer)");
    CHECK(a2k_ramlog_seq_of(p0) != a2k_ramlog_seq_of(p1) && a2k_ramlog_seq_of(p0) > 0 && a2k_ramlog_seq_of(p1) > 0, "each segment starts with its own sequence header, they differ");
    CHECK(hi0 == 29 || hi1 == 29, "the newest line is in one of them (nothing lost at the end)");

    /* an app restart: a new process (the ring forgets its state, the files stay) continues on the NEWER segment. A clean case: segment 0 is full (older), segment 1 holds the newest lines and has room. */
    {
        char b2[320], q0[340], q1[340];
        long q0_before;
        snprintf(b2, sizeof b2, "%s/log2", dir);
        snprintf(q0, sizeof q0, "%s", b2);
        snprintf(q1, sizeof q1, "%s.1", b2);
        a2k_ramlog_set(&r, b2, 1000);
        put_n(&r, 0, 22, "D");
        CHECK(a2k_ramlog_seq_of(q1) > a2k_ramlog_seq_of(q0) && fsize(q1) > 0 && fsize(q1) < 600, "setup: segment 1 is the newer one and has room");
        q0_before = fsize(q0);
        a2k_ramlog_set(&r, b2, 1000);
        put_n(&r, 1000, 1002, "E"); /* the ms clock of a new process restarts */
        CHECK(fsize(q0) == q0_before && has_line(q0, "line 0001") && !has_line(q0, "line 1001"), "after a restart the OLDER segment 0 is untouched (it used to be written again whichever was newer)");
        CHECK(has_line(q1, "line 1001") && has_line(q1, "line 0021"), "the restarted process appended to the newer segment 1, after the lines of the previous process");
        CHECK(has_line(q1, "app start]"), "the restart is marked by an 'app start' line (the ms clock restarts there)");
        unlink(q0);
        unlink(q1);
    }
    /* many restarts and lines: the two sequence numbers always differ and the cap always holds */
    for (int k = 0; k < 5; k++) {
        a2k_ramlog_set(&r, base, 1000);
        put_n(&r, 2000 + k * 40, 2000 + k * 40 + 40, "C");
    }
    CHECK(fsize(p0) <= 1000 && fsize(p1) <= 1000, "after 5 more restarts and 200 lines both segments are still capped");
    CHECK(a2k_ramlog_seq_of(p0) != a2k_ramlog_seq_of(p1), "and the headers still order them");
    range(p0, &lo0, &hi0);
    range(p1, &lo1, &hi1);
    CHECK(hi0 == 2199 || hi1 == 2199, "the newest line of the last restart is present");
    if (a2k_ramlog_seq_of(p0) > a2k_ramlog_seq_of(p1))
        CHECK(lo0 >= 2000 && hi0 == 2199, "the higher sequence number holds the newest lines");
    else
        CHECK(lo1 >= 2000 && hi1 == 2199, "the higher sequence number holds the newest lines");
    a2k_ramlog_set(&r, base, 1000);
    unlink(p0);
    unlink(p1);
    rmdir(dir);
    printf("rot2k ramlog unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
