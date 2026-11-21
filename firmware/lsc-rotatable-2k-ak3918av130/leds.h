/* Status LED policy of the Rotatable 2K own app: the front blue LED ("blue" of the hal_light descriptor), driven by a pattern per
 * rule. PURE engine (leds.c: no clock of its own, no network, no pin number; the driver and the inputs are injected, so
 * tests/rot2k_led_unit.c runs it on a fake clock). The process-wide singleton and its thread are leds_run.c.
 *
 * Pattern: "on" | "off" | "N:on_ms:off_ms:pause_ms": N blinks (on_ms lit, off_ms dark), then pause_ms dark, repeat. Integers only,
 * N 1..10, on_ms and off_ms 50..60000, pause_ms 0 or 50..60000, whole cycle <= 120000 ms. Example "3:250:250:5000".
 * Rules, first true wins: boot (first A2K_LEDS_BOOT_MS or until the Wi-Fi had an IPv4 address once), nowifi (no IPv4), person
 * (person detection on), stream (>= 1 RTSP client playing), night (night mode), idle (always true).
 * Master mode: auto = the rules, on / off = forced. Defaults keep the stock look (the LED stays on). While the wanted pattern is
 * "on" and the engine never wrote the LED, it is left as the system set it (never written for nothing). */
#ifndef AKAM2K_LEDS_H
#define AKAM2K_LEDS_H

#include <pthread.h>
#include <stddef.h>

#include "hal_light.h"
#include "led_pat.h"
#include "light_logic.h"

#define A2K_LEDS_BOOT_MS 20000

enum { A2K_RULE_BOOT, A2K_RULE_NOWIFI, A2K_RULE_PERSON, A2K_RULE_STREAM, A2K_RULE_NIGHT, A2K_RULE_IDLE, A2K_RULE_N };
const char *a2k_rule_key(int rule);     /* "led_boot", "led_nowifi", ... as in akam2k.conf */
const char *a2k_rule_default(int rule); /* the default pattern text */
enum { A2K_LEDS_ON, A2K_LEDS_OFF, A2K_LEDS_AUTO };
int a2k_leds_parse_mode(const char *s); /* exact "on" "off" "auto" -> enum, else -1 */
const char *a2k_leds_mode_name(int mode);

typedef struct {
    int boot, nowifi, person, stream, night;
} a2k_leds_in_t;
/* The rule that wins (first true in the order above; idle when none). */
int a2k_leds_pick(const a2k_leds_in_t *in);

typedef struct {
    /* fills nowifi, person, stream, night (boot is the engine's) */
    void (*inputs)(void *user, a2k_leds_in_t *in);
    void *user;
} a2k_leds_ops_t;

typedef struct {
    pthread_mutex_t mu;
    light_t light;
    a2k_leds_ops_t ops;
    int mode;
    a2k_pat_t pat[A2K_RULE_N];
    long start_ms; /* engine start (boot rule) */
    int wifi_seen;
    int taken;     /* the engine has written the LED at least once */
    int cur;       /* the rule driving now, -1 forced, -2 none yet, -3 a transient blip (a2k_leds_blip) */
    a2k_pat_t cur_pat;
    long pat_start_ms;
    int level;     /* last level written, -1 none */
    int last_rule;
    int fl_on;     /* a transient override (the button feedback) is set */
    a2k_pat_t fl_pat;
    long fl_until_ms;
} a2k_leds_t;

/* desc / drv: the hal_light descriptor ("blue" must exist) and driver. 0, or -1. */
int a2k_leds_init(a2k_leds_t *l, const hal_light_desc_t *desc, const hal_light_ops_t *drv, const a2k_leds_ops_t *ops, long now_ms);
void a2k_leds_destroy(a2k_leds_t *l);
int a2k_leds_set_mode(a2k_leds_t *l, int mode);
int a2k_leds_get_mode(a2k_leds_t *l);
/* Runtime pattern of a rule: 0, or -1 (invalid text: the old one is kept). */
int a2k_leds_set_pattern(a2k_leds_t *l, int rule, const char *text);
void a2k_leds_get_pattern(a2k_leds_t *l, int rule, char *out, size_t cap);
/* Transient override (the button feedback): `pattern` (the syntax above) replaces the rules for dur_ms (1..60000) from now_ms on, then the
 * rules come back. Ignored while the master mode is "off" (a dark LED stays dark); it does show in "on" and "auto". 0, or -1 (bad
 * pattern or duration: nothing changes). a2k_leds_blip_clear ends it at once. */
int a2k_leds_blip(a2k_leds_t *l, const char *pattern, long dur_ms, long now_ms);
void a2k_leds_blip_clear(a2k_leds_t *l);
/* One evaluation: reads the inputs, picks the pattern, writes the LED only when its level changes. Call every <= 50 ms. */
void a2k_leds_tick(a2k_leds_t *l, long now_ms);
/* Back to on (the stock state) when the engine had taken the LED over. */
void a2k_leds_release(a2k_leds_t *l);
int a2k_leds_last_rule(a2k_leds_t *l);

/* ---- singleton (leds_run.c) ---- */
/* Starts the thread (50 ms tick). mode and pats[] are the akam2k.conf values (pats may be NULL: defaults). 0 or -1 (logged). */
int a2k_leds_start(int mode, const char *const pats[A2K_RULE_N]);
void a2k_leds_stop(void);          /* before a2k_night_stop (which closes the pin files) */
void a2k_leds_person(int present); /* detection thread */
int a2k_leds_run_set_mode(int mode);
int a2k_leds_run_mode(void);
int a2k_leds_run_set_pattern(int rule, const char *text);
void a2k_leds_run_pattern(int rule, char *out, size_t cap);
/* The transient override on the real LED (see a2k_leds_blip); -1 when the engine is not running. */
int a2k_leds_run_blip(const char *pattern, long dur_ms);
void a2k_leds_run_blip_clear(void);

#endif
