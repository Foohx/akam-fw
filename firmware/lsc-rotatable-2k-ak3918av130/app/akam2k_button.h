/* The front push button of the Rotatable 2K own app: one small thread (20 ms poll of the hal_input line "button", never touches capture, the
 * network or the NPU) running the portable policy of button_logic.h:
 *   short press (< 1 s)   MQTT Home Assistant `event` entity "Button" (event_type press) + the blue LED double blink
 *   long press (>= button_long_ms, default 2 s)   a2k_ptz_toggle_park() (parked -> home, anything else -> park); the blue LED blinks fast from
 *                         500 ms of hold ("keep holding"), then at the threshold a double blink and solid until release ("release now")
 *   during the PTZ calibration a press does nothing (LED feedback only).
 * A missing or unreadable line is logged once and the thread ends: the app goes on without the button. */
#ifndef AKAM2K_BUTTON_H
#define AKAM2K_BUTTON_H

#define A2K_BUTTON_POLL_MS 20
#define A2K_BUTTON_ACK_PATTERN "2:150:150:0"   /* the double blink of a short press */
#define A2K_BUTTON_ACK_MS 700
#define A2K_BUTTON_HOLD_PATTERN "1:100:100:0"  /* fast blink while held past the threshold */
#define A2K_BUTTON_HOLD_MAX_MS 15000
#define A2K_BUTTON_LONG_PATTERN "2:150:150:0"   /* confirm at the long threshold: double blink ... */
#define A2K_BUTTON_LONG_BLINK_MS 600
#define A2K_BUTTON_SOLID_PATTERN "1:60000:50:0" /* ... then solid until release */

/* long_ms: BTN_LONG_MIN_MS..BTN_LONG_MAX_MS (button_logic.h), else the default 2000. 0 started, -1 not (logged). */
int a2k_button_start(int long_ms);
void a2k_button_stop(void); /* before a2k_night_stop (which closes the pin files) */

#endif
