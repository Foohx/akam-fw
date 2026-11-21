/* Host unit test of the JPEG snapshot module of the Rotatable 2K app (snap.c, stub encoder = the test itself): the ring
 * bookkeeping bounds (files, bytes), the rate limit, one request at a time, oversized / empty / unrequested JPEGs, three
 * failed encodes disable the snapshots, low memory refuses, the file ring on disk stays bounded across any number of
 * snapshots, and submit() never waits for a slow publisher. Prints "rot2k snap unit: N failed". */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "snap.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ---- stubs of what the module links against ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_log[4096];
static long g_avail = 100000;
static unsigned g_pub_n, g_pub_slow_ms;
static size_t g_pub_len;
static uint8_t g_pub_first;

void a2k_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_mu);
    if (strlen(g_log) + strlen(line) + 2 < sizeof g_log) {
        strcat(g_log, line);
        strcat(g_log, "\n");
    }
    pthread_mutex_unlock(&g_mu);
}
long a2k_mem_avail_kb(void) { return g_avail; }
void a2k_mqtt_snapshot(const uint8_t *jpeg, size_t len)
{
    if (g_pub_slow_ms)
        usleep(g_pub_slow_ms * 1000);
    pthread_mutex_lock(&g_mu);
    g_pub_n++;
    g_pub_len = len;
    g_pub_first = len ? jpeg[0] : 0;
    pthread_mutex_unlock(&g_mu);
}

static int log_count(const char *what)
{
    int n = 0;
    char *p;
    pthread_mutex_lock(&g_mu);
    for (p = g_log; (p = strstr(p, what)); p++)
        n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int wait_pub(unsigned n)
{
    for (int i = 0; i < 4000; i++) {
        unsigned v;
        pthread_mutex_lock(&g_mu);
        v = g_pub_n;
        pthread_mutex_unlock(&g_mu);
        if (v >= n)
            return 1;
        usleep(1000);
    }
    return 0;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *g_dir;

/* the files of the ring directory: count, total bytes, number of .tmp leftovers */
static void dir_stat(int *files, size_t *bytes, int *tmps)
{
    DIR *d = opendir(g_dir);
    struct dirent *e;
    *files = *tmps = 0;
    *bytes = 0;
    if (!d)
        return;
    while ((e = readdir(d))) {
        char p[400];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        if (strstr(e->d_name, ".tmp"))
            (*tmps)++;
        if (strncmp(e->d_name, "akam2k_snap_", 12) == 0 && stat(p, &st) == 0) {
            (*files)++;
            *bytes += (size_t)st.st_size;
        }
    }
    closedir(d);
}

static void test_ring(void)
{
    a2k_snapring_t r;
    unsigned drop;
    int slot, i, ok = 1;
    printf("== ring bookkeeping (pure)\n");
    a2k_snapring_init(&r, 1000);
    slot = a2k_snapring_add(&r, 100, &drop);
    CHECK(slot == 0 && drop == 0 && r.n == 1 && r.total == 100, "first entry: slot 0");
    CHECK(a2k_snapring_add(&r, 100, &drop) == 1 && a2k_snapring_add(&r, 100, &drop) == 2 && r.n == 3 && r.total == 300, "slots 1 and 2");
    slot = a2k_snapring_add(&r, 250, &drop);
    CHECK(slot == 0 && drop == 0 && r.n == 3 && r.total == 450, "the fourth reuses slot 0: the oldest leaves the accounting");
    /* the byte cap: 600 bytes of a new file with 450 held and a cap of 1000 evicts the oldest until it fits */
    slot = a2k_snapring_add(&r, 600, &drop);
    CHECK(slot == 1 && r.total <= 1000 && r.n >= 1 && r.e[r.n - 1].len == 600, "over the byte cap: older entries are evicted");
    CHECK((drop & (1u << slot)) == 0, "the slot being written is never in the drop mask");
    a2k_snapring_init(&r, 600u << 10);
    for (i = 0; i < 5000 && ok; i++) {
        uint32_t len = (uint32_t)(rand() % (A2K_SNAP_MAX_JPEG + 1));
        a2k_snapring_add(&r, len, &drop);
        ok = r.n <= A2K_SNAP_RING && r.total <= r.cap;
        if (ok) { /* slots are distinct, the sum matches */
            size_t sum = 0;
            for (int a = 0; a < r.n; a++) {
                sum += r.e[a].len;
                for (int b = a + 1; b < r.n; b++)
                    ok &= r.e[a].slot != r.e[b].slot;
            }
            ok &= sum == r.total;
        }
    }
    CHECK(ok, "5000 random sizes up to 200 KB: at most 3 entries, at most 600 KB, distinct slots, exact total");
    a2k_snapring_init(&r, 100);
    slot = a2k_snapring_add(&r, 5000, &drop);
    CHECK(r.n == 1 && r.total == 5000 && slot == 0, "a single file bigger than the cap is still kept alone (the producer caps the size)");
}

static void fake_jpeg(uint8_t *b, size_t n, uint8_t tag)
{
    memset(b, tag, n);
    if (n >= 2) {
        b[0] = 0xff;
        b[1] = 0xd8;
    }
}

static void test_pipeline(void)
{
    struct a2k_snap_cfg c = { 1, 5 };
    static uint8_t jpg[A2K_SNAP_MAX_JPEG + 4096];
    unsigned rq, st, dr, fl;
    int files, tmps, i;
    size_t bytes;
    int64_t t = 100000;

    printf("== request, rate limit, submit (stub encoder)\n");
    CHECK(!a2k_snap_request(t) && !a2k_snap_enabled() && !a2k_snap_pending(), "before start: no request is accepted");
    c.interval_s = 4;
    CHECK(a2k_snap_start(&c) == -1, "an interval below 5 s is refused");
    c.interval_s = 301;
    CHECK(a2k_snap_start(&c) == -1, "an interval above 300 s is refused");
    c.interval_s = 5;
    c.enabled = 0;
    CHECK(a2k_snap_start(&c) == 0 && !a2k_snap_enabled(), "enabled = 0: nothing started");
    c.enabled = 1;
    a2k_snap_test_set_dir(g_dir);
    CHECK(a2k_snap_start(&c) == 0 && a2k_snap_enabled(), "started");
    CHECK(a2k_snap_request(t) == 1 && a2k_snap_pending(), "first request accepted, the capture thread sees it");
    CHECK(a2k_snap_request(t + 10) == 0, "a second request while one is pending is refused");
    a2k_snap_submit(NULL, 0);
    CHECK(!a2k_snap_pending(), "a NULL JPEG clears the request");
    a2k_snap_stats(&rq, &st, &dr, &fl);
    CHECK(rq == 1 && st == 0 && dr == 1, "an empty JPEG is dropped and counted");
    CHECK(a2k_snap_request(t + 4999) == 0, "inside the minimum interval: refused");
    CHECK(a2k_snap_request(t + 5000) == 1, "at the minimum interval: accepted");
    fake_jpeg(jpg, A2K_SNAP_MAX_JPEG + 1, 1);
    a2k_snap_submit(jpg, A2K_SNAP_MAX_JPEG + 1);
    a2k_snap_stats(&rq, &st, &dr, &fl);
    CHECK(!a2k_snap_pending() && dr == 2 && st == 0, "a JPEG over 200 KB is dropped, never stored nor published");
    CHECK(g_pub_n == 0, "...and nothing reached MQTT");
    a2k_snap_submit(jpg, 100);
    a2k_snap_stats(&rq, &st, &dr, &fl);
    CHECK(dr == 2 && st == 0, "a JPEG nobody asked for is ignored");
    t += 10000;
    CHECK(a2k_snap_request(t) == 1, "next request");
    fake_jpeg(jpg, 5000, 0xa1);
    a2k_snap_submit(jpg, 5000);
    CHECK(wait_pub(1), "the JPEG is handed to MQTT");
    pthread_mutex_lock(&g_mu);
    CHECK(g_pub_len == 5000 && g_pub_first == 0xff, "...whole");
    pthread_mutex_unlock(&g_mu);
    dir_stat(&files, &bytes, &tmps);
    CHECK(files == 1 && bytes == 5000 && tmps == 0, "and stored as one file, no temp file left");
    {
        char p[300];
        FILE *f;
        uint8_t back[5000];
        snprintf(p, sizeof p, "%s/akam2k_snap_0.jpg", g_dir);
        f = fopen(p, "rb");
        CHECK(f && fread(back, 1, 5000, f) == 5000 && !memcmp(back, jpg, 5000), "slot 0 holds exactly the JPEG");
        if (f)
            fclose(f);
    }

    printf("== a request the capture thread never serves is asked again after 2 s\n");
    t += 10000;
    CHECK(a2k_snap_request(t) == 1, "request");
    a2k_snap_failed(); /* (clears it: no encode) */
    a2k_snap_stats(&rq, &st, &dr, &fl);
    CHECK(!a2k_snap_pending() && fl == 1, "a failed encode clears the request and is counted");
    t += 10000;
    CHECK(a2k_snap_request(t) == 1 && !a2k_snap_request(t + 1999), "pending: refused before 2 s...");
    CHECK(a2k_snap_request(t + 6000) == 1, "...and a stale request is replaced (interval and timeout elapsed)");
    a2k_snap_submit(jpg, 100);
    CHECK(wait_pub(2), "served");
    t += 6000;

    printf("== the ring on disk stays bounded over many snapshots\n");
    for (i = 0; i < 12; i++) {
        size_t n = 1000 + (size_t)i * 7000;
        t += 6000;
        if (!a2k_snap_request(t)) { CHECK(0, "request accepted"); break; }
        fake_jpeg(jpg, n, (uint8_t)i);
        a2k_snap_submit(jpg, n);
        if (!wait_pub((unsigned)(3 + i)))
            CHECK(0, "published");
    }
    usleep(20000);
    dir_stat(&files, &bytes, &tmps);
    CHECK(files == A2K_SNAP_RING && tmps == 0 && bytes <= A2K_SNAP_TOTAL_MAX, "12 snapshots: exactly 3 files, no temp file, within 600 KB");
    a2k_snap_stats(&rq, &st, &dr, &fl);
    CHECK(st >= 13, "all of them were stored");

    printf("== memory guard, three failures, a slow publisher\n");
    g_avail = 2000;
    t += 6000;
    CHECK(a2k_snap_request(t) == 0, "MemAvailable below 4 MB: no snapshot");
    g_avail = 100000;
    CHECK(a2k_snap_request(t) == 1, "memory back: accepted");
    a2k_snap_failed();
    t += 6000;
    CHECK(a2k_snap_request(t) == 1, "request");
    a2k_snap_failed();
    CHECK(a2k_snap_enabled(), "two failures: still on");
    t += 6000;
    CHECK(a2k_snap_request(t) == 1, "request");
    a2k_snap_failed();
    CHECK(!a2k_snap_enabled() && !a2k_snap_pending() && log_count("snapshot: disabled") == 1, "three failures in a row: disabled, logged once");
    a2k_snap_disable("again");
    CHECK(log_count("snapshot: disabled") == 1, "disabling again does not log again");
    t += 6000;
    CHECK(a2k_snap_request(t) == 0, "disabled: no request");
    a2k_snap_stop();

    c.interval_s = 5;
    g_pub_slow_ms = 300;
    CHECK(a2k_snap_start(&c) == 0 && a2k_snap_enabled(), "restarted");
    t += 60000;
    CHECK(a2k_snap_request(t) == 1, "request");
    fake_jpeg(jpg, 20000, 7);
    {
        int64_t t0 = mono_ms();
        unsigned before;
        pthread_mutex_lock(&g_mu);
        before = g_pub_n;
        pthread_mutex_unlock(&g_mu);
        a2k_snap_submit(jpg, 20000);
        usleep(30000); /* the writer is now inside the 300 ms publish */
        t += 6000;
        CHECK(a2k_snap_request(t) == 1, "a new request while the publisher is slow");
        a2k_snap_submit(jpg, 20000);
        CHECK(mono_ms() - t0 < 150, "submit() returned at once although the publisher takes 300 ms");
        a2k_snap_stats(&rq, &st, &dr, &fl);
        usleep(900000);
        pthread_mutex_lock(&g_mu);
        CHECK(g_pub_n >= before + 1, "the publisher catches up");
        pthread_mutex_unlock(&g_mu);
    }
    g_pub_slow_ms = 0;
    a2k_snap_stop();
    a2k_snap_stop();
    CHECK(1, "stop twice is safe");
    dir_stat(&files, &bytes, &tmps);
    CHECK(files <= A2K_SNAP_RING && tmps == 0 && bytes <= A2K_SNAP_TOTAL_MAX, "after all of it: still at most 3 files within the cap");
}

int main(void)
{
    char dir[] = "/tmp/rot2k_snap_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    g_dir = dir;
    srand(7);
    test_ring();
    test_pipeline();
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k snap unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
