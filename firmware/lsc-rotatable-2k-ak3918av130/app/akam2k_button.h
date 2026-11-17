/* The front push button of the Rotatable 2K own app: one small thread (20 ms poll of the hal_input line "button", never touches capture, the
 * network or the NPU) running the portable policy of button_logic.h / button_gesture.h and the runtime of akam2k_buttonrt.h. Every press pattern
 * (gesture: short, double, triple, long, very_long, or a sequence like "S S L") is bound to up to four whitelisted actions through
 * GET / POST /akam/v1/button; every gesture is an API event `button` and an MQTT publish on <base>/button. The DEFAULT bindings are the factory
 * behaviour of this button:
 *   short press (< 1 s)   the event + the blue LED double blink (MQTT Home Assistant `event` entity "Button", event_type press)
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
#define A2K_BUTTON_VLONG_PATTERN "3:150:150:0"  /* ... at the very long threshold (only when a gesture uses it): triple blink ... */
#define A2K_BUTTON_LONG_BLINK_MS 600
#define A2K_BUTTON_SOLID_PATTERN "1:60000:50:0" /* ... then solid until release */
#define A2K_BUTTON_LED_ACTION_MS 3000           /* the "led pattern" action: how long the pattern shows */

/* Loads the bindings and modes (before a2k_mqtt_start: the MQTT triggers are made from them). long_ms: BTN_LONG_MIN_MS..BTN_LONG_MAX_MS (button_logic.h),
 * else the default 2000; it is the long press of the default bindings. */
void a2k_button_prepare(int long_ms);
/* 0 started, -1 not (logged). Calls a2k_button_prepare when that was not done. */
int a2k_button_start(int long_ms);
void a2k_button_stop(void); /* before a2k_night_stop (which closes the pin files) */

#endif
