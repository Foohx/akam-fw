/* See akam2k_store.h. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "akam2k_store.h"

int a2k_store_read(const char *path, char *buf, size_t cap)
{
    struct stat st;
    size_t n;
    FILE *f;
    if (!cap)
        return -1;
    buf[0] = '\0';
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    if (n == cap - 1 && fgetc(f) != EOF) {
        fclose(f);
        buf[0] = '\0';
        return -2;
    }
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static volatile unsigned g_store_writes;
unsigned a2k_store_writes(void) { return g_store_writes; }

int a2k_store_write(const char *path, const char *text, size_t len)
{
    return a2k_store_write_mode(path, text, len, 0);
}

int a2k_store_write_mode(const char *path, const char *text, size_t len, unsigned mode)
{
    char tmp[200], old[A2K_STORE_MAX + 1];
    FILE *f;
    int n = a2k_store_read(path, old, sizeof old);
    if (n >= 0 && (size_t)n == len && memcmp(old, text, len) == 0)
        return 0;
    if (len > A2K_STORE_MAX || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;
    f = fopen(tmp, "w");
    if (!f && errno == ENOENT) { /* the directory of the store is missing (first run): make it, one level, then retry once */
        char dir[200];
        char *sl;
        snprintf(dir, sizeof dir, "%s", path);
        sl = strrchr(dir, '/');
        if (sl && sl != dir) {
            *sl = '\0';
            if (mkdir(dir, 0755) == 0 || errno == EEXIST)
                f = fopen(tmp, "w");
        }
    }
    if (!f)
        return -1;
    if (mode && fchmod(fileno(f), (mode_t)mode) != 0) { /* before any byte is written: a secret never sits in a wider file */
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fwrite(text, 1, len, f) != len || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    __atomic_add_fetch(&g_store_writes, 1u, __ATOMIC_RELAXED);
    return 0;
}
