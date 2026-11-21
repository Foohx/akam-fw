/* The 2K binding of the portable HAL modules (docs/hal-architecture.md): the descriptors of this model. hal_light and hal_ptz. */
#ifndef AKAM2K_HAL_H
#define AKAM2K_HAL_H

#include "hal_input.h"
#include "hal_light.h"
#include "hal_ptz.h"

/* The lights of the Rotatable 2K: "ir" (gpio 48) and "blue", the front status LED (gpio 56), both active high, no exclusion. gpio 49 / 22
 * are NOT lights. The IR-cut pins 24 / 25 are NOT lights either (a later hal_ircut). */
extern const hal_light_desc_t a2k_light_desc;

/* The inputs of the Rotatable 2K: "button", the front push button (line 71, active low: reads 1 released, 0 pressed; hardware verified),
 * 50 ms debounce. */
#define A2K_BUTTON_DEBOUNCE_MS 50 /* contact bounce of the button: believed after this long (hal_input debounce_ms) */
extern const hal_input_desc_t a2k_input_desc;

/* The pan/tilt motor of the Rotatable 2K (hal_ptz.h); defined in ptz_desc.c. */
extern const hal_ptz_desc_t a2k_ptz_desc;

#endif
