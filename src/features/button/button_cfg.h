/* Button gesture bindings and named modes (portable, PURE: no I/O, no clock, no hardware; host-tested by tests/rot2k_button_unit.c).
 *
 * The configuration is a list of bindings {gesture -> [actions]} (button_gesture.h names) plus the gesture timing and the LED confirmation switch.
 * Up to BC_MAX_BIND bindings, each with up to BC_MAX_ACT actions. The actions are a WHITELIST, written as text:
 *   none                          nothing (the gesture is still reported: event, MQTT)
 *   ptz.home | ptz.park | ptz.recalibrate | ptz.toggle_park (parked -> home, anything else -> park: the factory long press)
 *   ptz.privacy on|off|toggle     on = park, off = home, toggle = ptz.toggle_park (the camera looks at the wall; the video is withheld
 *                                 while parked only with ptz_privacy=1)
 *   ptz.goto_preset NAME          a patrol preset (a-z first, then a-z 0-9 _)
 *   track on|off|toggle           person tracking
 *   track.lost_policy VALUE       the track.lost_policy setting (the tracker validates the value)
 *   patrol start ROUTE | patrol stop
 *   led pattern P                 a transient LED pattern for a moment: P = on | off | N:on_ms:off_ms:pause_ms | a name (ack blink fast slow)
 *   mode NAME                     apply a named mode (below)
 * NOTHING destructive can be written: no reset, no key or network change, no reboot. The parser accepts exactly this list.
 *
 * A mode is a named set of up to BC_MAX_PAIRS "key=value" settings (at most BC_MAX_MODES modes) that is applied ALL OR NOTHING through the
 * settings registry. Which keys are allowed (writable, not secret, live, not net.* / api.*) is the caller's check hook. */
#ifndef BUTTON_CFG_H
#define BUTTON_CFG_H

#include <stddef.h>
#include <stdint.h>

#include "button_gesture.h"

#define BC_MAX_BIND 16
#define BC_MAX_ACT 4
#define BC_MAX_MODES 8
#define BC_MAX_PAIRS 8
#define BC_ARG_MAX 80
#define BC_NAME_MAX 16
#define BC_KEY_MAX 48
#define BC_VAL_MAX 64
#define BC_TEXT_MAX 3900   /* a serialised config or mode list must fit the store (4096) with room to spare */
#define BC_IN_MAX 8192     /* longest JSON body parsed */
#define BC_ACT_TEXT_MAX 100

enum { BA_NONE = 0, BA_HOME, BA_PARK, BA_TOGGLE_PARK, BA_PRIVACY, BA_RECAL, BA_GOTO, BA_TRACK, BA_LOST, BA_PATROL_START, BA_PATROL_STOP, BA_LED, BA_MODE };

enum {
    BC_OK = 0,
    BC_E_SYNTAX = -1,   /* not JSON of the expected shape */
    BC_E_VALUE = -2,    /* a value out of range, an unknown action / key, a duplicate */
    BC_E_LIMIT = -3,    /* too many bindings / actions / modes / pairs */
    BC_E_TOOBIG = -4,
    BC_E_REF = -5       /* a binding names a mode that does not exist, or a mode in use is deleted */
};

typedef struct {
    uint8_t kind;               /* BA_* */
    char arg[BC_ARG_MAX + 1];
} bc_action_t;

typedef struct {
    bg_seq_t seq;
    uint8_t nact;
    bc_action_t act[BC_MAX_ACT];
} bc_bind_t;

typedef struct {
    bg_timing_t t;
    int led_ack;                /* 1: blink the LED for a recognised gesture that ends with a short press */
    int nbind;
    bc_bind_t bind[BC_MAX_BIND];
} bc_cfg_t;

typedef struct {
    char name[BC_NAME_MAX + 1];
    int n;
    char key[BC_MAX_PAIRS][BC_KEY_MAX + 1];
    char val[BC_MAX_PAIRS][BC_VAL_MAX + 1];
} bc_mode_t;

typedef struct {
    int n;
    bc_mode_t m[BC_MAX_MODES];
} bc_modes_t;

/* The defaults: today's button: short = nothing but the event + the double blink, long = ptz.toggle_park; the default timing; led_ack on. */
void bc_defaults(bc_cfg_t *c, int long_ms);
/* Text of one action, "none" for BA_NONE. Length, or -1. */
int bc_action_text(const bc_action_t *a, char *out, size_t cap);
/* The whitelist parser: 0 or BC_E_VALUE (why = a short reason, may be NULL). */
int bc_action_parse(const char *text, bc_action_t *a, char *why, size_t wcap);
/* The LED pattern of a "led pattern" argument (a name resolved to its pattern text). */
const char *bc_led_pattern(const char *arg);

typedef struct {
    char detail[112];   /* where and why, for the error reply */
} bc_diag_t;

/* The whole config JSON: {"timing":{...},"led_ack":bool,"bindings":[{"gesture":"..","actions":[".."]}]}. Missing timing fields / led_ack = defaults;
 * "bindings" is required; unknown keys, a duplicate gesture, a bad timing set, an action outside the whitelist: refused, *out untouched. */
int bc_cfg_parse(const char *s, size_t len, bc_cfg_t *out, bc_diag_t *dg);
/* Canonical compact JSON. Length, or -1. */
int bc_cfg_json(const bc_cfg_t *c, char *out, size_t cap);
/* Every "mode NAME" action names an existing mode. 0 or BC_E_REF. */
int bc_cfg_check_refs(const bc_cfg_t *c, const bc_modes_t *m, bc_diag_t *dg);
const bc_bind_t *bc_cfg_find(const bc_cfg_t *c, const bg_seq_t *seq);
/* The bound sequences for the gesture engine. Returns their count. */
int bc_cfg_bound(const bc_cfg_t *c, bg_seq_t *out, int cap);

/* check: NULL, or the registry rule for one pair (0 = allowed). */
typedef int (*bc_key_check_fn)(const char *key, const char *val);
/* {"settings":{"key":"value",...}} (values: string, integer, true / false as 1 / 0), the name comes from the caller. */
int bc_mode_parse(const char *name, const char *s, size_t len, bc_key_check_fn check, bc_mode_t *out, bc_diag_t *dg);
/* {"name":"..","settings":{..}} / {"modes":[...]} */
int bc_mode_json(const bc_mode_t *m, char *out, size_t cap);
int bc_modes_json(const bc_modes_t *m, char *out, size_t cap);
/* The whole persisted list (the modes file): {"modes":[{"name","settings"}]} back into *out; check as above. */
int bc_modes_parse(const char *s, size_t len, bc_key_check_fn check, bc_modes_t *out, bc_diag_t *dg);
int bc_modes_find(const bc_modes_t *m, const char *name);
/* Adds or replaces (BC_E_LIMIT when a new one does not fit). */
int bc_modes_set(bc_modes_t *m, const bc_mode_t *mode);
/* Removes; BC_E_REF when a binding of c uses it, BC_E_VALUE when it does not exist. */
int bc_modes_del(bc_modes_t *m, const char *name, const bc_cfg_t *c);

/* ---- dispatch ---- */
enum { BC_R_OK = 0, BC_R_UNBOUND, BC_R_PARTIAL, BC_R_FAILED };
const char *bc_result_name(int r);    /* "ok" "unbound" "partial" "failed" */

typedef struct {
    void *user;
    /* one action (any kind but BA_MODE): 0 done, -1 refused / failed. arg is the action argument (the LED pattern is already resolved). */
    int (*run)(void *user, int kind, const char *arg);
    /* a mode: the whole pair list, ALL OR NOTHING (the registry validates every pair before any is applied). 0 or -1. */
    int (*apply)(void *user, int n, const char *const *keys, const char *const *vals);
} bc_ops_t;

/* Runs every action of the binding of `seq` in order (all of them, a failure does not stop the next ones). BC_R_UNBOUND when none exists;
 * BC_R_OK when every action worked (or "none"); BC_R_FAILED when all failed; BC_R_PARTIAL otherwise. */
int bc_dispatch(const bc_cfg_t *c, const bc_modes_t *modes, const bg_seq_t *seq, const bc_ops_t *ops);

/* The event / MQTT JSON {"gesture":"..","actions":[".."],"result":".."}, at most max_len bytes (actions that do not fit are replaced by "more":N). Length, or -1. */
int bc_event_json(const bg_seq_t *seq, const bc_bind_t *b, int result, char *out, size_t cap, size_t max_len);

#endif
