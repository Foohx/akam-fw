/* Minimal bounded JSON for the control API (specs/control-api.md section 9): a writer into a caller buffer and a parser for
 * ONE shape only, a flat object of scalars (the PATCH /settings and POST /actions bodies). No allocation, no recursion.
 * ASCII only (settings values are ASCII); anything else is an error, never a silent conversion. */
#ifndef AKAM_JSON_MIN_H
#define AKAM_JSON_MIN_H
#include <stddef.h>

/* ---- writer: sticky error flag, so a long document needs one check at the end ---- */
typedef struct {
    char *buf;
    size_t cap, len;
    int err; /* set when the buffer was too small */
} jw_t;

void jw_init(jw_t *w, char *buf, size_t cap);
void jw_raw(jw_t *w, const char *s);          /* verbatim (structure, literals) */
void jw_str(jw_t *w, const char *s);          /* "quoted", escapes " \ and every byte < 0x20 or >= 0x7f as \u00XX */
void jw_long(jw_t *w, long v);
void jw_bool(jw_t *w, int v);
int jw_end(jw_t *w);                          /* NUL terminates; the length, or -1 when it overflowed */

/* ---- flat object parser ---- */
#define JFLAT_MAX 32
#define JFLAT_KEY_MAX 48
#define JFLAT_VAL_MAX 128
#define JFLAT_POOL 4096

enum { JF_OK = 0, JF_E_SYNTAX = -1, JF_E_TYPE = -2, JF_E_LIMIT = -3, JF_E_DUP = -4, JF_E_STRING = -5 };

typedef struct {
    size_t n;
    const char *key[JFLAT_MAX];
    const char *val[JFLAT_MAX]; /* canonical text: strings unescaped, true/false -> "1"/"0", integers as written */
    char pool[JFLAT_POOL];
} jflat_t;

/* Parses {"k":v,...}: v = string | integer (no fraction, no exponent) | true | false. null, arrays, objects, duplicate
 * keys, keys over 48 or values over 128 bytes, more than 32 pairs, non-ASCII bytes, trailing text: errors. */
int jflat_parse(jflat_t *j, const char *s, size_t len);

#endif
