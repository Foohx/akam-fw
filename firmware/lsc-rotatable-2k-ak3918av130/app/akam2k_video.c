/* See akam2k_video.h. The live copy of the settings; the rules (ranges, parsing, the file form) are in akam2k_vidcfg.c. */
#include "akam2k_video.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "akam2k_log.h"
#include "akam2k_store.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static a2k_vid_t g_base[A2K_VID_STREAMS], g_cur[A2K_VID_STREAMS], g_good[A2K_VID_STREAMS];
static char g_path[160];
static void (*g_notify)(void);

int a2k_vid_init(const a2k_vid_t base[A2K_VID_STREAMS], const char *store_path)
{
    char buf[A2K_STORE_MAX + 1];
    a2k_vid_t t[A2K_VID_STREAMS];
    int used = 0;
    pthread_mutex_lock(&g_mu);
    memcpy(g_base, base, sizeof g_base);
    memcpy(g_cur, base, sizeof g_cur);
    g_path[0] = '\0';
    if (store_path && strlen(store_path) < sizeof g_path) {
        int n;
        strcpy(g_path, store_path);
        n = a2k_store_read(g_path, buf, sizeof buf);
        if (n > 0) {
            if (a2k_vid_parse_file(buf, (size_t)n, g_base, t) == 0) {
                memcpy(g_cur, t, sizeof g_cur);
                used = 1;
                a2k_log("video: runtime file %s used", g_path);
            } else {
                a2k_log("video: %s is invalid, ignored (akam2k.conf and the defaults stay in use)", g_path);
            }
        }
    }
    memcpy(g_good, g_cur, sizeof g_good);
    pthread_mutex_unlock(&g_mu);
    return used;
}

void a2k_vid_get(int chn, a2k_vid_t *out)
{
    pthread_mutex_lock(&g_mu);
    *out = g_cur[chn >= 0 && chn < A2K_VID_STREAMS ? chn : 0];
    pthread_mutex_unlock(&g_mu);
}

int a2k_vid_get_text(int chn, int p, char *out, size_t cap)
{
    a2k_vid_t t;
    if (chn < 0 || chn >= A2K_VID_STREAMS || p < 0 || p >= A2K_VP_N)
        return -1;
    a2k_vid_get(chn, &t);
    return a2k_vid_text(p, t.v[p], out, cap);
}

/* g_mu held: stores the runtime file for the current values (the keys that differ from the base). Flash is touched by user actions only. */
static void persist_locked(void)
{
    char file[A2K_STORE_MAX + 1];
    int n;
    if (!g_path[0])
        return;
    n = a2k_vid_format_file(g_base, g_cur, file, sizeof file);
    if (n < 0 || a2k_store_write(g_path, file, (size_t)n) != 0)
        a2k_log("video: cannot write %s (the change holds until the next restart)", g_path);
}

int a2k_vid_set(int chn, int p, const char *text)
{
    a2k_vid_t t;
    int v;
    if (chn < 0 || chn >= A2K_VID_STREAMS || p < 0 || p >= A2K_VP_N || a2k_vid_parse(p, text, &v) != 0)
        return -1;
    pthread_mutex_lock(&g_mu);
    t = g_cur[chn];
    t.v[p] = v;
    if (!a2k_vid_valid(chn, &t)) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    if (t.v[p] == g_cur[chn].v[p]) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    g_cur[chn] = t;
    persist_locked();
    pthread_mutex_unlock(&g_mu);
    {
        char k[32];
        a2k_vid_key(chn, p, k, sizeof k);
        a2k_log("video: %s = %s (Home Assistant), the encoder of stream %d is re-opened", k, text, chn);
    }
    return 0;
}

int a2k_vid_pending(int chn, a2k_vid_t *want)
{
    int r;
    pthread_mutex_lock(&g_mu);
    r = memcmp(&g_cur[chn], &g_good[chn], sizeof g_cur[chn]) != 0;
    if (r)
        *want = g_cur[chn];
    pthread_mutex_unlock(&g_mu);
    return r;
}

void a2k_vid_applied(int chn, int ok, const a2k_vid_t *ran)
{
    void (*fn)(void) = NULL;
    pthread_mutex_lock(&g_mu);
    if (ok) {
        g_good[chn] = *ran; /* what the encoder runs with, not g_cur: it may have moved during the re-open */
    } else {
        g_cur[chn] = g_good[chn];
        persist_locked();
        fn = g_notify;
    }
    pthread_mutex_unlock(&g_mu);
    if (!ok)
        a2k_log("video: stream %d: the encoder refused the new settings, the previous ones are back", chn);
    if (fn)
        fn();
}

int a2k_vid_reset_runtime(void)
{
    int changed;
    pthread_mutex_lock(&g_mu);
    changed = memcmp(g_cur, g_base, sizeof g_cur) != 0;
    memcpy(g_cur, g_base, sizeof g_cur);
    memcpy(g_good, g_base, sizeof g_good);
    if (changed)
        persist_locked();
    pthread_mutex_unlock(&g_mu);
    if (changed)
        a2k_log("video: the runtime settings could not be opened, forgotten (akam2k.conf and the defaults are back)");
    return changed;
}

void a2k_vid_set_notify(void (*fn)(void))
{
    pthread_mutex_lock(&g_mu);
    g_notify = fn;
    pthread_mutex_unlock(&g_mu);
}
