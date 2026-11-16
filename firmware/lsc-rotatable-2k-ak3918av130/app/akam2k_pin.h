/* The four output pins of the day/night and light hardware of the Rotatable 2K, driven through sysfs (/sys/class/gpio, no /dev/mem, the
 * same mechanism the boot stub uses for the Wi-Fi enable pin). This is the ONLY app file that may name the GPIO subsystem
 * or open a file by number: it refuses every pin that is not one of the four below (the anti-brick scan in
 * tests/rot2k_overlay_test.sh checks that no other pin number appears in it and that no other app file touches the subsystem).
 *
 *   24 / 25  IR-cut filter coil, two pins: a short pulse on one moves the filter one way, the other pin the other way, and
 *            BOTH must go low again afterwards (an energised coil heats and wears). RE of yy_cam, [ircut] p_no / m_no.
 *   48       IR LED, active high. RE of yy_cam, [light] ir_no / ir_polarity 1.
 *   56       front blue status LED, active high (verified on the camera). Driven by hal_light only.
 *   71       (INPUT only) front push button, read by hal_input only: direction "in", value opened read-only.
 *
 * Everything here is RAM state of the SoC: a reboot puts the pins back to their reset state. Pins are never unexported. */
#ifndef AKAM2K_PIN_H
#define AKAM2K_PIN_H

#define A2K_PIN_IRCUT_P 24
#define A2K_PIN_IRCUT_M 25
#define A2K_PIN_IRLED 48
#define A2K_PIN_BLUE 56
/* The ONE input pin: the front push button (hardware verified: reads 1 released, 0 pressed). It is never written: it has no output slot,
 * a2k_pin_allowed() stays 0 for it and every output call refuses it. Read through a2k_pin_open_in / a2k_pin_read only. */
#define A2K_PIN_BUTTON 71

/* 1 for the four OUTPUT pins above, 0 for any other number (the input pin included: outputs and input are separate lists). */
int a2k_pin_allowed(int pin);
/* Exports the pin when needed, makes it an output at `level` (0 / 1). 0, or -1 (not allowed, or any step failed: logged by
 * the caller). The value file stays open for a2k_pin_set and a2k_pin_emergency_low. */
int a2k_pin_open(int pin, int level);
/* Drives an opened pin. 0 or -1. */
int a2k_pin_set(int pin, int level);
/* 1 for the one input pin (A2K_PIN_BUTTON), 0 for any other number. */
int a2k_pin_input_allowed(int pin);
/* Exports the input pin when needed, makes it an input and opens its value file read-only. 0, or -1 (not an input pin, or any step failed). */
int a2k_pin_open_in(int pin);
/* Raw level of an opened input pin: 1 / 0, or -1 (not opened, read failed). Never writes anything. */
int a2k_pin_read(int pin);
/* Async-signal-safe (pwrite only): drives the two coil pins low. For the crash and timeout handlers. */
void a2k_pin_emergency_low(void);
/* Closes the value files (the pins keep their last level, which the caller has set to the safe one). */
void a2k_pin_close_all(void);

/* TEST ONLY: another sysfs root (a temporary directory laid out like /sys/class/gpio). NULL = the real one. */
void a2k_pin_test_root(const char *root);

#endif
