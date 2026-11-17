/* hal_light: the portable light interface (docs/hal-architecture.md). A model describes its emitters in a descriptor and picks
 * a driver; the logic (light_logic.c) and the apps only ever name an emitter ("ir", "blue"). No GPIO number and no
 * sysfs path appears anywhere but in the descriptor of the model binding (the 2K: akam2k_hal.c).
 *
 * The file has no model name and no akam2k_ prefix on purpose: it moves to src/common unchanged in a later phase-B step. */
#ifndef HAL_LIGHT_H
#define HAL_LIGHT_H

enum hal_light_kind {
    LIGHT_GPIO,     /* a raw GPIO line: `gpio` + `active_high` */
    LIGHT_LEDCLASS  /* a kernel LED-class device: <led_dir>/brightness, trigger set to "none" once */
};

typedef struct hal_light_emitter {
    const char *name;     /* "ir", "blue", ... */
    enum hal_light_kind kind;
    int gpio;             /* LIGHT_GPIO only */
    int active_high;      /* 1: on writes 1, 0: on writes 0 (both kinds) */
    const char *led_dir;  /* LIGHT_LEDCLASS only, e.g. "/sys/class/leds/state_led" */
} hal_light_emitter_t;

typedef struct hal_light_desc {
    const hal_light_emitter_t *emitters;
    int count;
    /* optional mutual exclusion (the stock: white on forces IR off and the other way round); NULL = none */
    const char *excl_a, *excl_b;
} hal_light_desc_t;

/* A driver. set() returns 0, or -1 when the emitter could not be driven. */
typedef struct hal_light_ops {
    int (*set)(const hal_light_emitter_t *e, int on);
} hal_light_ops_t;

/* The generic sysfs driver (hal_light_sysfs.c): GPIO through the pin helper, LED class through brightness/trigger. */
extern const hal_light_ops_t hal_light_sysfs_ops;

#endif
