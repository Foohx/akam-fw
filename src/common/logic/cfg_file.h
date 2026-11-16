#pragma once
#include <stddef.h>

/* Atomic write of a small saved-settings file: creates the parent directory (mode 0700), writes `path`.tmp (mode
 * 0600), fsync, rename over `path`. Returns 0, or -1 with errno set (the temporary file is removed). */
int cfg_write_atomic(const char *path, const char *data, size_t len);
