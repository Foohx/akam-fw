/* LED blink patterns (shared by the Rotatable 2K and the 1080p): the text grammar, its canonical form and the level of a
 * pattern at a time. Pure: no I/O, no clock, no model knowledge, no allocation.
 *
 * Pattern: "on" | "off" | "N:on_ms:off_ms:pause_ms": N blinks (on_ms lit, off_ms dark), then pause_ms dark, repeat. Integers only,
 * N 1..10, on_ms and off_ms 50..60000, pause_ms 0 or 50..60000, whole cycle <= 120000 ms. Example "3:250:250:5000".
 * The a2k_ prefix is the 2K history of the file (it was akam2k_leds.c); only the file moved, no symbol was renamed. */
#ifndef AKAM_LED_PAT_H
#define AKAM_LED_PAT_H
#include <stddef.h>

#define A2K_PAT_MAX 32 /* longest pattern text with the NUL */

enum { A2K_PAT_ON, A2K_PAT_OFF, A2K_PAT_BLINK };
typedef struct {
    int kind, n, on_ms, off_ms, pause_ms;
} a2k_pat_t;

/* 0 and *p filled, -1 for anything not exactly in the syntax or out of bounds (*p untouched). */
int a2k_pat_parse(const char *s, a2k_pat_t *p);
/* Canonical text ("on", "off", "3:250:250:5000"). */
void a2k_pat_format(const a2k_pat_t *p, char *out, size_t cap);
/* 1 lit / 0 dark, t_ms since the pattern started (>= 0). */
int a2k_pat_level(const a2k_pat_t *p, long t_ms);
/* Milliseconds from t_ms (>= 0) until the level of the pattern may change next: 0 < n <= the cycle; -1 for "on" and "off" (never). */
long a2k_pat_next_ms(const a2k_pat_t *p, long t_ms);

#endif
