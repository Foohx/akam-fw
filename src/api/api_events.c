/* See api_events.h. */
#include "api_events.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint32_t seq;
    long up_s;
    char type[A2K_EVT_TYPE + 1];
    char data[A2K_EVT_DATA + 1];
} evt_t;

/* detections have a ring of their own (larger entries, few of them): a burst of them never evicts a button / ptz / setting event */
typedef struct {
    uint32_t seq;
    long up_s;
    char data[A2K_EVT_DDATA + 1];
} devt_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static evt_t g_ring[A2K_EVT_RING];
static devt_t g_dring[A2K_EVT_DRING];
static evt_t g_pring[A2K_EVT_PRING]; /* patrol events */
static uint32_t g_nring, g_ndring, g_npring; /* entries written to each ring so far */
static uint32_t g_seq;
static int g_wake = -1;
static int g_person = -1;

static long up_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

void a2k_apievt_set_wake(int fd)
{
    pthread_mutex_lock(&g_mu);
    g_wake = fd;
    pthread_mutex_unlock(&g_mu);
}

static int type_ok(const char *t)
{
    size_t i, n = t ? strlen(t) : 0;
    if (n < 1 || n > A2K_EVT_TYPE)
        return 0;
    for (i = 0; i < n; i++)
        if (!((t[i] >= 'a' && t[i] <= 'z') || t[i] == '_'))
            return 0;
    return 1;
}

void a2k_api_event(const char *type, const char *data)
{
    int det = type && strcmp(type, A2K_EVT_DETECTIONS) == 0, pat = type && strcmp(type, A2K_EVT_PATROL) == 0;
    if (!type_ok(type) || (data && strlen(data) > (det ? A2K_EVT_DDATA : A2K_EVT_DATA)))
        return;
    pthread_mutex_lock(&g_mu);
    if (det) {
        devt_t *d = &g_dring[g_ndring++ % A2K_EVT_DRING];
        d->seq = ++g_seq;
        d->up_s = up_s();
        snprintf(d->data, sizeof d->data, "%s", data ? data : "{}");
    } else {
        evt_t *e = pat ? &g_pring[g_npring++ % A2K_EVT_PRING] : &g_ring[g_nring++ % A2K_EVT_RING];
        e->seq = ++g_seq;
        e->up_s = up_s();
        snprintf(e->type, sizeof e->type, "%s", type);
        snprintf(e->data, sizeof e->data, "%s", data ? data : "{}");
    }
    if (g_wake >= 0 && write(g_wake, "e", 1) < 0) {
        /* a full pipe already wakes the reader */
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_api_event_button(void)
{
    a2k_api_event("button", "{\"gesture\":\"short\",\"actions\":[\"none\"],\"result\":\"ok\"}");
}

void a2k_api_event_person(int present, int score)
{
    char d[48];
    int p = present ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_person == p) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_person = p;
    pthread_mutex_unlock(&g_mu);
    snprintf(d, sizeof d, "{\"present\":%s,\"score\":%d}", p ? "true" : "false", score);
    a2k_api_event("person", d);
}

int a2k_apievt_person(void)
{
    int p;
    pthread_mutex_lock(&g_mu);
    p = g_person == 1;
    pthread_mutex_unlock(&g_mu);
    return p;
}

uint32_t a2k_apievt_seq(void)
{
    uint32_t s;
    pthread_mutex_lock(&g_mu);
    s = g_seq;
    pthread_mutex_unlock(&g_mu);
    return s;
}

/* The oldest entry of a ring still held: start index and count. */
static void ring_span(uint32_t written, uint32_t size, uint32_t *start, uint32_t *n)
{
    *n = written < size ? written : size;
    *start = written < size ? 0 : written % size;
}

int a2k_apievt_json(uint32_t since, char *out, size_t cap, int *count)
{
    size_t len;
    int n = 0, first = 1, w;
    uint32_t s, ms, mn, ds, dn, ps, pn, i = 0, j = 0, q = 0;
    pthread_mutex_lock(&g_mu);
    s = g_seq;
    if (since > s)
        since = s; /* a cursor from before a restart: start from now */
    ring_span(g_nring, A2K_EVT_RING, &ms, &mn);
    ring_span(g_ndring, A2K_EVT_DRING, &ds, &dn);
    ring_span(g_npring, A2K_EVT_PRING, &ps, &pn);
    /* every sequence number after `since` is in one of the two rings unless it was overwritten: count the ones held */
    {
        int held = 0;
        for (uint32_t k = 0; k < mn; k++)
            held += g_ring[(ms + k) % A2K_EVT_RING].seq > since;
        for (uint32_t k = 0; k < dn; k++)
            held += g_dring[(ds + k) % A2K_EVT_DRING].seq > since;
        for (uint32_t k = 0; k < pn; k++)
            held += g_pring[(ps + k) % A2K_EVT_PRING].seq > since;
        w = snprintf(out, cap, "{\"seq\":%lu,\"missed\":%s,\"events\":[", (unsigned long)s,
                     (uint32_t)held != s - since ? "true" : "false");
    }
    if (w < 0 || (size_t)w >= cap) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    len = (size_t)w;
    for (;;) { /* merge the two rings by sequence number, oldest first */
        const evt_t *e = NULL, *pe = NULL;
        const devt_t *d = NULL;
        const char *type, *data;
        uint32_t seq;
        long up;
        while (i < mn && g_ring[(ms + i) % A2K_EVT_RING].seq <= since)
            i++;
        while (j < dn && g_dring[(ds + j) % A2K_EVT_DRING].seq <= since)
            j++;
        while (q < pn && g_pring[(ps + q) % A2K_EVT_PRING].seq <= since)
            q++;
        if (q < pn)
            pe = &g_pring[(ps + q) % A2K_EVT_PRING];
        if (i < mn)
            e = &g_ring[(ms + i) % A2K_EVT_RING];
        if (j < dn)
            d = &g_dring[(ds + j) % A2K_EVT_DRING];
        if (!e && !d && !pe)
            break;
        if (pe && (!e || pe->seq < e->seq) && (!d || pe->seq < d->seq)) {
            seq = pe->seq, up = pe->up_s, type = pe->type, data = pe->data;
            q++;
        } else if (e && (!d || e->seq < d->seq)) {
            seq = e->seq, up = e->up_s, type = e->type, data = e->data;
            i++;
        } else {
            seq = d->seq, up = d->up_s, type = A2K_EVT_DETECTIONS, data = d->data;
            j++;
        }
        if (!first) {
            if (len + 1 >= cap) {
                pthread_mutex_unlock(&g_mu);
                return -1;
            }
            out[len++] = ',';
        }
        w = snprintf(out + len, cap - len, "{\"seq\":%lu,\"up_s\":%ld,\"type\":\"%s\",\"data\":%s}", (unsigned long)seq, up,
                     type, data);
        if (w < 0 || (size_t)w >= cap - len) {
            pthread_mutex_unlock(&g_mu);
            return -1;
        }
        len += (size_t)w;
        first = 0;
        n++;
    }
    pthread_mutex_unlock(&g_mu);
    if (len + 3 > cap)
        return -1;
    memcpy(out + len, "]}", 3);
    if (count)
        *count = n;
    return (int)(len + 2);
}

void a2k_apievt_reset(void)
{
    pthread_mutex_lock(&g_mu);
    memset(g_ring, 0, sizeof g_ring);
    memset(g_dring, 0, sizeof g_dring);
    memset(g_pring, 0, sizeof g_pring);
    g_nring = g_ndring = g_npring = 0;
    g_seq = 0;
    g_person = -1;
    pthread_mutex_unlock(&g_mu);
}
