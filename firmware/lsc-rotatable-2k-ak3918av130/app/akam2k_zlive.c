/* See akam2k_zlive.h. */
#include <pthread.h>
#include <string.h>

#include "akam2k_log.h"
#include "akam2k_store.h"
#include "akam2k_zlive.h"
#include "hal_ptz.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER; /* serialises the edits and their file writes: g_mu (read by the detection thread at every inference) never waits for flash */
static a2k_zones_t g_z;
static unsigned g_gen = 1;
static char g_path[160];
static void (*g_notify)(const a2k_zones_t *);

int a2k_zlive_init(const a2k_zones_t *initial, const char *store_path)
{
    char buf[A2K_STORE_MAX + 1];
    a2k_zones_t t;
    int used = 0;
    pthread_mutex_lock(&g_mu);
    memset(&g_z, 0, sizeof g_z);
    if (initial)
        g_z = *initial;
    g_path[0] = '\0';
    if (store_path && strlen(store_path) < sizeof g_path) {
        int n;
        strcpy(g_path, store_path);
        n = a2k_store_read(g_path, buf, sizeof buf);
        if (n >= 0) {
            if (a2k_zones_parse(buf, (size_t)n, &t) == 0) {
                g_z = t;
                used = 1;
                a2k_log("zones: runtime file %s used (%d zone(s), %d ignore rectangle(s))", g_path, t.n, t.ni);
            } else {
                a2k_log("zones: %s is invalid, ignored (the card file stays in use)", g_path);
            }
        }
    }
    g_gen++;
    pthread_mutex_unlock(&g_mu);
    return used;
}

void a2k_zlive_get(a2k_zones_t *out, unsigned *gen)
{
    pthread_mutex_lock(&g_mu);
    *out = g_z;
    if (gen)
        *gen = g_gen;
    pthread_mutex_unlock(&g_mu);
}

int a2k_zlive_set_text(int which, const char *text)
{
    a2k_zones_t t;
    char file[A2K_STORE_MAX + 1];
    void (*fn)(const a2k_zones_t *);
    int n;
    char path[sizeof g_path];
    pthread_mutex_lock(&g_wmu);
    pthread_mutex_lock(&g_mu);
    t = g_z;
    if (a2k_zones_set_text(&t, which, text) != 0) {
        pthread_mutex_unlock(&g_mu);
        pthread_mutex_unlock(&g_wmu);
        return -1;
    }
    n = a2k_zones_format(&t, A2K_ZF_FILE, '\n', file, sizeof file);
    if (n < 0 || n > A2K_ZONES_FILE_MAX) { /* would not load back: refused */
        pthread_mutex_unlock(&g_mu);
        pthread_mutex_unlock(&g_wmu);
        return -1;
    }
    g_z = t;
    g_gen++;
    fn = g_notify;
    memcpy(path, g_path, sizeof path);
    pthread_mutex_unlock(&g_mu);
    if (path[0] && a2k_store_write(path, file, (size_t)n) != 0)
        a2k_log("zones: cannot write %s (the change holds until the next restart)", path);
    pthread_mutex_unlock(&g_wmu);
    a2k_log("zones: %s edited: %d zone(s), %d ignore rectangle(s)", which == A2K_ZF_ZONES ? "zones" : "ignore_zones", t.n, t.ni);
    if (fn)
        fn(&t);
    return 0;
}

void a2k_zlive_get_text(int which, char *out, size_t cap)
{
    a2k_zones_t t;
    a2k_zlive_get(&t, NULL);
    if (a2k_zones_format(&t, which, ';', out, cap) < 0 && cap)
        out[0] = '\0';
}

int a2k_zlive_applies(int ptz_state) { return ptz_state == HAL_PTZ_HOME || ptz_state == HAL_PTZ_UNKNOWN; }

void a2k_zlive_set_notify(void (*fn)(const a2k_zones_t *))
{
    pthread_mutex_lock(&g_mu);
    g_notify = fn;
    pthread_mutex_unlock(&g_mu);
}
