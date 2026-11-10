#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "isp_tune.h"

int isp_tune_parse(const char *s, int *out)
{
    char *end;
    if (!s || !*s) return -1;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < ISP_TUNE_MIN || v > ISP_TUNE_MAX) return -1;
    *out = (int)v;
    return 0;
}

int isp_tune_load(const char *path, isp_tune_t *t)
{
    char buf[512];
    memset(t, 0, sizeof(*t));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        size_t vl = strlen(eq + 1);
        if (vl && eq[vl] == '\r') eq[vl] = '\0';
        int v;
        if (isp_tune_parse(eq + 1, &v) < 0) continue;
        if      (!strcmp(line, "brightness")) t->brightness = v;
        else if (!strcmp(line, "contrast"))   t->contrast = v;
        else if (!strcmp(line, "saturation")) t->saturation = v;
        else if (!strcmp(line, "sharpness"))  t->sharpness = v;
    }
    return 1;
}

int isp_tune_save(const char *path, const isp_tune_t *t)
{
    if (!t->brightness && !t->contrast && !t->saturation && !t->sharpness) {
        if (unlink(path) < 0 && errno != ENOENT) return -1;
        return 0;
    }
    char out[160], tmp[512], dir[512];
    int w = snprintf(out, sizeof(out), "brightness=%d\ncontrast=%d\nsaturation=%d\nsharpness=%d\n",
                     t->brightness, t->contrast, t->saturation, t->sharpness);
    if (w < 0 || (size_t)w >= sizeof(out)) { errno = ENOSPC; return -1; }
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < (size_t)w) {
        ssize_t n = write(fd, out + off, (size_t)w - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int e = errno; close(fd); unlink(tmp); errno = e; return -1; }
        off += (size_t)n;
    }
    if (fsync(fd) < 0) { int e = errno; close(fd); unlink(tmp); errno = e; return -1; }
    if (close(fd) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}
