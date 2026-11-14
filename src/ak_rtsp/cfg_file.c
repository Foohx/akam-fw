#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cfg_file.h"

int cfg_write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[512], dir[512];
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
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int e = errno; close(fd); unlink(tmp); errno = e; return -1; }
        off += (size_t)n;
    }
    if (fsync(fd) < 0 || close(fd) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}
