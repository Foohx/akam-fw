/* See akam2k_patrol.h and specs/ptz-patrol.md. */
#include "akam2k_patrol.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "akam2k_log.h"

enum { CMD_MANUAL = 1, CMD_PARK = 2, CMD_CAL = 4 };
static const char k_none[1] = { 0 }; /* the empty text (the C source scan forbids an empty string literal) */

static struct {
    pthread_mutex_t mu;   /* run state, config, hooks' data */
    pthread_mutex_t edit; /* one config edit at a time (the flash write runs outside mu) */
    pthread_cond_t cv;
    pthread_t tid;
    int thread, stop;
    a2k_patrol_ops_t ops;
    int ready;
    pp_cfg_t cfg;
    pp_t run;
    int idle_s;
    char def[PP_NAME_MAX + 1];
    int64_t last_activity, last_person;
    int resume_ms;
    unsigned ticket;
    int have_ticket;
    int instant;          /* the step target is where the head already is: arrived without a move */
    unsigned long travel_budget; /* steps (a2k_ptz_travel) after which a patrol stops (reason travel) */
    char edit_route[PP_NAME_MAX + 1]; /* a config edit of this route is in flight: it cannot be started */
    int idle_warned;
    unsigned writes;
    int tokens;           /* config write limiter */
    int64_t token_ms;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .edit = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static int g_min_idle_s = A2K_PATROL_MIN_IDLE_S;
static volatile int g_cmd_mask;
static volatile int g_phase; /* mirror of run.phase for the lock free reader */

/* ------------------------------------------------------------------ helpers (G.mu held unless noted) */
static void emit(const pp_out_t *o, const char *route, const char *old)
{
    int i;
    for (i = 0; i < o->nev; i++) {
        char d[112];
        const pp_event_t *e = &o->ev[i];
        const char *rt = e->type == PP_EV_STOPPED && old ? old : route; /* a replaced route reports its own stop */
        if (e->reason)
            snprintf(d, sizeof d, "{\"event\":\"%s\",\"route\":\"%s\",\"step\":%d,\"reason\":\"%s\"}", pp_event_name(e->type), rt, e->step, e->reason);
        else
            snprintf(d, sizeof d, "{\"event\":\"%s\",\"route\":\"%s\",\"step\":%d}", pp_event_name(e->type), rt, e->step);
        if (G.ops.event)
            G.ops.event("patrol", d);
        if (e->type != PP_EV_STEP)
            a2k_log("patrol: %s %s step %d %s", pp_event_name(e->type), rt, e->step, e->reason ? e->reason : k_none);
    }
}

static int64_t now_ms(void);

static void apply(const pp_out_t *o)
{
    int i;
    for (i = 0; i < o->nev; i++)
        if (o->ev[i].type == PP_EV_STOPPED)
            G.last_activity = now_ms(); /* any end of a patrol (done, failed, stopped) restarts the idle clock */
    if (o->act == PP_ACT_CANCEL) {
        G.have_ticket = 0;
        if (G.ops.cancel)
            G.ops.cancel();
    }
    emit(o, G.run.route.name, NULL);
    g_phase = G.run.phase;
}

static void do_stop(const char *reason, int64_t now)
{
    pp_out_t o;
    pp_stop(&G.run, reason, &o);
    G.last_activity = now;
    apply(&o);
}

static int64_t now_ms(void) { return G.ops.now_ms ? G.ops.now_ms() : 0; }

static const char *reason_of(int mask) { return mask & CMD_CAL ? "calibration" : mask & CMD_PARK ? "park" : "manual"; }

/* the motor can take a patrol: position known, not calibrating, the video not withheld. 0 ok, else a reason word */
static const char *motor_block(const a2k_ptz_info_t *i)
{
    if (G.ops.privacy && G.ops.privacy())
        return "privacy";
    if (i->state == HAL_PTZ_CALIBRATING)
        return "calibration";
    if (i->state == HAL_PTZ_UNKNOWN)
        return "state";
    return NULL;
}

/* the head has travelled more than the budget since the last calibration: no (more) unattended motion until the next cold boot recalibrates */
static int over_budget(void)
{
    return G.ops.travel && G.ops.travel() > G.travel_budget;
}

static int start_locked(const char *name, int64_t now)
{
    pp_limits_t lim;
    a2k_ptz_info_t i;
    pp_out_t o;
    char old[PP_NAME_MAX + 1];
    int rc;
    if (!G.ops.limits || G.ops.limits(&lim) != 0)
        return PP_E_BUSY;
    G.ops.info(&i);
    if (motor_block(&i) || over_budget() || (name && G.edit_route[0] && !strcmp(name, G.edit_route)))
        return PP_E_BUSY;
    snprintf(old, sizeof old, "%s", G.run.phase != PP_IDLE ? G.run.route.name : k_none);
    rc = pp_start(&G.run, &G.cfg, &lim, name, now, &o);
    if (rc != PP_OK)
        return rc;
    G.last_activity = now;
    G.have_ticket = 0;
    G.instant = 0;
    (void)__atomic_exchange_n(&g_cmd_mask, 0, __ATOMIC_SEQ_CST); /* a command accepted before this start is older than it: it does not stop the new patrol */
    if (o.act == PP_ACT_CANCEL && G.ops.cancel)
        G.ops.cancel();
    emit(&o, G.run.route.name, old[0] ? old : NULL);
    g_phase = G.run.phase;
    return PP_OK;
}

/* ------------------------------------------------------------------ the tick */
void a2k_patrol_tick(void)
{
    int mask = __atomic_exchange_n(&g_cmd_mask, 0, __ATOMIC_SEQ_CST);
    int64_t now;
    a2k_ptz_info_t info;
    const char *blk;
    pthread_mutex_lock(&G.mu);
    if (!G.ready) {
        pthread_mutex_unlock(&G.mu);
        return;
    }
    now = now_ms();
    if (mask) { /* a manual command: the patrol stops, the idle clock restarts */
        G.last_activity = now;
        if (G.run.phase != PP_IDLE)
            do_stop(reason_of(mask), now);
    }
    memset(&info, 0, sizeof info);
    G.ops.info(&info);
    blk = motor_block(&info);
    if (G.run.phase != PP_IDLE && blk)
        do_stop(blk, now);
    if (G.run.phase != PP_IDLE && over_budget()) {
        a2k_log("patrol: %lu steps travelled since the last calibration (limit %lu): stopped, the next cold boot recalibrates", G.ops.travel(), G.travel_budget);
        do_stop("travel", now);
    }
    if (G.run.phase == PP_PAUSED && pp_resume_due(&G.run, G.last_person, G.resume_ms, now)) {
        pp_out_t o;
        pp_resume(&G.run, now, &o);
        apply(&o);
    }
    if (G.run.phase == PP_RUNNING) {
        pp_in_t in;
        pp_out_t o;
        in.now_ms = now;
        in.arrived = in.failed = 0;
        if (G.run.sub == PP_ST_MOVING) {
            int rc = 0;
            if (G.instant) {
                in.arrived = 1;
                G.instant = 0;
            } else if (G.have_ticket && G.ops.result(G.ticket, &rc) == 0) {
                /* still moving */
            } else if (G.have_ticket && rc == 0) {
                in.arrived = 1;
                G.have_ticket = 0;
            } else {
                in.failed = 1; /* failed, cancelled by someone else, or the ticket was lost */
                G.have_ticket = 0;
            }
        }
        pp_tick(&G.run, &in, &o);
        apply(&o);
        if (o.act == PP_ACT_MOVE && G.run.phase == PP_RUNNING) {
            unsigned t = 0;
            pp_out_t o2;
            int st = 0;
            if (info.known && abs(o.pan - (int)info.pan) <= PP_ARRIVE_TOL && abs(o.tilt - (int)info.tilt) <= PP_ARRIVE_TOL) {
                G.instant = 1; /* already there: no zero-distance move */
            } else {
                st = G.ops.move(o.pan, o.tilt, o.speed, &t);
                if (st == 0) {
                    G.ticket = t;
                    G.have_ticket = 1;
                }
            }
            pp_moved(&G.run, st == 0 ? 0 : st > 0 ? 1 : -1, now, &o2);
            apply(&o2);
        }
    } else if (G.run.phase == PP_IDLE && G.idle_s > 0 &&
               (blk || over_budget() || !(info.state == HAL_PTZ_HOME || info.state == HAL_PTZ_MOVED) || (G.ops.idle_hold && G.ops.idle_hold()))) {
        /* the PTZ is not ready (calibrating, unknown, parked, private) or still saving its home scene reference: the idle clock starts when it is ready */
        G.last_activity = now;
    } else if (G.run.phase == PP_IDLE && G.idle_s > 0 && pp_idle_due(G.idle_s < g_min_idle_s ? g_min_idle_s : G.idle_s, G.last_activity, now)) {
        if (!G.def[0] || pp_find_route(&G.cfg, G.def) < 0) {
            if (!G.idle_warned) {
                G.idle_warned = 1;
                a2k_log("patrol: idle trigger set but the default route \"%s\" does not exist", G.def);
            }
            G.last_activity = now; /* look again after another idle period */
        } else {
            int rc = start_locked(G.def, now);
            G.last_activity = now;
            if (rc != PP_OK)
                a2k_log("patrol: idle start refused (%s)", pp_err_code(rc));
        }
    }
    g_phase = G.run.phase;
    pthread_mutex_unlock(&G.mu);
}

static void *thread_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&G.mu);
    while (!G.stop) {
        struct timespec ts;
        long ms = G.run.phase != PP_IDLE ? A2K_PATROL_TICK_MS : A2K_PATROL_IDLE_TICK_MS;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += (ms % 1000) * 1000000L;
        ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        (void)pthread_cond_timedwait(&G.cv, &G.mu, &ts);
        if (G.stop)
            break;
        pthread_mutex_unlock(&G.mu);
        a2k_patrol_tick();
        pthread_mutex_lock(&G.mu);
    }
    pthread_mutex_unlock(&G.mu);
    return NULL;
}

/* ------------------------------------------------------------------ init / stop */
int a2k_patrol_init(const a2k_patrol_ops_t *ops)
{
    static char buf[PP_TEXT_MAX + 1];
    int n;
    pthread_mutex_lock(&G.mu);
    G.ops = *ops;
    G.stop = 0;
    pp_cfg_init(&G.cfg);
    pp_init(&G.run);
    g_phase = PP_IDLE;
    g_cmd_mask = 0;
    G.have_ticket = 0;
    G.instant = 0;
    G.edit_route[0] = '\0';
    G.travel_budget = A2K_PTZ_TRAVEL_BUDGET;
    G.idle_warned = 0;
    G.last_activity = G.last_person = ops->now_ms ? ops->now_ms() : 0;
    G.resume_ms = 33000;
    G.edit_route[0] = '\0';
    G.tokens = A2K_PATROL_WRITE_BURST;
    G.token_ms = G.last_activity;
    G.writes = 0;
    n = ops->load ? ops->load(buf, sizeof buf) : -1;
    if (n > 0 && pp_cfg_parse(buf, (size_t)n, &G.cfg) != PP_OK)
        a2k_log("patrol: %s is invalid, ignored (the config starts empty)", A2K_STORE_PATROL);
    G.idle_s = 0;
    G.def[0] = '\0';
    G.ready = 1;
    a2k_log("patrol: %d preset(s), %d route(s), idle start %s (off by default)", G.cfg.npresets, G.cfg.nroutes, G.idle_s ? "set" : "off");
    pthread_mutex_unlock(&G.mu);
    return 0;
}

int a2k_patrol_thread_start(void)
{
    pthread_attr_t at;
    pthread_condattr_t ca;
    int rc;
    if (G.thread)
        return 0;
    G.stop = 0;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); /* the timed wait is not disturbed by a wall clock step (NTP) */
    pthread_cond_destroy(&G.cv);
    pthread_cond_init(&G.cv, &ca);
    pthread_condattr_destroy(&ca);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 32 * 1024);
    rc = pthread_create(&G.tid, &at, thread_main, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0)
        return -1;
    G.thread = 1;
    return 0;
}

void a2k_patrol_stop(void)
{
    pthread_mutex_lock(&G.mu);
    G.stop = 1;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    if (G.thread) {
        pthread_join(G.tid, NULL);
        G.thread = 0;
    }
    pthread_mutex_lock(&G.mu);
    if (G.ready && G.run.phase != PP_IDLE)
        do_stop("shutdown", now_ms());
    g_phase = PP_IDLE;
    pthread_mutex_unlock(&G.mu);
}

void a2k_patrol_test_reset(void)
{
    pthread_mutex_lock(&G.mu);
    G.ready = 0;
    G.stop = 0;
    G.idle_s = 0;
    G.def[0] = '\0';
    pp_init(&G.run);
    pp_cfg_init(&G.cfg);
    g_phase = PP_IDLE;
    g_cmd_mask = 0;
    pthread_mutex_unlock(&G.mu);
}

/* ------------------------------------------------------------------ hooks */
void a2k_patrol_note_cmd(int kind)
{
    int bit = kind == A2K_PTZ_PARK ? CMD_PARK : kind == A2K_PTZ_RECAL ? CMD_CAL : CMD_MANUAL;
    __atomic_or_fetch(&g_cmd_mask, bit, __ATOMIC_SEQ_CST);
}

void a2k_patrol_note_person(int tracking, int resume_ms)
{
    pthread_mutex_lock(&G.mu);
    if (G.ready) {
        int64_t now = now_ms();
        G.last_activity = now; /* any person restarts the idle clock */
        if (tracking) { /* only a person the tracker acts on pauses the patrol and keeps it paused */
            G.last_person = now;
            if (resume_ms >= 0)
                G.resume_ms = resume_ms < PP_RESUME_MIN_MS ? PP_RESUME_MIN_MS : resume_ms;
            if (G.run.phase == PP_RUNNING) {
                pp_out_t o;
                pp_interrupt(&G.run, now, &o);
                apply(&o);
            }
        }
    }
    pthread_mutex_unlock(&G.mu);
}

int a2k_patrol_active(void) { return g_phase != PP_IDLE; }

/* ------------------------------------------------------------------ actions */
int a2k_patrol_start_route(const char *name)
{
    int rc;
    pthread_mutex_lock(&G.mu);
    rc = G.ready ? start_locked(name, now_ms()) : PP_E_BUSY;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_patrol_stop_route(void)
{
    pthread_mutex_lock(&G.mu);
    if (G.ready && G.run.phase != PP_IDLE)
        do_stop("manual", now_ms());
    pthread_mutex_unlock(&G.mu);
    return 0;
}

int a2k_patrol_goto_preset(const char *name)
{
    int i, pan = 0, tilt = 0, rc;
    pthread_mutex_lock(&G.mu);
    i = G.ready ? pp_find_preset(&G.cfg, name) : -1;
    if (i >= 0) {
        pan = G.cfg.preset[i].pan;
        tilt = G.cfg.preset[i].tilt;
    }
    pthread_mutex_unlock(&G.mu);
    if (i < 0)
        return PP_E_NOTFOUND;
    /* a manual move: the controller calls the command hook when it accepts it, which stops a running patrol */
    rc = G.ops.move ? G.ops.move(pan, tilt, 0, NULL) : -1;
    return rc == 0 ? PP_OK : PP_E_BUSY;
}

/* ------------------------------------------------------------------ settings rows */
int a2k_patrol_get_travel_budget(void)
{
    unsigned long v;
    pthread_mutex_lock(&G.mu);
    v = G.travel_budget;
    pthread_mutex_unlock(&G.mu);
    return (int)v;
}

int a2k_patrol_set_travel_budget(int steps)
{
    if (steps < 1000 || steps > 2000000000)
        return -1;
    pthread_mutex_lock(&G.mu);
    G.travel_budget = (unsigned long)steps;
    pthread_mutex_unlock(&G.mu);
    return 0;
}

void a2k_patrol_test_set_min_idle(int s) { g_min_idle_s = s; }

int a2k_patrol_get_idle_s(void)
{
    int v;
    pthread_mutex_lock(&G.mu);
    v = G.idle_s;
    pthread_mutex_unlock(&G.mu);
    return v;
}

int a2k_patrol_set_idle_s(int s)
{
    if (s < 0 || s > 86400)
        return -1;
    pthread_mutex_lock(&G.mu);
    G.idle_s = s;
    G.idle_warned = 0;
    G.last_activity = now_ms(); /* the idle period counts from the change */
    pthread_mutex_unlock(&G.mu);
    return 0;
}

void a2k_patrol_get_default(char *out, size_t cap)
{
    pthread_mutex_lock(&G.mu);
    snprintf(out, cap, "%s", G.def);
    pthread_mutex_unlock(&G.mu);
}

int a2k_patrol_set_default(const char *name)
{
    if (name[0] && !pp_name_ok(name))
        return -1;
    pthread_mutex_lock(&G.mu);
    snprintf(G.def, sizeof G.def, "%s", name);
    G.idle_warned = 0;
    pthread_mutex_unlock(&G.mu);
    return 0;
}

void a2k_patrol_state(char *state, size_t scap, char *route, size_t rcap, int *step)
{
    pthread_mutex_lock(&G.mu);
    snprintf(state, scap, "%s", pp_phase_name(&G.run));
    snprintf(route, rcap, "%s", G.run.phase != PP_IDLE ? G.run.route.name : k_none);
    *step = G.run.phase != PP_IDLE ? G.run.step : -1;
    pthread_mutex_unlock(&G.mu);
}

unsigned a2k_patrol_writes(void)
{
    unsigned w;
    pthread_mutex_lock(&G.mu);
    w = G.writes;
    pthread_mutex_unlock(&G.mu);
    return w;
}

/* ------------------------------------------------------------------ HTTP */
int a2k_patrol_path(const char *sub)
{
    size_t n = 0;
    if (!strncmp(sub, "presets", 7))
        n = 7;
    else if (!strncmp(sub, "routes", 6))
        n = 6;
    else
        return 0;
    return sub[n] == '\0' || (sub[n] == '/' && sub[n + 1] != '\0');
}

static int http_status(int err)
{
    switch (err) {
    case PP_E_SYNTAX: return 400;
    case PP_E_NOTFOUND: return 404;
    case PP_E_INUSE:
    case PP_E_FULL:
    case PP_E_BUSY: return 409;
    case PP_E_TOOBIG: return 413;
    default: return 422;
    }
}

static int fail(int err, const char *detail, char *out, size_t cap)
{
    pp_diag_t dg;
    memset(&dg, 0, sizeof dg);
    if (detail)
        snprintf(dg.detail, sizeof dg.detail, "%s", detail);
    if (pp_error_json(err, &dg, out, cap) < 0)
        snprintf(out, cap, "{\"error\":\"too_big\",\"detail\":\"\"}");
    return http_status(err);
}

static int fail_dg(int err, const pp_diag_t *dg, char *out, size_t cap)
{
    if (pp_error_json(err, dg, out, cap) < 0)
        snprintf(out, cap, "{\"error\":\"too_big\",\"detail\":\"\"}");
    return http_status(err);
}

static int send_json(int n, char *out, size_t cap)
{
    if (n < 0) {
        snprintf(out, cap, "{\"error\":\"too_big\",\"detail\":\"reply does not fit\"}");
        return 500;
    }
    return 200;
}

/* the write limiter (G.mu held): 1 when a write may happen now */
static int token_take(int64_t now, unsigned *retry_s)
{
    int64_t gap = (int64_t)A2K_PATROL_WRITE_GAP_S * 1000;
    while (G.tokens < A2K_PATROL_WRITE_BURST && now - G.token_ms >= gap) {
        G.tokens++;
        G.token_ms += gap;
    }
    if (G.tokens >= A2K_PATROL_WRITE_BURST)
        G.token_ms = now;
    if (G.tokens > 0) {
        G.tokens--;
        return 1;
    }
    *retry_s = (unsigned)((G.token_ms + gap - now + 999) / 1000);
    return 0;
}

typedef struct {
    pp_cfg_t cfg;
    char a[PP_TEXT_MAX + 1], b[PP_TEXT_MAX + 1];
} work_t;

/* Applies the change to a copy, saves the text (once), then swaps the copy in. All or nothing. */
static int mutate(const char *method, int is_preset, const char *name, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    work_t *w;
    pp_limits_t lim;
    pp_diag_t dg;
    int rc, n1, n2, created = 0, st, wr;
    char pname[PP_NAME_MAX + 1], rname[PP_NAME_MAX + 1];
    pp_route_t *route = NULL;
    int del = !strcmp(method, "DELETE");
    memset(&dg, 0, sizeof dg);
    w = malloc(sizeof *w);
    if (!w)
        return fail(PP_E_TOOBIG, "out of memory", out, cap);
    pthread_mutex_lock(&G.edit);
    pthread_mutex_lock(&G.mu);
    w->cfg = G.cfg;
    {
        int running = G.run.phase != PP_IDLE;
        snprintf(rname, sizeof rname, "%s", G.run.phase != PP_IDLE ? G.run.route.name : k_none);
        pthread_mutex_unlock(&G.mu);
        if (!G.ops.limits || G.ops.limits(&lim) != 0) {
            st = fail(PP_E_BUSY, "the PTZ is not available", out, cap);
            goto done;
        }
        if (is_preset && !del) {
            int pan = 0, tilt = 0, here = 0;
            rc = pp_preset_from_json(body ? body : k_none, len, name, pname, &pan, &tilt, &here, &dg);
            if (rc == PP_OK && here) {
                a2k_ptz_info_t i;
                memset(&i, 0, sizeof i);
                G.ops.info(&i);
                if (!i.known) {
                    st = fail(PP_E_BUSY, "the position is not known", out, cap);
                    goto done;
                }
                pan = (int)i.pan;
                tilt = (int)i.tilt;
            }
            if (rc == PP_OK) {
                created = pp_find_preset(&w->cfg, pname) < 0;
                rc = pp_preset_set(&w->cfg, &lim, pname, pan, tilt, &dg);
            }
        } else if (is_preset) {
            rc = pp_preset_del(&w->cfg, name, &dg);
        } else if (!del) {
            route = malloc(sizeof *route);
            if (!route) {
                st = fail(PP_E_TOOBIG, "out of memory", out, cap);
                goto done;
            }
            rc = pp_route_from_json(body ? body : k_none, len, &lim, route, &dg);
            if (rc == PP_OK) { /* marked as being edited under the lock: a start of this route is refused until the swap, a running one refuses the edit */
                int busy;
                pthread_mutex_lock(&G.mu);
                busy = G.run.phase != PP_IDLE && !strcmp(G.run.route.name, route->name);
                if (!busy)
                    snprintf(G.edit_route, sizeof G.edit_route, "%s", route->name);
                pthread_mutex_unlock(&G.mu);
                if (busy) {
                    st = 409;
                    snprintf(out, cap, "{\"error\":\"running\",\"detail\":\"route %s is running: stop it first\"}", route->name);
                    goto done;
                }
            }
            if (rc == PP_OK) {
                created = pp_find_route(&w->cfg, route->name) < 0;
                rc = pp_route_set(&w->cfg, &lim, route, &dg);
            }
        } else {
            char def[PP_NAME_MAX + 1];
            a2k_patrol_get_default(def, sizeof def);
            {
                int busy;
                pthread_mutex_lock(&G.mu);
                busy = G.run.phase != PP_IDLE && !strcmp(G.run.route.name, name);
                if (!busy)
                    snprintf(G.edit_route, sizeof G.edit_route, "%s", name);
                pthread_mutex_unlock(&G.mu);
                running = busy;
            }
            if (running) {
                st = 409;
                snprintf(out, cap, "{\"error\":\"running\",\"detail\":\"route %s is running: stop it first\"}", name);
                goto done;
            }
            if (!strcmp(name, def)) {
                st = 409;
                snprintf(out, cap, "{\"error\":\"in_use\",\"detail\":\"route %s is the default route (ptz.patrol.route_default)\"}", name);
                goto done;
            }
            rc = pp_route_del(&w->cfg, name, &dg);
        }
    }
    if (rc != PP_OK) {
        st = fail_dg(rc, &dg, out, cap);
        goto done;
    }
    n1 = pp_cfg_format(&w->cfg, w->a, sizeof w->a);
    pthread_mutex_lock(&G.mu);
    n2 = pp_cfg_format(&G.cfg, w->b, sizeof w->b);
    pthread_mutex_unlock(&G.mu);
    if (n1 < 0 || n2 < 0) {
        st = fail(PP_E_TOOBIG, "the configuration does not fit", out, cap);
        goto done;
    }
    wr = 0;
    if (n1 != n2 || memcmp(w->a, w->b, (size_t)n1) != 0) { /* a real change: one flash write, rate limited */
        int ok;
        pthread_mutex_lock(&G.mu);
        ok = token_take(G.ops.now_ms ? G.ops.now_ms() : 0, retry_s);
        pthread_mutex_unlock(&G.mu);
        if (!ok) {
            snprintf(out, cap, "{\"error\":\"rate\",\"detail\":\"config writes are rate limited\"}");
            st = 429;
            goto done;
        }
        wr = G.ops.save ? G.ops.save(w->a, (size_t)n1) : -1;
        if (wr < 0) {
            a2k_log("patrol: cannot write the config (the change is refused)");
            snprintf(out, cap, "{\"error\":\"store_failed\",\"detail\":\"the configuration could not be saved\"}");
            st = 500;
            goto done;
        }
    }
    pthread_mutex_lock(&G.mu);
    G.cfg = w->cfg;
    if (wr > 0)
        G.writes++;
    pthread_mutex_unlock(&G.mu);
    if (del)
        snprintf(out, cap, "{\"ok\":true,\"deleted\":\"%s\"}", name);
    else
        snprintf(out, cap, "{\"ok\":true,\"created\":%s,\"name\":\"%s\"}", created ? "true" : "false", is_preset ? pname : route->name);
    st = 200;
done:
    pthread_mutex_lock(&G.mu);
    G.edit_route[0] = '\0';
    pthread_mutex_unlock(&G.mu);
    pthread_mutex_unlock(&G.edit);
    free(route);
    free(w);
    return st;
}

int a2k_patrol_http(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    int is_preset = !strncmp(sub, "presets", 7), n;
    const char *name = strchr(sub, '/');
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");
    if (name)
        name++;
    if (!a2k_patrol_path(sub))
        return fail(PP_E_NOTFOUND, "no such path", out, cap);
    if (!(get || post || del) || (!name && del) || (name && post && !is_preset)) {
        (void)fail(PP_E_SYNTAX, "method not allowed on this path", out, cap);
        return 405;
    }
    if (name && !pp_name_ok(name))
        return fail(PP_E_NAME, "bad name in the path", out, cap);
    if (get) {
        int st;
        pthread_mutex_lock(&G.mu);
        if (!G.ready) {
            pthread_mutex_unlock(&G.mu);
            return fail(PP_E_BUSY, "not available", out, cap);
        }
        if (is_preset && !name) {
            st = send_json(pp_presets_json(&G.cfg, out, cap), out, cap);
        } else if (is_preset) {
            int i = pp_find_preset(&G.cfg, name);
            st = i < 0 ? fail(PP_E_NOTFOUND, "no such preset", out, cap) : send_json(pp_preset_json(&G.cfg.preset[i], out, cap), out, cap);
        } else if (!name) {
            int run = G.run.phase != PP_IDLE ? pp_find_route(&G.cfg, G.run.route.name) : -1;
            st = send_json(pp_routes_json(&G.cfg, run, G.def, out, cap), out, cap);
        } else {
            int i = pp_find_route(&G.cfg, name);
            st = i < 0 ? fail(PP_E_NOTFOUND, "no such route", out, cap) : send_json(pp_route_json(&G.cfg.route[i], out, cap), out, cap);
        }
        pthread_mutex_unlock(&G.mu);
        return st;
    }
    n = G.ready;
    if (!n)
        return fail(PP_E_BUSY, "not available", out, cap);
    return mutate(method, is_preset, name, body, len, out, cap, retry_s);
}
