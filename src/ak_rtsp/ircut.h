#pragma once

/* Initialize the IR cut filter and IR LED GPIOs.
 *
 * Mirrors ht_night_mode_init → FUN_000d57f4 → FUN_000d4878 from anyka_ipc.
 * Exports GPIO 65/66 (ircut solenoid) and GPIO 70 (IR LED), sets them to
 * day mode (visible-light pass), and fires the reset pulse once per boot
 * to home the solenoid.  Produces the characteristic two-click sound. */
int ircut_init(void);

/* Switch the IR-cut filter + IR LED to day (night=0) or night (night=1)
 * position at runtime. Called by night.c's auto day/night monitor after
 * ircut_init() has already run. */
void ircut_switch(int night);

/* night.ir_led policy: allowed=1 (auto) lets the IR LED follow night mode,
 * allowed=0 (off) keeps it dark in night mode too (the filter still moves).
 * Takes effect at once, also in the middle of a night. Thread safe; may be
 * called before ircut_init(). */
void ircut_set_ir_led_allowed(int allowed);

/* 1 when the IR LEDs light in night mode (night.ir_led=auto), 0 when the policy keeps them dark. */
int ircut_ir_led_allowed(void);

/* Ambient light probe of night.c: off=1 darkens the IR LEDs while the filter stays in the night position, off=0
 * gives them back. ircut_switch() ends any probe by itself. No effect in day mode. */
void ircut_ir_led_probe(int off);
