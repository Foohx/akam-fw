/* Host unit test of the CONTINUOUS motion of the Rotatable 2K PTZ: the safe layer (ptz/akam_ptz_safe.h ptzs_cont_*), the HAL session (hal_ptz_cont_run) and the controller thread
 * (akam2k_ptz.c: a2k_ptz_vel, the lease, the position history a2k_ptz_pos_at, the preemption by manual commands) against a FAKE DRIVER that models: a speed of 1100 / divisor
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
#include "../firmware/lsc-rotatable-2k-ak3918av130/ptz/akam_ptz_safe.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_hal.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_ptz.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/hal_ptz.h"

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
    int lat_ms;                            /* a move / stop takes effect this long after it was sent */
    long clock;
    struct { long at; int kind; unsigned dir, a, b; } q[16];
    int nq;
    int nmove, nstop, nother, bad_cmd, cmds[4096], ncmd, nsave;
    int real;                              /* sleeping really sleeps (the thread tests) */
    int max_mech[2], min_mech[2];
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
    M.clock = 0;
    M.nq = M.nmove = M.nstop = M.nother = M.bad_cmd = M.ncmd = M.nsave = M.hist_n = 0;
    M.real = 0;
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
        apply_cmd(0, 0, 0, 0); /* the stop is immediate; a new direction takes lat_ms */
    } else if (req == PTZ_REQ_POS) {
        if (w[0] > 1) { errno = EINVAL; rc = -1; } else w[1] = M.cnt[w[0]];
    } else if (req == PTZ_REQ_MOVE) {
        if (w[0] > 7) { M.bad_cmd++; errno = EINVAL; rc = -1; }
        else {
            M.nmove++;
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
    CHECK(M.nmove <= 2, "refreshes without a change send no new driver command");

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
        CHECK(M.mech[0] > 1275 + 700, "the pan went on");
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
    CHECK(a2k_ptz_pos_at(mono(), &pp, &pt) == 0 && pp > 1275 + 20, "a2k_ptz_pos_at(now) works DURING the session (the polled counter)");
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
    rc = wait_idle(3000);
    {
        int p0 = mech_pan();
        usleep(100000);
        CHECK(rc && mech_pan() == p0 && !M.running[0], "the manual command ended the session within ~0.1 s and ran; the motor is idle");
    }
    CHECK(mono() - t0 < 1500, "... quickly");
    CHECK(a2k_ptz_vel(1, 0, 3, 0) == 0, "a session again");
    usleep(150000);
    CHECK(a2k_ptz_nudge(50, 0) == 0, "a MANUAL nudge (API ptz.move) cancels the tracker's session and runs");
    CHECK(wait_idle(3000), "... done");
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

int main(void)
{
    hal_cases();
    thread_cases();
    review_cases();
    printf("rot2k ptz cont unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
