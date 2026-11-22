#pragma once
#include <pthread.h>
#include <stddef.h>

#include "hal_light.h"
#include "led_pat.h"
#include "light_logic.h"

/* Front status LED and IR LED policy (specs/image-day-night-leds.md).
 *
 * led.status  on | off | auto   (default on: the LED is left as the system
 *                               set it, behaviour unchanged)
 *   auto: blinks 1 Hz while the camera is starting or Wi-Fi has no IPv4
 *   address, blinks slowly (0.5 Hz) while at least one RTSP client is
 *   playing (privacy indicator), solid on otherwise.
 * night.ir_led  auto | off      (default auto: IR LEDs follow night mode;
 *                               off: the IR-cut filter and the night image
 *                               still switch, the IR LEDs never light)
 *
 * The status LED is the kernel LED class device state_led: the module only
 * writes <led_dir>/brightness ("0"/"1") and, once, <led_dir>/trigger ("none").
 * Both settings are saved in <conf_dir>/leds (key=value lines) and applied at
 * start-up.
 *
 * Engine (convergence P6): the LED is the emitter "status" of a hal_light descriptor (LIGHT_LEDCLASS, the same driver contract as
 * the Rotatable 2K) written through light_logic; what it shows in auto mode is a PATTERN per RULE (led_pat.h, the grammar of the
 * 2K: "on", "off", "N:on_ms:off_ms:pause_ms"), the first true rule wins:
 *   stream  at least one RTSP client is playing       default 1 s on / 1 s off   (privacy indicator, wins over everything)
 *   boot    start-up is not finished                  default 0.5 s on / 0.5 s off
 *   nowifi  no IPv4 address on the interface          default 0.5 s on / 0.5 s off
 *   night   night mode                                default on
 *   idle    anything else                             default on
 * The defaults reproduce the behaviour of the engine before P6 exactly (tests/native/leds_oracle_test.c drives both with the
 * same traces). status=on is a solid LED and status=off a dark one, whatever the rules say. A pattern of a rule can be set
 * (leds_pat_set, saved as led_<rule>=... in the leds file only while it differs from its default) and a transient pattern can
 * override the rules for a while (leds_blip: "find the camera").
 *
 * The engine (leds_*) works on an instance with an injectable sysfs
 * directory, network check and IR hook so the host tests need no camera.
 * The ledsys_* functions are the process-wide singleton used by main.c,
 * control.c and rtsp_session.c. */

enum leds_status { LEDS_STATUS_ON = 0, LEDS_STATUS_OFF, LEDS_STATUS_AUTO };
enum leds_ir     { LEDS_IR_AUTO = 0, LEDS_IR_OFF };

/* What the LED is doing now (led.state). */
enum leds_state {
    LEDS_ST_UNMANAGED = 0, /* status=on and never touched: left as the system set it */
    LEDS_ST_ON,
    LEDS_ST_OFF,
    LEDS_ST_BLINK_WIFI,    /* auto: starting, or no Wi-Fi: 1 Hz */
    LEDS_ST_BLINK_STREAM,  /* auto: a client is playing: 0.5 Hz */
    LEDS_ST_BLIP           /* an explicit blip overrides the mode (off included) for its duration */
};

#define LEDS_CONF_FILE "leds"
/* Blink half period. In auto the thread wakes once per tick (2 per second);
 * in on/off it wakes 4 times less often. */
#define LEDS_TICK_MS   500

typedef struct leds_ops {
    /* 1 when the network is usable (IPv4 address on the interface). NULL = built-in check. */
    int  (*net_ok)(void *user);
    /* Called with 1 = IR LEDs allowed, 0 = never light them. NULL = nothing. */
    void (*ir_allowed)(void *user, int allowed);
    void *user;
} leds_ops_t;

/* Rules, in priority order (see above). */
enum leds_rule { LEDS_RULE_STREAM = 0, LEDS_RULE_BOOT, LEDS_RULE_NOWIFI, LEDS_RULE_NIGHT, LEDS_RULE_IDLE, LEDS_RULE_N };
const char *leds_rule_name(int rule); /* "stream" "boot" "nowifi" "night" "idle" (the 8091/registry keys are led.<name>) */
int         leds_rule_find(const char *name); /* index, or -1 */

typedef struct leds_cfg {
    const char *led_dir;    /* "/sys/class/leds/state_led" */
    const char *conf_dir;   /* "/etc/config/akam" */
    const char *net_ifname; /* "wlan0" */
    const char *net_dir;    /* "/sys/class/net": operstate is <net_dir>/<ifname>/operstate */
    int         tick_ms;    /* 0 = LEDS_TICK_MS */
} leds_cfg_t;

/* Fields are internal; do not touch them from outside leds.c. */
typedef struct leds {
    leds_ops_t ops;
    char led_dir[96], conf_dir[96], conf_path[128], net_ifname[24], net_dir[96];
    int  tick_ms;

    /* the emitter and its driver: `em` must stay the first member of the wrapper that the driver casts back to the owner */
    struct { hal_light_emitter_t em; struct leds *owner; } emitter;
    hal_light_desc_t light_desc;
    light_t light;
    a2k_pat_t pat[LEDS_RULE_N]; /* the pattern of each rule */
    int  (*night_fn)(void *user); /* 1 while the camera is in night mode (the "night" rule); NULL = never. Only asked in auto mode */
    void *night_user;
    int  rule_now;            /* the rule that drove the LED at the last evaluation, -1 none (not auto) */
    long next_ms;             /* ms until the level of the current pattern may change, -1 never */
    int  blip_on;             /* a transient pattern overrides the rules */
    a2k_pat_t blip_pat;
    long blip_start_ms, blip_until_ms; /* monotonic */
    long t0_ms;               /* monotonic time of leds_init: the real-time phase origin of the patterns */
    int  use_real;            /* the thread is running: patterns are evaluated at real time, else at tick * tick_ms */

    pthread_mutex_t lock;     /* settings and LED state; held for one sysfs write at most */
    int  status;              /* enum leds_status */
    int  ir;                  /* enum leds_ir */
    int  take_over;           /* the module drives the LED (status was not "on" at least once) */
    int  ready;               /* start-up finished (leds_set_ready) */
    int  last_level;          /* -1 unknown, else the last value written */
    int  trigger_done;
    int  err_logged;          /* a sysfs failure was already reported */
    int  state;               /* enum leds_state */
    unsigned tick;

    pthread_mutex_t cl_lock;  /* the client count only: the RTSP loop waits for nothing else */
    int  clients;

    int  wake_r, wake_w;      /* self-pipe: interrupts the thread's sleep */
    volatile int thread_running;
    int  thread_started;
    pthread_t tid;
} leds_t;

/* ---- engine --------------------------------------------------------------- */

/* Defaults: status on, ir auto. ops may be NULL (built-in net check, no IR hook). */
void leds_init(leds_t *l, const leds_ops_t *ops, const leds_cfg_t *cfg);
void leds_destroy(leds_t *l);
/* Reads <conf_dir>/leds ("status=on|off|auto", "ir_led=auto|off"); unknown
 * lines and bad values keep the defaults. Returns 1 when the file was read. */
int  leds_load_config(leds_t *l);
/* Atomic tmp + rename, file 0600, directory 0700; keeps the lines it does not
 * own. 0 or -1 (errno). */
int  leds_save_config(leds_t *l);
/* Applies the loaded settings: the IR hook, and the LED when it is managed. */
void leds_apply(leds_t *l);

void leds_set_ready(leds_t *l, int ready);
/* delta +1 when an RTSP client starts playing, -1 when it stops. Never below 0. */
void leds_client_delta(leds_t *l, int delta);
int  leds_clients(leds_t *l);
/* One evaluation of the state machine: reads the network and the client
 * count, writes the LED only when the level changed. The thread calls it
 * every tick; the tests call it directly. */
void leds_tick(leds_t *l);
int  leds_thread_start(leds_t *l);   /* 0 or -1 */
void leds_thread_stop(leds_t *l);    /* joins */

int  leds_get_status(leds_t *l);
int  leds_get_ir(leds_t *l);
int  leds_get_state(leds_t *l);
const char *leds_status_name(int status);
const char *leds_ir_name(int ir);
const char *leds_state_name(int state);

/* Pure state machine (tested on the host): the LED level (0/1) for tick
 * number `tick`, and the resulting state in *state_out. */
int  leds_compute(int status, int ready, int net_ok, int clients, unsigned tick, int *state_out);

/* Built-in network check: an IPv4 address on ifname (SIOCGIFADDR) and an
 * operstate that is not down/dormant/lowerlayerdown/notpresent. */
int  leds_net_check(const char *ifname, const char *net_dir);

/* The "night" rule input (see above); NULL = the rule never applies. */
void leds_set_night_hook(leds_t *l, int (*night)(void *user), void *user);

/* Pattern of a rule (canonical text "on" "off" "N:on:off:pause"). get: 0, or -1 for a bad rule. set: 0 (saved only when it
 * changed), -1 for a bad rule or a text that is not exactly the grammar (nothing changes), -2 the change applies but could not
 * be saved (errno). A rule set back to its default leaves no line in the file. */
int  leds_pat_get(leds_t *l, int rule, char *out, size_t cap);
int  leds_pat_set(leds_t *l, int rule, const char *text);
/* The default pattern text of a rule at this instance's tick. */
void leds_pat_default(const leds_t *l, int rule, char *out, size_t cap);
/* A transient pattern replaces the rules for dur_ms (1..60000) from now (real time, needs the thread); it overrides every status, off
 * included, and led.state reads "blip" meanwhile. 0, -1 (bad pattern or duration) or -2 (the LED could not be written). leds_blip_clear ends it at once. */
int  leds_blip(leds_t *l, const char *pattern, long dur_ms);
void leds_blip_clear(leds_t *l);
/* The rule that is driving the LED now (auto mode), else -1. */
int  leds_rule_now(leds_t *l);

/* ---- 8091 keys ------------------------------------------------------------ */
/* led.status, led.state (read-only), night.ir_led.
 * get: 0 and the "key=value" line in out, or -1 when the key is not ours.
 * set: 0 and the "OK ..." line in msg, 1 with an "ERR ..." line, or -1 when
 *      the key is not ours (msg untouched). A change is saved at once; a
 *      same-value SET does not touch the flash.
 * list: emit(arg, "key=value") for each key. */
int  leds_ctl_get(leds_t *l, const char *key, char *out, size_t cap);
int  leds_ctl_set(leds_t *l, const char *key, const char *val, char *msg, size_t cap);
void leds_ctl_list(leds_t *l, void (*emit)(void *arg, const char *line), void *arg);

/* ---- singleton used by the camera ---------------------------------------- */
/* Loads the saved settings, applies them (IR hook, LED) and starts the thread. */
int  ledsys_start(void);
void ledsys_stop(void);
void ledsys_boot_done(void);            /* auto: the camera is up, end the start-up blink */
void ledsys_stream_delta(int delta);    /* rtsp_session.c: +1 on PLAY, -1 when it ends; non-blocking */
int  ledsys_pat_get(int rule, char *out, size_t cap);   /* the registry rows led.<rule> (leds_pat_get) */
int  ledsys_pat_set(int rule, const char *text);        /* leds_pat_set */
void ledsys_pat_default(int rule, char *out, size_t cap);
int  ledsys_blip(const char *pattern, long dur_ms);     /* leds_blip */
void ledsys_blip_clear(void);
int  ledsys_ctl_get(const char *key, char *out, size_t cap);
int  ledsys_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void ledsys_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
