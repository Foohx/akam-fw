/* VERBATIM COPY of platforms/anyka-ak3918/leds.h as it was before the LED engine moved to light_logic (convergence P6), with every leds_/LEDS_/ledsys_
 * identifier prefixed legacy_/LEGACY_. It is the ORACLE of tests/native/leds_oracle_test.c: do not edit, do not fix. */
#pragma once
#include <pthread.h>
#include <stddef.h>

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
 * The engine (legacy_leds_*) works on an instance with an injectable sysfs
 * directory, network check and IR hook so the host tests need no camera.
 * The legacy_ledsys_* functions are the process-wide singleton used by main.c,
 * control.c and rtsp_session.c. */

enum legacy_leds_status { LEGACY_LEDS_STATUS_ON = 0, LEGACY_LEDS_STATUS_OFF, LEGACY_LEDS_STATUS_AUTO };
enum legacy_leds_ir     { LEGACY_LEDS_IR_AUTO = 0, LEGACY_LEDS_IR_OFF };

/* What the LED is doing now (led.state). */
enum legacy_leds_state {
    LEGACY_LEDS_ST_UNMANAGED = 0, /* status=on and never touched: left as the system set it */
    LEGACY_LEDS_ST_ON,
    LEGACY_LEDS_ST_OFF,
    LEGACY_LEDS_ST_BLINK_WIFI,    /* auto: starting, or no Wi-Fi: 1 Hz */
    LEGACY_LEDS_ST_BLINK_STREAM   /* auto: a client is playing: 0.5 Hz */
};

#define LEGACY_LEDS_CONF_FILE "leds"
/* Blink half period. In auto the thread wakes once per tick (2 per second);
 * in on/off it wakes 4 times less often. */
#define LEGACY_LEDS_TICK_MS   500

typedef struct legacy_leds_ops {
    /* 1 when the network is usable (IPv4 address on the interface). NULL = built-in check. */
    int  (*net_ok)(void *user);
    /* Called with 1 = IR LEDs allowed, 0 = never light them. NULL = nothing. */
    void (*ir_allowed)(void *user, int allowed);
    void *user;
} legacy_leds_ops_t;

typedef struct legacy_leds_cfg {
    const char *led_dir;    /* "/sys/class/leds/state_led" */
    const char *conf_dir;   /* "/etc/config/akam" */
    const char *net_ifname; /* "wlan0" */
    const char *net_dir;    /* "/sys/class/net": operstate is <net_dir>/<ifname>/operstate */
    int         tick_ms;    /* 0 = LEGACY_LEDS_TICK_MS */
} legacy_leds_cfg_t;

/* Fields are internal; do not touch them from outside leds.c. */
typedef struct legacy_leds {
    legacy_leds_ops_t ops;
    char led_dir[96], conf_dir[96], conf_path[128], net_ifname[24], net_dir[96];
    int  tick_ms;

    pthread_mutex_t lock;     /* settings and LED state; held for one sysfs write at most */
    int  status;              /* enum legacy_leds_status */
    int  ir;                  /* enum legacy_leds_ir */
    int  take_over;           /* the module drives the LED (status was not "on" at least once) */
    int  ready;               /* start-up finished (legacy_leds_set_ready) */
    int  last_level;          /* -1 unknown, else the last value written */
    int  trigger_done;
    int  err_logged;          /* a sysfs failure was already reported */
    int  state;               /* enum legacy_leds_state */
    unsigned tick;

    pthread_mutex_t cl_lock;  /* the client count only: the RTSP loop waits for nothing else */
    int  clients;

    int  wake_r, wake_w;      /* self-pipe: interrupts the thread's sleep */
    volatile int thread_running;
    int  thread_started;
    pthread_t tid;
} legacy_leds_t;

/* ---- engine --------------------------------------------------------------- */

/* Defaults: status on, ir auto. ops may be NULL (built-in net check, no IR hook). */
void legacy_leds_init(legacy_leds_t *l, const legacy_leds_ops_t *ops, const legacy_leds_cfg_t *cfg);
void legacy_leds_destroy(legacy_leds_t *l);
/* Reads <conf_dir>/leds ("status=on|off|auto", "ir_led=auto|off"); unknown
 * lines and bad values keep the defaults. Returns 1 when the file was read. */
int  legacy_leds_load_config(legacy_leds_t *l);
/* Atomic tmp + rename, file 0600, directory 0700; keeps the lines it does not
 * own. 0 or -1 (errno). */
int  legacy_leds_save_config(legacy_leds_t *l);
/* Applies the loaded settings: the IR hook, and the LED when it is managed. */
void legacy_leds_apply(legacy_leds_t *l);

void legacy_leds_set_ready(legacy_leds_t *l, int ready);
/* delta +1 when an RTSP client starts playing, -1 when it stops. Never below 0. */
void legacy_leds_client_delta(legacy_leds_t *l, int delta);
int  legacy_leds_clients(legacy_leds_t *l);
/* One evaluation of the state machine: reads the network and the client
 * count, writes the LED only when the level changed. The thread calls it
 * every tick; the tests call it directly. */
void legacy_leds_tick(legacy_leds_t *l);
int  legacy_leds_thread_start(legacy_leds_t *l);   /* 0 or -1 */
void legacy_leds_thread_stop(legacy_leds_t *l);    /* joins */

int  legacy_leds_get_status(legacy_leds_t *l);
int  legacy_leds_get_ir(legacy_leds_t *l);
int  legacy_leds_get_state(legacy_leds_t *l);
const char *legacy_leds_status_name(int status);
const char *legacy_leds_ir_name(int ir);
const char *legacy_leds_state_name(int state);

/* Pure state machine (tested on the host): the LED level (0/1) for tick
 * number `tick`, and the resulting state in *state_out. */
int  legacy_leds_compute(int status, int ready, int net_ok, int clients, unsigned tick, int *state_out);

/* Built-in network check: an IPv4 address on ifname (SIOCGIFADDR) and an
 * operstate that is not down/dormant/lowerlayerdown/notpresent. */
int  legacy_leds_net_check(const char *ifname, const char *net_dir);

/* ---- 8091 keys ------------------------------------------------------------ */
/* led.status, led.state (read-only), night.ir_led.
 * get: 0 and the "key=value" line in out, or -1 when the key is not ours.
 * set: 0 and the "OK ..." line in msg, 1 with an "ERR ..." line, or -1 when
 *      the key is not ours (msg untouched). A change is saved at once; a
 *      same-value SET does not touch the flash.
 * list: emit(arg, "key=value") for each key. */
int  legacy_leds_ctl_get(legacy_leds_t *l, const char *key, char *out, size_t cap);
int  legacy_leds_ctl_set(legacy_leds_t *l, const char *key, const char *val, char *msg, size_t cap);
void legacy_leds_ctl_list(legacy_leds_t *l, void (*emit)(void *arg, const char *line), void *arg);

/* ---- singleton used by the camera ---------------------------------------- */
/* Loads the saved settings, applies them (IR hook, LED) and starts the thread. */
int  legacy_ledsys_start(void);
void legacy_ledsys_stop(void);
void legacy_ledsys_boot_done(void);            /* auto: the camera is up, end the start-up blink */
void legacy_ledsys_stream_delta(int delta);    /* rtsp_session.c: +1 on PLAY, -1 when it ends; non-blocking */
int  legacy_ledsys_ctl_get(const char *key, char *out, size_t cap);
int  legacy_ledsys_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void legacy_ledsys_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
