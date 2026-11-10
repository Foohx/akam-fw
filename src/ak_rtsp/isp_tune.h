#pragma once

/* Saved picture settings (isp.brightness / contrast / saturation / sharpness, each -50..50, 0 = neutral).
 * File: key=value lines in ISP_TUNE_FILE, mode 0600, written atomically (tmp + fsync + rename) and only when
 * a value is changed over 8091; a file that is absent means defaults (all 0). Values outside the range, or
 * that are not numbers, are ignored on load. control.c applies them once the ISP is up. */

#ifndef ISP_TUNE_FILE
#define ISP_TUNE_FILE "/etc/config/akam/picture"
#endif

#define ISP_TUNE_MIN (-50)
#define ISP_TUNE_MAX 50

typedef struct {
    int brightness;
    int contrast;
    int saturation;
    int sharpness;
} isp_tune_t;

/* Fills `t` (defaults first, then every valid line of `path`). Returns 1 when the file existed, else 0. */
int isp_tune_load(const char *path, isp_tune_t *t);

/* Writes the file atomically; when every value is 0 it removes the file instead (same as the defaults).
 * Returns 0, or -1 with errno set. */
int isp_tune_save(const char *path, const isp_tune_t *t);

/* Parses a whole decimal number in range; 0 and *out set, else -1. */
int isp_tune_parse(const char *s, int *out);
