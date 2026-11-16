/* The 2K settings registry wired to the real modules, and the settings worker (specs/control-api.md sections 2, 6 and 7.4).
 *
 * Rows (see docs/porting/lsc-rotatable-2k/control-api.md for the full list): led.mode, led.<rule>, night.ir_led, image.*, track.enabled,
 * video<N>.*, net.* (akam2k_netreg.h) and the read-only state values (sys.*, net.wifi_*, rtsp.clients, detect.person, night.active,
 * ptz.*, api.keys). Every writable row calls the setter the Home Assistant entities already use; nothing here touches hardware.
 *
 * ONE worker thread (32 KiB stack) serialises every registry write and every action: the HTTP thread posts a job and waits at most
 * A2K_SET_WAIT_MS; on timeout the reply is 504 busy, the job still completes and a new job is refused (503) until it is done. The worker
 * also owns the persistence of /config/akam/settings: writes are coalesced (checked once a second) and limited to a burst of 4 then one
 * per 30 s (flash wear); a change that cannot be saved yet still applies live and is saved by the next tick.
 *
 * net.* rows have no process of their own to run (the app never spawns one): a change writes ONE request line into the RAM file
 * /tmp/akam_net.req which the supervisor (own_app_run.sh) hands to akam_net.sh, detached, within a few seconds. The value read back is
 * the persisted one (/config/akam/hostname, /config/akam/net), so a reply says "applied" when the request was queued and the next GET
 * shows the new value once the script finished (Wi-Fi: after its dead-man confirmed it, at most 60 s). */
#ifndef AKAM2K_SETTINGS_H
#define AKAM2K_SETTINGS_H
#include <stddef.h>

#include "akam2k_image.h"
#include "hal_sys.h"
#include "json_min.h"
#include "settings_reg.h"

#define A2K_SET_WAIT_MS 3000
#define A2K_SET_FILE "/config/akam/settings"
#define A2K_SET_NET_DIR "/config/akam"
#define A2K_SET_NET_REQ "/tmp/akam_net.req"
#define A2K_SET_FLASH_BURST 4
#define A2K_SET_FLASH_GAP_S 30

typedef struct {
    const char *settings_path; /* NULL = A2K_SET_FILE */
    const char *net_dir;       /* where hostname / net live; NULL = A2K_SET_NET_DIR */
    const char *net_req;       /* the request file; NULL = A2K_SET_NET_REQ */
    const a2k_image_cfg_t *image; /* what main applied at start (the image.* getters); NULL = defaults */
} a2k_settings_cfg_t;

/* Builds and validates the table, starts the worker. 0 or -1 (logged). Call once, after the modules it drives were started. */
int a2k_settings_start(const a2k_settings_cfg_t *cfg);
/* Loads the persisted file (default < akam2k.conf < this file): the whole file or nothing. Call after a2k_settings_start. */
void a2k_settings_load(void);
/* Saves what is pending, stops the worker. Idempotent. */
void a2k_settings_stop(void);
const streg_t *a2k_settings_reg(void);

/* The latest hal_sys sample (cheap, thread safe): the sys.* and net.wifi_* state rows. */
void a2k_settings_note_sys(const hal_sys_sample_t *s);
/* Number of API keys, for the api.keys state row (set by the API layer). */
void a2k_settings_set_keys(int n);

/* ---- jobs ---- */
enum { A2K_JOB_PATCH, A2K_JOB_ACTION };
typedef struct {
    int kind;            /* A2K_JOB_* */
    int scope;           /* AKEY_* level of the caller */
    st_src_t src;
    jflat_t in;          /* the parsed body (PATCH: setting pairs; ACTION: the arguments) */
    const char *name;    /* ACTION: the action name (a pointer into the action table) */
    /* results */
    int http;            /* status to send */
    st_result_t res[JFLAT_MAX];
    const char *err;     /* ACTION: machine error code, NULL when ok */
    char detail[64];
} a2k_job_t;

/* Runs the job on the worker and waits at most A2K_SET_WAIT_MS. 0 = done (results in *j), -1 = timeout (the job still runs: the caller
 * must not touch *j until a2k_settings_idle() is 1, and answers 504), -2 = a job is already running (answer 503). */
int a2k_settings_exec(a2k_job_t *j);
int a2k_settings_idle(void);

/* Actions: name, minimum scope, the argument list as text for the schema. */
typedef struct {
    const char *name;
    int min_scope;
    const char *args;
} a2k_action_info_t;
size_t a2k_settings_action_count(void);
const a2k_action_info_t *a2k_settings_action(size_t i);
int a2k_settings_action_find(const char *name); /* index or -1 */

/* Test hooks. */
void a2k_settings_test_flush(void); /* save now, ignoring the flash limiter */

#endif
