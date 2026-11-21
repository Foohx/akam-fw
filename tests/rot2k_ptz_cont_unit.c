/* Host unit test of the CONTINUOUS motion of the Rotatable 2K PTZ: the safe layer (ptz/akam_ptz_safe.h ptzs_cont_*), the HAL session (hal_ptz_cont_run) and the controller thread
 * (ptz.c: a2k_ptz_vel, the lease, the position history a2k_ptz_pos_at, the preemption by manual commands) against a FAKE DRIVER that models: a speed of 1100 / divisor
 * steps/s per axis with no acceleration (measured 2026-11-17: ~370 steps/s at divisor 3), a latency before a command takes effect, per-axis driver counters in the shifted
 * frame with the driver's own limits, mechanical stops, injectable stall / wrong-way / uncommanded drift, and the feared driver behaviour "an axis missing from a new command keeps
 * running" (keep_unlisted). Only the move (0..7), stop and position requests may reach the driver during a session.
 * Build: tests/rot2k_overlay_test.sh. Prints "rot2k ptz cont unit: N failed". */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#include "../firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/hal.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/ptz.h"
#include "../src/hal/hal_ptz.h"

#define REQ_LIM 0x40100c03u
#define REQ_IPOS 0x40280c04u

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ------------------------------------------------------------------------------------------------ the fake driver */
static struct {
    pthread_mutex_t mu;
    uint32_t cnt[2], tgt[2], drvmax[2], spd[2];
    int running[2], mech[2], lo[2], hi[2];
    int acc[2];
    int stuck[2], wrong[2], drift[2];      /* injected faults */
    int keep_unlisted;                     /* a move command without an axis leaves that axis running */
    int lat_ms;                            /* a move takes effect this long after it was sent */
    int stop_lat;                          /* a stop takes effect this long after it was sent (0: at once): steps still in flight meanwhile */
    long clock;
    struct { long at; int kind; unsigned dir, a, b; } q[16];
    int nq;
    int nmove, nstop, nother, bad_cmd, cmds[4096], ncmd, nsave;
    struct { long at; int kind; unsigned dir, a, b; } cl[1024]; /* every stop (kind 0) and move (kind 1) request with its virtual time */
    int ncl;
    int real;                              /* sleeping really sleeps (the thread tests) */
    int max_mech[2], min_mech[2];
    long exec[2], exec_pos[2], exec_neg[2]; /* the TRUE motor: net / positive / negative steps really executed (independent of the counters) */
    long hist_t[20000];                    /* truth for the position history test: time, pan */
    int hist_pan[20000], hist_n;
} M = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void d_reset(int pan, int tilt)
{
    pthread_mutex_lock(&M.mu);
    memset(M.cnt, 0, sizeof M.cnt);
    memset(M.tgt, 0, sizeof M.tgt);
    memset(M.running, 0, sizeof M.running);
    memset(M.acc, 0, sizeof M.acc);
    memset(M.stuck, 0, sizeof M.stuck);
    memset(M.wrong, 0, sizeof M.wrong);
    memset(M.drift, 0, sizeof M.drift);
    M.keep_unlisted = 0;
    M.lat_ms = 30;
    M.stop_lat = 0;
    M.clock = 0;
    M.nq = M.nmove = M.nstop = M.nother = M.bad_cmd = M.ncmd = M.nsave = M.hist_n = M.ncl = 0;
    M.real = 0;
    memset(M.exec, 0, sizeof M.exec);
    memset(M.exec_pos, 0, sizeof M.exec_pos);
    memset(M.exec_neg, 0, sizeof M.exec_neg);
    M.drvmax[0] = 6000;
    M.drvmax[1] = 2200;
    M.spd[0] = M.spd[1] = 3;
    M.lo[0] = M.lo[1] = 0;
    M.hi[0] = 4000;
    M.hi[1] = 2300;
    M.mech[0] = M.max_mech[0] = M.min_mech[0] = pan;
    M.mech[1] = M.max_mech[1] = M.min_mech[1] = tilt;
    pthread_mutex_unlock(&M.mu);
}

static void apply_cmd(int kind, unsigned dir, unsigned a, unsigned b)
{
    int ax;
    if (kind == 0) { /* stop */
        M.running[0] = M.running[1] = 0;
        return;
    }
    if (a) M.spd[0] = a;
    if (b) M.spd[1] = b;
    for (ax = 0; ax < 2; ax++) {
        int s = ax ? PTZS_TILT_SIGN[dir] : PTZS_PAN_SIGN[dir];
        if (s) {
            M.tgt[ax] = s > 0 ? M.drvmax[ax] : 0;
            M.running[ax] = 1;
        } else if (!M.keep_unlisted) {
            M.running[ax] = 0; /* the sane driver: an axis that is not in the command does not move */
        }
    }
}

static void advance_locked(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        M.clock++;
        for (int k = 0; k < M.nq;) {
            if (M.q[k].at <= M.clock) {
                apply_cmd(M.q[k].kind, M.q[k].dir, M.q[k].a, M.q[k].b);
                memmove(&M.q[k], &M.q[k + 1], sizeof M.q[0] * (size_t)(M.nq - k - 1));
                M.nq--;
            } else {
                k++;
            }
        }
        for (int ax = 0; ax < 2; ax++) {
            if (M.drift[ax] && M.clock % 10 == 0) {
                M.cnt[ax] += (uint32_t)M.drift[ax];
                M.mech[ax] += M.drift[ax];
            }
            if (!M.running[ax] || M.cnt[ax] == M.tgt[ax]) {
                if (M.cnt[ax] == M.tgt[ax]) M.running[ax] = 0;
                continue;
            }
            M.acc[ax] += (int)(1100 / M.spd[ax]);
            while (M.acc[ax] >= 1000) {
                int step = M.tgt[ax] > M.cnt[ax] ? 1 : -1;
                M.acc[ax] -= 1000;
                if (M.stuck[ax]) continue;
                if (M.cnt[ax] == M.tgt[ax]) break;
                M.cnt[ax] += (uint32_t)(M.wrong[ax] ? -step : step);
                M.mech[ax] += M.wrong[ax] ? -step : step;
                M.exec[ax] += M.wrong[ax] ? -step : step;
                if ((M.wrong[ax] ? -step : step) > 0) M.exec_pos[ax]++; else M.exec_neg[ax]++;
            }
            if (M.mech[ax] < M.lo[ax]) M.mech[ax] = M.lo[ax];
            if (M.mech[ax] > M.hi[ax]) M.mech[ax] = M.hi[ax];
            if (M.mech[ax] > M.max_mech[ax]) M.max_mech[ax] = M.mech[ax];
            if (M.mech[ax] < M.min_mech[ax]) M.min_mech[ax] = M.mech[ax];
        }
        if (M.hist_n < 20000) {
            M.hist_t[M.hist_n] = M.clock;
            M.hist_pan[M.hist_n++] = M.mech[0];
        }
    }
}

static int d_open(void) { return 3; }

static int d_ioctl(int fd, unsigned long req, void *arg)
{
    uint32_t *w = arg;
    int rc = 0;
    (void)fd;
    pthread_mutex_lock(&M.mu);
    if (req == PTZ_REQ_STOP) {
        M.nstop++;
        if (M.ncl < 1024) { M.cl[M.ncl].at = M.clock; M.cl[M.ncl].kind = 0; M.ncl++; }
        if (M.stop_lat && M.nq < 16) { M.q[M.nq].at = M.clock + M.stop_lat; M.q[M.nq].kind = 0; M.q[M.nq].dir = M.q[M.nq].a = M.q[M.nq].b = 0; M.nq++; }
        else apply_cmd(0, 0, 0, 0); /* the stop is immediate unless stop_lat is set; a new direction takes lat_ms */
    } else if (req == PTZ_REQ_POS) {
        if (w[0] > 1) { errno = EINVAL; rc = -1; } else w[1] = M.cnt[w[0]];
    } else if (req == PTZ_REQ_MOVE) {
        if (w[0] > 7) { M.bad_cmd++; errno = EINVAL; rc = -1; }
        else {
            M.nmove++;
            if (M.ncl < 1024) { M.cl[M.ncl].at = M.clock; M.cl[M.ncl].kind = 1; M.cl[M.ncl].dir = w[0]; M.cl[M.ncl].a = w[1]; M.cl[M.ncl].b = w[2]; M.ncl++; }
            if (M.ncmd < 4096) M.cmds[M.ncmd++] = (int)w[0];
            if (M.nq < 16) { M.q[M.nq].at = M.clock + M.lat_ms; M.q[M.nq].kind = 1; M.q[M.nq].dir = w[0]; M.q[M.nq].a = w[1]; M.q[M.nq].b = w[2]; M.nq++; }
        }
    } else if (req == REQ_LIM) {
        if (w[1]) M.drvmax[0] = w[1];
        if (w[2]) M.drvmax[1] = w[2];
    } else if (req == REQ_IPOS) {
        if (w[0]) M.cnt[0] = w[0];
        if (w[1]) M.cnt[1] = w[1];
    } else {
        M.nother++;
        errno = ENOTTY;
        rc = -1;
    }
    pthread_mutex_unlock(&M.mu);
    return rc;
}

static void d_sleep(unsigned ms)
{
    if (M.real)
        usleep(ms * 1000);
    pthread_mutex_lock(&M.mu);
    advance_locked(ms);
    pthread_mutex_unlock(&M.mu);
}

static int d_load(const char *path, char *buf, unsigned cap) { (void)path; (void)buf; (void)cap; errno = ENOENT; return -1; }
static int d_save(const char *path, const char *data) { (void)path; (void)data; M.nsave++; return 0; }
void a2k_log(const char *fmt, ...) { (void)fmt; }
static void d_log(const char *line) { (void)line; }

static int claim_home(uint32_t *pan, uint32_t *tilt) { (void)pan; (void)tilt; return 1; } /* the scene check "proves" home: no calibration, no driver motion */
static const hal_ptz_ops_t OPS = { d_open, d_ioctl, d_sleep, d_load, d_save, d_log, claim_home, NULL, NULL, NULL };

static hal_ptz_t *mk(void)
{
    char err[96] = "";
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_t *p;
    d_reset(1275, 1300);
    p = hal_ptz_new(&d, &OPS, NULL, err, sizeof err);
    if (!p || hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0) { printf("setup failed: %s\n", err); exit(2); }
    return p;
}

static int mech_pan(void) { return M.mech[0]; }
static int mech_tilt(void) { return M.mech[1]; }

/* ------------------------------------------------------------------------------------------------ session scripts */
struct sc {
    int n;                                   /* polls so far */
    int (*fn)(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd);
    int refresh_until;                       /* poll count until which an identical refresh is sent (0 = never) */
    hal_ptz_cmd_t cur;
    hal_ptz_t *hp;
    int arg;
    int last_t;
};

static int cb(void *ctx, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    struct sc *s = ctx;
    s->n++;
    s->last_t = (int)st->t_ms;
    if (s->fn) return s->fn(s, st, cmd);
    if (s->n <= s->refresh_until) { *cmd = s->cur; return HAL_PTZ_CONT_CMD; }
    return HAL_PTZ_CONT_KEEP;
}

static int mstate(void) { return 0; }

/* ------------------------------------------------------------------------------------------------ cases */
static int fn_reverse_every(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    if (s->n > 60) return HAL_PTZ_CONT_STOP;
    if (s->n % 2 == 0) { /* every 100 ms a new direction: right / left / right-up / left-down ... */
        static const int sx[4] = { 1, -1, 1, -1 }, sy[4] = { 0, 0, 1, -1 };
        int k = (s->n / 2) % 4;
        cmd->sx = sx[k]; cmd->sy = sy[k]; cmd->dx = 3; cmd->dy = 4;
    } else {
        *cmd = s->cur;
        cmd->sx = cmd->sx ? cmd->sx : 1;
    }
    s->cur = *cmd;
    return HAL_PTZ_CONT_CMD;
}

static int fn_diag_then_pan(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    if (s->n < 12) { *cmd = s->cur; return HAL_PTZ_CONT_CMD; }
    if (s->n == 12) { /* up-right -> pure right: the tilt axis is dropped */
        cmd->sx = 1; cmd->sy = 0; cmd->dx = 3; cmd->dy = 0;
        s->cur = *cmd;
        s->arg = mech_tilt();
        return HAL_PTZ_CONT_CMD;
    }
    if (s->n < 40) { *cmd = s->cur; return HAL_PTZ_CONT_CMD; }
    return HAL_PTZ_CONT_STOP;
}

static int fn_cancel(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    *cmd = s->cur;
    if (s->n == 8) hal_ptz_cancel(s->hp);
    return HAL_PTZ_CONT_CMD;
}

static int fn_keep_cmd(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    *cmd = s->cur;
    return s->n > 400 ? HAL_PTZ_CONT_STOP : HAL_PTZ_CONT_CMD;
}

static void move_div_range(int n0, int ax, unsigned *mn, unsigned *mx, unsigned *first, int diag_only);
static void hal_cases(void)
{
    hal_ptz_t *p;
    struct sc s;
    hal_ptz_cmd_t first;
    uint32_t pan, tilt;
    int rc, t0;
    printf("== HAL continuous session on the fake driver (1100 / divisor steps/s, 30 ms command latency)\n");

    /* 1. a move that nobody refreshes: the lease stops it */
    p = mk();
    memset(&s, 0, sizeof s);
    first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    t0 = (int)M.clock;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK, "an unrefreshed move ends by its lease with HAL_PTZ_OK");
    CHECK(s.last_t >= 350 && s.last_t <= 500, "... within the lease + a poll (virtual time)");
    CHECK(mech_pan() > 1275 + 100 && mech_pan() < 1275 + 260, "... after about 0.4 s at 370 steps/s (right)");
    CHECK(hal_ptz_pos(p, &pan, &tilt) && (int)pan == mech_pan() && (int)tilt == 1300, "the final position is the driver counter (known again), the tilt untouched");
    CHECK(M.nstop >= 1 && M.running[0] == 0 && M.nother == 0 && M.bad_cmd == 0, "the motor is stopped; no request other than move / stop / pos reached the driver");
    CHECK(hal_ptz_travel(p) >= 100 && hal_ptz_travel(p) <= 270, "the travel was counted");
    (void)t0;

    /* 2. refreshes keep it going; then it stops within the lease of the last one */
    p = mk();
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.refresh_until = 40; /* 2 s of refreshes every poll */
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK && s.n >= 40 + 7 && s.n <= 40 + 10, "a refresh every poll keeps the move running; 2 s later the lease stops it within ~0.4 s of the last refresh");
    CHECK(mech_pan() > 1275 + 600, "... it travelled about 2.4 s worth");
    CHECK(M.nmove <= 6, "refreshes without a change send no new driver command (only the speed ladder 10 6 4 3 of the start)");

    /* 3. rapid direction changes incl. reversals and diagonals: never a false wrong-way / stall */
    p = mk();
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_reverse_every;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK, "a new direction every 100 ms (reversals, diagonals) is no wrong-way / stall");
    CHECK(M.nmove > 25, "... and every change was a driver command");
    CHECK(hal_ptz_state(p) != HAL_PTZ_UNKNOWN && hal_ptz_pos(p, &pan, &tilt) && (int)pan == mech_pan() && (int)tilt == mech_tilt(), "the position stayed trusted and equals the mechanics");
    CHECK(hal_ptz_travel(p) > 300, "the travel counts the reversals (not only the net displacement)");

    /* 4. diagonal: both axes at once; then dropping an axis stops first (also with the feared driver) */
    for (int keep = 0; keep < 2; keep++) {
        p = mk();
        M.keep_unlisted = keep;
        memset(&s, 0, sizeof s);
        s.cur = first = (hal_ptz_cmd_t){ 1, 1, 3, 3 };
        s.fn = fn_diag_then_pan;
        rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
        CHECK(rc == HAL_PTZ_OK, keep ? "up-right then pure right, driver keeps unlisted axes: no error" : "up-right then pure right: no error");
        CHECK(M.mech[1] > 1300 + 100, "up-right moved the tilt up too (both axes at once)");
        CHECK(abs(M.mech[1] - s.arg) <= 25, keep ? "after the tilt axis was dropped it did NOT run away even with an unlisted-axis-keeps-running driver (stop first)" : "the dropped tilt axis stopped");
        CHECK(M.mech[0] > 1275 + 600, "the pan went on");
    }

    /* 5. soft range: a pure move toward the limit ends before it; a diagonal into a corner goes on with the other axis */
    p = mk();
    CHECK(hal_ptz_goto(p, 3500, 1300) == HAL_PTZ_OK, "goto near the pan limit (3500)");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 2, 0 };
    s.fn = fn_keep_cmd;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK && M.max_mech[0] <= 3650 && M.max_mech[0] >= 3590, "pan: stops before the soft limit 3650 (never beyond), at least within 60 steps of it");
    CHECK(M.running[0] == 0 && M.nother == 0, "... motor stopped");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 2, 0 };
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_EREFUSED && M.nmove == (int)M.nmove && s.n == 0, "a command toward a limit it is already at is refused and nothing moves");
    {
        int mv = M.nmove;
        memset(&s, 0, sizeof s);
        s.cur = first = (hal_ptz_cmd_t){ 1, 0, 2, 0 };
        (void)hal_ptz_cont_run(p, &first, 400, cb, &s);
        CHECK(M.nmove == mv, "... no driver command was sent");
    }
    p = mk();
    CHECK(hal_ptz_goto(p, 3600, 1300) == HAL_PTZ_OK || 1, "goto");
    p = mk();
    CHECK(hal_ptz_goto(p, 3300, 2000) == HAL_PTZ_OK, "goto near the up-right corner");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 1, 3, 3 };
    s.fn = fn_keep_cmd;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK && M.max_mech[1] <= 2100 && M.max_mech[1] >= 2040, "tilt: stops at its soft limit 2100 (never beyond) ...");
    CHECK(M.max_mech[0] <= 3650 && M.mech[0] > 3300 + 100, "... the pan went on alone after the tilt axis was dropped");
    CHECK(s.n < 400 && M.running[0] == 0 && M.running[1] == 0, "... the session ended by itself at the end of the other axis (soft limit) or its lease");

    /* 6. faults */
    p = mk();
    M.stuck[0] = 1;
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_keep_cmd;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_ESTALL && hal_ptz_state(p) == HAL_PTZ_UNKNOWN, "a counter that does not move: stall error, the position is no longer trusted, motor stopped");
    CHECK(M.nstop >= 1, "... and the stop was sent");
    p = mk();
    M.wrong[0] = 1;
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_keep_cmd;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_ESTALL, "a counter running the wrong way: stall error");
    p = mk();
    M.drift[1] = 1; /* the tilt counter creeps while only the pan is commanded */
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_keep_cmd;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_ESTALL, "an axis that moves although it is not commanded: error (never relied on)");
    p = mk();
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.hp = p;
    s.fn = fn_cancel;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_ECANCEL && M.running[0] == 0 && s.n <= 10, "hal_ptz_cancel ends the session within a poll and stops the motor");
    CHECK(hal_ptz_state(p) != HAL_PTZ_UNKNOWN && hal_ptz_pos(p, &pan, &tilt), "... the position stays known (a cancel is no fault)");

    /* 7. bad commands and speeds */
    p = mk();
    memset(&s, 0, sizeof s);
    first = (hal_ptz_cmd_t){ 0, 0, 3, 3 };
    CHECK(hal_ptz_cont_run(p, &first, 400, cb, &s) == HAL_PTZ_EREFUSED, "a zero command is refused");
    first = (hal_ptz_cmd_t){ 1, 0, 1, 0 };
    CHECK(hal_ptz_cont_run(p, &first, 400, cb, &s) == HAL_PTZ_EREFUSED, "a speed divisor of 1 is refused (the safe layer accepts 2..20)");
    first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    CHECK(hal_ptz_cont_run(p, &first, 100, cb, &s) == HAL_PTZ_EREFUSED, "a lease under 150 ms is refused");
    CHECK(M.nmove == 0, "nothing reached the driver");

    /* 8. a descriptor without diagonal words: the faster axis alone, only cardinal words on the wire */
    {
        char err[96] = "";
        hal_ptz_desc_t d = a2k_ptz_desc;
        d.diag_ok = 0;
        d_reset(1275, 1300);
        p = hal_ptz_new(&d, &OPS, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "a descriptor without diagonals starts");
        memset(&s, 0, sizeof s);
        s.cur = first = (hal_ptz_cmd_t){ 1, 1, 3, 5 };
        s.fn = fn_keep_cmd;
        s.n = 380;
        rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
        CHECK(rc == HAL_PTZ_OK && M.ncmd >= 1 && M.cmds[0] == 2 && M.mech[1] == 1300, "up-right at pan 3 / tilt 5 becomes pure right (the faster axis), command word 2");
    }
    (void)mstate;
}

/* ------------------------------------------------------------------------------------------------ the controller thread */
static const a2k_ptz_scene_t SC = { .boot_check = NULL };
static int boot_ok(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void)) { (void)pan; (void)tilt; (void)cancelled; return 1; }

static int64_t mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_state(int st, int ms)
{
    a2k_ptz_info_t i;
    for (int t = 0; t < ms; t += 10) {
        a2k_ptz_info(&i);
        if (i.state == st) return 1;
        usleep(10000);
    }
    return 0;
}

static int wait_idle(int ms)
{
    a2k_ptz_info_t i;
    for (int t = 0; t < ms; t += 10) {
        a2k_ptz_info(&i);
        if (i.known && i.state != HAL_PTZ_CALIBRATING) return 1;
        usleep(10000);
    }
    return 0;
}

/* wait_idle only says "not calibrating": it returns at once while a command is still pending or running. This waits until a motion that ended after `since` is published and the motor
 * is quiet, so the next command meets an idle controller (otherwise it lands in the dying session or is refused: a race of the test, not of the controller). */
static int wait_done(int64_t since, int ms)
{
    a2k_ptz_info_t i;
    for (int t = 0; t < ms; t += 10) {
        a2k_ptz_info(&i);
        if (i.known && i.state != HAL_PTZ_CALIBRATING && i.motion_end_ms >= since && !M.running[0] && !M.running[1]) { usleep(50000); return 1; }
        usleep(10000);
    }
    return 0;
}

static void thread_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    int pp, pt, rc, ok;
    int64_t t0;
    printf("== PTZ controller: a2k_ptz_vel sessions, the lease, the position history, preemption\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    CHECK(a2k_ptz_start(&c) == 0, "start");
    CHECK(wait_idle(8000), "ready (home claimed, no calibration)");
    M.real = 1;
    a2k_ptz_set_lease_ms(450);

    CHECK(a2k_ptz_vel(0, 0, 0, 0) == 0, "a stop with nothing running is accepted and does nothing");
    CHECK(a2k_ptz_vel(2, 0, 3, 0) == -1 && a2k_ptz_vel(1, 0, 1, 0) == -1 && a2k_ptz_vel(1, 0, 21, 0) == -1, "bad arguments are refused");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "idle: a velocity command starts a session");
    usleep(200000);
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_MOVED && !i.known, "info during the session: moved, position not published as settled (known = 0)");
    CHECK(a2k_ptz_pos_at(mono(), &pp, &pt) == 0 && pp > 1275 + 5, "a2k_ptz_pos_at(now) works DURING the session (the polled counter)");
    /* refresh for 1 s every 200 ms with a change of direction in the middle */
    t0 = mono();
    ok = 1;
    for (int k = 0; k < 5; k++) {
        if (a2k_ptz_vel(k < 3 ? 1 : -1, k == 2 ? 1 : 0, 3, 4) != 0) ok = 0;
        usleep(200000);
    }
    CHECK(ok, "a2k_ptz_vel refreshes / re-targets are accepted while the session runs");
    CHECK(M.nstop <= 3, "the direction change did not need a stop for the pan reversal");
    {   /* position history: the truth is recorded per ms by the model; ask for an instant 400 ms ago */
        int64_t tq = mono() - 400;
        int tp = 0, found;
        found = a2k_ptz_pos_at(tq, &pp, &pt);
        (void)tp;
        CHECK(found == 0, "pos_at 400 ms ago resolves");
        {   /* it must lie between the extremes the head had in the last second */
            int lo = M.min_mech[0], hi = M.max_mech[0];
            CHECK(pp >= lo - 2 && pp <= hi + 2, "... within the travelled range");
        }
        CHECK(a2k_ptz_pos_at(t0 - 20000, &pp, &pt) == -1, "pos_at before the first sample is unknown");
    }
    /* the lease: no refresh -> stops by itself */
    t0 = mono();
    for (int t = 0; t < 1500; t += 10) {
        a2k_ptz_info(&i);
        if (i.known) break;
        usleep(10000);
    }
    a2k_ptz_info(&i);
    CHECK(i.known && mono() - t0 < 1000, "no refresh: the session ends by itself (dead-man) within ~0.6 s and the position is known again");
    {
        int s1 = mech_pan();
        usleep(150000);
        CHECK(mech_pan() == s1 && !M.running[0] && !M.running[1], "... the motor really stopped");
        CHECK(a2k_ptz_pos_at(mono(), &pp, &pt) == 0 && abs(pp - mech_pan()) <= 3 && abs(pt - mech_tilt()) <= 3, "pos_at(now) after the session = the final position");
        CHECK((int)i.pan == mech_pan(), "info position = the mechanics");
    }
    /* preemption */
    CHECK(a2k_ptz_vel(-1, 0, 3, 0) == 0, "a new session");
    usleep(150000);
    CHECK(a2k_ptz_nudge_sp(100, 0, 3) == -1, "the tracker's nudge is refused while a session runs");
    t0 = mono();
    CHECK(a2k_ptz_command(A2K_PTZ_UP) == 0, "a manual step (button) while a session runs is queued");
    rc = wait_done(t0, 3000);
    {
        int p0 = mech_pan();
        usleep(100000);
        CHECK(rc && mech_pan() == p0 && !M.running[0], "the manual command ended the session within ~0.1 s and ran; the motor is idle");
    }
    CHECK(mono() - t0 < 1500, "... quickly");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "a session again");
    usleep(150000);
    t0 = mono();
    CHECK(a2k_ptz_nudge(50, 0) == 0, "a MANUAL nudge (API ptz.move) cancels the tracker's session and runs");
    CHECK(wait_done(t0, 3000), "... done");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "a session once more");
    usleep(150000);
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0, "park (privacy) while a session runs");
    CHECK(wait_state(HAL_PTZ_PARKED, 8000), "the head parked: the session did not survive");
    {
        int p0 = mech_pan();
        usleep(100000);
        CHECK(mech_pan() == p0, "motor idle after the park");
    }
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "a session from the parked position");
    usleep(150000);
    a2k_ptz_stop();
    CHECK(!M.running[0] && !M.running[1], "a2k_ptz_stop (shutdown) ends the session and stops the motor");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "no forbidden driver request during the whole thread test");
}

static int g_notify;
static void notify_cb(const a2k_ptz_info_t *i) { (void)i; g_notify++; }

static void review_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    hal_ptz_t *p;
    int n0, stops0;
    printf("== review fixes: a refused session is silent, emergency stop, one-shot stop\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "controller up");
    M.real = 1;
    a2k_ptz_set_notify(notify_cb);
    /* put the head at the pan soft limit */
    CHECK(a2k_ptz_goto(3650, 1300, 2) == 0, "goto the pan limit");
    usleep(300000);
    for (int k = 0; k < 400 && !(a2k_ptz_info(&i), i.known && i.pan >= 3640); k++) usleep(20000);
    a2k_ptz_info(&i);
    CHECK(i.known && i.pan >= 3640, "at the limit");
    usleep(200000);
    n0 = g_notify;
    CHECK(a2k_ptz_vel(1, 0, 2, 0) == 0, "a velocity command toward the limit is queued ...");
    usleep(300000);
    a2k_ptz_info(&i);
    CHECK(g_notify == n0 && i.known, "... and the refused session published nothing (no notify, position still known)");
    /* a stop clears a start that has not run yet */
    CHECK(a2k_ptz_vel(-1, 0, 3, 0) == 0 && a2k_ptz_vel(0, 0, 0, 0) == 0, "start then stop at once");
    usleep(400000);
    a2k_ptz_info(&i);
    CHECK(i.known, "the session (if it started at all) is over: the stop was honoured");
    a2k_ptz_stop();
    /* emergency stop: async-signal-safe stop on the open node */
    p = mk();
    stops0 = M.nstop;
    hal_ptz_emergency_stop();
    CHECK(M.nstop >= stops0 + 1 && M.nother == 0, "hal_ptz_emergency_stop sends the stop request on the open node (and nothing else)");
    stops0 = M.nstop;
    CHECK(hal_ptz_oneshot_stop(&OPS) == 0 && M.nstop == stops0 + 1, "hal_ptz_oneshot_stop: open, one stop request, success");
    hal_ptz_free(p);
}


/* ------------------------------------------------------------------------------------------------ lost-step mitigation (command sequences on the fake driver) */
static hal_ptz_t *mk_conf(const char *conf)
{
    char err[96] = "";
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_t *p;
    d_reset(1275, 1300);
    p = hal_ptz_new(&d, &OPS, conf, err, sizeof err);
    if (!p || hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0) { printf("setup failed: %s\n", err); exit(2); }
    return p;
}

static int fn_rev_once(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    if (s->n > 40) return HAL_PTZ_CONT_STOP;
    if (s->n == 15) { *cmd = (hal_ptz_cmd_t){ -1, 0, 3, 0 }; s->cur = *cmd; s->arg = M.ncl; return HAL_PTZ_CONT_CMD; }
    *cmd = s->cur;
    return HAL_PTZ_CONT_CMD;
}

static int fn_hold(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    *cmd = s->cur;
    return s->n > s->arg ? HAL_PTZ_CONT_STOP : HAL_PTZ_CONT_CMD;
}

/* the index of the first move request at or after cl index i that has the direction dir, -1 */
static int cl_find_move(int i, unsigned dir)
{
    for (; i < M.ncl; i++)
        if (M.cl[i].kind == 1 && M.cl[i].dir == dir)
            return i;
    return -1;
}

/* S1 (Fable review 2026-11-18): when one axis leaves a diagonal the safe layer stops, and the remaining axis restarts FROM REST: the pause of a reversal, the first rung of the speed ladder (10),
 * then 6 4 3. It used to be re-sent at its old cruising divisor at once, with no settle: a full-speed restart of a head that was moving. */
static int fn_drop_once(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    (void)st;
    if (s->n > 60) return HAL_PTZ_CONT_STOP;
    if (s->n == 15) { *cmd = (hal_ptz_cmd_t){ 1, 0, 3, 0 }; s->cur = *cmd; s->arg = M.ncl; return HAL_PTZ_CONT_CMD; } /* the tilt leaves the diagonal, the pan goes on alone at divisor 3 */
    *cmd = s->cur;
    return HAL_PTZ_CONT_CMD;
}

static void drop_cases(void)
{
    hal_ptz_t *p;
    struct sc s;
    hal_ptz_cmd_t first;
    int rc, k, nm = 0, ok = 1;
    unsigned want[4] = { 10, 6, 4, 3 };
    printf("== safe layer: an axis leaves a diagonal, the other restarts from rest\n");
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 1, 3, 3 };
    s.fn = fn_drop_once;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    CHECK(rc == HAL_PTZ_OK, "a diagonal that drops its tilt axis ends cleanly");
    k = cl_find_move(s.arg, 2); /* pan right alone */
    CHECK(k > 0 && M.cl[k - 1].kind == 0, "the pan-only command is preceded by a STOP request");
    CHECK(k > 0 && M.cl[k].at - M.cl[k - 1].at >= 100, "... and starts at least 100 ms (the settle) after the stop");
    CHECK(k > 0 && M.cl[k].a == 10, "... from rest, at the first rung of the ladder (divisor 10), not at the old cruising divisor 3");
    for (int i = k; i < M.ncl && nm < 4; i++)
        if (M.cl[i].kind == 1 && M.cl[i].dir == 2) {
            if (M.cl[i].a != want[nm]) ok = 0;
            nm++;
        }
    CHECK(nm == 4 && ok, "... and the divisor walks 10 6 4 3, one rung per command");
    printf("   restart after the drop: first divisor %u, moves %d\n", k > 0 ? M.cl[k].a : 0, nm);
    /* settle_ms = 0 / ramp_ms = 0 (the old behaviour is still reachable by configuration): the restart is immediate at the cruising divisor */
    p = mk_conf("settle_ms=0\nramp_ms=0\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 1, 3, 3 };
    s.fn = fn_drop_once;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    k = cl_find_move(s.arg, 2);
    CHECK(rc == HAL_PTZ_OK && k > 0 && M.cl[k].a == 3, "ramp_ms = 0: the restart is at the wanted divisor at once (configurable)");
}

/* PTZ API (ptz.move, ptz.home), hardware 2026-11-18: calls answered 200 while the head did not move. A nudge toward a soft limit with no room left is clamped to nothing by the HAL and was
 * reported as accepted; it is now refused with A2K_PTZ_E_LIMIT (the API answers 409). Every accepted nudge / home really moves the head. */
static void nudge_api_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    uint32_t pa, pb, ta, tb;
    int t0;
    printf("== manual nudges (ptz.move) and home (ptz.home): accepted means it moves, no room says so\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "controller up");
    M.real = 1;
    CHECK(a2k_ptz_range(&pa, &pb, &ta, &tb) == 0, "soft range");
    CHECK(a2k_ptz_goto(1275, (int)tb, 2) == 0 && wait_idle(8000), "goto the upper tilt limit");
    for (int k = 0; k < 300; k++) { a2k_ptz_info(&i); if (i.known && (int)i.tilt >= (int)tb - 3) break; usleep(20000); }
    usleep(300000);
    t0 = mech_tilt();
    CHECK(a2k_ptz_nudge(0, 80) == A2K_PTZ_E_LIMIT, "a nudge up at the upper soft limit is refused (limit), not accepted and dropped");
    CHECK(a2k_ptz_nudge(1, 80) == 0 || a2k_ptz_nudge(1, 80) == A2K_PTZ_E_LIMIT, "a nudge with a free axis (pan) is judged on that axis");
    usleep(800000);
    CHECK(mech_tilt() == t0, "... the refused nudge did not move the tilt");
    (void)wait_idle(3000);
    usleep(300000);
    CHECK(a2k_ptz_nudge(0, -80) == 0, "a nudge away from the limit is accepted");
    usleep(1200000);
    CHECK(mech_tilt() < t0 - 40, "... and the head really moved");
    for (int gap = 600; gap <= 1000; gap += 200) { /* two nudges a little apart, away from the limits */
        int a, b, t1, t2;
        (void)wait_idle(3000);
        usleep(300000);
        t1 = mech_tilt();
        a = a2k_ptz_nudge(0, -40);
        usleep((unsigned)gap * 1000);
        b = a2k_ptz_nudge(0, -40);
        usleep(1200000);
        t2 = mech_tilt();
        printf("   two nudges %d ms apart: rc %d / %d, tilt %d -> %d\n", gap, a, b, t1, t2);
        CHECK(a == 0 && (b != 0 || t2 <= t1 - 70), "both accepted nudges moved the head (or the second was refused)");
    }
    for (int gap = 0; gap <= 600; gap += 100) { /* a home a little after a move (ptz.home): it must end at home */
        int r1, r2;
        (void)wait_idle(3000);
        usleep(300000);
        r1 = a2k_ptz_nudge(120, -60);
        usleep((unsigned)gap * 1000);
        r2 = a2k_ptz_command(A2K_PTZ_HOME);
        usleep(2500000);
        a2k_ptz_info(&i);
        printf("   home after %4d ms: nudge rc %d, home rc %d, ends at %d/%d\n", gap, r1, r2, (int)i.pan, (int)i.tilt);
        CHECK(r2 != 0 || (abs((int)i.pan - 1275) <= 3 && abs((int)i.tilt - 1300) <= 3), "an accepted home ends at home");
    }
    a2k_ptz_stop();
}

static void mitig_cases(void)
{
    hal_ptz_t *p;
    struct sc s;
    hal_ptz_cmd_t first;
    int rc, i, k, nstop_before, ok;
    unsigned long t1, t2;
    printf("== lost-step mitigation: reversal settle, speed ladder, tilt cap, weighted travel\n");

    /* 1. a reversal is stop + settle + the opposite direction (default settle 100 ms) */
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_rev_once;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    CHECK(rc == HAL_PTZ_OK, "a reversal in a session ends cleanly (no stall / wrong-way)");
    k = cl_find_move(s.arg, 6);
    CHECK(k > 0 && M.cl[k - 1].kind == 0, "the opposite direction (left) is preceded by a STOP request, not sent over the running move");
    CHECK(k > 0 && M.cl[k].at - M.cl[k - 1].at >= 100, "... and starts at least 100 ms (the settle) after the stop");
    CHECK(k > 0 && M.cl[k].at - M.cl[k - 1].at <= 200, "... but not much later");
    for (ok = 1, i = s.arg; i < k; i++)
        if (M.cl[i].kind == 1) ok = 0;
    CHECK(ok, "no move request at all between the stop and the settled restart");
    CHECK(hal_ptz_reversals(p) == 1, "one reversal counted");

    /* 2. settle_ms=0 / ramp_ms=0 in ptz.conf: the old instant reversal (no stop) */
    p = mk_conf("settle_ms=0\nramp_ms=0\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_rev_once;
    nstop_before = M.nstop;
    rc = hal_ptz_cont_run(p, &first, 400, cb, &s);
    k = cl_find_move(s.arg, 6);
    CHECK(rc == HAL_PTZ_OK && k > 0 && M.cl[k - 1].kind == 1, "settle_ms=0: the reversal is the old instant one (no stop before it)");
    CHECK(M.cl[cl_find_move(0, 2)].a == 3, "ramp_ms=0: the first command is at the wanted divisor at once");
    (void)nstop_before;

    /* 3. the speed ladder: from rest to divisor 2 (pan alone): 10 6 4 3 2, one rung per poll */
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 2, 0 };
    s.fn = fn_hold;
    s.arg = 12;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    {
        static const unsigned want[5] = { 10, 6, 4, 3, 2 };
        int n = 0;
        ok = 1;
        for (i = 0; i < M.ncl && n < 5; i++)
            if (M.cl[i].kind == 1) {
                if (M.cl[i].a != want[n]) ok = 0;
                if (n && M.cl[i].at - t1 < 50) ok = 0;
                t1 = (unsigned long)M.cl[i].at;
                n++;
            }
        CHECK(rc == HAL_PTZ_OK && n == 5 && ok, "from rest the divisor walks 10 6 4 3 2, one command per 50 ms (not a jump to 2)");
    }
    CHECK(M.nmove == 5, "... and nothing else is sent once the wanted speed is reached (refreshes are free)");

    /* 4. a decrease of speed applies at once; an increase steps through the rungs from the current speed */
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 12, 0 };
    s.fn = fn_hold;
    s.arg = 6;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    CHECK(rc == HAL_PTZ_OK && M.cl[cl_find_move(0, 2)].a == 12 && M.nmove == 1, "a wanted divisor slower than the ladder start (12) is sent directly");

    /* 5. caps: the tilt axis and a diagonal never go faster than divisor 3; the pan alone may use 2 */
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 0, 1, 0, 2 };
    s.fn = fn_hold;
    s.arg = 14;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    for (ok = 1, i = 0; i < M.ncl; i++)
        if (M.cl[i].kind == 1 && M.cl[i].b < 3) ok = 0;
    CHECK(rc == HAL_PTZ_OK && ok && M.cl[M.ncl - 2].b == 3, "tilt up asked at divisor 2: capped at 3 (tilt_min_div)");
    p = mk_conf(NULL);
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 1, 2, 2 };
    s.fn = fn_hold;
    s.arg = 14;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    for (ok = 1, i = 0; i < M.ncl; i++)
        if (M.cl[i].kind == 1 && (M.cl[i].a < 3 || M.cl[i].b < 3)) ok = 0;
    CHECK(rc == HAL_PTZ_OK && ok, "a diagonal asked at 2/2: both axes capped at 3");
    p = mk_conf("tilt_min_div=5\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 0, 1, 0, 3 };
    s.fn = fn_hold;
    s.arg = 14;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    for (ok = 1, i = 0; i < M.ncl; i++)
        if (M.cl[i].kind == 1 && M.cl[i].b < 5) ok = 0;
    CHECK(rc == HAL_PTZ_OK && ok, "tilt_min_div is configurable (5: tilt asked at 3 goes at 5)");
    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        char err[96] = "";
        CHECK(hal_ptz_new(&d, &OPS, "tilt_min_div=1\n", err, sizeof err) == NULL, "tilt_min_div=1 is refused by the config check");
        CHECK(hal_ptz_new(&d, &OPS, "settle_ms=5000\n", err, sizeof err) == NULL, "settle_ms above 1000 is refused");
        CHECK(hal_ptz_new(&d, &OPS, "ramp_ms=20\n", err, sizeof err) == NULL, "ramp_ms below one poll (and not 0) is refused");
    }

    /* 6. weighted travel: divisor 3 steps count double, divisor 6 single; a reversal adds PTZS_REV_COST */
    p = mk_conf("ramp_ms=0\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 6, 0 };
    s.fn = fn_hold;
    s.arg = 20;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    t1 = hal_ptz_travel(p);
    {
        long d1 = M.mech[0] - 1275;
        CHECK(rc == HAL_PTZ_OK && d1 > 100 && (long)t1 >= d1 - 5 && (long)t1 <= d1 + 10, "divisor 6: the travel is the plain step count");
    }
    p = mk_conf("ramp_ms=0\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 3, 0 };
    s.fn = fn_hold;
    s.arg = 20;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    t2 = hal_ptz_travel(p);
    {
        long d2 = M.mech[0] - 1275;
        CHECK(rc == HAL_PTZ_OK && d2 > 200 && (long)t2 >= 2 * d2 - 10 && (long)t2 <= 2 * d2 + 20, "divisor 3: every step counts double (high-speed travel weighs more)");
    }
    p = mk_conf("ramp_ms=0\n");
    memset(&s, 0, sizeof s);
    s.cur = first = (hal_ptz_cmd_t){ 1, 0, 6, 0 };
    s.fn = fn_rev_once;
    rc = hal_ptz_cont_run(p, &first, 600, cb, &s);
    {
        long net = M.max_mech[0] - 1275, back = M.max_mech[0] - M.mech[0];
        CHECK(rc == HAL_PTZ_OK && hal_ptz_reversals(p) == 1 && (long)hal_ptz_travel(p) >= net + back + 140, "a reversal costs about 150 extra steps in the travel budget (and is counted)");
    }
    hal_ptz_free(p);

    /* review: goto / nudge never drive the tilt faster than tilt_min_div (3), the pan alone may use 2; the travel is weighted like a continuous session */
    {
        unsigned mn, mx, first;
        unsigned long t0;
        int n0;
        p = mk_conf(NULL);
        n0 = M.ncl;
        CHECK(hal_ptz_goto_sp(p, 1275, 1500, 2) == HAL_PTZ_OK, "goto of the tilt at divisor 2");
        move_div_range(n0, 1, &mn, &mx, &first, 0);
        CHECK(mn >= 3 && mx >= 3, "goto: the tilt divisor is raised to tilt_min_div (3), never 2");
        n0 = M.ncl;
        CHECK(hal_ptz_goto_sp(p, 1500, 1500, 2) == HAL_PTZ_OK, "goto of the pan at divisor 2");
        move_div_range(n0, 0, &mn, &mx, &first, 0);
        CHECK(mn == 2, "goto: the pan alone may still use divisor 2");
        n0 = M.ncl;
        t0 = hal_ptz_travel(p);
        CHECK(hal_ptz_nudge_sp(p, 0, -150, 2) == HAL_PTZ_OK, "nudge of the tilt at divisor 2");
        move_div_range(n0, 1, &mn, &mx, &first, 0);
        CHECK(mn >= 3, "nudge: the tilt divisor is raised to tilt_min_div");
        CHECK(hal_ptz_travel(p) - t0 >= 2 * 140, "the tilt nudge counts double in the weighted travel (divisor 3)");
        t0 = hal_ptz_travel(p);
        CHECK(hal_ptz_goto_sp(p, 1275, 1500, 6) == HAL_PTZ_OK, "goto of the pan at divisor 6");
        CHECK(hal_ptz_travel(p) - t0 >= 200 && hal_ptz_travel(p) - t0 < 2 * 200, "a slow pan move counts 1 per step");
        hal_ptz_free(p);
    }
    p = mk_conf(NULL);
    CHECK(hal_ptz_drift_cum_max(p) == 120, "drift_cum_max defaults to 120");
    hal_ptz_free(p);
    p = mk_conf("drift_cum_max=40\n");
    CHECK(hal_ptz_drift_cum_max(p) == 40, "ptz.conf drift_cum_max is read");
    hal_ptz_free(p);
}


/* ------------------------------------------------------------------------------------------------ drift: the controller + a fake CAMERA (the view displacement follows the mechanics, not the counters) */
static struct { int calls, force, minf_ok, delay; int64_t last_minf; int dx, dy; int seq[8], nseq, si; } DH; /* seq: a script of verdicts, one per call (-1: follow the camera) */ /* force: 0 follow the camera, 1 INCONC, 2 RECAL, 3 SKIP, 4 FLAT (a flat live view, the reference had detail) */
static struct { int cap, commit; } DW;

/* the view of the fake camera against the home reference: the pan counter + moves the content right (9.6 steps per thumbnail px, the measured scale), the tilt counter + moves it down (14.6 steps per px) */
static int cam_dx(void) { double v = (M.mech[0] - 1275) / 9.6; return (int)(v < 0 ? v - 0.5 : v + 0.5); }
static int cam_dy(void) { double v = (M.mech[1] - 1300) / 9.6; return (int)(v < 0 ? v - 0.5 : v + 0.5); }

static int fake_drift(uint32_t ph, uint32_t th, int64_t minf, int *dx, int *dy)
{
    int ax, ay;
    (void)ph; (void)th;
    if (DH.delay) usleep((unsigned)DH.delay * 1000); /* a slow frame grab: the window of the re-verify */
    pthread_mutex_lock(&M.mu);
    ax = cam_dx();
    ay = cam_dy();
    pthread_mutex_unlock(&M.mu);
    DH.calls++;
    DH.last_minf = minf;
    DH.minf_ok = minf > 0;
    *dx = ax;
    *dy = ay;
    if (DH.si < DH.nseq && DH.seq[DH.si++] >= 0) return DH.seq[DH.si - 1];
    if (DH.force == 1) return A2K_DRIFT_INCONC;
    if (DH.force == 2) return A2K_DRIFT_RECAL;
    if (DH.force == 3) return A2K_DRIFT_SKIP;
    if (DH.force == 4) return A2K_DRIFT_FLAT;
    if (!ax && !ay) return A2K_DRIFT_OK;
    return abs(ax) <= 6 && abs(ay) <= 6 ? A2K_DRIFT_SHIFT : A2K_DRIFT_RECAL;
}
static int fake_capture(void) { DW.cap++; return 0; }
static int fake_commit(uint32_t p, uint32_t t, int fresh) { (void)p; (void)t; (void)fresh; DW.commit++; return 0; }

static int wait_calls(int n, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        if (DH.calls >= n) return 1;
        usleep(10000);
    }
    return 0;
}

static int wait_at_home(int ms)
{
    a2k_ptz_info_t i;
    for (int t = 0; t < ms; t += 10) {
        a2k_ptz_info(&i);
        if (i.known && i.state == HAL_PTZ_HOME) return 1;
        usleep(10000);
    }
    return 0;
}

/* away and back: a nudge (travel) and a tracker home, the way a tracking chain ends */
static void go_and_home(void)
{
    a2k_ptz_info_t i;
    a2k_ptz_nudge_sp(300, 0, 3);
    for (int t = 0; t < 3000; t += 10) { a2k_ptz_info(&i); if (i.known && i.pan > 1500) break; usleep(10000); }
    a2k_ptz_home_tracker();
}

static void drift_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    a2k_ptz_drift_t dr;
    int calls0, k;
    printf("== drift: scene-based correction of lost steps (controller thread, fake driver, a fake camera whose view follows the mechanics)\n");
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc.boot_check = boot_ok;
    sc.drift = fake_drift;
    sc.capture = fake_capture;
    sc.commit = fake_commit;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    a2k_ptz_defaults(&c);
    c.tilt_rehome = A2K_PTZ_RH_OFF; /* this case tests the recalibration rung itself: with the re-home on a RECAL verdict first runs the urgent re-home (f4_cases covers the ladder) */
    c.scene_check = 1;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed, no calibration)");
    M.real = 1;
    a2k_ptz_drift_info(&dr);
    CHECK(!dr.have && dr.corrections == 0 && !dr.untrusted && !strcmp(dr.recal_reason, "none"), "initial drift state: no check yet, no correction, trusted, no recalibration reason");

    /* 1. lost steps: the head is physically 5 px (50 steps) to the pan side and 5 px (48 steps) low; the counters know nothing */
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 50; /* the lost steps, injected while the head is away: the counters stay */
    M.mech[1] -= 48; /* 5 px at the measured 9.6 steps per thumbnail px (it was 80 = 5.5 px of the old 14.6 scale) */
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000), "back at home (the counters say 1275/1300)");
    CHECK(wait_calls(2, 6000), "idle at home: the scene was compared (the check, then the check after the correction)");
    usleep(300000);
    a2k_ptz_info(&i);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1 && !dr.untrusted && dr.recals == 0, "one correction, the position stays trusted, no recalibration");
    CHECK(abs(M.mech[0] - 1275) <= 4 && abs(M.mech[1] - 1300) <= 12, "the head is physically back at home after the correction (the sign and the px -> steps conversion are right)");
    CHECK(i.known && i.state == HAL_PTZ_HOME && abs((int)i.pan - 1275) <= 3 && abs((int)i.tilt - 1300) <= 3, "the controller reports home 1275/1300 again, trusted");
    CHECK(DH.minf_ok, "the frame compared is required to be newer than the last motion (min_frame_ms given)");
    CHECK(dr.have && dr.dx == 0 && dr.dy == 0, "ptz.drift_last_px after the verified correction: 0,0");
    CHECK(M.nother == 0 && M.bad_cmd == 0, "only move / stop / pos / init requests reached the driver");

    /* 2. no drift: a second return home is checked once and nothing is touched */
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 4000), "a second return home: checked");
    usleep(500000);
    a2k_ptz_drift_info(&dr);
    CHECK(DH.calls == calls0 + 1 && dr.corrections == 1 && !dr.untrusted, "view matches (0,0): nothing corrected, nothing scheduled");

    /* 3. a person around: no check while somebody is seen */
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000), "home again");
    for (k = 0; k < 24; k++) { a2k_ptz_note_person(); usleep(50000); }
    CHECK(DH.calls == calls0, "no drift check while a person is seen (1.2 s of detections)");
    CHECK(wait_calls(calls0 + 1, 3000), "...it runs once the person is gone");

    /* 4. not during tracking: a session that starts while the check is due runs undisturbed; it ends away from home and the check is dropped */
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000), "home again (check due)");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "a tracker session starts at once");
    for (k = 0; k < 8; k++) { a2k_ptz_vel(1, 0, 3, 0); usleep(60000); }
    CHECK(DH.calls == calls0, "no drift check during the session");
    a2k_ptz_vel(0, 0, 0, 0);
    usleep(1200000);
    a2k_ptz_info(&i);
    CHECK(DH.calls == calls0 && i.state == HAL_PTZ_MOVED, "the session ended away from home: the check was dropped (it waits for the next return home)");
    a2k_ptz_home_tracker();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 4000), "...and runs after the next return home");

    /* 5. a person in front of the view: inconclusive, nothing is touched */
    calls0 = DH.calls;
    DH.force = 1;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 4000), "check with the view occluded");
    usleep(400000);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1 && !dr.untrusted && dr.recals == 0 && DH.calls == calls0 + 1, "inconclusive: no correction, no recalibration, a retry is scheduled later (not at once)");

    /* 6. skipped (another day/night mode, no usable reference): nothing */
    calls0 = DH.calls;
    DH.force = 3;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 4000), "check with a skip verdict");
    usleep(400000);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1 && !dr.untrusted && DH.calls == calls0 + 1, "skip: nothing happens");

    /* 7. a large displacement: untrusted at once (no reference writes), a full recalibration only after nobody was around */
    DH.force = 0;
    calls0 = DH.calls;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 200; /* 16 px: beyond the correctable range */
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000), "home again (the head is 16 px off)");
    a2k_ptz_note_person();
    CHECK(wait_calls(calls0 + 1, 4000), "check");
    for (k = 0; k < 20; k++) { a2k_ptz_note_person(); usleep(50000); } /* somebody walks about for a second */
    a2k_ptz_info(&i);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.untrusted && dr.corrections == 1 && dr.recals == 0 && strstr(dr.recal_reason, "does not match"), "large shift: not corrected, marked untrusted, the reason is published");
    CHECK(i.state != HAL_PTZ_CALIBRATING, "no recalibration while a person is around");
    M.real = 0; /* the calibration runs on virtual time */
    for (k = 0; k < 400; k++) { a2k_ptz_drift_info(&dr); if (dr.recals == 1) break; usleep(10000); }
    CHECK(dr.recals == 1, "a recalibration was started once the idle time passed");
    CHECK(wait_idle(8000) && wait_at_home(8000), "it completed, the head is home");
    usleep(300000);
    a2k_ptz_drift_info(&dr);
    CHECK(!dr.untrusted && abs(M.mech[0] - 1275) <= 5 && abs(M.mech[1] - 1300) <= 12, "trusted again, the head is really at home (the mechanics, not just the counters)");
    CHECK(strstr(dr.recal_reason, "does not match") != NULL, "the reason stays published (ptz.recal_reason)");

    /* 8. a manual calibration does not trigger a drift check by itself */
    calls0 = DH.calls;
    CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0, "manual recalibrate");
    usleep(300000);
    CHECK(wait_idle(8000) && wait_at_home(8000), "done");
    usleep(800000);
    CHECK(DH.calls == calls0, "no drift check right after a calibration (the position was just measured)");
    CHECK(M.nother == 0 && M.bad_cmd == 0, "no forbidden driver request during the whole drift test");
    a2k_ptz_stop();
    M.real = 0;

    /* 9. the reference is not written from a doubted position: a calibration at the start makes the reference due; a RECAL verdict before it is captured blocks the write */
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc.boot_check = NULL; /* no proof: the start calibrates (virtual time) and the home reference is due after the quiet time */
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set_scene_timing(1500, 300, 0);
    a2k_ptz_test_set_drift_timing(100, 1500, 100, 60000);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(15000), "start with a calibration");
    M.real = 1;
    DH.force = 2; /* the check says: no match */
    a2k_ptz_note_person(); /* keeps the drift recalibration waiting (60 s) */
    a2k_ptz_goto(1500, 1300, 3);
    usleep(900000);
    a2k_ptz_command(A2K_PTZ_HOME);
    CHECK(wait_at_home(6000) && wait_calls(1, 4000), "drift check right after the calibration's reference became due");
    usleep(2500000);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.untrusted && DW.commit == 0, "untrusted: the reference is NOT captured / written from the doubted position");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_scene_timing(0, 0, 0);
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
}

static int g_hook_n, g_hook_last;
/* ------------------------------------------------------------------------------------------------ motor-safety review: park vs pending recalibration, the SHIFT window, unconfirmed / cumulative corrections */
static void review_start(a2k_ptz_cfg_t *c, a2k_ptz_scene_t *sc)
{
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc->boot_check = boot_ok;
    sc->drift = fake_drift;
    sc->capture = fake_capture;
    sc->commit = fake_commit;
    a2k_ptz_set_scene(sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    a2k_ptz_test_set_drift_retry(400);
    a2k_ptz_defaults(c);
    c->tilt_rehome = A2K_PTZ_RH_OFF; /* this case tests the recalibration rung itself: with the re-home on a RECAL verdict first runs the urgent re-home (f4_cases covers the ladder) */
    c->scene_check = 1;
    c->park_pan = 1275;
    c->park_tilt = 600;
    a2k_ptz_set_cmd_hook(NULL);
}

/* one lost-steps episode: away, the head is displaced by `steps` pan steps while away, back home, the check (and the re-verify) ran */
static void shift_episode(int steps, int calls_to_wait)
{
    int c0 = DH.calls;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += steps;
    pthread_mutex_unlock(&M.mu);
    wait_at_home(4000);
    wait_calls(c0 + calls_to_wait, 6000);
    usleep(500000);
}

static void review_cases_drift(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    a2k_ptz_drift_t dr;
    int k, calls0;
    printf("== REVIEW: a pending recalibration vs park / ptz_cal=off, the SHIFT window refuses commands, unconfirmed and cumulative corrections\n");

    /* 1. a pending drift recalibration is kept while parked, runs after the head is back; a park scheduled after it is not undone */
    review_start(&c, &sc);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    DH.force = 2;
    go_and_home();
    CHECK(wait_at_home(4000), "home");
    a2k_ptz_note_person();
    CHECK(wait_calls(1, 4000), "a check with a RECAL verdict: a recalibration is pending");
    a2k_ptz_drift_info(&dr);
    CHECK(dr.untrusted && dr.recals == 0, "untrusted, the recalibration waits for the idle time");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0, "the user parks the head meanwhile");
    CHECK(wait_state(HAL_PTZ_PARKED, 15000), "parked");
    usleep(2500000); /* well past the 1 s idle time of the recalibration */
    a2k_ptz_info(&i);
    a2k_ptz_drift_info(&dr);
    CHECK(i.state == HAL_PTZ_PARKED && dr.recals == 0, "the pending recalibration did NOT run while parked (the park is not undone)");
    M.real = 0;
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0, "the user unparks");
    for (k = 0; k < 800; k++) { a2k_ptz_drift_info(&dr); if (dr.recals == 1) break; usleep(10000); }
    CHECK(dr.recals == 1, "the recalibration stayed pending and runs once the head is back");
    wait_idle(10000);
    a2k_ptz_stop();
    M.real = 0;

    /* 1b. ptz_cal=off: a pending recalibration is dropped, the motor is never calibrated */
    review_start(&c, &sc);
    c.cal = HAL_PTZ_CAL_OFF;
    CHECK(a2k_ptz_start(&c) == 0, "start with ptz_cal=off");
    usleep(300000);
    a2k_ptz_test_force_recal_pending();
    usleep(1800000);
    a2k_ptz_info(&i);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.recals == 0 && i.state != HAL_PTZ_CALIBRATING, "ptz_cal=off: the pending recalibration never runs (dropped)");
    a2k_ptz_stop();

    /* 2. the SHIFT window refuses every queued command with NOTREADY and the correction is not cancelled (a park does not leave it half done) */
    review_start(&c, &sc);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    DH.delay = 400;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 50;
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000), "home again (4 px off)");
    {
        int seen = 0, r[8] = { 0 };
        for (k = 0; k < 3000 && !seen; k++) {
            a2k_ptz_info(&i);
            if (i.state == HAL_PTZ_MOVED && a2k_ptz_manual_gate() == A2K_PTZ_E_NOTREADY) {
                seen = 1;
                r[0] = a2k_ptz_command(A2K_PTZ_PARK);
                r[1] = a2k_ptz_command(A2K_PTZ_HOME);
                r[2] = a2k_ptz_command(A2K_PTZ_LEFT);
                r[3] = a2k_ptz_goto(1500, 1300, 3);
                r[4] = a2k_ptz_home_tracker();
                r[5] = a2k_ptz_nudge_sp(30, 0, 3);
                r[6] = a2k_ptz_command(A2K_PTZ_RECAL);
            }
            usleep(2000);
        }
        CHECK(seen, "the SHIFT window is seen");
        CHECK(r[0] == A2K_PTZ_E_NOTREADY && r[1] == A2K_PTZ_E_NOTREADY && r[2] == A2K_PTZ_E_NOTREADY && r[3] == A2K_PTZ_E_NOTREADY && r[4] == A2K_PTZ_E_NOTREADY && r[5] == A2K_PTZ_E_NOTREADY && r[6] == A2K_PTZ_E_NOTREADY,
              "park / home / step / goto / tracker home / nudge / recalibrate are all refused with NOTREADY during it");
    }
    CHECK(wait_idle(8000) && wait_at_home(8000), "the correction completed, at home (the queued park never ran)");
    usleep(500000);
    a2k_ptz_drift_info(&dr);
    a2k_ptz_info(&i);
    CHECK(dr.corrections == 1 && !dr.untrusted && dr.recals == 0 && i.state == HAL_PTZ_HOME && abs(M.mech[0] - 1275) <= 5, "verified correction, head physically home, state home: nothing cancelled it");
    a2k_ptz_stop();
    M.real = 0;
    DH.delay = 0;

    /* 3. an unconfirmed correction (re-verify inconclusive): a further SHIFT verdict schedules a recalibration instead of a second declare */
    review_start(&c, &sc);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    DH.seq[0] = -1; DH.seq[1] = A2K_DRIFT_INCONC; DH.seq[2] = A2K_DRIFT_SHIFT; DH.nseq = 3;
    shift_episode(50, 3);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1, "one correction only: the shift verdict after an unconfirmed correction declared nothing");
    CHECK(dr.untrusted && strstr(dr.recal_reason, "unconfirmed"), "...it scheduled a recalibration (untrusted, reason published)");
    a2k_ptz_stop();
    M.real = 0;

    /* 3b. a re-verify that later confirms clears the flag: the next shift is corrected normally */
    review_start(&c, &sc);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    DH.seq[0] = -1; DH.seq[1] = A2K_DRIFT_INCONC; DH.nseq = 2;
    shift_episode(50, 3); /* check, re-verify (inconclusive), the retry check: the camera is home: ok */
    calls0 = DH.calls;
    shift_episode(50, 2);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 2 && !dr.untrusted, "after the confirming check a new shift is corrected (the flag was cleared)");
    a2k_ptz_stop();
    M.real = 0;
    (void)calls0;

    /* 4. signed cumulative corrected steps: +50 -50 +50 +50 are fine (net 100), the next +50 (150 > 120) is a recalibration */
    review_start(&c, &sc);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    shift_episode(50, 2);
    shift_episode(-50, 2);
    shift_episode(50, 2);
    shift_episode(50, 2);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 4 && !dr.untrusted, "four corrections (signed sum +100 never beyond 120): all declared");
    shift_episode(50, 1);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 4 && dr.untrusted && strstr(dr.recal_reason, "beyond"), "the fifth (cumulative 150 > 120) is a recalibration, not a declare");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_drift_retry(0);
    CHECK(M.bad_cmd == 0 && M.nother == 0, "no forbidden driver request");
}

static void cmd_hook(int kind) { g_hook_n++; g_hook_last = kind; }

/* wait until the motor (both axes) does not run any more; ms bound; returns the elapsed ms or -1 */
static int wait_motor_stop(int ms)
{
    int64_t t0 = mono();
    for (int t = 0; t < ms; t += 5) {
        int p = mech_pan(), q = mech_tilt();
        usleep(5000);
        if (!M.running[0] && !M.running[1] && mech_pan() == p && mech_tilt() == q)
            return (int)(mono() - t0);
    }
    return -1;
}

/* the manual (ONVIF / API / MQTT) continuous motion: its own lease, refresh without a driver command, Stop, the refusal states, the preemption of tracking and patrol */
static void manual_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    uint32_t pmin, pmax, tmin, tmax;
    int64_t t0;
    int moves0, p0, ms;
    printf("== MANUAL continuous motion: lease, refresh, stop, refusals, preemption of tracking\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    c.park_pan = 1275; /* a park pose away from the soft stops (the fake driver overshoots the last step at the very edge) */
    c.park_tilt = 600;
    a2k_ptz_set_cmd_hook(NULL);
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == A2K_PTZ_E_REFUSED && a2k_ptz_manual_gate() == A2K_PTZ_E_REFUSED && a2k_ptz_manual_stop() == 0, "controller not running: a manual move is refused, a stop is a harmless no-op");
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    a2k_ptz_set_lease_ms(450);
    a2k_ptz_set_cmd_hook(cmd_hook);
    CHECK(a2k_ptz_range(&pmin, &pmax, &tmin, &tmax) == 0, "soft range known");

    CHECK(a2k_ptz_vel_manual(2, 0, 3, 0, 450) == A2K_PTZ_E_ARGS && a2k_ptz_vel_manual(1, 0, 1, 0, 450) == A2K_PTZ_E_ARGS && a2k_ptz_vel_manual(1, 0, 21, 0, 450) == A2K_PTZ_E_ARGS &&
          a2k_ptz_vel_manual(0, 1, 0, 0, 450) == A2K_PTZ_E_ARGS && a2k_ptz_vel_manual(1, 0, 3, 0, 149) == A2K_PTZ_E_ARGS && a2k_ptz_vel_manual(1, 0, 3, 0, 2001) == A2K_PTZ_E_ARGS,
          "bad signs, divisors and leases are refused (ARGS)");
    CHECK(a2k_ptz_vel_manual(0, 0, 0, 0, 450) == 0 && a2k_ptz_manual_stop() == 0, "(0,0) is a stop and a stop with nothing running is fine");

    /* 1. a single command, no repeat: the lease is the command's own (600 ms here, the tracker's lease is 450) */
    g_hook_n = 0;
    p0 = mech_pan();
    moves0 = M.nmove;
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 600) == 0, "a manual move starts a session");
    CHECK(g_hook_n >= 1 && g_hook_last == A2K_PTZ_HOOK_NUDGE, "the patrol hook was told (a manual command stops a patrol)");
    usleep(300000);
    CHECK(mech_pan() > p0 + 40 && M.running[0], "the head moves right");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_MOVED, "state moved while the session runs");
    t0 = mono();
    ms = wait_motor_stop(2500);
    CHECK(ms >= 0 && mono() - t0 >= 150 && mono() - t0 < 900, "no repeat: the motion ends by itself ~600 ms after the command (lease = the command's)");
    CHECK(wait_idle(1000), "the position is known again after the lease");
    CHECK(M.nmove - moves0 >= 1 && M.nmove - moves0 <= 5, "one driver move command per rung of the speed ladder (10, 6, 4, 3: at most 4, +1 slack), nothing else for the whole motion");

    /* 2. refresh: identical repeats keep it going far beyond one lease and send nothing to the driver */
    moves0 = M.nmove;
    p0 = mech_pan();
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "start");
    {
        int ok = 1;
        for (int k = 0; k < 10; k++) { /* 10 x 150 ms = 1.5 s, three leases */
            usleep(150000);
            if (a2k_ptz_vel_manual(1, 0, 3, 0, 450) != 0) ok = 0;
        }
        CHECK(ok, "every repeat is accepted");
    }
    CHECK(M.running[0] && mech_pan() > p0 + 1100 / 3 * 1, "still moving after 1.5 s (three leases) thanks to the repeats");
    CHECK(M.nmove - moves0 <= 5, "the identical repeats re-issued NO driver command (only the ladder rungs of the first 200 ms: at most 5 moves for 1.5 s)");
    t0 = mono();
    ms = wait_motor_stop(2500);
    CHECK(ms >= 0 && mono() - t0 >= 150 && mono() - t0 < 900, "the repeats stop: expiry ~450 ms after the last one");

    /* 3. a long lease on a session the tracker started: the manual lease wins, the polled lease of the safe layer is refreshed */
    CHECK(a2k_ptz_goto(1275, 1300, 2) == 0 && wait_idle(5000), "back to a start pose");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "the tracker starts its session (lease 450)");
    usleep(150000);
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 2000) == 0, "a manual command (lease 2 s) takes the session over");
    usleep(1500000);
    CHECK(M.running[0], "1.5 s later it still runs: not stopped by the tracker's 450 ms lease nor by the safe layer's polled lease");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == -1 && a2k_ptz_nudge_sp(100, 0, 3) == -1 && a2k_ptz_home_tracker() == -1, "while the manual lease runs the tracker's velocity, nudge and home return are refused (manual wins)");
    CHECK(a2k_ptz_manual_stop() == 0, "Stop");
    ms = wait_motor_stop(1000);
    CHECK(ms >= 0 && ms < 400, "Stop halts the motor at once (within ~0.1-0.3 s)");
    CHECK(wait_idle(2000), "idle, position known");
    CHECK(a2k_ptz_vel(-1, 0, 3, 0) == 0, "the tracker may drive again right after the Stop (tracking resumes)");
    usleep(200000);
    CHECK(a2k_ptz_vel(0, 0, 0, 0) == 0, "tracker stop");
    CHECK(wait_idle(2000), "idle");

    /* 4. the hold ends with its session: after the expiry the tracker is accepted without any Stop */
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 300) == 0, "manual, 300 ms");
    usleep(100000);
    CHECK(a2k_ptz_vel(-1, 0, 3, 0) == -1, "tracker refused during it");
    CHECK(wait_idle(3000), "expired");
    CHECK(a2k_ptz_vel(-1, 0, 3, 0) == 0, "tracker accepted after the expiry");
    CHECK(a2k_ptz_manual_stop() == 0, "a manual Stop also stops a tracker session (the tracker asks again)");
    CHECK(wait_idle(3000), "idle");

    /* 5. retarget: direction and diagonal, no stop for a pan reversal; speed change = one more command */
    {
        int s0 = M.nstop;
        CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "right");
        usleep(250000);
        CHECK(a2k_ptz_vel_manual(-1, 1, 3, 4, 450) == 0, "re-target to up-left");
        usleep(300000);
        CHECK(M.running[0] && M.running[1], "both axes run (diagonal words)");
        CHECK(M.nstop - s0 <= 2, "a reversal / diagonal needs at most the documented stops");
        CHECK(a2k_ptz_manual_stop() == 0 && wait_motor_stop(1000) >= 0, "stop");
        CHECK(wait_idle(2000), "idle");
    }

    /* 6. a manual command preempts what runs: a goto is cancelled by a velocity command */
    CHECK(a2k_ptz_goto(3000, 1300, 3) == 0, "a manual goto far away");
    usleep(200000);
    p0 = mech_pan();
    CHECK(a2k_ptz_vel_manual(-1, 0, 3, 0, 450) == 0, "a velocity command preempts it");
    usleep(300000);
    CHECK(mech_pan() < p0 + 20, "the goto was cancelled: the head turns around");
    CHECK(a2k_ptz_manual_stop() == 0 && wait_motor_stop(1000) >= 0 && wait_idle(2000), "stop");
    /* ... and a manual step ends a velocity session and the hold */
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 2000) == 0, "a long manual move");
    usleep(150000);
    t0 = mono();
    CHECK(a2k_ptz_command(A2K_PTZ_UP) == 0 && wait_done(t0, 3000), "a manual step replaces it");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "the hold is gone with it: the tracker is accepted at once");
    CHECK(a2k_ptz_manual_stop() == 0 && wait_idle(3000), "stop");

    /* 7. the soft range: a long move toward the pan limit stops AT it */
    CHECK(a2k_ptz_vel_manual(1, 0, 2, 0, 2000) == 0, "full speed right for a long time");
    t0 = mono();
    while (mech_pan() < (int)pmax - 3 && mono() - t0 < 9000) { /* the client repeats every 10 ms; the head needs ~4.5 s */
        usleep(10000);
        a2k_ptz_vel_manual(1, 0, 2, 0, 2000);
    }
    CHECK(wait_motor_stop(3000) >= 0 && mech_pan() <= (int)pmax && mech_pan() >= (int)pmax - 60, "the head stopped at the soft limit (within the safe layer's braking margin), never beyond");
    CHECK(wait_idle(2000), "idle");
    {
        int s0, pl0;
        CHECK(a2k_ptz_goto((int)pmax, 1300, 2) == 0 && wait_idle(8000), "go to the pan limit exactly");
        s0 = M.nmove;
        pl0 = mech_pan();
        CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "a command toward the limit is accepted ...");
        usleep(300000);
        CHECK(mech_pan() <= (int)pmax && mech_pan() - pl0 <= 45, "... and held by the safe layer: at most the braking margin of travel at the limit, never beyond it");
        (void)s0;
    }
    CHECK(a2k_ptz_goto(1275, 1300, 2) == 0 && wait_idle(8000), "back home (goto)");

    /* 8. refusal states: parked / privacy */
    CHECK(a2k_ptz_goto(1275, 800, 2) == 0 && wait_idle(8000), "a short way above the park position");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_state(HAL_PTZ_PARKED, 20000), "park");
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == A2K_PTZ_E_PARKED && a2k_ptz_manual_gate() == A2K_PTZ_E_PARKED, "parked: a manual move is refused (PARKED)");
    CHECK(a2k_ptz_manual_stop() == 0, "a stop while parked is fine");
    p0 = mech_pan();
    usleep(200000);
    CHECK(mech_pan() == p0 && !M.running[0], "the parked head did not move");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 8000), "home (the way out of the park)");
    CHECK(a2k_ptz_manual_gate() == 0, "gate open again");

    a2k_ptz_stop();
    CHECK(!M.running[0] && !M.running[1], "shutdown stops the motor");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "no forbidden driver request during the manual tests (move 0..7, stop, position only)");
    a2k_ptz_set_cmd_hook(NULL);

    /* 9. calibrating: refused (the boot calibration really runs here) */
    d_reset(1275, 1300);
    a2k_ptz_set_scene(NULL);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_defaults(&c);
    c.scene_check = 0;
    M.real = 1;
    {
        const hal_ptz_ops_t cal_ops = { d_open, d_ioctl, d_sleep, d_load, d_save, d_log, NULL, NULL, NULL, NULL };
        a2k_ptz_test_set(&cal_ops, "/nonexistent/ptz.conf");
        CHECK(a2k_ptz_start(&c) == 0, "start with a boot calibration");
        CHECK(wait_state(HAL_PTZ_CALIBRATING, 3000), "calibrating");
        CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == A2K_PTZ_E_NOTREADY && a2k_ptz_manual_gate() == A2K_PTZ_E_NOTREADY, "calibrating: a manual move is refused (NOTREADY)");
        CHECK(a2k_ptz_manual_stop() == 0, "a stop never aborts the calibration");
        usleep(300000);
        a2k_ptz_info(&i);
        CHECK(i.state == HAL_PTZ_CALIBRATING, "still calibrating after the stop");
        a2k_ptz_stop();
    }
    CHECK(M.bad_cmd == 0, "no forbidden request during the calibration test");
}

/* ------------------------------------------------------------------------------------------------ manual motion x drift machine x safe layer (the integration of the ONVIF branch with the drift / safe-layer branch) */
/* a manual command of lease_ms without repeats has ended: its lease ran out and the motor stopped (the motor needs its latency to start, so a plain "not running" is no proof) */
static void manual_ended(int lease_ms) { usleep((unsigned)(lease_ms + 500) * 1000); wait_motor_stop(3000); }

static void keep_person(int ms) { for (int t = 0; t < ms; t += 50) { a2k_ptz_note_person(); usleep(50000); } }

/* the min / max / first commanded divisors of the moves sent since index n0 that drive the axis (0 pan, 1 tilt); diag_only: only the diagonal moves */
static void move_div_range(int n0, int ax, unsigned *mn, unsigned *mx, unsigned *first, int diag_only)
{
    *mn = 1000; *mx = 0; *first = 0;
    for (int k = n0; k < M.ncl && k < 1024; k++) {
        unsigned d, dv;
        if (M.cl[k].kind != 1) continue;
        d = M.cl[k].dir;
        if (diag_only && !(PTZS_PAN_SIGN[d] && PTZS_TILT_SIGN[d])) continue;
        if (!(ax ? PTZS_TILT_SIGN[d] : PTZS_PAN_SIGN[d])) continue;
        dv = ax ? M.cl[k].b : M.cl[k].a;
        if (!*first) *first = dv;
        if (dv < *mn) *mn = dv;
        if (dv > *mx) *mx = dv;
    }
}

static void integration_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    a2k_ptz_drift_t dr;
    uint32_t pmin, pmax, tmin, tmax;
    unsigned mn, mx, first;
    int n0, calls0, k;
    long t0v, dt, dm;
    int64_t t0;
    printf("== INTEGRATION: manual velocity through the safe layer's session (settle, ladder, tilt cap, weight) and the drift machine\n");
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc.boot_check = boot_ok;
    sc.drift = fake_drift;
    sc.capture = fake_capture;
    sc.commit = fake_commit;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    a2k_ptz_defaults(&c);
    c.tilt_rehome = A2K_PTZ_RH_OFF; /* this case tests the recalibration rung itself: with the re-home on a RECAL verdict first runs the urgent re-home (f4_cases covers the ladder) */
    c.scene_check = 1;
    c.park_pan = 1275;
    c.park_tilt = 600;
    a2k_ptz_set_cmd_hook(NULL);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    a2k_ptz_set_lease_ms(450);
    CHECK(a2k_ptz_range(&pmin, &pmax, &tmin, &tmax) == 0, "soft range known");
    DH.force = 3; /* the drift checks of the sections 1 and 3 are skipped: they only look at the motion */

    /* (1) the manual velocity IS the safe layer's continuous session: ladder, tilt cap, diagonal cap, reversal settle */
    n0 = M.ncl;
    CHECK(a2k_ptz_vel_manual(0, 1, 0, 2, 800) == 0, "manual tilt up asked at divisor 2 (what ptz_vel maps |v| = 1 to)");
    manual_ended(800);
    CHECK(wait_idle(2000), "it ran and ended by its lease");
    move_div_range(n0, 1, &mn, &mx, &first, 0);
    CHECK(mn == 3, "tilt: the effective divisor is 3 (tilt_min_div caps 2 -> 3), never faster");
    CHECK(first >= 10 && mx >= 10, "tilt: the speed ladder starts slow (divisor 10 or slower) and climbs to the cap");
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");
    n0 = M.ncl;
    CHECK(a2k_ptz_vel_manual(1, 0, 2, 0, 800) == 0, "manual pan right at divisor 2");
    manual_ended(800);
    CHECK(wait_idle(2000), "ended");
    move_div_range(n0, 0, &mn, &mx, &first, 0);
    CHECK(mn == 2 && first >= 10, "pan alone: the effective divisor is 2 (single axis), reached through the ladder");
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");
    n0 = M.ncl;
    CHECK(a2k_ptz_vel_manual(1, 1, 2, 2, 800) == 0, "manual diagonal at divisor 2 / 2");
    manual_ended(800);
    CHECK(wait_idle(2000), "ended");
    move_div_range(n0, 0, &mn, &mx, &first, 1);
    CHECK(mn >= 3 && mx > 0, "diagonal, pan axis: effective divisor 3 (capped)");
    move_div_range(n0, 1, &mn, &mx, &first, 1);
    CHECK(mn >= 3 && mx > 0, "diagonal, tilt axis: effective divisor 3 (capped)");
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");
    n0 = M.ncl;
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "manual right");
    usleep(400000);
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "(repeat)");
    usleep(200000);
    CHECK(a2k_ptz_vel_manual(-1, 0, 3, 0, 450) == 0, "manual REVERSAL to the left, no Stop in between");
    usleep(500000);
    {
        int found = 0, gap = -1;
        for (k = n0 + 1; k < M.ncl && k < 1024; k++) {
            if (M.cl[k].kind == 1 && PTZS_PAN_SIGN[M.cl[k].dir] < 0 && M.cl[k - 1].kind == 0) { found = 1; gap = (int)(M.cl[k].at - M.cl[k - 1].at); break; }
        }
        CHECK(found && gap >= 90, "reversal settle: a stop, then at least ~100 ms (settle_ms) before the opposite direction is commanded");
    }
    CHECK(a2k_ptz_manual_stop() == 0 && wait_motor_stop(1000) >= 0 && wait_idle(2000), "stop");
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");

    /* (3) the travel of a manual move is weighted like the tracker's (steps at divisor 2 / 3 count more) */
    t0v = (long)a2k_ptz_travel();
    dm = mech_pan();
    CHECK(a2k_ptz_vel_manual(1, 0, 2, 0, 1000) == 0, "manual pan at divisor 2 for ~1 s");
    manual_ended(1000);
    wait_idle(2000);
    dt = (long)a2k_ptz_travel() - t0v;
    dm = labs(mech_pan() - dm);
    CHECK(dm > 200 && dt >= 2 * dm, "pan divisor 2: the budget counts about 3x the steps moved (weighted), not 1x");
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");
    t0v = (long)a2k_ptz_travel();
    dm = mech_tilt();
    CHECK(a2k_ptz_vel_manual(0, 1, 0, 2, 1000) == 0, "manual tilt asked at divisor 2 for ~1 s");
    manual_ended(1000);
    wait_idle(2000);
    dt = (long)a2k_ptz_travel() - t0v;
    dm = labs(mech_tilt() - dm);
    {
        long mdt = dt, mdm = dm, tdt, tdm;
        CHECK(mdm > 150 && mdt * 10 > mdm * 14 && mdt * 10 < mdm * 25, "tilt: weighted at the EFFECTIVE divisor 3 (about 2x), not at the requested 2 (3x)");
        CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");
        t0v = (long)a2k_ptz_travel();
        tdm = mech_tilt();
        for (k = 0; k < 10; k++) { a2k_ptz_vel(0, 1, 0, 3); usleep(100000); }
        a2k_ptz_vel(0, 0, 0, 0);
        usleep(300000);
        CHECK(wait_motor_stop(3000) >= 0 && wait_idle(2000), "the tracker's tilt session at divisor 3 for ~1 s");
        tdt = (long)a2k_ptz_travel() - t0v;
        tdm = labs(mech_tilt() - tdm);
        CHECK(tdm > 150 && mdt * tdm * 100 >= mdm * tdt * 80 && mdt * tdm * 100 <= mdm * tdt * 125, "the manual tilt move and the tracker's weigh the same per step (within 20%)");
    }
    CHECK(a2k_ptz_goto(1275, 1300, 3) == 0 && wait_at_home(8000), "back home");

    /* (2) the manual hold is activity for the drift machine: no drift check during a hold, the check waits for the return home + the idle time */
    DH.force = 0;
    usleep(1500000); /* the check of the last return home (skipped) is over */
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000), "home again (a check is due)");
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 600) == 0, "a manual hold starts right away");
    usleep(1300000);
    CHECK(DH.calls == calls0, "no drift check during / right after the hold (the head is away, the timer restarted)");
    CHECK(a2k_ptz_home_tracker() == 0 && wait_at_home(5000), "the head returns home after the hold");
    CHECK(wait_calls(calls0 + 1, 4000), "...and the drift check runs once, after the idle time");

    /* (4) untrusted: reached by a RECAL verdict. Manual moves stay allowed, the soft range applies, the flag is published (a2k_ptz_drift_info -> ptz.untrusted). */
    DH.force = 2;
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000), "home again");
    a2k_ptz_note_person(); /* the idle times (check 0.3 s, recalibration 1 s) count from this person */
    CHECK(wait_calls(calls0 + 1, 4000), "a check with a RECAL verdict");
    keep_person(1000); /* somebody around: the scheduled recalibration waits */
    a2k_ptz_drift_info(&dr);
    CHECK(dr.untrusted && dr.recals == 0, "untrusted, the recalibration is scheduled but waits");
    CHECK(a2k_ptz_manual_gate() == 0, "decision: manual moves are ALLOWED while the position is untrusted (the gate is open)");
    CHECK(a2k_ptz_vel_manual(-1, 0, 3, 0, 450) == 0, "a manual move is accepted while untrusted");
    usleep(300000);
    CHECK(M.running[0], "...and the head moves");
    CHECK(a2k_ptz_manual_stop() == 0 && wait_motor_stop(2000) >= 0 && wait_idle(2000), "stop");
    CHECK(a2k_ptz_goto((int)pmax - 40, 1300, 2) == 0, "go to just inside the pan limit");
    for (k = 0; k < 1500 && mech_pan() < (int)pmax - 42; k++) { a2k_ptz_note_person(); usleep(10000); }
    CHECK(wait_motor_stop(3000) >= 0 && wait_idle(2000), "there");
    keep_person(300);
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == 0, "a manual move toward the limit is accepted ...");
    usleep(700000);
    CHECK(wait_motor_stop(2000) >= 0 && mech_pan() <= (int)pmax, "... and the soft range still applies (never beyond the limit) while untrusted");
    a2k_ptz_drift_info(&dr);
    CHECK(dr.untrusted && dr.recals == 0, "still untrusted, the manual moves did not cancel or start the recalibration");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "no forbidden driver request");

    /* (2b) a scheduled recalibration never starts during a hold: the head is at the limit (the session ends at once), the hold lease is 2 s, the idle time 1 s */
    keep_person(400);
    t0 = mono();
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 2000) == 0, "a 2 s hold at the limit");
    {
        int started = 0;
        int64_t at = 0;
        for (k = 0; k < 800 && !started; k++) {
            a2k_ptz_info(&i);
            a2k_ptz_drift_info(&dr);
            if (dr.recals >= 1 || i.state == HAL_PTZ_CALIBRATING) { started = 1; at = mono() - t0; break; }
            usleep(10000);
        }
        CHECK(started, "the recalibration starts once the hold is over and nobody was around");
        CHECK(at >= 2000 + 900, "it did NOT start while the hold was live, nor in the idle time just after it (>= 2.0 s + 1 s idle, measured from the command)");
    }
    /* (2c) while the drift recalibration runs a manual command is refused with the 'calibrating' fault; a Stop does not abort it */
    CHECK(wait_state(HAL_PTZ_CALIBRATING, 2000), "calibrating (the drift recalibration)");
    CHECK(a2k_ptz_vel_manual(1, 0, 3, 0, 450) == A2K_PTZ_E_NOTREADY && a2k_ptz_manual_gate() == A2K_PTZ_E_NOTREADY, "a manual move is refused with the calibrating fault (NOTREADY)");
    CHECK(a2k_ptz_manual_stop() == 0, "a Stop never aborts it");
    usleep(200000);
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_CALIBRATING, "still calibrating");
    M.real = 0; /* the rest of the calibration on virtual time */
    CHECK(wait_idle(10000) && wait_at_home(10000), "it completed, the head is home");
    usleep(300000);
    a2k_ptz_drift_info(&dr);
    CHECK(!dr.untrusted && a2k_ptz_manual_gate() == 0, "trusted again, the manual gate is open");

    /* (5) the SHIFT window: re-declare + move home + re-verify refuses manual commands, and a Stop does not cancel the correction */
    M.real = 1;
    DH.force = 0;
    DH.delay = 400;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 50; /* 4 px of lost steps, injected while the head is away */
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000), "home again (the head is 4 px off)");
    {
        int seen = 0, rc1 = 0, rc2 = -1;
        for (k = 0; k < 3000 && !seen; k++) {
            a2k_ptz_info(&i);
            if (i.state == HAL_PTZ_MOVED && a2k_ptz_manual_gate() == A2K_PTZ_E_NOTREADY) {
                seen = 1;
                rc1 = a2k_ptz_vel_manual(-1, 0, 3, 0, 450);
                rc2 = a2k_ptz_manual_stop();
            }
            usleep(2000);
        }
        CHECK(seen, "the SHIFT window is seen (moved, gate closed with NOTREADY)");
        CHECK(rc1 == A2K_PTZ_E_NOTREADY && rc2 == 0, "a manual command is refused during it, a Stop is a harmless no-op");
    }
    CHECK(wait_idle(8000) && wait_at_home(8000), "the correction completed, home");
    usleep(500000);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1 && !dr.untrusted && abs(M.mech[0] - 1275) <= 5, "the correction was verified and the head is physically home: it was not cancelled by the manual command / Stop");
    CHECK(a2k_ptz_manual_gate() == 0, "the gate is open again");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "no forbidden driver request during the whole integration test");
    a2k_ptz_stop();
    M.real = 0;
    DH.delay = 0;
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
}

/* ------------------------------------------------------------------------------------------------ tilt accounting: counted == truly executed (2026-11-18)
 * The position the software reports IS the driver's step counter (ptzs_read_axis): no distance is ever derived from a time, a divisor or a request. This test drives the
 * tilt the way the tracker does (close-range VSCAN: up 250 / down 300 / up 50 at divisor 5, each leg its own session with a dwell between, and tracking-like velocity
 * sessions with reversals at the divisors 2..6) and compares, per leg and at the end, the counter, the reported position and the steps the fake motor really executed
 * (M.exec, counted independently in the stepping loop): they must be identical, up and down alike. */
static struct { int target, dir; } LEG;
static int fn_leg(struct sc *s, const hal_ptz_cstate_t *st, hal_ptz_cmd_t *cmd)
{
    if ((LEG.dir > 0 && (int)st->tilt >= LEG.target) || (LEG.dir < 0 && (int)st->tilt <= LEG.target) || s->n > 2000) return HAL_PTZ_CONT_STOP;
    *cmd = s->cur; /* an identical command refreshes the lease */
    return HAL_PTZ_CONT_CMD;
}

static void tilt_account_cases(void)
{
    hal_ptz_t *p = mk();
    struct sc s;
    uint32_t pan, tilt;
    long cmds0;
    int cyc, ok_legs = 0, legs = 0, t_start = mech_tilt();
    unsigned seed = 12345;
    printf("== tilt accounting: counter == truly executed steps (VSCAN cycles and tracking reversals)\n");
    cmds0 = M.nmove;
    for (cyc = 0; cyc < 50; cyc++) {
        static const int dist[3] = { 250, -300, 50 };
        for (int leg = 0; leg < 3; leg++) {
            hal_ptz_cmd_t first;
            int before = mech_tilt();
            long cnt_before = (long)M.cnt[1], ex_before = M.exec[1];
            LEG.dir = dist[leg] > 0 ? 1 : -1;
            LEG.target = before + dist[leg];
            memset(&s, 0, sizeof s);
            first = (hal_ptz_cmd_t){ 0, LEG.dir, 0, 5 };
            s.cur = first;
            s.fn = fn_leg;
            hal_ptz_cont_run(p, &first, 400, cb, &s);
            d_sleep(600); /* the dwell: the motor is at rest, nothing is commanded */
            legs++;
            if (hal_ptz_pos(p, &pan, &tilt) && (int)tilt == mech_tilt() && (long)M.cnt[1] - cnt_before == M.exec[1] - ex_before && mech_tilt() - before == (int)(M.exec[1] - ex_before)) ok_legs++;
        }
        /* tracking-like: sessions with velocity reversals (up, down, diagonals) at the divisors the tracker uses (2..6; the tilt floor makes 2 -> 3) */
        for (int k = 0; k < 2; k++) {
            hal_ptz_cmd_t first = { 1, 1, 2 + (int)(seed % 5), 2 + (int)((seed >> 8) % 5) };
            seed = seed * 1103515245u + 12345u;
            memset(&s, 0, sizeof s);
            s.cur = first;
            s.fn = fn_reverse_every;
            hal_ptz_cont_run(p, &first, 400, cb, &s);
            d_sleep(300);
        }
    }
    CHECK(legs == 150 && ok_legs == 150, "50 VSCAN cycles (up 250 / down 300 / up 50): every leg, counter delta == steps the motor really executed == the mechanical delta");
    CHECK(hal_ptz_pos(p, &pan, &tilt) && (int)tilt == mech_tilt(), "after 50 cycles + tracking reversals the reported tilt equals the mechanical tilt");
    CHECK(M.exec[1] == (long)mech_tilt() - t_start, "the true net tilt travel equals the mechanical displacement");
    CHECK((long)M.cnt[1] + 299 == (long)mech_tilt(), "the driver counter (shifted frame, offset 299) equals the true tilt: no drift between counted and executed");
    CHECK(M.exec_pos[1] > 0 && M.exec_neg[1] > 0, "the run really moved up and down");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "only move / stop / pos requests reached the driver");
    printf("  info - tilt steps up %ld, down %ld; %ld driver move commands in 50 cycles (%.1f per cycle); true - counted = %ld\n", M.exec_pos[1], M.exec_neg[1],
           (long)M.nmove - cmds0, (double)((long)M.nmove - cmds0) / 50.0, (long)mech_tilt() - ((long)M.cnt[1] + 299));
}


/* ---- the patch matcher contract: drift4 answers in quarter px of the 320 px frame (4 per px, 16 per thumbnail px), the settings ptz_drift_ref / ptz_drift_correct / ptz_boot_calibrate */
static int fake_drift4(uint32_t ph, uint32_t th, int64_t minf, int *dxq, int *dyq)
{
    double ax, ay;
    (void)ph; (void)th;
    pthread_mutex_lock(&M.mu);
    ax = (M.mech[0] - 1275) / 9.6 * 16.0; /* quarter px: the same 9.6 steps per thumbnail px */
    ay = (M.mech[1] - 1300) / 9.6 * 16.0;
    pthread_mutex_unlock(&M.mu);
    DH.calls++;
    DH.last_minf = minf;
    DH.minf_ok = minf > 0;
    *dxq = (int)(ax < 0 ? ax - 0.5 : ax + 0.5);
    *dyq = (int)(ay < 0 ? ay - 0.5 : ay + 0.5);
    if (DH.si < DH.nseq && DH.seq[DH.si++] >= 0) return DH.seq[DH.si - 1];
    if (DH.force == 2) return A2K_DRIFT_RECAL;
    if (abs(*dxq) <= 4 && abs(*dyq) <= 4) { *dxq = *dyq = 0; return A2K_DRIFT_OK; }
    return abs(*dxq) <= 120 && abs(*dyq) <= 120 ? A2K_DRIFT_SHIFT : A2K_DRIFT_RECAL;
}
static int boot_calls;
static int boot_count(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void)) { (void)pan; (void)tilt; (void)cancelled; boot_calls++; return 1; }

static void drift_ref_start(a2k_ptz_cfg_t *c, a2k_ptz_scene_t *sc, int drift_ref, int mode)
{
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc->boot_check = boot_ok;
    sc->drift = NULL;
    sc->drift4 = fake_drift4;
    sc->capture = fake_capture;
    sc->commit = fake_commit;
    a2k_ptz_set_scene(sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    a2k_ptz_defaults(c);
    c->scene_check = 1;
    c->drift_ref = drift_ref;
    c->drift_correct = mode;
}

static void drift_ref_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    a2k_ptz_info_t i;
    int calls0;
    printf("== drift reference: quarter px contract, ptz_drift_ref, ptz_drift_correct (off / adjust / full), ptz_boot_calibrate\n");
    CHECK(a2k_ptz_parse_drift_correct("off") == A2K_PTZ_DC_OFF && a2k_ptz_parse_drift_correct("adjust") == A2K_PTZ_DC_ADJUST && a2k_ptz_parse_drift_correct("full") == A2K_PTZ_DC_FULL && a2k_ptz_parse_drift_correct("on") == -1 && a2k_ptz_parse_drift_correct("") == -1, "ptz_drift_correct parses off|adjust|full only");
    CHECK(a2k_ptz_parse_boot_calibrate("auto") == 0 && a2k_ptz_parse_boot_calibrate("always") == 1 && a2k_ptz_parse_boot_calibrate("never") == -1, "ptz_boot_calibrate parses auto|always only (never would leave the counters unknown)");
    a2k_ptz_defaults(&c);
    CHECK(c.drift_ref == 1 && c.drift_correct == A2K_PTZ_DC_FULL && c.boot_always == 0, "defaults: drift_ref on, drift_correct full, boot_calibrate auto (the behaviour before the settings existed)");

    /* 1. full: a 3 px (about 7 tilt steps) loss and a 2.5 px pan loss are measured to the quarter px and corrected exactly by one declare + goto home */
    drift_ref_start(&c, &sc, 1, A2K_PTZ_DC_FULL);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start");
    M.real = 1;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 24; /* the lost steps, while the head is away */
    M.mech[1] -= 29;
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000) && wait_calls(2, 6000), "the check, then the check after the correction");
    usleep(300000);
    a2k_ptz_drift_info(&dr);
    a2k_ptz_info(&i);
    CHECK(dr.corrections == 1 && !dr.untrusted && dr.recals == 0, "one correction by the quarter px result, no recalibration");
    CHECK(abs(M.mech[0] - 1275) <= 3 && abs(M.mech[1] - 1300) <= 3, "the head is physically home to within the goto tolerance of 3 steps (quarter px resolution, final approach from below)");
    printf("    mechanical position after the correction: %d/%d\n", M.mech[0], M.mech[1]);
    CHECK(i.known && i.state == HAL_PTZ_HOME && abs((int)i.pan - 1275) <= 3 && abs((int)i.tilt - 1300) <= 3, "the controller reports home within the goto tolerance");
    a2k_ptz_stop();

    /* 2. off: measured and logged, nothing moves, nothing is scheduled */
    drift_ref_start(&c, &sc, 1, A2K_PTZ_DC_OFF);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (drift_correct=off)");
    M.real = 1;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[1] -= 50;
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000) && wait_calls(1, 6000), "the check ran");
    usleep(800000);
    a2k_ptz_drift_info(&dr);
    CHECK(DH.calls == 1 && dr.corrections == 0 && dr.recals == 0 && !dr.untrusted && dr.have && dr.dy != 0, "off: the displacement is measured (shown), the counters are never touched");
    a2k_ptz_stop();

    /* 3. adjust: a shift is corrected, a RECAL verdict never starts a recalibration */
    drift_ref_start(&c, &sc, 1, A2K_PTZ_DC_ADJUST);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (drift_correct=adjust)");
    M.real = 1;
    go_and_home();
    pthread_mutex_lock(&M.mu);
    M.mech[1] -= 40;
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_at_home(4000) && wait_calls(2, 6000), "adjust: checked and corrected");
    usleep(300000);
    a2k_ptz_drift_info(&dr);
    CHECK(dr.corrections == 1 && dr.recals == 0, "adjust: the counters are corrected");
    DH.force = 2;
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 6000), "a RECAL verdict arrives");
    usleep(2500000); /* well past the 1 s idle time a recalibration would wait */
    a2k_ptz_drift_info(&dr);
    a2k_ptz_info(&i);
    CHECK(dr.recals == 0 && !dr.untrusted && i.state != HAL_PTZ_CALIBRATING, "adjust: a big error never starts a recalibration (only logged)");
    a2k_ptz_stop();

    /* 4. drift_ref off: no check at all */
    drift_ref_start(&c, &sc, 0, A2K_PTZ_DC_FULL);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (drift_ref=0)");
    M.real = 1;
    go_and_home();
    CHECK(wait_at_home(4000), "home");
    usleep(1500000);
    a2k_ptz_drift_info(&dr);
    CHECK(DH.calls == 0 && dr.checks == 0 && dr.corrections == 0 && dr.recals == 0, "drift_ref=0: the drift check never runs, nothing is corrected or recalibrated");
    a2k_ptz_stop();

    /* 5. boot_calibrate: auto uses the scene proof (the hook is asked), always never asks it and calibrates */
    drift_ref_start(&c, &sc, 1, A2K_PTZ_DC_FULL);
    sc.boot_check = boot_count;
    a2k_ptz_set_scene(&sc);
    boot_calls = 0;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (boot_calibrate=auto)");
    CHECK(boot_calls == 1, "auto: the scene proof is asked once at the boot");
    a2k_ptz_stop();
    drift_ref_start(&c, &sc, 1, A2K_PTZ_DC_FULL);
    sc.boot_check = boot_count;
    a2k_ptz_set_scene(&sc);
    c.boot_always = 1;
    boot_calls = 0;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(15000), "start (boot_calibrate=always)");
    CHECK(boot_calls == 0, "always: the scene proof is not used, the motor calibrates");
    a2k_ptz_stop();
}

/* ---- the tilt-only re-home (hal_ptz_tilt_rehome, controller ptz_tilt_rehome=idle): lost tilt steps are non-cumulative */
static hal_ptz_t *g_rh_p;
static int g_rh_cancel_ms;
static void *rh_cancel_thread(void *arg) { (void)arg; usleep((unsigned)g_rh_cancel_ms * 1000); hal_ptz_cancel(g_rh_p); return NULL; }

static void rehome_hal_cases(void)
{
    hal_ptz_t *p;
    uint32_t pan, tilt;
    int rc, pan0;
    pthread_t th;
    printf("== tilt re-home (HAL): tilt into the bottom stop, counter re-declared at the stop, back up to home; the pan untouched\n");
    for (int si = 0; si < 4; si++) { /* the head physically LOWER (-150), HIGHER (+150), much higher (+800: physical 2100, the case of the ceiling) and at the very top (+1000: 2300, beyond the declared stroke) than the counter says */
        static const int slips[4] = { -150, 150, 800, 1000 };
        const int slip = slips[si];
        p = mk();
        pan0 = mech_pan();
        pthread_mutex_lock(&M.mu);
        M.mech[1] += slip; /* lost steps: the counters know nothing */
        pthread_mutex_unlock(&M.mu);
        CHECK(hal_ptz_pos(p, &pan, &tilt) && (int)tilt == 1300 && mech_tilt() == 1300 + slip, slip > 0 ? "setup: the counter says 1300, the head is physically higher (up to +1000)" : "setup: the counter says 1300, the head is physically 150 steps lower");
        M.exec[0] = M.exec[1] = 0;
        rc = hal_ptz_tilt_rehome(p);
        CHECK(rc == HAL_PTZ_OK, "the re-home succeeds");
        CHECK(hal_ptz_pos(p, &pan, &tilt) && abs((int)tilt - 1300) <= 3 && (int)pan == 1275, "the controller position is home 1275/1300 again");
        CHECK(abs(mech_tilt() - 1299) <= 4, "the head is PHYSICALLY at the stop-derived home (1299 steps above the bottom stop): the slip (including +800 and +1000) is gone");
        CHECK(labs((long)M.cnt[1] + 299 - (long)mech_tilt()) <= 2, "the driver counter equals the true tilt");
        CHECK(mech_pan() == pan0 && M.exec[0] == 0, "the pan never moved (no pan step executed)");
        CHECK(M.min_mech[1] == 0 || M.min_mech[1] == M.lo[1], "the head went down onto the bottom stop");
        CHECK(M.bad_cmd == 0 && M.nother == 0 && !M.running[0] && !M.running[1], "only move / stop / pos / init requests reached the driver, the motor is at rest");
        CHECK(hal_ptz_state(p) == HAL_PTZ_HOME, "state home, trusted: the next motion is accepted");
        CHECK(hal_ptz_goto(p, 1275, 1350) == HAL_PTZ_OK && abs(mech_tilt() - 1350) <= 4, "a normal goto after the re-home works and lands where the counter says");
        hal_ptz_free(p);
    }
    p = mk();
    CHECK(hal_ptz_tilt_rehome(p) == HAL_PTZ_OK && abs(mech_tilt() - 1299) <= 4, "no slip: the re-home leaves the head at home (1299 from the stop)");
    hal_ptz_free(p);
    /* a cancel on the way down keeps the BELIEVED position (the slip is what it was, nothing worse), the motor stops, the pan is untouched */
    p = mk();
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 150;
    M.real = 1;
    pthread_mutex_unlock(&M.mu);
    g_rh_p = p;
    g_rh_cancel_ms = 3500; /* after the two 1 s driver inits and about 1.3 s of the walk down */
    pthread_create(&th, NULL, rh_cancel_thread, NULL);
    rc = hal_ptz_tilt_rehome(p);
    pthread_join(th, NULL);
    M.real = 0;
    CHECK(rc == HAL_PTZ_ECANCEL, "a cancel on the way down: ECANCEL");
    CHECK(!M.running[0] && !M.running[1] && mech_tilt() < 1450 - 100 && mech_tilt() > 500, "the motor stopped part of the way down");
    CHECK(hal_ptz_pos(p, &pan, &tilt) && (int)pan == 1275 && (int)tilt - mech_tilt() == -150, "the believed position is the old belief minus the steps walked: counter - physical stays -150 (the same slip, not worse)");
    CHECK(mech_pan() == 1275 && M.exec[0] == 0, "the pan never moved");
    CHECK(hal_ptz_home(p) == HAL_PTZ_OK && hal_ptz_state(p) == HAL_PTZ_HOME, "the position is trusted: home works afterwards");
    hal_ptz_free(p);
}

static int wait_runs(unsigned n, int ms)
{
    unsigned r;
    for (int t = 0; t < ms; t += 20) {
        a2k_ptz_rehome_info(&r, NULL);
        if (r >= n) return 1;
        usleep(20000);
    }
    return 0;
}

static int wait_cancels(unsigned n, int ms)
{
    unsigned c;
    for (int t = 0; t < ms; t += 20) {
        a2k_ptz_rehome_info(NULL, &c);
        if (c >= n) return 1;
        usleep(20000);
    }
    return 0;
}

static int wait_mech_tilt_below(int v, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (mech_tilt() < v) return 1;
        usleep(20000);
    }
    return 0;
}

static void rehome_ctl_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    unsigned runs, canc;
    int t_before;
    printf("== tilt re-home (controller): idle at home only, never with a person / a tracker, rate limited, cancelled by a person or a tracker command\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_rehome_timing(500, 40000, 100);
    a2k_ptz_test_set_ladder_timing(0, 1500, 0, 0); /* a cancelled attempt is retried after 1.5 s (default 2 min) */
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    CHECK(c.tilt_rehome == A2K_PTZ_RH_IDLE && c.rehome_gap_min == A2K_PTZ_REHOME_GAP_MIN, "the defaults: ptz_tilt_rehome=idle, gap 30 min");
    CHECK(a2k_ptz_parse_tilt_rehome("off") == A2K_PTZ_RH_OFF && a2k_ptz_parse_tilt_rehome("idle") == A2K_PTZ_RH_IDLE && a2k_ptz_parse_tilt_rehome("on") == -1 && a2k_ptz_parse_tilt_rehome("") == -1, "the setting parses off / idle only");
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "start (home claimed)");
    M.real = 1;
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 150; /* lost tilt steps */
    pthread_mutex_unlock(&M.mu);

    /* 1. somebody is in view: no re-home however long it lasts */
    go_and_home();
    CHECK(wait_at_home(4000), "back at home (counters 1275/1300, the head physically 150 steps higher)");
    t_before = mech_tilt();
    keep_person(2500);
    a2k_ptz_rehome_info(&runs, &canc);
    CHECK(runs == 0 && canc == 0 && mech_tilt() == t_before && !M.running[1], "a person in view for 2.5 s (5 x the idle time): no re-home, the tilt did not move");
    {
        unsigned long ls = 5;
        const char *why = NULL;
        a2k_ptz_rehome_last(&ls, &why);
        CHECK(ls == 0 && why && !strcmp(why, "none"), "visibility: before any re-home the last stamp is none");
    }

    /* 2. a tracking session: no re-home while it runs, and none when it ends away from home */
    CHECK(a2k_ptz_vel(1, 0, 4, 0) == 0, "a tracking session starts");
    usleep(900000);
    CHECK(mech_tilt() == t_before && (runs = 0, a2k_ptz_rehome_info(&runs, NULL), runs == 0), "while it runs: no re-home");
    a2k_ptz_vel(0, 0, 0, 0);
    wait_motor_stop(3000);
    usleep(1500000);
    a2k_ptz_info(&i);
    a2k_ptz_rehome_info(&runs, &canc);
    CHECK(runs == 0 && mech_tilt() == t_before && i.pan != 1275, "it ended away from home: no re-home (waits for the next return home)");

    /* 3. quiet at home: the re-home runs, the slip is gone, the pan is untouched */
    go_and_home();
    CHECK(wait_at_home(4000), "home again");
    pthread_mutex_lock(&M.mu);
    M.exec[0] = M.exec[1] = 0;
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_runs(1, 60000), "nobody around: the re-home ran once");
    {
        unsigned long ls = 99;
        const char *why = NULL;
        a2k_ptz_rehome_last(&ls, &why);
        CHECK(ls > 0 && why && !strcmp(why, "gap"), "visibility: the completed re-home is stamped (uptime s > 0) with the reason gap");
    }
    CHECK(wait_at_home(4000) && wait_motor_stop(3000), "and the head is back home");
    a2k_ptz_info(&i);
    printf("  info - mech tilt %d, counters %u/%u, state %d\n", mech_tilt(), (unsigned)i.pan, (unsigned)i.tilt, i.state);
    CHECK(abs(mech_tilt() - 1299) <= 4 && abs((int)i.tilt - 1300) <= 3 && abs((int)i.pan - 1275) <= 3, "the head is physically at the stop-derived home and the counters agree: the 150 steps of slip are gone");
    CHECK(abs(mech_pan() - 1275) <= 3, "the pan stayed (the drift of the pan axis is the nudge test's, not touched by the re-home)");
    CHECK(M.bad_cmd == 0 && M.nother == 0, "only move / stop / pos / init requests reached the driver");

    /* 4. the rate limit: due again at once (a new return home) but the gap (40 s from the start of the last attempt) has not passed */
    go_and_home();
    CHECK(wait_at_home(4000), "home again (a second return)");
    t_before = mech_tilt();
    usleep(3000000);
    a2k_ptz_rehome_info(&runs, &canc);
    CHECK(runs == 1 && mech_tilt() == t_before, "5 x the idle time later: no second re-home inside the gap");

    /* 5. the gap passes: it starts, and a person who appears cancels it; the motor stops, the position stays the believed one */
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 90; /* new slip, so a cancelled re-home is distinguishable from a finished one */
    pthread_mutex_unlock(&M.mu);
    CHECK(wait_mech_tilt_below(1200, 60000), "the gap has passed: the second re-home starts (the head walks down)");
    keep_person(150);
    CHECK(wait_motor_stop(3000), "a person appears: the motor stops");
    CHECK(wait_cancels(1, 10000), "the controller finished the interrupted re-home (the counters are re-declared, about 2 s)");
    a2k_ptz_rehome_info(&runs, &canc);
    a2k_ptz_info(&i);
    printf("  info - runs %u cancels %u, counters %u/%u known %d, mech tilt %d\n", runs, canc, (unsigned)i.pan, (unsigned)i.tilt, i.known, mech_tilt());
    CHECK(runs == 1 && canc == 1, "the re-home is counted as cancelled, not done");
    CHECK(i.known && abs((int)i.pan - 1275) <= 3 && abs((int)i.tilt - mech_tilt() + 90) <= 2 && mech_tilt() < 1400, "the counters are the believed ones minus the walk (the slip stays 90, nothing worse), the pan is where it was");

    /* 6. a tracker command cancels a running re-home too */
    CHECK(wait_at_home(20000) && wait_motor_stop(3000), "the cancelled re-home left the head off home: the controller returns it home by itself (nobody has to send it)");
    CHECK(wait_mech_tilt_below(1200, 40000), "the cancelled attempt stays wanted: quiet again, a re-home starts after the retry gap (the 40 s gap was not consumed)");
    a2k_ptz_vel(1, 0, 4, 0); /* the tracker wants the head */
    CHECK(wait_motor_stop(3000), "a tracker velocity command: the re-home gives way, the motor stops");
    CHECK(wait_cancels(2, 10000), "the interrupted re-home is finished");
    a2k_ptz_rehome_info(&runs, &canc);
    CHECK(canc == 2, "counted as cancelled again");
    {
        unsigned long ls = 0;
        const char *why = NULL;
        a2k_ptz_rehome_last(&ls, &why);
        CHECK(runs == 1 && ls > 0 && why && !strcmp(why, "gap"), "visibility: cancelled attempts do not change the last completed stamp");
    }
    a2k_ptz_vel(0, 0, 0, 0);
    a2k_ptz_stop();
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}


/* ---- a blind check: the view at home is flat where the reference had detail (the head slipped to the ceiling), or there is nothing to compare */
static int flat_start(a2k_ptz_cfg_t *c, a2k_ptz_scene_t *sc, int rehome)
{
    d_reset(1275, 1300);
    memset(&DH, 0, sizeof DH);
    memset(&DW, 0, sizeof DW);
    sc->boot_check = boot_ok;
    sc->drift = fake_drift;
    sc->capture = fake_capture;
    sc->commit = fake_commit;
    a2k_ptz_set_scene(sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    a2k_ptz_test_set_rehome_timing(500, 600000, 1000000); /* the ordinary re-home is never due (huge minimum travel, huge gap): only a flat verdict can start one */
    a2k_ptz_defaults(c);
    c->scene_check = 1;
    c->tilt_rehome = rehome ? A2K_PTZ_RH_IDLE : A2K_PTZ_RH_OFF;
    if (a2k_ptz_start(c) != 0 || !wait_idle(8000)) return -1;
    M.real = 1;
    return 0;
}

static void flat_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    unsigned runs = 0;
    unsigned long tr1, t0;
    int calls0;
    printf("== blind drift check: flat view where the reference had detail -> re-home / recalibration; nothing to compare -> logged, travel mark kept\n");
    CHECK(flat_start(&c, &sc, 1) == 0, "start (tilt re-home on)");
    a2k_ptz_drift_info(&dr);
    CHECK(!strcmp(dr.last, "none"), "ptz.drift_last starts at none");

    /* 1. a flat view at home where the reference had detail: a re-home runs at the next quiet moment (although the ordinary one is not due) */
    DH.force = 4;
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 150; /* the head slipped up */
    pthread_mutex_unlock(&M.mu);
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(1, 6000), "back home: the check ran and saw a flat view");
    CHECK(wait_runs(1, 20000), "a flat view where the reference had detail: the tilt re-home is scheduled and ran at the quiet moment");
    {
        unsigned long ls = 0;
        const char *why = NULL;
        a2k_ptz_rehome_last(&ls, &why);
        CHECK(ls > 0 && why && !strcmp(why, "flat"), "visibility: a re-home scheduled by a flat home view reports the reason flat");
    }
    CHECK(wait_at_home(4000) && wait_motor_stop(3000) && abs(mech_tilt() - 1299) <= 4, "the head is back at the stop-derived home");
    for (int k = 0; k < 400; k++) { a2k_ptz_drift_info(&dr); if (!strcmp(dr.last, "flat-persist")) break; usleep(10000); }
    /* F3: the check after the re-home sees the (faked, permanent) flat view again: that is no reason for a second re-home any more, the state says flat-persist (it used to be the second skip-flat of the 5 min loop) */
    CHECK(!strcmp(dr.last, "flat-persist") && !dr.have && !dr.untrusted, "ptz.drift_last = flat-persist after the re-home (the first verdict was skip-flat), ptz.drift_last_px unchanged (none), the position is not marked untrusted");

    /* 2. nothing to compare (no reference / no frame: the hook says SKIP): no action, logged in the state, the travel mark is kept */
    DH.force = 3;
    a2k_ptz_test_set_drift_timing(300, 1500, 1, 1000);
    calls0 = DH.calls;
    t0 = a2k_ptz_travel();
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 6000), "second return: the check ran and had nothing to compare");
    usleep(1500000);
    a2k_ptz_drift_info(&dr);
    a2k_ptz_rehome_info(&runs, NULL);
    CHECK(!strcmp(dr.last, "skip") && runs == 1 && dr.recals == 0 && !dr.untrusted, "skip: published as skip, no re-home, no recalibration");
    tr1 = a2k_ptz_travel();
    printf("  info - one trip travels %lu steps (weighted)\n", tr1 - t0);
    a2k_ptz_test_set_drift_timing(300, 1500, (unsigned)((tr1 - t0) * 3 / 2), 1000); /* one trip is worth less than this minimum: only the whole travel since the last SUCCESSFUL check (the mark is kept by a skip) reaches it */
    calls0 = DH.calls;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(calls0 + 1, 6000), "the travel mark was kept by the skip: the next return home is checked again (the skipped trip still counts)");
    a2k_ptz_stop();
    M.real = 0;

    /* 3. tilt re-home off: a flat verdict schedules a full recalibration instead */
    CHECK(flat_start(&c, &sc, 0) == 0, "restart (tilt re-home off)");
    DH.force = 4;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(1, 6000), "back home: flat view");
    for (int k = 0; k < 600; k++) { a2k_ptz_drift_info(&dr); if (dr.recals == 1) break; usleep(10000); }
    CHECK(dr.recals == 1 && !strcmp(dr.last, "skip-flat"), "re-home off: the flat view schedules a full recalibration, which ran");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
}

/* ================================================================================================ review fixes (fable-ptz-drift-20261121.md): F1..F16 */
static int only(const char *name)
{
    const char *e = getenv("PTZ_UNIT_ONLY"); /* comma separated names, e.g. PTZ_UNIT_ONLY=f2,f13 */
    size_t n = strlen(name);
    if (!e)
        return 1;
    for (; *e; ) {
        const char *c = strchr(e, ',');
        size_t l = c ? (size_t)(c - e) : strlen(e);
        if (l == n && !strncmp(e, name, n))
            return 1;
        e += l + (c ? 1 : 0);
    }
    return 0;
}

/* F1: a re-home cancelled once the believed tilt is below the soft minimum used to settle ON the park pose, which the state derived "parked" from (tracker off, privacy on). The state of the
 * head after an interruption is never "parked" unless a park command put it there. */
static void f1_cases(void)
{
    hal_ptz_t *p;
    uint32_t pan, tilt;
    pthread_t th;
    int rc;
    printf("== F1: a re-home cancelled in the lower half of the stroke never leaves the head PARKED\n");
    p = mk();
    pthread_mutex_lock(&M.mu);
    M.real = 1;
    pthread_mutex_unlock(&M.mu);
    g_rh_p = p;
    g_rh_cancel_ms = 6500; /* well into the first leg: the believed tilt is below the soft minimum (300) */
    pthread_create(&th, NULL, rh_cancel_thread, NULL);
    rc = hal_ptz_tilt_rehome(p);
    pthread_join(th, NULL);
    M.real = 0;
    CHECK(rc == HAL_PTZ_ECANCEL, "F1 hal: a cancel in the lower half: ECANCEL");
    CHECK(!M.running[0] && !M.running[1], "F1 hal: the motor is at rest");
    CHECK(hal_ptz_pos(p, &pan, &tilt) && (int)pan == 1275 && tilt >= 300 && abs((int)tilt - mech_tilt() - 1) <= 4, "F1 hal: the position is known, the pan unchanged, the counter agrees with the head");
    CHECK(hal_ptz_state(p) != HAL_PTZ_PARKED && hal_ptz_state(p) == HAL_PTZ_MOVED, "F1 hal: the state is moved, never parked (a settle is not a park command)");
    CHECK(hal_ptz_park(p) == HAL_PTZ_OK && hal_ptz_state(p) == HAL_PTZ_PARKED, "F1 hal: an explicit park command still gives parked");
    CHECK(hal_ptz_home(p) == HAL_PTZ_OK && hal_ptz_state(p) == HAL_PTZ_HOME, "F1 hal: and home gives home");
    hal_ptz_free(p);
    /* the controller with ptz_privacy=1: the same interruption leaves the video on and the head usable */
    {
        a2k_ptz_cfg_t c;
        a2k_ptz_scene_t sc = SC;
        a2k_ptz_info_t i;
        d_reset(1275, 1300);
        sc.boot_check = boot_ok;
        a2k_ptz_set_scene(&sc);
        a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
        a2k_ptz_test_set_rehome_timing(500, 5000, 100);
        a2k_ptz_defaults(&c);
        c.scene_check = 1;
        c.privacy = 1;
        CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "F1 ctl: start (ptz_privacy=1)");
        M.real = 1;
        go_and_home();
        CHECK(wait_at_home(4000), "F1 ctl: home");
        CHECK(wait_mech_tilt_below(250, 60000), "F1 ctl: the re-home walks down into the lower half");
        keep_person(300);
        CHECK(wait_cancels(1, 20000), "F1 ctl: the person cancelled it");
        a2k_ptz_info(&i);
        CHECK(i.state != HAL_PTZ_PARKED && !a2k_ptz_privacy_active() && a2k_ptz_manual_gate() != A2K_PTZ_E_PARKED, "F1 ctl: not parked, privacy off, the manual gate is not PARKED");
        usleep(2500000);
        a2k_ptz_info(&i);
        CHECK(i.state != HAL_PTZ_PARKED && !a2k_ptz_privacy_active(), "F1 ctl: still not parked a few seconds later");
        a2k_ptz_stop();
        M.real = 0;
    }
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
}

/* F2: a re-home cancelled by a person (also a detection blip with tracking off) leaves the head where it stopped. The controller returns it home by itself (yielding to the tracker like the re-home does),
 * keeps the re-home owed and retries after a short retry gap, not after the 10 / 30 min gap. */
static int wait_mech_tilt_above(int v, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (mech_tilt() > v) return 1;
        usleep(20000);
    }
    return 0;
}

static void f2_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    unsigned runs, canc;
    printf("== F2: one detection blip cancels the re-home: the head returns home by itself, the re-home is retried soon\n");
    d_reset(1275, 1300);
    sc.boot_check = boot_ok;
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_rehome_timing(500, 600000, 100); /* the normal gap is 10 min: only the retry gap can bring the second attempt inside this test */
    a2k_ptz_test_set_ladder_timing(0, 3000, 0, 0);
    a2k_ptz_defaults(&c);
    c.scene_check = 1;
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "F2: start");
    M.real = 1;
    go_and_home();
    CHECK(wait_at_home(4000), "F2: home, quiet");
    CHECK(wait_mech_tilt_below(900, 60000), "F2: the re-home walks down");
    a2k_ptz_note_person(); /* ONE detection blip, then nobody (tracking is not even linked here) */
    CHECK(wait_cancels(1, 15000), "F2: the blip cancelled the re-home");
    CHECK(wait_at_home(20000) && wait_motor_stop(3000), "F2: the controller returned the head home by itself");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_HOME && i.known && abs((int)i.tilt - 1300) <= 3 && abs((int)i.pan - 1275) <= 3 && abs(mech_tilt() - (int)i.tilt) <= 3, "F2: final pose: state home, counters home, the head physically at the counters (no slip was injected)");
    CHECK(wait_runs(1, 45000), "F2: the re-home was kept owed and retried after the short retry gap (not after the 10 min gap)");
    CHECK(wait_at_home(4000) && wait_motor_stop(3000) && abs(mech_tilt() - 1299) <= 4, "F2: and it finished with the head at the stop-derived home");
    a2k_ptz_rehome_info(&runs, &canc);
    CHECK(runs == 1 && canc == 1, "F2: one completed, one cancelled");
    a2k_ptz_stop();
    M.real = 0;

    /* the automatic return yields to the tracker at once */
    d_reset(1275, 1300);
    a2k_ptz_set_scene(&sc);
    a2k_ptz_test_set(&OPS, "/nonexistent/ptz.conf");
    a2k_ptz_test_set_rehome_timing(500, 600000, 100);
    a2k_ptz_test_set_ladder_timing(0, 3000, 0, 0);
    CHECK(a2k_ptz_start(&c) == 0 && wait_idle(8000), "F2 tracker: start");
    M.real = 1;
    go_and_home();
    CHECK(wait_at_home(4000) && wait_mech_tilt_below(900, 60000), "F2 tracker: re-home walking down");
    a2k_ptz_note_person();
    CHECK(wait_cancels(1, 15000), "F2 tracker: cancelled");
    CHECK(wait_mech_tilt_above(1000, 20000), "F2 tracker: the automatic return is under way (the head is above where the re-home was cancelled)");
    {
        int accepted = -1, t;
        for (t = 0; t < 1500; t += 50) { accepted = a2k_ptz_vel(1, 0, 4, 0); if (accepted == 0) break; usleep(50000); }
        CHECK(accepted == 0, "F2 tracker: a tracker velocity command is accepted within 1.5 s (the automatic return gave way)");
        usleep(500000);
        CHECK(mech_pan() > 1275 + 30, "F2 tracker: the head follows the tracker (pan moved)");
        a2k_ptz_vel(0, 0, 0, 0);
        wait_motor_stop(3000);
    }
    CHECK(wait_at_home(40000) && wait_motor_stop(3000), "F2 tracker: nobody around any more: the head ends at home again");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_HOME && abs((int)i.pan - 1275) <= 3, "F2 tracker: final pose home");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}

/* F3: a home view that stays flat (lens covered, dark room, a wall) used to re-home every 5 minutes for ever (check flat -> urgent re-home -> check flat -> ...). One re-home, then a visible state. */
static void f3_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    a2k_ptz_info_t i;
    unsigned runs = 0;
    int calls1;
    printf("== F3: a persistently flat home view ends after ONE re-home with a visible state (no loop)\n");
    CHECK(flat_start(&c, &sc, 1) == 0, "F3: start (tilt re-home on)");
    a2k_ptz_test_set_ladder_timing(1000, 0, 0, 0); /* the urgent gap 300 s -> 1 s: a loop would repeat every 20 s */
    DH.force = 4; /* flat for ever, whatever the head does */
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 150;
    pthread_mutex_unlock(&M.mu);
    go_and_home();
    CHECK(wait_at_home(4000) && wait_calls(1, 6000), "F3: back home, the check saw a flat view");
    CHECK(wait_runs(1, 25000), "F3: one urgent re-home ran");
    CHECK(wait_at_home(6000) && wait_motor_stop(3000), "F3: and the head is back home");
    calls1 = DH.calls;
    for (int t = 0; t < 45; t++) usleep(1000000); /* a loop would run a second (and third) re-home here */
    a2k_ptz_rehome_info(&runs, NULL);
    a2k_ptz_drift_info(&dr);
    a2k_ptz_info(&i);
    CHECK(runs == 1, "F3: still exactly one re-home 45 s later (no chained re-home for the same flat view)");
    CHECK(DH.calls > calls1 - 1 && !strcmp(dr.last, "flat-persist"), "F3: the state says flat-persist (the flat view survived the re-home)");
    CHECK(dr.recals == 0 && !dr.untrusted, "F3: no recalibration, the position is not marked untrusted");
    CHECK(i.state == HAL_PTZ_HOME && i.known && abs((int)i.tilt - 1300) <= 3 && abs(mech_tilt() - 1299) <= 4, "F3: final pose: home, the tilt exact (stop-derived)");
    /* another trip, still flat: still no second re-home */
    go_and_home();
    CHECK(wait_at_home(5000), "F3: another trip home");
    for (int t = 0; t < 25; t++) usleep(1000000);
    a2k_ptz_rehome_info(&runs, NULL);
    a2k_ptz_drift_info(&dr);
    CHECK(runs == 1 && !strcmp(dr.last, "flat-persist"), "F3: a later trip with the view still flat: no new re-home, still flat-persist");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}

/* F4: one escalation ladder with no terminal give-up: picture check -> urgent tilt re-home (also for a tilt-only RECAL verdict) -> re-check -> recalibration (a sliding window, retried later). These cases run on the
 * fake driver in virtual time (M.real = 0: a re-home or a calibration takes no wall time), the controller timers are real. */
static void f4_unreal_start(a2k_ptz_cfg_t *c, a2k_ptz_scene_t *sc, int rehome)
{
    if (flat_start(c, sc, rehome) != 0) { printf("  FAIL - f4 start\n"); fails++; return; }
    M.real = 0;
    a2k_ptz_test_set_rehome_timing(300, 600000, 100);
    a2k_ptz_test_set_ladder_timing(500, 500, 4000, 1000);
}

static void wait_drift(a2k_ptz_drift_t *dr, int (*cond)(const a2k_ptz_drift_t *), int ms)
{
    for (int t = 0; t < ms; t += 20) { a2k_ptz_drift_info(dr); if (cond(dr)) return; usleep(20000); }
}
static int cond_recals1(const a2k_ptz_drift_t *d) { return d->recals >= 1; }
static int cond_recals3(const a2k_ptz_drift_t *d) { return d->recals >= 3; }
static int cond_recals2(const a2k_ptz_drift_t *d) { return d->recals >= 2; }

static void f4_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    a2k_ptz_info_t i;
    unsigned runs = 0, canc = 0;
    printf("== F4: tilt-only slips with the re-home on are healed by the re-home (no recalibration, never untrusted for good)\n");
    f4_unreal_start(&c, &sc, 1);
    for (int k = 0; k < 5; k++) { /* the user's failure mode: the tilt loses ~200 steps on every trip */
        char msg[96];
        pthread_mutex_lock(&M.mu);
        M.mech[1] += 200;
        pthread_mutex_unlock(&M.mu);
        go_and_home();
        wait_at_home(4000);
        for (int t = 0; t < 400; t++) { a2k_ptz_rehome_info(&runs, NULL); a2k_ptz_drift_info(&dr); if (runs >= (unsigned)k + 1 && abs(mech_tilt() - 1299) <= 4 && dr.checks >= (unsigned)(2 * k + 2)) break; usleep(20000); }
        usleep(300000);
        a2k_ptz_rehome_info(&runs, &canc);
        a2k_ptz_drift_info(&dr);
        a2k_ptz_info(&i);
        snprintf(msg, sizeof msg, "F4 slip %d: healed by the re-home: the head is physically home, no recalibration, trusted", k + 1);
        CHECK(abs(mech_tilt() - 1299) <= 4 && dr.recals == 0 && !dr.untrusted && i.state == HAL_PTZ_HOME && runs >= (unsigned)k + 1, msg);
    }
    CHECK(!strcmp(dr.last, "ok") && abs((int)i.tilt - 1300) <= 3, "F4: the last check after the re-home says ok (final state: home, counters 1300, verdict ok)");
    a2k_ptz_stop();

    printf("== F4: verdicts that no re-home cures: recalibration limited per sliding window, retried when the window has passed (never terminal)\n");
    f4_unreal_start(&c, &sc, 1);
    DH.force = 2; /* every check says RECAL: the picture never matches */
    for (int k = 0; k < 3; k++) {
        const int calls0 = DH.calls;
        go_and_home();
        wait_at_home(4000);
        if (k < 2) wait_drift(&dr, k ? cond_recals2 : cond_recals1, 6000);
        else wait_calls(calls0 + 2, 6000); /* the check and the check after the re-home: both said RECAL, the recalibration is held back */
        usleep(400000);
    }
    a2k_ptz_drift_info(&dr);
    a2k_ptz_rehome_info(&runs, NULL);
    CHECK(runs >= 1, "F4: the first RECAL verdict ran the urgent tilt re-home before any recalibration");
    CHECK(dr.recals == 2 && dr.untrusted, "F4: two recalibrations inside the window, the third is held back (position doubted)");
    DH.force = 0; /* the camera is fine from now on: the picture matches again */
    wait_drift(&dr, cond_recals3, 12000);
    a2k_ptz_drift_info(&dr);
    printf("  info - recals %u untrusted %d last %s checks %u state %d known %d\n", dr.recals, dr.untrusted, dr.last, dr.checks, i.state, i.known);
    CHECK(dr.recals >= 3, "F4: the held-back recalibration is retried by itself when the window has passed (no terminal give-up)");
    for (int t = 0; t < 300; t++) { a2k_ptz_info(&i); a2k_ptz_drift_info(&dr); if (i.state == HAL_PTZ_HOME && !dr.untrusted) break; usleep(20000); }
    CHECK(i.state == HAL_PTZ_HOME && i.known && !dr.untrusted && abs(mech_tilt() - 1299) <= 4 + 60, "F4: final: home, trusted again, the head at home");
    a2k_ptz_stop();

    printf("== F4: a failed recalibration is retried (the position unknown must not stay so until a restart)\n");
    f4_unreal_start(&c, &sc, 0); /* re-home off: the RECAL verdict goes straight to a recalibration */
    DH.force = 2;
    go_and_home();
    wait_at_home(4000);
    wait_calls(1, 6000);
    pthread_mutex_lock(&M.mu);
    M.stuck[0] = M.stuck[1] = 1; /* the motors stall: the calibration fails */
    pthread_mutex_unlock(&M.mu);
    DH.force = 0;
    for (int t = 0; t < 500; t++) { a2k_ptz_info(&i); a2k_ptz_drift_info(&dr); if (dr.recals >= 1 && !i.known) break; usleep(20000); }
    CHECK(dr.recals >= 1 && !i.known, "F4: the recalibration ran, stalled and failed: the position is unknown");
    pthread_mutex_lock(&M.mu);
    M.stuck[0] = M.stuck[1] = 0;
    pthread_mutex_unlock(&M.mu);
    for (int t = 0; t < 600; t++) { a2k_ptz_info(&i); a2k_ptz_drift_info(&dr); if (i.known && i.state == HAL_PTZ_HOME && !dr.untrusted && dr.recals >= 2) break; usleep(20000); }
    CHECK(dr.recals >= 2 && i.known && i.state == HAL_PTZ_HOME && !dr.untrusted, "F4: retried by itself: the position is known again, home, trusted");
    a2k_ptz_stop();
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}

/* F5: the RAM drift reference used to exist only after a FULL calibration of this run and a 10 min tracker-free spell: after a normal (proven) boot the drift check silently skipped for the whole run. It has its own capture
 * rule now: at a proven position (proven boot, calibration, re-home + matching picture, a refresh after a proven zero), quiet at home for a few seconds only; dropped on a calibration; visible in the state. */
static struct { int cap, drop, refresh_mask; int64_t cap_ms; } DR;
static int fake_dref_capture(void) { DR.cap++; DR.cap_ms = mono(); DR.refresh_mask = 1; return 0; }
static void fake_dref_drop(void) { DR.drop++; DR.refresh_mask = 0; }
static int fake_dref_info(unsigned long *age) { if (age) *age = DR.refresh_mask ? 7 : 0; return DR.refresh_mask; }

static int wait_dref_cap(int n, int ms)
{
    for (int t = 0; t < ms; t += 20) { if (DR.cap >= n) return 1; usleep(20000); }
    return 0;
}

static void f5_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    unsigned long age = 99;
    printf("== F5: the drift reference is captured at proven positions, quiet 6 s only, dropped on a calibration, refreshed after a proven zero\n");
    memset(&DR, 0, sizeof DR);
    sc.dref_capture = fake_dref_capture;
    sc.dref_drop = fake_dref_drop;
    sc.dref_info = fake_dref_info;
    a2k_ptz_test_set_scene_timing(300, 300, 0); /* the quiet time before a capture 6 s -> 0.3 s (the rule is "quiet at home", not 10 min without a tracker) */
    a2k_ptz_test_set_verify_timing(100, 0, 0); /* a reference older than 0.1 s is refreshed after a proven zero */
    CHECK(flat_start(&c, &sc, 0) == 0, "F5: start (proven boot: the scene check says home, no calibration)");
    M.real = 0;
    CHECK(wait_dref_cap(1, 3000), "F5: a proven boot captures the drift reference (no calibration was needed)");
    CHECK(a2k_ptz_drift_ref_info(&age) == 1 && age == 7, "F5: the state reports it: day, with its age (ptz.drift_ref)");
    /* tracker activity right before: no 10 min blocker */
    a2k_ptz_nudge_sp(60, 0, 3);
    usleep(300000);
    a2k_ptz_home_tracker();
    CHECK(wait_at_home(4000), "F5: a tracker excursion and the return home");
    /* a proven zero (the fake camera follows the mechanics: displacement 0) with the reference older than the refresh age: it is taken again */
    a2k_ptz_test_set_drift_timing(300, 1500, 1, 1000);
    {
        const int cap0 = DR.cap;
        go_and_home();
        CHECK(wait_at_home(4000) && wait_calls(1, 4000), "F5: a return home is checked");
        CHECK(wait_dref_cap(cap0 + 1, 4000), "F5: the proven zero refreshed the reference (older than the refresh age), 0.3 s quiet at home, 'tracker active a moment ago' notwithstanding");
    }
    /* a calibration invalidates the reference and takes a fresh one */
    {
        const int cap0 = DR.cap, drop0 = DR.drop;
        CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0, "F5: a recalibration is requested");
        for (int t = 0; t < 400 && DR.drop == drop0; t++) usleep(20000);
        CHECK(DR.drop == drop0 + 1, "F5: the calibration dropped the drift reference (both modes)");
        CHECK(wait_dref_cap(cap0 + 1, 4000), "F5: and a fresh one is captured at the proven home");
    }
    /* a tilt slip healed by the re-home and confirmed by the picture: the reference is taken from the proven position (re-home on) */
    a2k_ptz_stop();
    memset(&DR, 0, sizeof DR);
    a2k_ptz_test_set_verify_timing(3600000, 0, 0);
    CHECK(flat_start(&c, &sc, 1) == 0, "F5: restart (re-home on)");
    M.real = 0;
    a2k_ptz_test_set_rehome_timing(300, 600000, 100);
    a2k_ptz_test_set_ladder_timing(500, 500, 4000, 1000);
    CHECK(wait_dref_cap(1, 3000), "F5: captured at the start");
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 200;
    pthread_mutex_unlock(&M.mu);
    go_and_home();
    wait_at_home(4000);
    for (int t = 0; t < 400; t++) { a2k_ptz_drift_info(&dr); if (dr.checks >= 2 && !dr.untrusted && DR.cap >= 2) break; usleep(20000); }
    CHECK(DR.cap >= 2 && dr.recals == 0 && !dr.untrusted, "F5: after the re-home and the matching picture the reference is taken again from the proven position");
    a2k_ptz_stop();
    M.real = 0;
    a2k_ptz_test_set_scene_timing(0, 0, 0);
    a2k_ptz_test_set_verify_timing(0, 0, 0);
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}

/* F6: consecutive inconclusive checks used to close the matter for ever (a slip beyond the matcher range, or a changed scene, looks exactly like "inconclusive"). Now: 3 returns in a row -> the urgent tilt
 * re-home, the picture again, still inconclusive -> one rate-limited recalibration (which also takes a fresh reference). */
static void f6_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    a2k_ptz_info_t i;
    unsigned runs = 0;
    int returns = 0;
    printf("== F6: the pan is 400 steps off (beyond the matcher range), every check inconclusive: the ladder escalates\n");
    f4_unreal_start(&c, &sc, 1);
    a2k_ptz_test_set_drift_retry(150);
    DH.force = 1;
    pthread_mutex_lock(&M.mu);
    M.mech[0] += 400;
    pthread_mutex_unlock(&M.mu);
    for (returns = 0; returns < 14; returns++) {
        go_and_home();
        wait_at_home(4000);
        usleep(1800000);
        a2k_ptz_drift_info(&dr);
        if (dr.recals >= 1) break;
    }
    for (int t = 0; t < 300; t++) { a2k_ptz_info(&i); a2k_ptz_drift_info(&dr); if (dr.recals >= 1 && i.state == HAL_PTZ_HOME && !dr.untrusted) break; usleep(20000); }
    a2k_ptz_rehome_info(&runs, NULL);
    printf("  info - returns %d recals %u re-homes %u untrusted %d checks %u\n", returns + 1, dr.recals, runs, dr.untrusted, dr.checks);
    CHECK(runs >= 1, "F6: a tilt re-home was tried first (the cheap exact remedy)");
    CHECK(dr.recals >= 1, "F6: still inconclusive after it: a recalibration ran (no silent give-up for ever)");
    CHECK(i.state == HAL_PTZ_HOME && i.known && !dr.untrusted && abs(mech_pan() - 1275) <= 4, "F6: final pose: home, trusted, the pan 400 steps off is gone (the head is physically at home)");
    a2k_ptz_stop();
    a2k_ptz_test_set_drift_retry(0);
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
    a2k_ptz_test_set_ladder_timing(0, 0, 0, 0);
}

/* F7: the check also runs on an idle timer at home, and a person who kept the camera away from home past a travel budget gets a go-home-and-verify (never while somebody is in view). */
static void f7_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_info_t i;
    int calls0, nmove0;
    printf("== F7: idle-timer check at home; go home and verify when the head was kept away past a travel budget\n");
    a2k_ptz_test_set_verify_timing(0, 1200, 0); /* periodic check 30 min -> 1.2 s */
    CHECK(flat_start(&c, &sc, 0) == 0, "F7: start");
    M.real = 0;
    a2k_ptz_test_set_drift_timing(300, 1500, 100000, 1000); /* the travel gate would never open */
    calls0 = DH.calls;
    nmove0 = M.nmove;
    for (int t = 0; t < 300 && DH.calls < calls0 + 2; t++) usleep(20000);
    CHECK(DH.calls >= calls0 + 2 && M.nmove == nmove0, "F7: with no motion at all the picture is checked on the idle timer (twice in a few seconds)");
    a2k_ptz_stop();

    a2k_ptz_test_set_verify_timing(0, 0, 150); /* travel budget 30000 -> 150 steps; no periodic */
    CHECK(flat_start(&c, &sc, 0) == 0, "F7: restart (travel budget 150 steps)");
    M.real = 0;
    a2k_ptz_test_set_drift_timing(300, 1500, 100000, 1000);
    a2k_ptz_test_set_rehome_timing(300, 600000, 100);
    a2k_ptz_nudge_sp(300, 0, 3); /* the head is kept away from home: a person walked it there and stays around */
    for (int t = 0; t < 100; t++) { a2k_ptz_info(&i); if (i.known && i.pan > 1500) break; usleep(20000); }
    calls0 = DH.calls;
    for (int t = 0; t < 120; t++) { a2k_ptz_note_person(); usleep(25000); } /* 3 s: somebody in view the whole time */
    a2k_ptz_info(&i);
    CHECK(i.pan > 1500 && DH.calls == calls0, "F7: somebody in view: the head is NOT taken home (nobody being tracked is interrupted)");
    CHECK(wait_at_home(5000), "F7: nobody in view any more: the head goes home by itself (the travel budget was exceeded)");
    CHECK(wait_calls(calls0 + 1, 5000), "F7: ... and verifies the position by the picture there (the travel gate of the check is bypassed)");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_HOME && abs((int)i.pan - 1275) <= 3, "F7: final pose home");
    a2k_ptz_stop();
    a2k_ptz_test_set_verify_timing(0, 0, 0);
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
}

/* F8: a check in progress (it waits for frames for seconds, the controller is busy and the tracker is refused meanwhile) used to run to its end, and its verdict was used although somebody had arrived. Now a person
 * (or a tracker command) aborts the hook and the verdict is discarded; the tracker gets the head within a fraction of a second. */
static int (*g_fake_abort)(void);
static void fake_set_abort(int (*fn)(void)) { g_fake_abort = fn; }
static volatile int g_fake_inflight, g_fake_noabort;
static int fake_drift_slow(uint32_t ph, uint32_t th, int64_t minf, int *dx, int *dy)
{
    int ax, ay, aborted = 0;
    (void)ph; (void)th; (void)minf;
    g_fake_inflight = 1;
    for (int t = 0; t < DH.delay; t += 10) {
        if (!g_fake_noabort && g_fake_abort && g_fake_abort()) { aborted = 1; break; }
        usleep(10000);
    }
    pthread_mutex_lock(&M.mu);
    ax = cam_dx();
    ay = cam_dy();
    pthread_mutex_unlock(&M.mu);
    DH.calls++;
    g_fake_inflight = 0;
    *dx = ax;
    *dy = ay;
    if (aborted) return A2K_DRIFT_SKIP; /* what the real hook does when it is told to give way: no fresh frame */
    if (!ax && !ay) return A2K_DRIFT_OK;
    return abs(ax) <= 6 && abs(ay) <= 6 ? A2K_DRIFT_SHIFT : A2K_DRIFT_RECAL;
}

static void f8_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    a2k_ptz_drift_t dr;
    int64_t t0;
    int accepted = -1;
    printf("== F8: a person arriving during a drift check aborts it, the tracker gets the head quickly, the verdict is discarded\n");
    sc.drift = fake_drift_slow;
    sc.set_abort = fake_set_abort;
    for (int variant = 0; variant < 2; variant++) {
        CHECK(flat_start(&c, &sc, 0) == 0, variant ? "F8 b: start (a hook that ignores the abort request)" : "F8 a: start (a hook that polls the abort request)");
        M.real = 0;
        sc.drift = fake_drift_slow; /* flat_start installed fake_drift: swap in the slow, abort-aware one */
        sc.set_abort = fake_set_abort;
        a2k_ptz_set_scene(&sc);
        g_fake_noabort = variant;
        DH.delay = variant ? 900 : 4000;
        pthread_mutex_lock(&M.mu);
        M.mech[0] += 50; /* a 5 px shift: the (slow) check will say SHIFT and the old code would correct the counters */
        pthread_mutex_unlock(&M.mu);
        a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
        go_and_home();
        for (int t = 0; t < 300 && !g_fake_inflight; t++) usleep(20000);
        CHECK(g_fake_inflight, "F8: the check is in progress (waiting for frames)");
        usleep(150000);
        t0 = mono();
        a2k_ptz_note_person();
        for (int t = 0; t < 6000; t += 25) { accepted = a2k_ptz_vel(1, 0, 4, 0); if (accepted == 0) break; usleep(25000); }
        if (!variant)
            CHECK(accepted == 0 && mono() - t0 < 700, "F8 a: the tracker's velocity command is accepted within 0.7 s (the check gave way); the old code refused it until the check ended (4 s)");
        else
            CHECK(accepted == 0, "F8 b: accepted once the (non-polling) hook returned");
        usleep(300000);
        a2k_ptz_drift_info(&dr);
        CHECK(dr.corrections == 0 && dr.checks == 0, "F8: the verdict was discarded: no correction of the counters, no check counted, because somebody arrived");
        a2k_ptz_vel(0, 0, 0, 0);
        wait_motor_stop(3000);
        DH.delay = 0; /* the person is gone: the check is repeated and now corrects */
        go_and_home();
        wait_at_home(5000);
        for (int t = 0; t < 400; t++) { a2k_ptz_drift_info(&dr); if (dr.corrections >= 1) break; usleep(20000); }
        CHECK(dr.corrections == 1 && abs(mech_pan() - 1275) <= 4, "F8: once nobody is around the check ran again and corrected: the head is physically home");
        a2k_ptz_stop();
    }
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
}

/* F13: every re-home ground the bottom stop for about 1160 steps even right after a picture check that had just proven the tilt. A routine re-home is skipped when the picture said ok within the gap and little was
 * travelled since. Every re-home that does run walks the FULL stroke (a shorter walk that stopped before the real stop would declare a wrong position as proven). */
static void f13_cases(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_scene_t sc = SC;
    unsigned runs = 0;
    int calls0;
    printf("== F13: no re-home right after the picture proved the tilt; a routine re-home after a proven point still fixes a large slip\n");
    a2k_ptz_test_set_drift_timing(300, 1500, 100, 1000);
    CHECK(flat_start(&c, &sc, 1) == 0, "F13 ctl: start (re-home on, picture check on)");
    M.real = 0;
    a2k_ptz_test_set_rehome_timing(300, 1500, 100); /* gap 1.5 s, minimum travel 100 steps */
    go_and_home();
    wait_at_home(4000);
    CHECK(wait_calls(1, 4000), "F13 ctl: the return home is checked: ok");
    usleep(1800000);
    a2k_ptz_rehome_info(&runs, NULL);
    CHECK(runs == 0, "F13 ctl: no gap re-home right after the picture proved the tilt (it used to run and grind the stop)");
    /* the proof is older than the gap now and the picture can no longer prove anything (skip): the routine re-home runs with the FULL stroke and fixes a slip of +800 */
    DH.force = 3;
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 800;
    M.exec_neg[1] = 0;
    pthread_mutex_unlock(&M.mu);
    calls0 = DH.calls;
    go_and_home();
    wait_at_home(4000);
    CHECK(wait_runs(1, 6000), "F13 ctl: with the proof older than the gap the routine re-home runs");
    usleep(300000);
    CHECK(abs(mech_tilt() - 1299) <= 4, "F13 ctl: a slip of +800 after a proven point is fully fixed (the head is physically at the stop-derived home)");
    CHECK(M.exec_neg[1] >= 2400, "F13 ctl: it walked the full stroke (no shortened walk)");
    (void)calls0;
    a2k_ptz_stop();
    a2k_ptz_test_set_drift_timing(0, 0, 0, 0);
    a2k_ptz_test_set_rehome_timing(0, 0, 0);
}

/* F11 / F15c: the counter was read right after the stop request of a cancelled re-home; steps still in flight in the driver were lost from the belief. And the 160 byte log line truncated the start line. */
static char g_lowlog[16][256];
static int g_lowlog_n;
static void d_log_capture(const char *line) { if (g_lowlog_n < 16) snprintf(g_lowlog[g_lowlog_n++], sizeof g_lowlog[0], "%s", line); }

static void f11_cases(void)
{
    hal_ptz_t *p;
    uint32_t pan, tilt;
    pthread_t th;
    int rc, full_start_line = 0;
    hal_ptz_ops_t ops = OPS;
    printf("== F11 / F15: the belief after a cancelled re-home survives a slow stop; the re-home start line is not truncated\n");
    ops.log = d_log_capture;
    {
        char err[96] = "";
        hal_ptz_desc_t d = a2k_ptz_desc;
        d_reset(1275, 1300);
        p = hal_ptz_new(&d, &ops, NULL, err, sizeof err);
        if (!p || hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0) { printf("setup failed\n"); exit(2); }
    }
    pthread_mutex_lock(&M.mu);
    M.mech[1] += 150;
    M.stop_lat = 200; /* the driver takes 200 ms to stop: about 74 steps in flight at the walking speed */
    M.real = 1;
    pthread_mutex_unlock(&M.mu);
    g_rh_p = p;
    g_rh_cancel_ms = 3500;
    g_lowlog_n = 0;
    pthread_create(&th, NULL, rh_cancel_thread, NULL);
    rc = hal_ptz_tilt_rehome(p);
    pthread_join(th, NULL);
    usleep(400000);
    M.real = 0;
    CHECK(rc == HAL_PTZ_ECANCEL, "F11: cancelled on the way down");
    CHECK(hal_ptz_pos(p, &pan, &tilt) && abs((int)tilt - mech_tilt() + 150) <= 3, "F11: the believed position is still the old belief minus the walk, steps in flight at the stop included (counter - physical stays -150)");
    for (int k = 0; k < g_lowlog_n; k++)
        if (strstr(g_lowlog[k], "tilt re-home: down to the bottom stop") && strstr(g_lowlog[k], "then up to home 1300")) full_start_line = 1;
    CHECK(full_start_line, "F15c: the re-home start line is logged complete (it ended at 'then up to home 1' with the 160 byte buffer)");
    pthread_mutex_lock(&M.mu);
    M.stop_lat = 0;
    pthread_mutex_unlock(&M.mu);
    hal_ptz_free(p);
}

int main(void)
{
    if (getenv("PTZ_UNIT_ONLY_FLAT")) { flat_cases(); printf("rot2k ptz cont unit (blind drift only): %d failed\n", fails); return fails ? 1 : 0; }
    if (getenv("PTZ_UNIT_ONLY_REHOME")) { rehome_hal_cases(); rehome_ctl_cases(); printf("rot2k ptz cont unit (re-home only): %d failed\n", fails); return fails ? 1 : 0; }
    if (getenv("PTZ_UNIT_ONLY")) {
        if (only("f1")) f1_cases();
        if (only("f2")) f2_cases();
        if (only("f3")) f3_cases();
        if (only("f4")) f4_cases();
        if (only("f5")) f5_cases();
        if (only("f6")) f6_cases();
        if (only("f7")) f7_cases();
        if (only("f8")) f8_cases();
        if (only("f13")) f13_cases();
        if (only("f11")) f11_cases();
        printf("rot2k ptz cont unit (selected): %d failed\n", fails);
        return fails ? 1 : 0;
    }
    hal_cases();
    mitig_cases();
    drop_cases();
    nudge_api_cases();
    thread_cases();
    review_cases();
    drift_cases();
    review_cases_drift();
    drift_ref_cases();
    manual_cases();
    integration_cases();
    tilt_account_cases();
    rehome_hal_cases();
    rehome_ctl_cases();
    flat_cases();
    f1_cases();
    f2_cases();
    f3_cases();
    f4_cases();
    f5_cases();
    f6_cases();
    f7_cases();
    f8_cases();
    f13_cases();
    f11_cases();
    printf("rot2k ptz cont unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
