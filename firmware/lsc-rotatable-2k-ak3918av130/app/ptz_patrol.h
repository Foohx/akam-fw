/* PTZ patrol engine: PURE logic (no I/O, no clock, no thread, no hardware), host-tested (tests/rot2k_ptz_patrol_unit.c). Model-agnostic:
 * the caller passes the limits of its model (soft range, allowed speeds) and drives the state machine with a clock and the result of its moves.
 * Specification: specs/ptz-patrol.md. Candidate for src/common/logic.
 *
 * Config: up to PP_PRESETS named presets and PP_ROUTES routes of up to PP_STEPS steps, with generated sweeps / rasters expanded into plain steps.
 * Run: pp_t is a state machine fed by pp_tick(): it returns "move there" actions and events; the caller issues the move (through the safe motion
 * layer of its model, never a driver) and reports the result. Everything is clamped to the limits; speeds are divisors validated against them. */
#ifndef PTZ_PATROL_H
#define PTZ_PATROL_H
#include <stddef.h>
#include <stdint.h>

#define PP_NAME_MAX 16
#define PP_PRESETS 16
#define PP_ROUTES 8
#define PP_STEPS 32
#define PP_DWELL_MAX_MS 3600000u
#define PP_MIN_DWELL_MS 250u   /* floor of the dwell at run time for a once route */
#define PP_LOOP_MIN_DWELL_MS 1500u /* the floor for loop and pingpong routes (bounded unattended motion) */
#define PP_RESUME_MIN_MS 3000  /* floor of the resume delay after a person (the binding applies it) */
#define PP_ARRIVE_TOL 5        /* steps: a target this close to the known position counts as reached, no move is made */
#define PP_TEXT_MAX 4096       /* the stored text of the whole config (the store limit of the 2K) */
#define PP_MAX_FAILS 3         /* consecutive failed moves that stop the patrol */
#define PP_RETRY_MS 1000       /* wait before a failed move is retried */
#define PP_STEP_EVENT_GAP_MS 1000
#define PP_BUSY_MAX_MS 60000   /* a controller busy this long (no move accepted) ends the patrol */

enum { PP_ONCE, PP_LOOP, PP_PINGPONG };

/* Error codes (negative returns). pp_err_code() gives the machine word used by the API. */
enum {
    PP_OK = 0,
    PP_E_SYNTAX = -1,      /* not valid JSON / text of the expected shape */
    PP_E_NAME = -2,
    PP_E_RANGE = -3,       /* a position outside the soft range, or a number out of bounds */
    PP_E_SPEED = -4,
    PP_E_DWELL = -5,
    PP_E_MODE = -6,
    PP_E_NOPRESET = -7,    /* a step names a preset that does not exist */
    PP_E_INUSE = -8,       /* the preset is referenced by a route */
    PP_E_NOTFOUND = -9,
    PP_E_FULL = -10,       /* too many presets or routes */
    PP_E_TOOMANY = -11,    /* more than PP_STEPS steps (after expansion) */
    PP_E_EMPTY = -12,      /* no step */
    PP_E_PATTERN = -13,    /* a bad generated pattern */
    PP_E_TOOBIG = -14,     /* the stored text would exceed PP_TEXT_MAX */
    PP_E_BUSY = -15        /* the action does not apply in the current state */
};

typedef struct {
    int pan_min, pan_max, tilt_min, tilt_max; /* the soft range (inclusive) */
    int speed_min, speed_max, speed_def;      /* allowed speed divisors and the default (smaller = faster) */
    int park_pan, park_tilt, park_tol;         /* the park position (privacy) and its tolerance: a patrol target within it is refused; park_tol 0 = none */
} pp_limits_t;

typedef struct {
    char name[PP_NAME_MAX + 1];
    int pan, tilt;
} pp_preset_t;

typedef struct {
    char preset[PP_NAME_MAX + 1]; /* "" = explicit pan / tilt */
    int pan, tilt;                /* explicit target (ignored when preset is set) */
    int speed;                    /* 0 = the default speed */
    unsigned dwell_ms;
} pp_step_t;

typedef struct {
    char name[PP_NAME_MAX + 1];
    int mode;
    int nsteps;
    pp_step_t step[PP_STEPS];
} pp_route_t;

typedef struct {
    int npresets, nroutes;
    pp_preset_t preset[PP_PRESETS];
    pp_route_t route[PP_ROUTES];
} pp_cfg_t;

/* A short explanation of the last refusal (the API puts it in "detail"). */
typedef struct {
    char detail[96];
} pp_diag_t;

const char *pp_err_code(int err);   /* "range", "speed", "no_preset", ... */
int pp_name_ok(const char *s);
const char *pp_mode_name(int mode); /* "once" "loop" "pingpong" */
int pp_mode_parse(const char *s);   /* mode or -1 */

/* ---- config editing (all or nothing: on error the config is unchanged) ---- */
void pp_cfg_init(pp_cfg_t *c);
int pp_find_preset(const pp_cfg_t *c, const char *name); /* index or -1 */
int pp_find_route(const pp_cfg_t *c, const char *name);
/* Creates or replaces. lim may be NULL (structure only: the range / speed checks are skipped, used for the stored file). */
int pp_preset_set(pp_cfg_t *c, const pp_limits_t *lim, const char *name, int pan, int tilt, pp_diag_t *dg);
int pp_preset_del(pp_cfg_t *c, const char *name, pp_diag_t *dg);                 /* PP_E_NOTFOUND, PP_E_INUSE */
int pp_route_set(pp_cfg_t *c, const pp_limits_t *lim, const pp_route_t *r, pp_diag_t *dg); /* creates or replaces */
int pp_route_del(pp_cfg_t *c, const char *name, pp_diag_t *dg);
/* Validates one route against the config (presets) and the limits (lim NULL: structure only). */
int pp_route_check(const pp_cfg_t *c, const pp_limits_t *lim, const pp_route_t *r, pp_diag_t *dg);

/* ---- generated patterns: expand into r->step[] (name / mode of r untouched). Returns the step count or a negative error. ---- */
int pp_expand_sweep_h(const pp_limits_t *lim, int pan_from, int pan_to, int tilt, int step, int speed, unsigned dwell_ms, pp_route_t *r, pp_diag_t *dg);
int pp_expand_sweep_v(const pp_limits_t *lim, int tilt_from, int tilt_to, int pan, int step, int speed, unsigned dwell_ms, pp_route_t *r, pp_diag_t *dg);
int pp_expand_raster(const pp_limits_t *lim, int pan_from, int pan_to, int tilt_from, int tilt_to, int rows, int cols, int speed, unsigned dwell_ms,
                     pp_route_t *r, pp_diag_t *dg);

/* ---- text (stored file): "p name pan tilt" / "r name mode" + "s preset|pan:tilt speed dwell" lines ---- */
int pp_cfg_format(const pp_cfg_t *c, char *out, size_t cap); /* length, PP_E_TOOBIG */
int pp_cfg_parse(const char *text, size_t len, pp_cfg_t *out); /* strict, structure only; 0, or an error and *out reset to an empty config */
/* The size the text would have, -1 when it exceeds PP_TEXT_MAX. */
int pp_cfg_text_size(const pp_cfg_t *c);

/* ---- JSON (API) ---- */
/* Route body: {"name","mode","steps":[{"preset"|"pan","tilt","speed","dwell_ms"}]} or {"name","mode","pattern":{"type",...}}. Strict: unknown or duplicate
 * keys, wrong types and anything after the object are errors. The pattern is expanded (lim needed). mode defaults to "loop". */
int pp_route_from_json(const char *s, size_t len, const pp_limits_t *lim, pp_route_t *out, pp_diag_t *dg);
/* Preset body: {"name","pan","tilt"} or {"name","here":true}. *here is set when "here" was true (pan / tilt then 0). name: PP_NAME_MAX + 1 bytes.
 * dflt: the name of the URL (the body may omit "name", or must equal it); NULL = the body names it. */
int pp_preset_from_json(const char *s, size_t len, const char *dflt, char *name, int *pan, int *tilt, int *here, pp_diag_t *dg);
int pp_preset_json(const pp_preset_t *p, char *out, size_t cap);
int pp_presets_json(const pp_cfg_t *c, char *out, size_t cap);
int pp_route_json(const pp_route_t *r, char *out, size_t cap);
/* routes: name, mode, steps; running_idx: index of the running route or -1; def: the default route name */
int pp_routes_json(const pp_cfg_t *c, int running_idx, const char *def, char *out, size_t cap);
/* Error body {"error":code,"detail":text}. */
int pp_error_json(int err, const pp_diag_t *dg, char *out, size_t cap);

/* ---- run ---- */
enum { PP_IDLE, PP_RUNNING, PP_PAUSED };
enum { PP_ST_PENDING, PP_ST_MOVING, PP_ST_DWELL }; /* inside PP_RUNNING */
enum { PP_EV_STARTED, PP_EV_STEP, PP_EV_INTERRUPTED, PP_EV_RESUMED, PP_EV_STOPPED };
enum { PP_ACT_NONE, PP_ACT_MOVE, PP_ACT_CANCEL };

typedef struct {
    int type;            /* PP_EV_* */
    int step;
    const char *reason;  /* STOPPED / INTERRUPTED: a static string, else NULL */
} pp_event_t;

typedef struct {
    int act;             /* PP_ACT_MOVE: issue a move to pan / tilt at speed; PP_ACT_CANCEL: cancel the move in flight */
    int pan, tilt, speed;
    int nev;
    pp_event_t ev[3];
} pp_out_t;

typedef struct {
    int phase;           /* PP_IDLE / PP_RUNNING / PP_PAUSED */
    int sub;             /* PP_ST_* while running; while paused: the sub-state it was interrupted in */
    pp_limits_t lim;
    pp_route_t route;    /* resolved copy: every step explicit, clamped, speed resolved */
    int step, dir;
    int64_t dwell_until, retry_at, last_step_ev, paused_at, started_at, busy_since;
    int fails;
    unsigned cycles;     /* completed passes (loop / pingpong) */
    int have_target;     /* a move was issued for the current step and has no result yet */
} pp_t;

typedef struct {
    int64_t now_ms;
    int arrived;         /* the move issued for the current step finished ok */
    int failed;          /* it failed or was cancelled by someone else */
} pp_in_t;

void pp_init(pp_t *p);
/* Starts route r of c (resolves the presets, clamps, validates). 0, PP_E_NOTFOUND, PP_E_NOPRESET, PP_E_EMPTY. Replaces a running route (the old one
 * is reported stopped with reason "replaced" first). Fills out with the events and a CANCEL action when a move was in flight. */
int pp_start(pp_t *p, const pp_cfg_t *c, const pp_limits_t *lim, const char *route, int64_t now_ms, pp_out_t *out);
void pp_stop(pp_t *p, const char *reason, pp_out_t *out);          /* any state -> idle; reason is a static string */
void pp_interrupt(pp_t *p, int64_t now_ms, pp_out_t *out);         /* person tracking: running -> paused (cancels a move in flight) */
void pp_resume(pp_t *p, int64_t now_ms, pp_out_t *out);            /* paused -> running at the interrupted step */
/* One tick (every ~100 ms). The caller reports the arrival / failure of the move issued earlier. A MOVE action is returned while the step needs its
 * move: the caller issues it and answers with pp_moved(). */
void pp_tick(pp_t *p, const pp_in_t *in, pp_out_t *out);
/* The answer to a MOVE action: 0 issued, 1 the controller was busy (try again at the next tick, no failure), -1 refused (counts as a failure). */
void pp_moved(pp_t *p, int status, int64_t now_ms, pp_out_t *out);

/* ---- trigger helpers (pure) ---- */
/* 1 when the idle trigger is due: idle_start_s > 0 and now - last_activity_ms >= idle_start_s. */
int pp_idle_due(int idle_start_s, int64_t last_activity_ms, int64_t now_ms);
/* 1 when a paused patrol may resume: no person seen for resume_after_ms. */
int pp_resume_due(const pp_t *p, int64_t last_person_ms, int64_t resume_after_ms, int64_t now_ms);
const char *pp_phase_name(const pp_t *p); /* "idle" "running" "paused" */
const char *pp_event_name(int type);      /* "started" "step" "interrupted" "resumed" "stopped" */

#endif
