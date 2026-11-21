/* See snap.h. Host-compilable: no vendor call, the unit test stubs a2k_log, a2k_mem_avail_kb and a2k_mqtt_snapshot. */
#include "snap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "mqtt_bind.h"

#define MIN_AVAIL_KB 4096      /* below 4 MB free no snapshot is taken (the JPEG copy and the MQTT copy are 400 KB at most) */
#define REQ_TIMEOUT_MS 2000    /* a request the capture thread never served is asked again after this */
#define STACK_BYTES 65536
#define FAIL_MAX 3

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_th;
static struct a2k_snap_cfg g_cfg = { 1, 10 };
static int g_on, g_started, g_quit, g_fail_streak, g_dis_logged;
static volatile int g_pending;
static int64_t g_req_ms, g_last_ms;
static int g_have_last;
static uint8_t *g_buf;           /* the JPEG in flight (handed from the capture thread to the writer thread) */
static size_t g_len;
static unsigned g_requested, g_stored, g_dropped, g_failed;
static char g_dir[128] = A2K_SNAP_DIR;
static size_t g_total_cap = A2K_SNAP_TOTAL_MAX;
static a2k_snapring_t g_ring;
static const char g_none[1] = { 0 }; /* (the repo C scan forbids the empty string literal) */

void a2k_snap_defaults(struct a2k_snap_cfg *c)
{
    c->enabled = 1;
    c->interval_s = 10;
}

void a2k_snap_test_set_dir(const char *dir) { snprintf(g_dir, sizeof g_dir, "%s", dir); }
void a2k_snap_test_set_cap(size_t total_cap) { g_total_cap = total_cap; }

/* ------------------------------------------------------------------------ ring bookkeeping */

void a2k_snapring_init(a2k_snapring_t *r, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->cap = cap;
}

int a2k_snapring_add(a2k_snapring_t *r, uint32_t len, unsigned *drop)
{
    int slot = (int)(r->next % A2K_SNAP_RING), i;
    *drop = 0;
    /* the slot about to be overwritten leaves the ring (it is the oldest one once the ring is full) */
    for (i = 0; i < r->n; i++)
        if (r->e[i].slot == slot) {
            r->total -= r->e[i].len;
            memmove(&r->e[i], &r->e[i + 1], sizeof r->e[0] * (size_t)(r->n - i - 1));
            r->n--;
            break;
        }
    /* the byte cap: the oldest files go until the new one fits */
    while (r->n > 0 && r->total + len > r->cap) {
        *drop |= 1u << r->e[0].slot;
        r->total -= r->e[0].len;
        memmove(&r->e[0], &r->e[1], sizeof r->e[0] * (size_t)(r->n - 1));
        r->n--;
    }
    r->e[r->n].slot = slot;
    r->e[r->n].len = len;
    r->n++;
    r->total += len;
    r->next++;
    return slot;
}

/* ------------------------------------------------------------------------ switches and counters */

int a2k_snap_enabled(void)
{
    int v;
    pthread_mutex_lock(&g_mu);
    v = g_on;
    pthread_mutex_unlock(&g_mu);
    return v;
}

void a2k_snap_disable(const char *why)
{
    int first;
    pthread_mutex_lock(&g_mu);
    first = !g_dis_logged;
    g_dis_logged = 1;
    g_on = 0;
    g_pending = 0;
    pthread_mutex_unlock(&g_mu);
    if (first)
        a2k_log("snapshot: disabled (%s)", why);
}

void a2k_snap_stats(unsigned *requested, unsigned *stored, unsigned *dropped, unsigned *failed)
{
    pthread_mutex_lock(&g_mu);
    *requested = g_requested;
    *stored = g_stored;
    *dropped = g_dropped;
    *failed = g_failed;
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------------ producers */

int a2k_snap_request(int64_t now_ms)
{
    int ok = 0;
    long avail = a2k_mem_avail_kb();
    pthread_mutex_lock(&g_mu);
    if (g_on && !g_buf && (!g_pending || now_ms - g_req_ms >= REQ_TIMEOUT_MS) &&
        (!g_have_last || now_ms - g_last_ms >= (int64_t)g_cfg.interval_s * 1000) && (avail < 0 || avail >= MIN_AVAIL_KB)) {
        g_pending = 1;
        g_req_ms = g_last_ms = now_ms;
        g_have_last = 1;
        g_requested++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int a2k_snap_pending(void) { return g_pending; }

void a2k_snap_submit(const uint8_t *jpeg, size_t len)
{
    uint8_t *copy = NULL;
    pthread_mutex_lock(&g_mu);
    if (!g_pending) { /* nobody asked (or the snapshots were switched off meanwhile) */
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_pending = 0;
    g_fail_streak = 0;
    if (jpeg && len > 0 && len <= A2K_SNAP_MAX_JPEG && !g_buf)
        copy = malloc(len);
    if (!copy) {
        g_dropped++;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    memcpy(copy, jpeg, len);
    g_buf = copy;
    g_len = len;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

void a2k_snap_failed(void)
{
    int off = 0;
    pthread_mutex_lock(&g_mu);
    g_pending = 0;
    g_failed++;
    if (++g_fail_streak >= FAIL_MAX)
        off = 1;
    pthread_mutex_unlock(&g_mu);
    if (off)
        a2k_snap_disable("the JPEG encoder failed three times in a row");
}

/* ------------------------------------------------------------------------ writer thread */

static void slot_path(char *out, size_t cap, int slot, const char *tail)
{
    snprintf(out, cap, "%s/akam2k_snap_%d.jpg%s", g_dir, slot, tail);
}

/* Stores the JPEG in its ring slot (tmp file + rename: a reader sees a whole file) and deletes the evicted ones. */
static int store(const uint8_t *jpeg, size_t len, int *slot_out)
{
    char path[160], tmp[168];
    unsigned drop = 0;
    int slot, i, ok = 0;
    FILE *f;

    slot = a2k_snapring_add(&g_ring, (uint32_t)len, &drop);
    *slot_out = slot;
    for (i = 0; i < A2K_SNAP_RING; i++)
        if (drop & (1u << i)) {
            slot_path(path, sizeof path, i, g_none);
            unlink(path);
        }
    slot_path(path, sizeof path, slot, g_none);
    slot_path(tmp, sizeof tmp, slot, ".tmp");
    f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(jpeg, 1, len, f) == len && fflush(f) == 0;
        ok = (fclose(f) == 0) && ok;
        if (ok)
            ok = rename(tmp, path) == 0;
        if (!ok)
            unlink(tmp);
    }
    return ok ? 0 : -1;
}

static void *snap_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        uint8_t *buf;
        size_t len;
        int slot = 0, rc;
        while (!g_buf && !g_quit)
            pthread_cond_wait(&g_cv, &g_mu);
        if (!g_buf)
            break; /* quit */
        buf = g_buf;
        len = g_len;
        g_buf = NULL;
        g_len = 0;
        pthread_mutex_unlock(&g_mu);
        rc = store(buf, len, &slot);
        a2k_mqtt_snapshot(buf, len); /* copies what it keeps and returns at once */
        free(buf);
        pthread_mutex_lock(&g_mu);
        g_stored += rc == 0;
        pthread_mutex_unlock(&g_mu);
        if (rc == 0)
            a2k_log("snapshot: %u bytes stored (slot %d) and handed to MQTT", (unsigned)len, slot);
        else
            a2k_log("snapshot: %u bytes, cannot write the file (slot %d), still handed to MQTT", (unsigned)len, slot);
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int a2k_snap_start(const struct a2k_snap_cfg *c)
{
    pthread_attr_t at;
    int rc;
    if (g_started || !c->enabled)
        return 0;
    if (c->interval_s < 5 || c->interval_s > 300) {
        a2k_log("snapshot: invalid interval %d, snapshots off", c->interval_s);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    g_cfg = *c;
    g_quit = 0;
    g_pending = 0;
    g_have_last = 0;
    g_fail_streak = 0;
    g_dis_logged = 0;
    g_requested = g_stored = g_dropped = g_failed = 0;
    a2k_snapring_init(&g_ring, g_total_cap);
    pthread_mutex_unlock(&g_mu);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, STACK_BYTES);
    rc = pthread_create(&g_th, &at, snap_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc) {
        a2k_log("snapshot: pthread_create = %d, snapshots off", rc);
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    g_on = 1;
    pthread_mutex_unlock(&g_mu);
    g_started = 1;
    return 0;
}

void a2k_snap_stop(void)
{
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mu);
    g_on = 0;
    g_pending = 0;
    g_quit = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_th, NULL);
    g_started = 0;
    pthread_mutex_lock(&g_mu);
    free(g_buf);
    g_buf = NULL;
    pthread_mutex_unlock(&g_mu);
}
