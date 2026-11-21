/* Tiny persistent text store of the runtime settings (/config/akam, jffs2): the files the Home Assistant controls write (zones, tracking
 * areas, video settings). An atomic replace ("<path>.tmp" then rename), skipped when the content is unchanged (flash wear: the writes are user actions,
 * never periodic). This is, with the detection state writer, the snapshot ring and the motor driver, one of the few app files that may
 * rename(): the anti-brick scan in tests/rot2k_overlay_test.sh lists it. */
#ifndef AKAM2K_STORE_H
#define AKAM2K_STORE_H
#include <stddef.h>

#define A2K_STORE_DIR "/config/akam"
#define A2K_STORE_ZONES A2K_STORE_DIR "/zones"
#define A2K_STORE_TRACK A2K_STORE_DIR "/track_areas"
#define A2K_STORE_VIDEO A2K_STORE_DIR "/video"
#define A2K_STORE_MAX 4096

/* Reads the file: length (0 = empty file), -1 absent or unreadable, -2 too big. buf is always NUL terminated. */
int a2k_store_read(const char *path, char *buf, size_t cap);
/* Atomically replaces the file with text (len bytes). Unchanged content is not rewritten. 0, or -1 (the old file stays). */
/* Number of files really written (a write of identical content is skipped) since the start: the flash wear counter, /state sys.flash_writes_store. */
unsigned a2k_store_writes(void);
int a2k_store_write(const char *path, const char *text, size_t len);
/* The same, with the temp file made owner-only first (mode e.g. 0600: the API key file). mode 0 = as a2k_store_write. */
int a2k_store_write_mode(const char *path, const char *text, size_t len, unsigned mode);

#endif
