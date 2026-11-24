/* The setting rows image.flip, image.mirror and image.ceiling, the same on every camera (Rotatable 2K and LSC Indoor 1080p).
 * Pure and header-only (static inline: no object to link, no state, no I/O); the cameras keep the state, the ISP call and the
 * persistence, this header keeps what must never differ between them: the names, the meaning and the file format.
 *
 *   image.flip     0|1  vertical    (the picture upside down)
 *   image.mirror   0|1  horizontal  (left and right swapped)
 *   image.ceiling  0|1  both at once (a camera mounted upside down: a 180 degree rotation); reads 1 only while both are on
 *
 * Saved text (the 1080p: /etc/config/akam/orient; the 2K keeps the two rows in its registry file instead):
 *   flip=0|1 and mirror=0|1, one per line, LF. Both off = no file at all (nothing is written by a camera that is never flipped). */
#ifndef AKAM_ORIENT_ROWS_H
#define AKAM_ORIENT_ROWS_H
#include <stdio.h>
#include <string.h>

#define ORIENT_FILE_MAX 64

/* 1 when key is one of the three rows. */
static inline int orient_row_is(const char *key)
{
    return key && (!strcmp(key, "image.flip") || !strcmp(key, "image.mirror") || !strcmp(key, "image.ceiling"));
}

/* The value (0 or 1) of the row for the current state, or -1 for another key. */
static inline int orient_row_value(const char *key, int flip, int mirror)
{
    if (!key) return -1;
    if (!strcmp(key, "image.flip")) return flip ? 1 : 0;
    if (!strcmp(key, "image.mirror")) return mirror ? 1 : 0;
    if (!strcmp(key, "image.ceiling")) return (flip && mirror) ? 1 : 0;
    return -1;
}

/* The state after writing v (0 or 1) to the row: 0 and *nf / *nm set, or -1 (not a row, or v not 0 / 1). */
static inline int orient_row_next(const char *key, int v, int flip, int mirror, int *nf, int *nm)
{
    if (!key || (v != 0 && v != 1)) return -1;
    *nf = flip ? 1 : 0;
    *nm = mirror ? 1 : 0;
    if (!strcmp(key, "image.flip")) *nf = v;
    else if (!strcmp(key, "image.mirror")) *nm = v;
    else if (!strcmp(key, "image.ceiling")) *nf = *nm = v;
    else return -1;
    return 0;
}

/* Parses the saved text over (0, 0). Strict: only the two keys, each at most once, values 0 or 1, LF or CRLF, blank lines allowed.
 * 0 when valid (*flip, *mirror set), else -1 (both left at 0: a bad file means "not flipped", the safe picture). */
static inline int orient_file_parse(const char *text, size_t len, int *flip, int *mirror)
{
    int f = 0, m = 0, seen_f = 0, seen_m = 0;
    size_t i = 0;
    *flip = *mirror = 0;
    if (len > ORIENT_FILE_MAX) return -1;
    while (i < len) {
        size_t s = i, e;
        while (i < len && text[i] != '\n') i++;
        e = i;
        if (i < len) i++;
        if (e > s && text[e - 1] == '\r') e--;
        if (e == s) continue;
        if (e - s == 6 && !memcmp(text + s, "flip=", 5) && (text[s + 5] == '0' || text[s + 5] == '1')) {
            if (seen_f) return -1;
            seen_f = 1;
            f = text[s + 5] - '0';
        } else if (e - s == 8 && !memcmp(text + s, "mirror=", 7) && (text[s + 7] == '0' || text[s + 7] == '1')) {
            if (seen_m) return -1;
            seen_m = 1;
            m = text[s + 7] - '0';
        } else {
            return -1;
        }
    }
    *flip = f;
    *mirror = m;
    return 0;
}

/* The saved text of a state: "" for both off (the file is then removed), else both lines. Length, or -1 when cap is too small. */
static inline int orient_file_format(int flip, int mirror, char *out, size_t cap)
{
    int n;
    if (!flip && !mirror) {
        if (cap < 1) return -1;
        out[0] = '\0';
        return 0;
    }
    n = snprintf(out, cap, "flip=%d\nmirror=%d\n", flip ? 1 : 0, mirror ? 1 : 0);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
#endif
