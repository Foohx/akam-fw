/* light_logic: pure light policy (no I/O, no clock, no model knowledge). It talks to the hardware through the injected
 * hal_light_ops_t and takes the time as an argument, so a host test drives it with a fake driver and a fake clock.
 *
 * Modes: off, on, blink fast (1 Hz: 500 ms on / 500 ms off), blink slow (0.5 Hz: 1000 ms on / 1000 ms off). A blink starts
 * "on" at the call that sets it; call light_tick() regularly (every <= 100 ms is plenty) to advance it. */
#ifndef LIGHT_LOGIC_H
#define LIGHT_LOGIC_H

#include "hal_light.h"

#define LIGHT_MAX 8
#define LIGHT_FAST_HALF_MS 500
#define LIGHT_SLOW_HALF_MS 1000

enum light_mode { LIGHT_OFF, LIGHT_ON, LIGHT_BLINK_FAST, LIGHT_BLINK_SLOW };

typedef struct light {
    const hal_light_desc_t *desc;
    const hal_light_ops_t *ops;
    int mode[LIGHT_MAX];
    int level[LIGHT_MAX];     /* last level written: 0, 1, or -1 not written yet */
    long start_ms[LIGHT_MAX]; /* when the current blink began */
} light_t;

/* 0, or -1 when the descriptor is NULL, empty, longer than LIGHT_MAX or the exclusion names an unknown emitter. */
int light_init(light_t *l, const hal_light_desc_t *desc, const hal_light_ops_t *ops);
/* The index of an emitter by name, or -1. */
int light_find(const light_t *l, const char *name);
/* "off" "on" "blink-fast" "blink-slow" -> mode; 0, or -1 for any other word. */
int light_parse_mode(const char *s, int *mode);
/* Sets an emitter. Turning one of the two exclusive emitters on or blinking forces the other off first
 * (only when this engine had it on: an emitter it never lit is never written). 0, or -1 (unknown
 * name, or the driver failed: the mode is then left as it was). */
int light_set(light_t *l, const char *name, int mode, long now_ms);
/* Advances the blinking emitters; returns the number of driver errors (0 normally). */
int light_tick(light_t *l, long now_ms);

#endif
