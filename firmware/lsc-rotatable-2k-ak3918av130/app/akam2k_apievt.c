/* See akam2k_apievt.h. */
#include "akam2k_apievt.h"

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

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static evt_t g_ring[A2K_EVT_RING];
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
    evt_t *e;
    if (!type_ok(type) || (data && strlen(data) > A2K_EVT_DATA))
        return;
    pthread_mutex_lock(&g_mu);
    e = &g_ring[g_seq % A2K_EVT_RING];
    e->seq = ++g_seq;
    e->up_s = up_s();
    snprintf(e->type, sizeof e->type, "%s", type);
    snprintf(e->data, sizeof e->data, "%s", data ? data : "{}");
    if (g_wake >= 0 && write(g_wake, "e", 1) < 0) {
        /* a full pipe already wakes the reader */
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_api_event_button(void)
{
    a2k_api_event("button", "{\"press\":\"short\"}");
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

int a2k_apievt_json(uint32_t since, char *out, size_t cap, int *count)
{
    size_t len;
    int n = 0, first = 1, missed;
    uint32_t s, oldest;
    int w;
    pthread_mutex_lock(&g_mu);
    s = g_seq;
    oldest = s > A2K_EVT_RING ? s - A2K_EVT_RING + 1 : 1;
    if (since > s)
        since = s; /* a cursor from before a restart: start from now */
    missed = since + 1 < oldest;
    w = snprintf(out, cap, "{\"seq\":%lu,\"missed\":%s,\"events\":[", (unsigned long)s, missed ? "true" : "false");
    if (w < 0 || (size_t)w >= cap) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    len = (size_t)w;
    for (uint32_t q = since + 1 > oldest ? since + 1 : oldest; q <= s; q++) {
        const evt_t *e = &g_ring[(q - 1) % A2K_EVT_RING];
        if (!first) {
            if (len + 1 >= cap) {
                pthread_mutex_unlock(&g_mu);
                return -1;
            }
            out[len++] = ',';
        }
        w = snprintf(out + len, cap - len, "{\"seq\":%lu,\"up_s\":%ld,\"type\":\"%s\",\"data\":%s}", (unsigned long)e->seq,
                     e->up_s, e->type, e->data);
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
    g_seq = 0;
    g_person = -1;
    pthread_mutex_unlock(&g_mu);
}
