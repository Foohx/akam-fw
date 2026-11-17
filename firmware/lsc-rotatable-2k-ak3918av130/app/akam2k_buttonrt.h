/* The runtime of the programmable button of the Rotatable 2K own app (specs/control-api.md, docs/porting/lsc-rotatable-2k/button-and-zones.md): the live
 * bindings and modes (button_cfg.h), their persistence, the HTTP endpoints, and the glue that feeds the pure policy (button_logic.h) and turns a
 * finished gesture into actions, an API event and an MQTT message. It knows no hardware: everything it does to the camera goes through the ops table
 * (the real one is in akam2k_button.c; the HTTP harness has a fake one), so a host test drives it with a fake line and a fake clock.
 *
 * Persistence: /config/akam/button (the bindings, one JSON text) and /config/akam/modes (the modes), each written ONLY when a POST / DELETE really
 * changes it (atomic through the store, an identical text is not written, at most A2K_BTN_WRITE_BURST writes at once then one per A2K_BTN_WRITE_GAP_S);
 * pressing the button never writes. The writes are counted: /state sys.flash_writes_button.
 *
 * Threads: the button thread calls a2k_btn_step; the HTTP thread the endpoints; the settings worker a2k_btn_mode_get. One mutex for the data, one to
 * serialise edits (the flash write happens outside the first). */
#ifndef AKAM2K_BUTTONRT_H
#define AKAM2K_BUTTONRT_H
#include <stddef.h>
#include <stdint.h>

#include "button_cfg.h"
#include "button_logic.h"

#define A2K_STORE_BUTTON "/config/akam/button"
#define A2K_STORE_MODES "/config/akam/modes"
#define A2K_BTN_WRITE_BURST 8
#define A2K_BTN_WRITE_GAP_S 10
#define A2K_BTN_EVT_MAX 144 /* the longest event JSON: A2K_EVT_DATA of api_events.h (an assert in the unit test keeps them equal) */

typedef struct {
    void *user;
    int (*run)(void *user, int kind, const char *arg);                                     /* bc_ops_t.run */
    int (*apply)(void *user, int n, const char *const *keys, const char *const *vals);     /* bc_ops_t.apply: the registry, all or nothing */
    int (*locked)(void *user);                                                             /* the PTZ calibration: gestures are not dispatched */
    void (*led)(void *user, int cue);                                                      /* BTN_LED_* */
    void (*event)(void *user, const char *json);                                           /* the API event "button" */
    void (*mqtt)(void *user, const char *gesture, const char *json);                       /* publish on <base>/button */
    void (*changed)(void *user);                                                           /* the bindings changed: MQTT announces the triggers again */
    int (*key_check)(const char *key, const char *val);                                    /* mode pair rule (the registry), NULL = syntax only */
    int64_t (*now_ms)(void *user);                                                         /* monotonic */
} a2k_btn_ops_t;

/* Loads the two files (invalid ones are ignored and logged: the defaults apply; nothing is rewritten) and resets the engine. long_ms = the default
 * long press (button_long_ms of akam2k.conf) when there is no file. 0. */
int a2k_btn_init(const a2k_btn_ops_t *ops, int long_ms);

/* One sample of the line (raw = -1: read failed) at monotonic now_ms; `in` is the input descriptor (polarity). Dispatches a finished gesture. */
int a2k_btn_step(const hal_input_t *in, int64_t now_ms, int raw);

/* ---- state ---- */
void a2k_btn_last_gesture(char *out, size_t cap);   /* "none" until the first gesture (a2k_btn_step); the state key button.last_gesture */
unsigned a2k_btn_writes(void);                      /* flash writes of the two files since the start (sys.flash_writes_button) */
/* The configured gestures (canonical names, binding order) for the MQTT device triggers: the count, at most BC_MAX_BIND. */
int a2k_btn_gestures(char names[][BG_NAME_MAX]);
int a2k_btn_led_ack(void);
/* A copy of the mode (0), or -1 when there is none of that name. */
int a2k_btn_mode_get(const char *name, bc_mode_t *out);

/* ---- HTTP (the API thread): sub = the path after "/akam/v1/": button, modes, modes/<name>. ---- */
int a2k_btn_path(const char *sub);
/* Returns the HTTP status (reply JSON in out); *retry_s is set on 429. method: GET, POST, DELETE. */
int a2k_btn_http(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s);

/* Where the two files live (NULL = the defaults above): the host tests point them at a temporary directory. Before a2k_btn_init. */
void a2k_btn_set_paths(const char *button, const char *modes);
void a2k_btn_test_reset(void);  /* forgets everything (the files stay) */

#endif
