/* Button gesture engine (portable, PURE: no I/O, no clock of its own, no model name; host-tested by tests/rot2k_button_unit.c). Fed with the
 * DEBOUNCED press / release edges of one button and a monotonic clock, it turns an interaction into exactly ONE gesture: a sequence of up to
 * BG_MAX_SEQ elements, each one a press classified by how long it was held:
 *   S  short       held < short_max_ms
 *   L  long        held >= long_ms
 *   V  very long   held >= very_long_ms (only when some bound sequence contains a V; otherwise such a hold is an L)
 *   a hold between short_max_ms and long_ms is a dead zone: the whole interaction is dropped (an unsure press never does anything).
 * The sequence ends when the next press does not come within gap_ms after a release (wait-for-gap), or at once when waiting is pointless:
 * nothing bound extends it (longest match) or it already has BG_MAX_SEQ elements. So with only "short" and "long" bound a short press is
 * reported at its release, with no delay at all; bind "double" and a single short press is reported gap_ms later.
 * Names: short = S, double = S S, triple = S S S, long = L, very_long = V; any other sequence is its tokens joined by spaces ("S S L"). */
#ifndef BUTTON_GESTURE_H
#define BUTTON_GESTURE_H

#include <stddef.h>
#include <stdint.h>

#define BG_MAX_SEQ 6
#define BG_MAX_BOUND 16
#define BG_NAME_MAX 16 /* longest canonical name incl. NUL: "S S S S S S" and "very_long" */

enum { BG_S = 1, BG_L, BG_V };

typedef struct {
    uint8_t n;
    uint8_t el[BG_MAX_SEQ]; /* BG_S, BG_L, BG_V */
} bg_seq_t;

typedef struct {
    int debounce_ms;   /* 10..200 (the hal_input debounce; the caller applies it) */
    int short_max_ms;  /* 200..1500 */
    int gap_ms;        /* 150..1000: the longest pause between two presses of one gesture */
    int long_ms;       /* 1500..10000 */
    int very_long_ms;  /* long_ms + 500 .. 30000 */
} bg_timing_t;

void bg_timing_defaults(bg_timing_t *t);       /* 50, 1000, 400, 2000, 5000 */
int bg_timing_ok(const bg_timing_t *t);        /* 1 when every value is in range and long_ms > short_max_ms */

/* "short" "double" "triple" "long" "very_long", or 1 to 6 tokens S / L / V separated by one space or '_' (case free). 0, or -1. */
int bg_seq_parse(const char *s, bg_seq_t *out);
/* The canonical name (aliases first, else tokens joined by a space). Length, or -1 when cap is too small. */
int bg_seq_name(const bg_seq_t *s, char *out, size_t cap);
int bg_seq_eq(const bg_seq_t *a, const bg_seq_t *b);
int bg_seq_has_v(const bg_seq_t *s);

typedef struct {
    bg_timing_t t;
    bg_seq_t bound[BG_MAX_BOUND];
    int nbound, use_v;
    bg_seq_t cur;     /* the elements so far (cur.n == 0: no interaction) */
    int down;
    int64_t t_down, t_up;
} bg_t;

enum { BG_NONE = 0, BG_GESTURE, BG_DEAD };

void bg_init(bg_t *g, const bg_timing_t *t);
/* The bound sequences (the bindings): they only decide whether to wait for more presses and whether V exists. At most BG_MAX_BOUND (the rest is ignored). */
void bg_set_bound(bg_t *g, const bg_seq_t *list, int n);
int bg_is_bound(const bg_t *g, const bg_seq_t *s);
void bg_press(bg_t *g, int64_t now_ms);
/* A release: BG_GESTURE (*out is the finished gesture), BG_DEAD (a dead-zone hold: the interaction was dropped) or BG_NONE (waiting for more). */
int bg_release(bg_t *g, int64_t now_ms, bg_seq_t *out);
/* The gap expired with the button up: BG_GESTURE or BG_NONE. Call at every sample while the button is up. */
int bg_tick(bg_t *g, int64_t now_ms, bg_seq_t *out);

#endif
