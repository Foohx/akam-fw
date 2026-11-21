/* IR-cut filter and IR LED of the Rotatable 2K, the switch sequence exactly as the stock yy_cam does it (RE, see
 * stock-firmware-reference.md "Day/night and image"):
 *
 *   day  (filter in)      pin 24 high, pin 25 low, 300 ms, pin 24 low
 *   night (filter out)    pin 24 low, pin 25 high, 300 ms, pin 25 low
 *
 * Both coil pins are low before and after every sequence: the coil is NEVER left energised. The two pins are never high
 * together. The IR LED (pin 48, active high) is independent. Which pin means which filter position is the stock's
 * mapping (night_to_day pulses 24, day_to_night pulses 25: HIGH from the code, UNVERIFIED on the camera: a wrong mapping
 * shows as colour pictures with IR at night; the fix is the swap of the two constants below).
 *
 * The pin access goes through ops so that the host unit test can record the sequence and its timing. */
#ifndef AKAM2K_IRCUT_H
#define AKAM2K_IRCUT_H

#include "pin.h"

#define A2K_IRCUT_PULSE_MS 300 /* the stock pulse width (yy_ircut_init passes 300) */
#define A2K_IRCUT_DAY_PIN A2K_PIN_IRCUT_P
#define A2K_IRCUT_NIGHT_PIN A2K_PIN_IRCUT_M

typedef struct {
    int (*claim)(int pin, int level); /* a2k_pin_open: export, output, first level */
    int (*set)(int pin, int level);  /* a2k_pin_set */
    void (*sleep_ms)(unsigned ms);
} a2k_ircut_ops_t;

/* Opens the three pins, all low (coil released, LED off). 0, or -1 (the first failure is returned, the pins already opened
 * stay low). */
int a2k_ircut_init(void);
/* One filter move: day = 1 filter in, 0 filter out. 0, or -1 when a write failed (both coil pins were driven low anyway). */
int a2k_ircut_pulse(int day);
/* IR LED. 0 or -1. */
int a2k_ircut_led(int on);
/* Both coil pins low (best effort, twice). 0 or -1. */
int a2k_ircut_release(void);

/* TEST ONLY: replaces the pin access and the sleep (NULL = the real ones). */
void a2k_ircut_test_ops(const a2k_ircut_ops_t *ops);

#endif
