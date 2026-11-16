/* hal_ptz driver for Linux: the motor node (descriptor path), ioctl, nanosleep and the position / conf files. The ONLY hal_ptz
 * file that touches the system: the node is opened read-write once, every file is replaced atomically (temp file + rename, nothing
 * is created in a directory that does not exist) and read whole. The request numbers are the engine's (hal_ptz.c). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "hal_ptz.h"

static const char *g_dev;

static int d_open(void) { return open(g_dev, O_RDWR); }
static int d_ioctl(int fd, unsigned long req, void *arg) { return ioctl(fd, req, arg); }

static void d_sleep(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

static int d_load(const char *path, char *buf, unsigned cap)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f) || !feof(f)) { /* an error or a file larger than the buffer: fail closed */
        int e = ferror(f) ? EIO : EFBIG;
        fclose(f);
        errno = e;
        return -1;
    }
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static int d_save(const char *path, const char *data)
{
    char tmp[128];
    FILE *f;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    if (fputs(data, f) == EOF || fflush(f) == EOF || fsync(fileno(f)) < 0 || fclose(f) == EOF) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void d_log(const char *line)
{
    fprintf(stderr, "%s\n", line);
}

const hal_ptz_ops_t *hal_ptz_linux_ops(const char *dev)
{
    static const hal_ptz_ops_t ops = { d_open, d_ioctl, d_sleep, d_load, d_save, d_log, NULL, NULL, NULL, NULL };
    g_dev = dev;
    return &ops;
}
