/* hal_input: the portable digital-input interface (docs/hal-architecture.md), first user: the push button. A model lists its inputs in a
 * descriptor {name, line, polarity, debounce} and picks a driver; the logic (the debouncer below, button_logic.c) and the apps only name
 * an input ("button"). No pin number or sysfs path appears anywhere but in the descriptor of the model binding (the 2K: akam2k_hal.c).
 * Inputs are READ ONLY: no call of this interface ever writes a line.
 *
 * The file has no model name and no akam2k_ prefix on purpose: it moves to src/common unchanged in a later phase-B step. */
#ifndef HAL_INPUT_H
#define HAL_INPUT_H

#include <stdint.h>

typedef struct hal_input {
    const char *name;     /* "button", ... */
    int gpio;             /* the line number (descriptor field only) */
    int active_low;       /* 1: a raw 0 means "pressed" */
    int debounce_ms;      /* the raw level must hold this long before it is believed */
} hal_input_t;

typedef struct hal_input_desc {
    const hal_input_t *inputs;
    int count;
} hal_input_desc_t;

/* A driver. setup() prepares the line, 0 or -1. read() returns the RAW level, 1 or 0, or -1 when it could not be read. */
typedef struct hal_input_ops {
    int (*setup)(const hal_input_t *in);
    int (*read)(const hal_input_t *in);
} hal_input_ops_t;

/* The generic sysfs driver (hal_input_sysfs.c): through the pin helper (whitelisted input line, read-only value file). */
extern const hal_input_ops_t hal_input_sysfs_ops;

const hal_input_t *hal_input_find(const hal_input_desc_t *d, const char *name);

/* Debouncer: pure, fed with raw samples and a monotonic clock. */
typedef struct {
    int init;             /* a first sample was seen */
    int raw;              /* last raw level */
    int64_t raw_since_ms; /* when the raw level last changed */
    int pressed;          /* the debounced logical state (polarity applied), 0 until believed */
} hal_deb_t;

void hal_deb_init(hal_deb_t *d);
/* Feeds one raw sample (1 / 0; -1 = read failed: ignored, the state is kept). Returns the debounced logical state (1 pressed). The first
 * sample is believed at once (a button already held at start reads "pressed", but the logic above ignores a press that began before
 * it looked: see button_logic.h). */
int hal_deb_update(hal_deb_t *d, const hal_input_t *in, int64_t now_ms, int raw);

#endif
