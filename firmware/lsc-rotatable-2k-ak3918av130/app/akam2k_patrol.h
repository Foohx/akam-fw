/* PTZ patrol of the Rotatable 2K own app: the binding core of the pure engine ptz_patrol.h (specs/ptz-patrol.md).
 *
 * One small thread ticks the state machine (100 ms while a patrol runs, 250 ms otherwise) and talks to the motor ONLY through the ops table
 * (the real one, akam2k_patrol_run.c, is the PTZ controller akam2k_ptz.h: the same safe motion layer as every other move). It never preempts: a move is
 * issued only when the controller is idle. Priorities (shutdown > privacy / park / calibration > manual command > tracking > patrol), the idle trigger and
 * the resume after tracking are decided here from the hooks below. State is in RAM only; the config (presets and routes) is one file, written once
 * per accepted change through the store (specs/ptz-patrol.md 6).
 *
 * Locking: one mutex for the run state and the config; a separate one serialises the config edits (flash write outside the first one). The hooks
 * (note_cmd: atomic only; note_person: the detection thread, brief) never block on the motor or the flash. Lock order: tracker -> patrol -> controller. */
#ifndef AKAM2K_PATROL_H
#define AKAM2K_PATROL_H
#include <stddef.h>
#include <stdint.h>

#include "akam2k_ptz.h"
#include "ptz_patrol.h"

#define A2K_STORE_PATROL "/config/akam/ptz_patrol"
#define A2K_PATROL_WRITE_BURST 8       /* config writes allowed at once, then one per A2K_PATROL_WRITE_GAP_S */
#define A2K_PATROL_WRITE_GAP_S 10
#define A2K_PATROL_TICK_MS 100         /* while a patrol runs or waits */
#define A2K_PATROL_IDLE_TICK_MS 250
#define A2K_PATROL_MIN_IDLE_S 30       /* the idle trigger never fires sooner than this after the PTZ became ready */

typedef struct {
    void (*info)(a2k_ptz_info_t *out);
    int (*limits)(pp_limits_t *out);                              /* 0, or -1 when the motor is not available */
    int (*move)(int pan, int tilt, int speed, unsigned *ticket);  /* 0 queued, 1 controller busy (retry), -1 refused */
    int (*result)(unsigned ticket, int *rc);                      /* 0 pending, 1 finished (*rc 0 = reached) */
    void (*cancel)(void);                                         /* cancel the patrol's pending / running move */
    int (*privacy)(void);                                         /* 1: the video is withheld (parked privacy) */
    void (*event)(const char *type, const char *json);
    int (*load)(char *buf, size_t cap);                           /* the config text: its length, -1 absent */
    int (*save)(const char *text, size_t len);                    /* 1 written, 0 unchanged (nothing written), -1 failed */
    int64_t (*now_ms)(void);                                      /* monotonic */
    int (*idle_hold)(void);                                       /* may be NULL: 1 = the idle trigger waits (the controller still saves its home scene reference) */
    unsigned long (*travel)(void);                                /* may be NULL: steps travelled since the last calibration */
} a2k_patrol_ops_t;

/* Loads the config (a file that does not parse is ignored, logged: the config starts empty; nothing is rewritten). Resets the run state. 0. */
int a2k_patrol_init(const a2k_patrol_ops_t *ops);
int a2k_patrol_thread_start(void);   /* 0, -1 */
void a2k_patrol_stop(void);          /* shutdown: stops a running patrol (reason "shutdown"), joins the thread. Idempotent. Call before a2k_ptz_stop. */
void a2k_patrol_tick(void);          /* one step of the state machine (the thread; tests call it directly with the thread not started) */

/* The real binding (akam2k_patrol_run.c): a2k_patrol_init with the controller / event ring / store, the command hook, the thread. */
int a2k_patrol_run_start(void);
void a2k_patrol_run_stop(void);

/* ---- actions (any thread) ---- */
/* 0 started, PP_E_NOTFOUND, PP_E_NOPRESET, PP_E_BUSY (the motor is unavailable: unknown position, calibrating, privacy). Replaces a running route. */
int a2k_patrol_start_route(const char *name);
int a2k_patrol_stop_route(void);     /* 0 (idempotent); reason "manual" */
/* A manual move to a preset (stops a running patrol). 0 queued, PP_E_NOTFOUND, PP_E_BUSY. */
int a2k_patrol_goto_preset(const char *name);

/* ---- settings rows ---- */
int a2k_patrol_get_travel_budget(void);              /* ptz.patrol.travel_budget: steps since the last calibration after which a patrol stops (default A2K_PTZ_TRAVEL_BUDGET) */
int a2k_patrol_set_travel_budget(int steps);         /* 1000..2000000000; -1 invalid */
void a2k_patrol_test_set_min_idle(int s);            /* TEST ONLY: replaces the 30 s minimum idle */
int a2k_patrol_get_idle_s(void);
int a2k_patrol_set_idle_s(int s);                    /* 0..86400, 0 = off; -1 invalid */
void a2k_patrol_get_default(char *out, size_t cap);
int a2k_patrol_set_default(const char *name);        /* a route name syntax or "" (the route need not exist yet); -1 invalid */
void a2k_patrol_state(char *state, size_t scap, char *route, size_t rcap, int *step); /* "idle" "running" "paused", route "" and step -1 when idle */
unsigned a2k_patrol_writes(void);                    /* config writes since the start (sys.flash_writes_patrol) */

/* ---- hooks ---- */
void a2k_patrol_note_cmd(int kind);                  /* ptz controller hook (A2K_PTZ_* or A2K_PTZ_HOOK_*): atomic, lock free */
void a2k_patrol_note_person(int tracking, int resume_ms); /* tracker hook (a2k_track_ops_t.person) */
int a2k_patrol_active(void);                         /* 1 while a patrol is running or paused (lock free): the tracker then does not send the head home */

/* ---- HTTP (the API thread). sub = the path after "/akam/v1/ptz/": presets, presets/<name>, routes, routes/<name>. ---- */
int a2k_patrol_path(const char *sub);                /* 1 when sub is one of those */
/* Returns the HTTP status (reply JSON in out); *retry_s is set on 429. method: GET, POST, DELETE. */
int a2k_patrol_http(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s);

void a2k_patrol_test_reset(void);                    /* forgets everything (after a2k_patrol_stop) */

#endif
