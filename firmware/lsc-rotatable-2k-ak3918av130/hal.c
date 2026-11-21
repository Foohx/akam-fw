/* 2K binding: the hal_light descriptor. Hardware facts (verified on the camera, docs/porting/lsc-rotatable-2k/hal-light.md): the
 * 2K lights are the IR LEDs (gpio48, owned by day/night) and the front blue status LED (gpio56, on at boot), both active high.
 * The other stock light pins are NOT lights: driving them stops the sensor frames. No exclusion pair. The pin numbers are the
 * macros of pin.h, whose whitelist is what the driver can reach. */
#include "hal.h"
#include "pin.h"

static const hal_light_emitter_t g_emitters[] = {
    { "ir", LIGHT_GPIO, A2K_PIN_IRLED, 1, 0 },
    { "blue", LIGHT_GPIO, A2K_PIN_BLUE, 1, 0 },
};

const hal_light_desc_t a2k_light_desc = { g_emitters, (int)(sizeof g_emitters / sizeof g_emitters[0]), 0, 0 };

static const hal_input_t g_inputs[] = {
    { "button", A2K_PIN_BUTTON, 1, A2K_BUTTON_DEBOUNCE_MS },
};

const hal_input_desc_t a2k_input_desc = { g_inputs, (int)(sizeof g_inputs / sizeof g_inputs[0]) };
