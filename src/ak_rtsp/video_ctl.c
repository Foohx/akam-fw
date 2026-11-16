#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cfg_file.h"
#include "video_cfg.h"
#include "video_ctl.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_persist = 1;

void video_ctl_set_persist(int on) { g_persist = on; }

static int is_ours(const char *k)
{
    return !strcmp(k, "video.bitrate_cfg") || !strcmp(k, "video.fps_cfg") || !strcmp(k, "video.gop_cfg") ||
           !strcmp(k, "video.minqp_cfg") || !strcmp(k, "video.maxqp_cfg") || !strcmp(k, "video.cfg_pending");
}

static int pending(const video_cfg_t *saved)
{
    const video_cfg_t *a = video_cfg_active();
    return saved->bitrate != a->bitrate || saved->fps != a->fps || saved->gop_s != a->gop_s ||
           saved->minqp != a->minqp || saved->maxqp != a->maxqp;
}

static void fmt_key(const video_cfg_t *c, const char *key, char *out, size_t cap)
{
    if (!strcmp(key, "video.bitrate_cfg")) snprintf(out, cap, "%s=%d", key, c->bitrate);
    else if (!strcmp(key, "video.fps_cfg")) snprintf(out, cap, "%s=%d", key, c->fps);
    else if (!strcmp(key, "video.gop_cfg")) snprintf(out, cap, "%s=%d", key, c->gop_s);
    else if (!strcmp(key, "video.minqp_cfg")) snprintf(out, cap, "%s=%d", key, c->minqp);
    else if (!strcmp(key, "video.maxqp_cfg")) snprintf(out, cap, "%s=%d", key, c->maxqp);
    else snprintf(out, cap, "%s=%d", key, pending(c));
}

int video_ctl_get(const char *key, char *out, size_t cap)
{
    if (!is_ours(key)) return -1;
    video_cfg_t c;
    pthread_mutex_lock(&g_lock);
    video_cfg_read(&c);
    pthread_mutex_unlock(&g_lock);
    fmt_key(&c, key, out, cap);
    return 0;
}

int video_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    if (!is_ours(key)) return -1;
    if (!strcmp(key, "video.cfg_pending")) {
        snprintf(msg, cap, "ERR video.cfg_pending is read-only");
        return 1;
    }
    if (!g_persist) {
        snprintf(msg, cap, "ERR %s can only be set from the camera itself (it applies at the next start)", key);
        return 1;
    }
    const char *want = !strcmp(key, "video.bitrate_cfg") ? "bitrate" : !strcmp(key, "video.fps_cfg") ? "fps"
                       : !strcmp(key, "video.gop_cfg") ? "gop" : !strcmp(key, "video.minqp_cfg") ? "minqp" : "maxqp";
    video_cfg_t c;
    pthread_mutex_lock(&g_lock);
    video_cfg_read(&c);
    char old[VIDEO_CFG_MAX];
    int n_old = video_cfg_format(&c, old, sizeof(old));
    int bad_pair = 0;
    if (video_cfg_set_key(&c, want, val) == 0 && !video_cfg_qp_ok(c.minqp, c.maxqp)) bad_pair = 1;
    if (bad_pair) {
        int lo = c.minqp, hi = c.maxqp;
        pthread_mutex_unlock(&g_lock);
        snprintf(msg, cap, "ERR %s=%s refused: minqp must stay below maxqp (would be %d and %d); set the other one first", key,
                 val, lo, hi);
        return 1;
    }
    if (video_cfg_set_key(&c, want, val) != 0) {
        pthread_mutex_unlock(&g_lock);
        if (!strcmp(want, "bitrate"))
            snprintf(msg, cap, "ERR invalid %s value (want %d..%d kbit/s)", key, VIDEO_KBPS_MIN, VIDEO_KBPS_MAX);
        else if (!strcmp(want, "fps"))
            snprintf(msg, cap, "ERR invalid %s value (want 10, 15, 20 or 25)", key);
        else if (!strcmp(want, "gop"))
            snprintf(msg, cap, "ERR invalid %s value (want %d..%d seconds)", key, VIDEO_GOP_S_MIN, VIDEO_GOP_S_MAX);
        else
            snprintf(msg, cap, "ERR invalid %s value (want %d..%d)", key, VIDEO_QP_MIN, VIDEO_QP_MAX);
        return 1;
    }
    char buf[VIDEO_CFG_MAX];
    int n = video_cfg_format(&c, buf, sizeof(buf));
    int rc = 0, err = 0;
    if (n >= 0 && n == n_old && memcmp(buf, old, (size_t)n) == 0) {
        /* unchanged: no flash write (the flash wears, and a slider may repeat a value) */
    } else if (n < 0) {
        rc = -1;
        err = ENOSPC;
    } else if (n == 0) {
        if (unlink(VIDEO_CFG_PATH) < 0 && errno != ENOENT) { rc = -1; err = errno; }
    } else if (cfg_write_atomic(VIDEO_CFG_PATH, buf, (size_t)n) < 0) {
        rc = -1;
        err = errno;
    }
    int pend = pending(&c);
    pthread_mutex_unlock(&g_lock);
    if (rc < 0) {
        snprintf(msg, cap, "ERR %s not saved: %s", key, strerror(err));
        return 1;
    }
    snprintf(msg, cap, "OK %s=%s%s", key, val, pend ? " (restart to apply)" : "");
    return 0;
}

void video_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = { "video.bitrate_cfg", "video.fps_cfg", "video.gop_cfg", "video.minqp_cfg",
                                         "video.maxqp_cfg", "video.cfg_pending" };
    char line[64];
    video_cfg_t c;
    pthread_mutex_lock(&g_lock);
    video_cfg_read(&c); /* once: a malformed file is logged once per LIST, not four times */
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        fmt_key(&c, keys[i], line, sizeof(line));
        emit(arg, line);
    }
}
