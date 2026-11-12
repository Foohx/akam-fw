#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "video_cfg.h"

static video_cfg_t g_active = { ENC_KBPS, ENC_FPS, VIDEO_GOP_S_DEF };
static int g_loaded;

void video_cfg_defaults(video_cfg_t *c)
{
    c->bitrate = ENC_KBPS;
    c->fps = ENC_FPS;
    c->gop_s = VIDEO_GOP_S_DEF;
}

int video_cfg_fps_ok(long fps) { return fps == 10 || fps == 15 || fps == 20 || fps == 25; }

int video_cfg_num(const char *s, long *out)
{
    if (!s || !*s || strlen(s) > 6) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    *out = v;
    return 0;
}

int video_cfg_set_key(video_cfg_t *c, const char *key, const char *val)
{
    long v;
    if (video_cfg_num(val, &v) != 0) return -1;
    if (!strcmp(key, "bitrate")) {
        if (v < VIDEO_KBPS_MIN || v > VIDEO_KBPS_MAX) return -1;
        c->bitrate = (int)v;
    } else if (!strcmp(key, "fps")) {
        if (!video_cfg_fps_ok(v)) return -1;
        c->fps = (int)v;
    } else if (!strcmp(key, "gop")) {
        if (v < VIDEO_GOP_S_MIN || v > VIDEO_GOP_S_MAX) return -1;
        c->gop_s = (int)v;
    } else {
        return -1;
    }
    return 0;
}

int video_cfg_parse(const char *text, size_t len, video_cfg_t *out)
{
    char buf[VIDEO_CFG_MAX + 1];
    int bad = 0;
    video_cfg_defaults(out);
    if (len > VIDEO_CFG_MAX) return 1; /* oversized file: all defaults */
    memcpy(buf, text, len);
    buf[len] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';
        if (!*line) continue;
        char *eq = strchr(line, '=');
        if (!eq) { bad++; continue; }
        *eq = '\0';
        if (video_cfg_set_key(out, line, eq + 1) != 0) bad++;
    }
    return bad;
}

int video_cfg_format(const video_cfg_t *c, char *out, size_t cap)
{
    video_cfg_t d;
    video_cfg_defaults(&d);
    size_t n = 0;
    if (!cap) return -1;
    out[0] = '\0';
#define EMIT(name, field) \
    if (c->field != d.field) { \
        int w = snprintf(out + n, cap - n, name "=%d\n", c->field); \
        if (w < 0 || (size_t)w >= cap - n) return -1; \
        n += (size_t)w; \
    }
    EMIT("bitrate", bitrate)
    EMIT("fps", fps)
    EMIT("gop", gop_s)
#undef EMIT
    return (int)n;
}

int video_cfg_read(video_cfg_t *out)
{
    char buf[VIDEO_CFG_MAX + 2];
    video_cfg_defaults(out);
    int fd = open(VIDEO_CFG_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); /* a FIFO must not hang the start */
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        printf("[video] settings file unreadable (%s): defaults\n", strerror(errno));
        return -1;
    }
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || n > VIDEO_CFG_MAX) {
        printf("[video] settings file unreadable or too large: defaults\n");
        return -1;
    }
    int bad = video_cfg_parse(buf, (size_t)n, out);
    if (bad) printf("[video] settings file: %d invalid line(s) ignored (valid keys: bitrate, fps, gop)\n", bad);
    return 0;
}

void video_cfg_load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    video_cfg_t c;
    video_cfg_read(&c);
    g_active = c;
    printf("[video] settings: bitrate=%d kbps fps=%d gop=%d s (%d frames)%s\n", g_active.bitrate, g_active.fps,
           g_active.gop_s, g_active.gop_s * g_active.fps,
           (g_active.bitrate == ENC_KBPS && g_active.fps == ENC_FPS && g_active.gop_s == VIDEO_GOP_S_DEF) ? " (defaults)" : "");
}

const video_cfg_t *video_cfg_active(void) { return &g_active; }
int video_fps(void) { return g_active.fps; }
int video_kbps(void) { return g_active.bitrate; }
int video_gop_frames(void) { return g_active.gop_s * g_active.fps; }
int video_day_vts(void)
{
    switch (g_active.fps) { /* 34260 / fps; 10 and 15 are the stock values */
    case 10: return 3426;
    case 20: return 1713;
    case 25: return 1370;
    default: return 2284;
    }
}

void video_cfg_reset_for_test(void)
{
    g_loaded = 0;
    video_cfg_defaults(&g_active);
}
