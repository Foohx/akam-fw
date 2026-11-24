/* hal_ptz engine (hal_ptz.h): boot calibration, saved position, goto / step / nudge on top of the safe-motion layer
 * helpers/ptz/akam_ptz_safe.h. No I/O of its own: the driver and the files come in through hal_ptz_ops_t.
 *
 * Calibration, "declare-max-then-move" (the stock mechanism, ptz-findings.md "Calibration / self-check in the stock app": the stock
 * `check` commands 12 / 9 set the counter to the step maximum and walk the motor toward 0 until the driver stops at 0, so the
 * motor ends against the mechanical stop whatever the start). We do exactly that with the hardware-verified directional commands
 * instead of the unverified 9 / 12 (so the anti-brick rule "commands 0..7 only" still holds):
 *   1. init the driver in the stock frame: limits = step maximum, counters = step maximum ("I am at the far end");
 *   2. tilt DOWN (command 4), then pan LEFT (command 6) at the configured speed: the driver walks its counter to 0 and stops by
 *      itself (at most step_max steps); the real tilt travel (about 2315) is longer than the declared maximum (2160), so the tilt sweep is
 *      repeated once for TILT_EXTRA_STEPS (counter re-declared, walk down again): both axes then sit on their stops = real 0 (measured);
 *   3. init 1/1 in an "approach" frame whose driver limits are home + 20, and goto home (the safe layer, polled): even a killed
 *      process cannot overshoot home;
 *   4. init the final shifted frame (driver limits = soft range) with the camera at home.
 * Grinding against the stop for the rest of the counted steps is inherent (the stock does it every boot, at speed 1). It happens
 * at every cold boot where the scene check cannot prove the head is at HOME, and never on an app restart in the same boot (adoption).
 *
 * Position persistence: NONE on the flash. The position lives in RAM (and the frame files in /tmp, RAM too, for an adoption in the same
 * boot). A cold boot claims only HOME, and only when the scene check proves it from the image; anything else calibrates. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#define PTZS_QUIET 1
#include "akam_ptz_safe.h"

#include "hal_ptz.h"

/* cancel / canceled are set by one thread (the caller) and consumed by another (the motion thread): atomic, no data race */
#define LD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define PTZ_REQ_INIT_LIMITS 0x40100c03u /* _IOW(0x0c,3,16), as the stock app sends it once at start */
#define PTZ_REQ_INIT_POS 0x40280c04u    /* _IOW(0x0c,4,40) */
#define PTZ_INIT_LIMITS_WORDS 4
#define PTZ_INIT_POS_WORDS 10
#define APPROACH_MARGIN 20u             /* driver limit of the approach: home + this */
#define STATE_TOL 15u                   /* a position this close to home / park is "home" / "parked" */
#define SAVED_MAX 48                    /* longest "pan=N tilt=M\n" kept in RAM */
#define VERIFY_TOL 4u                   /* steps: counters re-read before a scene reference is written */
#define SHORT_PAN_MARGIN 400u           /* short calibration: the counters are declared this far above home (pan) ... */
#define SHORT_TILT_MARGIN 300u          /* ... and this far (tilt): the head is believed within this of home, the sweep still ends on the stop */
#define TILT_EXTRA_STEPS 300u           /* second tilt sweep: the real travel (about 2315) is longer than the declared step maximum (2160) */

struct hal_ptz {
    hal_ptz_desc_t d;
    hal_ptz_ops_t ops;
    struct ptz_ops sops; /* the safe layer's view of ops (the saved position is routed to the store) */
    struct ptzs_conf cf;
    struct ptzs_ctx x;
    int fd;
    int trusted;    /* the counters and the frame can be relied on */
    int cal_active;
    int nocancel;   /* a cancel is not taken by the position reads (the re-declare steps of the tilt re-home) */
    int moving;
    int defer_frames; /* drv_init keeps the frame in RAM only (a probe: the files are written once the position is proven) */
    int pos_valid;  /* pan / tilt below are the last known position (false while moving) */
    int explicit_pose; /* the pose is the result of an explicit command (goto / park / home / step / session) or of the start: only then can it count as "parked"; a re-home settle or a re-declare clears it (the park pose is never inferred from a settle) */
    uint32_t pan, tilt;
    int cancel;
    volatile unsigned cancel_gen; /* bumped by every hal_ptz_cancel (any thread) */
    unsigned mark_gen;            /* cancel_gen at hal_ptz_mark() */
    int marked;
    int canceled;
    int scene_claim;             /* this start's position is a claim proven by the scene check (a failure of the position later forgets the reference) */
    unsigned cal_runs;           /* completed calibrations (this process) */
    uint32_t bpan, btilt;        /* the position when the current motion began */
    unsigned wpan, wtilt;        /* the weight of a step of each axis in the travel of the current goto / step / nudge (1, 2 at divisor 3, 3 at divisor 2) */
    unsigned long travel;        /* weighted steps travelled by tracked motions since the last calibration (this process): see the continuous block of akam_ptz_safe.h */
    unsigned long reversals;     /* axis reversals of continuous sessions since the last calibration */
};

static int near(uint32_t a, uint32_t b);
static hal_ptz_t *g_hp; /* the safe layer's ops carry no context: one motor per process */

static void plog(hal_ptz_t *p, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    if (!p->ops.log)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    p->ops.log(line);
}

/* ---- the position: RAM only. The safe layer reports it through PTZ_POS_PATH ("pan=N tilt=M\n" or "unknown\n"); nothing reaches the flash
 * (no recurring write in normal operation: a cold boot never trusts a stored position anyway, it proves HOME from the image or calibrates). ---- */
static int store_pos(hal_ptz_t *p, const char *data)
{
    uint32_t a, b;
    if (!strcmp(data, "unknown\n")) {
        p->pos_valid = 0;
        return 0;
    }
    if (ptzs_pos_parse(data, &a, &b))
        return -1;
    p->pan = a;
    p->tilt = b;
    p->pos_valid = 1;
    p->x.pos_marked = 0; /* a position was stored by another path than ptzs_save_pos (the repair read of end_t, a declared position): the next move marks it unknown again */
    return 0;
}

/* The stored scene reference must no longer be used (best effort: a rare one-off event, the hook counts its write). */
static void scene_forget(hal_ptz_t *p, const char *why)
{
    p->scene_claim = 0;
    if (p->ops.scene_drop && p->ops.scene_drop() != 0)
        plog(p, "ptz: the scene reference could not be invalidated (%s)", why);
}

/* ---- ops seen by the safe layer ---- */
static int w_open(void) { return g_hp->ops.open_dev(); }
static int w_ioctl(int fd, unsigned long req, void *arg)
{
    if (req == PTZ_REQ_POS && LD(g_hp->cancel) && !g_hp->nocancel) { /* (nocancel: the re-declare steps of the tilt re-home must finish: a stale or repeated cancel is taken later) a cancel is taken at the next poll: the loops see a read error and stop the motor */
        ST(g_hp->cancel, 0);
        ST(g_hp->canceled, 1);
        errno = EINTR;
        return -1;
    }
    return g_hp->ops.ioctl_fn(fd, req, arg);
}
static void w_sleep(unsigned ms) { g_hp->ops.sleep_ms(ms); }
static int w_load(const char *path, char *buf, unsigned cap) { return g_hp->ops.load_file(path, buf, cap); }
static int w_save(const char *path, const char *data)
{
    return strcmp(path, PTZ_POS_PATH) ? g_hp->ops.save_file(path, data) : store_pos(g_hp, data);
}

/* ---- construction ---- */
static void conf_from_desc(struct ptzs_conf *c, const hal_ptz_desc_t *d)
{
    c->pan_min = d->pan_min;
    c->pan_max = d->pan_max;
    c->tilt_min = d->tilt_min;
    c->tilt_max = d->tilt_max;
    c->pan_home = d->pan_home;
    c->tilt_home = d->tilt_home;
    c->pan_step_max = d->pan_step_max;
    c->tilt_step_max = d->tilt_step_max;
    c->speed = d->speed;
    c->tilt_min_div = PTZS_DEF_TILT_MIN_DIV;
    c->settle_ms = PTZS_DEF_SETTLE_MS;
    c->ramp_ms = PTZS_DEF_RAMP_MS;
    c->px_pan_x10 = PTZS_DEF_PX_PAN_X10;
    c->px_tilt_x10 = PTZS_DEF_PX_TILT_X10;
    c->drift_cum_max = PTZS_DEF_DRIFT_CUM_MAX;
}

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

hal_ptz_t *hal_ptz_new(const hal_ptz_desc_t *d, const hal_ptz_ops_t *ops, const char *conf_text, char *err, size_t errcap)
{
    static hal_ptz_t inst; /* one motor per process: no heap */
    hal_ptz_t *p = &inst;
    char e[96] = { 0 };
    int i;
    memset(p, 0, sizeof *p);
    p->d = *d;
    p->ops = *ops;
    p->fd = -1;
    p->explicit_pose = 1;
    for (i = 0; i < HAL_PTZ_NDIR; i++)
        if (d->dir[i] > 7)
            goto bad_dir;
    conf_from_desc(&p->cf, d);
    if (conf_text ? ptzs_conf_parse_over(conf_text, &p->cf, e, sizeof e) : ptzs_conf_check(&p->cf, e, sizeof e)) {
        if (err)
            snprintf(err, errcap, "%s", e);
        return NULL;
    }
    p->d.pan_min = p->cf.pan_min;
    p->d.pan_max = p->cf.pan_max;
    p->d.tilt_min = p->cf.tilt_min;
    p->d.tilt_max = p->cf.tilt_max;
    p->d.pan_home = p->cf.pan_home;
    p->d.tilt_home = p->cf.tilt_home;
    p->d.pan_step_max = p->cf.pan_step_max;
    p->d.tilt_step_max = p->cf.tilt_step_max;
    p->d.speed = p->cf.speed;
    if (p->d.step < 1 || p->d.step > 1000)
        p->d.step = 200;
    p->d.pan_park = clampu(p->d.pan_park, p->d.pan_min, p->d.pan_max); /* a park outside the soft range is pulled in */
    p->d.tilt_park = clampu(p->d.tilt_park, p->d.tilt_min, p->d.tilt_max);
    p->sops.open_dev = w_open;
    p->sops.ioctl_fn = w_ioctl;
    p->sops.sleep_ms = w_sleep;
    p->sops.load_file = w_load;
    p->sops.save_file = w_save;
    p->x.ops = &p->sops;
    p->x.cf = &p->cf;
    g_hp = p;
    return p;
bad_dir:
    if (err)
        snprintf(err, errcap, "direction map: command words must be 0..7");
    return NULL;
}

void hal_ptz_free(hal_ptz_t *p)
{
    if (p == g_hp)
        g_hp = NULL;
}

const hal_ptz_desc_t *hal_ptz_desc(const hal_ptz_t *p) { return &p->d; }

/* The frame files hold the frame the driver was initialised with: written after a verified init, invalid otherwise. The app only
 * ever adopts PTZ_APP_FRAME_PATH (the tool invalidates it on any motion). Invalidated at every init, stall and failed start. */
static int frames_invalidate(hal_ptz_t *p)
{
    int a = p->ops.save_file(PTZ_FRAME_PATH, "invalid\n"), b = p->ops.save_file(PTZ_APP_FRAME_PATH, "invalid\n");
    return a || b ? -1 : 0;
}

/* ---- driver init (the stock sequence: limits, 1 s, counters, 1 s) ---- */
static int drv_init(hal_ptz_t *p, const uint32_t lim[2], const uint32_t cnt[2], const struct ptzs_frame *fr)
{
    struct ptzs_ctx *x = &p->x;
    uint32_t l[PTZ_INIT_LIMITS_WORDS] = { 0, lim[0], lim[1], 0 }, c[PTZ_INIT_POS_WORDS] = { 0 }, rp, rt;
    char s[128];
    c[0] = cnt[0];
    c[1] = cnt[1];
    p->trusted = 0;
    x->have_frame = 0;
    if (frames_invalidate(p)) {
        plog(p, "ptz: cannot write %s: %s", PTZ_FRAME_PATH, strerror(errno));
        return HAL_PTZ_EIO;
    }
    if (p->ops.ioctl_fn(p->fd, PTZ_REQ_INIT_LIMITS, l)) {
        plog(p, "ptz: init (limits) failed: %s", strerror(errno));
        return HAL_PTZ_EIO;
    }
    p->ops.sleep_ms(1000);
    if (p->ops.ioctl_fn(p->fd, PTZ_REQ_INIT_POS, c)) {
        plog(p, "ptz: init (position) failed: %s", strerror(errno));
        return HAL_PTZ_EIO;
    }
    p->ops.sleep_ms(1000);
    if (ptzs_read_pos(x, &rp, &rt) || x->raw[0] != cnt[0] || x->raw[1] != cnt[1]) {
        plog(p, "ptz: init: the driver reads %u/%u, expected %u/%u", (unsigned)x->raw[0], (unsigned)x->raw[1],
             (unsigned)cnt[0], (unsigned)cnt[1]);
        return HAL_PTZ_EIO;
    }
    if (fr) {
        x->fr = *fr;
        ptzs_frame_format(fr, s, sizeof s);
        if (!p->defer_frames && (p->ops.save_file(PTZ_FRAME_PATH, s) || p->ops.save_file(PTZ_APP_FRAME_PATH, s)))
            return HAL_PTZ_EIO;
        x->have_frame = 1;
    }
    return 0;
}

/* The camera is at real (pan, tilt): declare it in the final shifted frame (the driver's limits are the soft range). */
static int declare_final(hal_ptz_t *p, uint32_t pan, uint32_t tilt)
{
    struct ptzs_frame fr;
    char s[SAVED_MAX];
    uint32_t lim[2], cnt[2];
    int rc;
    ptzs_frame_from_conf(&fr, &p->cf);
    lim[0] = fr.mx[0] - (fr.mn[0] - 1);
    lim[1] = fr.mx[1] - (fr.mn[1] - 1);
    cnt[0] = pan - (fr.mn[0] - 1);
    cnt[1] = tilt - (fr.mn[1] - 1);
    rc = drv_init(p, lim, cnt, &fr);
    if (rc)
        return rc;
    snprintf(s, sizeof s, "pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
    (void)store_pos(p, s);
    p->trusted = 1;
    return 0;
}

/* One sweep: the driver counter was declared at its maximum; walk the axis to 0 (the driver stops itself there). */
static int sweep(hal_ptz_t *p, unsigned ax, uint32_t steps)
{
    struct ptzs_ctx *x = &p->x;
    const unsigned dir = p->d.dir[ax ? HAL_PTZ_DOWN : HAL_PTZ_LEFT], sp = p->cf.speed;
    uint32_t cur, last;
    unsigned el = 0, still = 0, limit = steps * sp * 3 / 2 + 1500;
    int rc = 0;
    if (ptzs_read_axis(x, ax, &cur) || cur != steps)
        return HAL_PTZ_EIO;
    if (ptzs_send_move(x, dir, sp, sp)) {
        plog(p, "ptz: calibration: move failed: %s", strerror(errno));
        (void)ptzs_stop(x);
        return HAL_PTZ_EIO;
    }
    last = cur;
    while (!rc && cur) {
        ptzs_wait(x, PTZS_POLL_MS);
        el += PTZS_POLL_MS;
        if (ptzs_read_axis(x, ax, &cur)) {
            rc = HAL_PTZ_EIO;
        } else if (cur < last) {
            last = cur;
            still = 0;
        } else if (cur > last + 2 || (still += PTZS_POLL_MS) > PTZS_STALL_MS) {
            plog(p, "ptz: calibration: the %s counter %s", ax ? "tilt" : "pan", cur > last ? "runs the wrong way" : "does not move");
            rc = HAL_PTZ_ESTALL;
        } else if (el > limit) {
            plog(p, "ptz: calibration: the %s sweep timed out", ax ? "tilt" : "pan");
            rc = HAL_PTZ_ESTALL;
        }
    }
    if (ptzs_stop(x) && !rc)
        rc = HAL_PTZ_EIO;
    return rc;
}

static int calibrate_x(hal_ptz_t *p, int quick);
static int calibrate(hal_ptz_t *p) { return calibrate_x(p, 0); }

/* quick = 1: the SHORT calibration (see hal_ptz.h cold_check answer 3): the counters are declared at home + a margin instead of the stock maxima, one tilt sweep. */
static int calibrate_x(hal_ptz_t *p, int quick)
{
    struct ptzs_ctx *x = &p->x;
    struct ptzs_conf ac = p->cf;
    struct ptzs_frame fr;
    uint32_t lim[2] = { p->cf.pan_step_max, p->cf.tilt_step_max }, cnt[2] = { lim[0], lim[1] };
    int rc;
    if (quick) {
        cnt[0] = p->cf.pan_home + SHORT_PAN_MARGIN < lim[0] ? p->cf.pan_home + SHORT_PAN_MARGIN : lim[0];
        cnt[1] = p->cf.tilt_home + SHORT_TILT_MARGIN < lim[1] ? p->cf.tilt_home + SHORT_TILT_MARGIN : lim[1];
    }
    p->cal_active = 1;
    if (quick) {
        plog(p, "ptz: SHORT calibration (the head is very likely at home): tilt down from %u, pan left from %u, then home and the scene check again", (unsigned)cnt[1], (unsigned)cnt[0]);
    } else {
        plog(p, "ptz: calibrating: tilt down to the bottom stop, pan left to the left stop (speed %u, one sweep per axis, up to %u s)",
             (unsigned)p->cf.speed, (unsigned)((p->cf.pan_step_max + p->cf.tilt_step_max + TILT_EXTRA_STEPS) * p->cf.speed / 1000 + 3));
    }
    rc = drv_init(p, lim, cnt, NULL);
    if (!rc)
        rc = sweep(p, 1, cnt[1]);
    if (!rc && !quick) {
        /* The real tilt travel (about 2315 steps, measured) is longer than the declared maximum (2160): the first sweep can end 150 steps
         * above the bottom stop. Declare a short distance again (the pan counter keeps its maximum) and walk down once more: the motor
         * always ends ON the bottom stop. */
        uint32_t cnt2[2] = { p->cf.pan_step_max, TILT_EXTRA_STEPS };
        rc = drv_init(p, lim, cnt2, NULL);
        if (!rc)
            rc = sweep(p, 1, TILT_EXTRA_STEPS);
    }
    if (!rc)
        rc = sweep(p, 0, cnt[0]);
    if (!rc) {
        plog(p, "ptz: calibration: both stops reached, going home %u/%u", (unsigned)p->cf.pan_home, (unsigned)p->cf.tilt_home);
        ac.pan_min = ac.tilt_min = 1;
        ac.pan_max = p->cf.pan_home + APPROACH_MARGIN;
        ac.tilt_max = p->cf.tilt_home + APPROACH_MARGIN;
        ptzs_frame_from_conf(&fr, &ac);
        lim[0] = ac.pan_max;
        lim[1] = ac.tilt_max;
        cnt[0] = cnt[1] = 1; /* real 1/1: one step off the stops (a raw 0/0 would read as "not initialised") */
        rc = drv_init(p, lim, cnt, &fr);
    }
    if (!rc) {
        x->cf = &ac;
        rc = ptzs_goto(x, p->cf.pan_home, p->cf.tilt_home, p->cf.speed);
        x->cf = &p->cf;
        p->pos_valid = 0;
    }
    if (!rc)
        rc = declare_final(p, p->cf.pan_home, p->cf.tilt_home);
    p->cal_active = 0;
    if (!rc && !quick) { /* a SHORT calibration does not reset the travel (the wear budget) nor count as a full one: the periodic full calibration keeps counting */
        p->cal_runs++;
        p->travel = 0;
        p->reversals = 0;
    }
    if (rc)
        (void)frames_invalidate(p); /* a half-done calibration is never adopted by a restart */
    if (rc) {
        plog(p, "ptz: calibration FAILED (%d): the position stays unknown, motion refused until the next start", rc);
        if (!LD(p->canceled) && !LD(p->cancel)) /* not a shutdown: the head is somewhere unknown, the home reference is forgotten (one rare write) */
            scene_forget(p, "calibration failed");
    } else {
        p->scene_claim = 0;
        plog(p, "ptz: calibrated, at home %u/%u", (unsigned)p->cf.pan_home, (unsigned)p->cf.tilt_home);
    }
    return rc;
}

/* The driver already holds our frame and counters (this process restarted in the same boot): take them over. */
static int try_adopt(hal_ptz_t *p)
{
    struct ptzs_ctx *x = &p->x;
    struct ptzs_frame fr, want;
    uint32_t pan, tilt;
    char s[SAVED_MAX];
    x->have_frame = 0;
    if (ptzs_read_pos(x, &pan, &tilt) || (x->raw[0] == 0 && x->raw[1] == 0))
        return 0;
    ptzs_frame_from_conf(&want, &p->cf);
    if (ptzs_frame_load_from(&p->sops, PTZ_APP_FRAME_PATH, &fr) || !ptzs_frame_eq(&fr, &want))
        return 0;
    x->fr = fr;
    x->have_frame = 1;
    if (ptzs_read_pos(x, &pan, &tilt) || pan < fr.mn[0] - 1 || pan > fr.mx[0] || tilt < fr.mn[1] - 1 || tilt > fr.mx[1]) {
        x->have_frame = 0;
        return 0;
    }
    snprintf(s, sizeof s, "pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
    (void)store_pos(p, s);
    p->trusted = 1;
    p->scene_claim = 0;
    plog(p, "ptz: the driver already holds our frame: position %u/%u adopted, no calibration", (unsigned)pan, (unsigned)tilt);
    return 1;
}

#define PROBE_MAX_OFFSETS 8
#define PROBE_MARKER "/tmp/akam_ptz.probes" /* RAM: the multi-view probes started in this boot (a crash loop must not walk the head by an offset at every restart) */
#define PROBE_MAX_PER_BOOT 2
#define PROBE_MAX_STEP 200 /* per axis: an offset beyond this is refused whatever the hook says (the margin to the stops is 190 steps in tilt, 282 in pan) */

/* The in-RAM frame is written to the two frame files (the driver holds our frame: a restart of the app may adopt it). 0 ok. */
static int frames_save(hal_ptz_t *p)
{
    char s[128];
    ptzs_frame_format(&p->x.fr, s, sizeof s);
    return p->ops.save_file(PTZ_FRAME_PATH, s) || p->ops.save_file(PTZ_APP_FRAME_PATH, s) ? -1 : 0;
}

/* The claimed position (pan, tilt) is declared and the head moved by the offsets of the hook; 1 when a stored view proved the position (the
 * head is back at the claimed position, trusted), 0 when not (nothing trusted any more: the caller calibrates), -1 cancelled.
 * The frame files stay INVALID until the position is proven (a hard kill during the probe must not let a restart adopt an unproven claim:
 * ptzs_goto only needs the in-RAM frame). The head returns to the claimed position between two offsets, so the travel from the real
 * position never exceeds one offset even when the claim is wrong. cold_probe: 1 match, 0 no, -1 cancelled (shutdown). */
static int cold_probe_offsets(hal_ptz_t *p, uint32_t pan, uint32_t tilt, unsigned gen0)
{
    unsigned i, tries = 0;
    int proven = 0, rc, cancelled = 0;
    /* gen0: cancel_gen snapshot taken when the start began: a cancel after it (the shutdown) is never lost to begin() or a poll clearing the flag */
    char mk[16];
    int mn = p->ops.load_file(PROBE_MARKER, mk, sizeof mk);
    if (mn > 0 && mn < (int)sizeof mk)
        tries = (unsigned)strtoul(mk, NULL, 10);
    if (tries >= PROBE_MAX_PER_BOOT) { /* each attempt moves a head whose position is only claimed: bounded per boot, then the calibration decides */
        plog(p, "ptz: multi-view check: %u attempts already made in this boot: calibrating", tries);
        return 0;
    }
    snprintf(mk, sizeof mk, "%u\n", tries + 1);
    if (p->ops.save_file(PROBE_MARKER, mk)) {
        plog(p, "ptz: multi-view check: the attempt marker cannot be written (%s): calibrating", strerror(errno));
        return 0;
    }
    p->defer_frames = 1;
    rc = declare_final(p, pan, tilt);
    p->defer_frames = 0;
    if (rc != 0) {
        plog(p, "ptz: multi-view check: the driver init failed: calibrating");
        (void)frames_invalidate(p);
        return 0;
    }
    (void)frames_invalidate(p); /* belt and braces: the files are invalid until proven */
    plog(p, "ptz: multi-view check: the saved view is featureless or ambiguous: position %u/%u declared for the probe, moving by a small offset", (unsigned)pan, (unsigned)tilt);
    for (i = 0; i < PROBE_MAX_OFFSETS && !proven && !cancelled; i++) {
        int dp = 0, dt = 0, pr;
        int64_t tp, tt;
        if (LD(p->cancel) || __atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != gen0) {
            cancelled = 1;
            break;
        }
        if (p->ops.cold_offset(i, &dp, &dt) != 0)
            continue;
        tp = (int64_t)pan + dp;
        tt = (int64_t)tilt + dt;
        if ((!dp && !dt) || dp > PROBE_MAX_STEP || dp < -PROBE_MAX_STEP || dt > PROBE_MAX_STEP || dt < -PROBE_MAX_STEP || tp < (int64_t)p->cf.pan_min ||
            tp > (int64_t)p->cf.pan_max || tt < (int64_t)p->cf.tilt_min || tt > (int64_t)p->cf.tilt_max)
            continue; /* never leave the soft range, never move far */
        if (__atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != gen0) { /* tested right before each move (begin() would clear the flag) */
            cancelled = 1;
            break;
        }
        rc = hal_ptz_goto(p, (uint32_t)tp, (uint32_t)tt);
        if (rc != HAL_PTZ_OK) {
            plog(p, "ptz: multi-view check: the move to offset %u failed (%d): calibrating", i, rc);
            cancelled = rc == HAL_PTZ_ECANCEL;
            break;
        }
        pr = p->ops.cold_probe(i);
        if (pr < 0) {
            cancelled = 1;
            break;
        }
        /* match or not, the head goes back to the claimed position before anything else */
        if (__atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != gen0) {
            cancelled = 1;
            break;
        }
        rc = hal_ptz_goto(p, pan, tilt);
        if (rc != HAL_PTZ_OK) {
            plog(p, "ptz: multi-view check: the move back failed (%d): calibrating", rc);
            cancelled = rc == HAL_PTZ_ECANCEL;
            break;
        }
        if (pr == 1) {
            if (frames_save(p) == 0)
                proven = 1;
            else
                plog(p, "ptz: multi-view check: the frame files cannot be written: calibrating");
            break;
        }
    }
    if (proven)
        return 1;
    p->trusted = 0;
    p->pos_valid = 0;
    (void)frames_invalidate(p);
    return cancelled || LD(p->cancel) || __atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != gen0 ? -1 : 0;
}

int hal_ptz_start(hal_ptz_t *p, int cal)
{
    const unsigned gen0 = __atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST);
#define START_CANCELLED() (LD(p->cancel) || __atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != gen0)
    p->fd = p->ops.open_dev();
    if (p->fd < 0) {
        plog(p, "ptz: cannot open the motor: %s", strerror(errno));
        return HAL_PTZ_EIO;
    }
    p->x.fd = p->fd;
    (void)ptzs_stop(&p->x); /* a previous process may have left it walking */
    if (try_adopt(p))
        return 0;
    if (cal == HAL_PTZ_CAL_OFF) {
        plog(p, "ptz: ptz_cal=off: the motor is not initialised, motion is refused (akam_ptz init, then restart)");
        return 0;
    }
    /* A cold boot calibrates (like the stock) unless the scene check proves the head is at HOME: the head can move outside the app (by hand,
     * akam_ptz, the stock firmware, a power cut while it was away) and the open-loop counters cannot tell. No position is stored on the flash:
     * the only claim a cold boot can make is the configured home, proven by the home reference image. */
    if (p->ops.cold_check) {
        uint32_t pan = p->d.pan_home, tilt = p->d.tilt_home; /* in: the claim; the hook must leave it (the reference is keyed to home) */
        /* No motor command so far (only the stop above). */
        int cc = p->ops.cold_check(&pan, &tilt);
        int ok = pan == p->d.pan_home && tilt == p->d.tilt_home && pan >= p->cf.pan_min && pan <= p->cf.pan_max && tilt >= p->cf.tilt_min &&
                 tilt <= p->cf.tilt_max && !START_CANCELLED() && !(near(pan, p->d.pan_park) && near(tilt, p->d.tilt_park));
        if (cc && !ok && !START_CANCELLED())
            plog(p, "ptz: scene check: the claim is not the home position: calibrating");
        if (cc == 2 && ok && p->ops.cold_offset && p->ops.cold_probe) {
            int pr = cold_probe_offsets(p, pan, tilt, gen0);
            if (pr == 1) {
                p->scene_claim = 1;
                plog(p, "ptz: scene check passed (multi-view): the head is at home, position %u/%u declared, no calibration", (unsigned)pan, (unsigned)tilt);
                return 0;
            }
            if (pr < 0)
                return HAL_PTZ_ECANCEL;
        }
        if (cc == 3 && ok && !START_CANCELLED()) { /* very likely home, not proven: the short calibration, then the proof; anything else is the full calibration below */
            int qrc;
            p->defer_frames = 1; /* the frame files stay INVALID until the proof: a restart (kill, cancel) during it must never adopt an unproven home */
            qrc = calibrate_x(p, 1);
            p->defer_frames = 0;
            if (qrc == 0) {
                uint32_t vp = p->d.pan_home, vt = p->d.tilt_home;
                int v;
                p->trusted = 0; /* declared by calibrate_x, but not proven */
                (void)frames_invalidate(p);
                v = p->ops.cold_check(&vp, &vt); /* the live view at home against the reference, again: the strict check */
                if (v == 1 && vp == p->d.pan_home && vt == p->d.tilt_home && !START_CANCELLED() && frames_save(p) == 0) {
                    p->trusted = 1;
                    p->scene_claim = 1;
                    plog(p, "ptz: short calibration done and verified by the scene check: the head is at home %u/%u", (unsigned)vp, (unsigned)vt);
                    return 0;
                }
                p->trusted = 0;
                p->pos_valid = 0;
                (void)frames_invalidate(p);
                if (START_CANCELLED())
                    return HAL_PTZ_ECANCEL;
                plog(p, "ptz: short calibration: the home view is not proven afterwards: full calibration");
            } else {
                (void)frames_invalidate(p);
                if (START_CANCELLED())
                    return HAL_PTZ_ECANCEL;
            }
        }
        if (cc == 1 && ok) {
            if (declare_final(p, pan, tilt) == 0) {
                p->scene_claim = 1;
                plog(p, "ptz: scene check passed: the head is at home, position %u/%u declared, no calibration", (unsigned)pan, (unsigned)tilt);
                return 0;
            }
            (void)frames_invalidate(p);
            plog(p, "ptz: scene check passed but the driver init failed: calibrating");
        }
    }
    if (START_CANCELLED())
        return HAL_PTZ_ECANCEL;
    plog(p, "ptz: cold boot (the driver holds no frame of ours, the home view is not proven): calibrating");
    return calibrate(p);
#undef START_CANCELLED
}

int hal_ptz_recalibrate(hal_ptz_t *p)
{
    if (p->fd < 0)
        return HAL_PTZ_EREFUSED;
    ST(p->cancel, 0); /* a stale flag goes first: a shutdown cancel that arrives later must survive */
    ST(p->canceled, 0);
    if (p->marked) { /* like begin(): a cancel between the caller's dequeue (hal_ptz_mark) and now is not lost */
        p->marked = 0;
        if (__atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != p->mark_gen)
            return HAL_PTZ_ECANCEL;
    }
    plog(p, "ptz: recalibration requested");
    return calibrate(p);
}

/* ---- the tilt-only re-home: tilt slip made non-cumulative. The head is believed at (pan, tilt) with the counters possibly wrong on TILT by lost steps (open loop). The FULL STROKE is walked DOWN at
 * the configured (calibration) speed (counter declared at the stock maximum and walked to 0, then declared 300 and walked again, exactly like the boot calibration), which ends ON the bottom stop
 * whatever the slip; the counter is then re-declared as real 1 (the stop, like a calibration) and the head goes UP to the configured tilt home (last leg from below, like
 * hal_ptz_home). The PAN is never commanded and its counter is re-declared with the value it had: untouched. RAM only (the frame files in /tmp, no flash). A cancel (hal_ptz_cancel: a person,
 * a user command) stops the motor and keeps the BELIEVED position of before (the counter minus what was walked: the slip stays what it was, nothing worse). ---- */

/* Reads an axis counter after a stop request: steps still in flight in the driver would otherwise be lost from the belief. Re-read after a short wait until two reads agree (at most 6 waits of 50 ms). */
static int read_settled(hal_ptz_t *p, unsigned ax, uint32_t *cur)
{
    uint32_t prev;
    int i, got = ptzs_read_axis(&p->x, ax, cur);
    for (i = 0; !got && i < 6; i++) {
        prev = *cur;
        ptzs_wait(&p->x, 50);
        got = ptzs_read_axis(&p->x, ax, cur);
        if (!got && *cur == prev)
            break;
    }
    return got;
}

/* settle at the believed real tilt `belief` (pan cp unchanged) after an interrupted re-home: inside the soft range the counters are simply re-declared; below it (the head went under the
 * soft range) the head climbs to the soft minimum in the approach frame first. */
static int rehome_settle(hal_ptz_t *p, uint32_t cp, uint32_t belief)
{
    struct ptzs_ctx *x = &p->x;
    struct ptzs_conf ac = p->cf;
    struct ptzs_frame fr;
    uint32_t lim[2], cnt[2];
    int rc;
    p->explicit_pose = 0; /* wherever this ends (the park pose included), it is a settle, not a park command: the state is never "parked" because of it */
    if (belief >= p->cf.tilt_min)
        return declare_final(p, cp, belief > p->cf.tilt_max ? p->cf.tilt_max : belief);
    ac.pan_min = ac.tilt_min = 1;
    ac.pan_max = p->cf.pan_max;
    ac.tilt_max = p->cf.tilt_min + APPROACH_MARGIN;
    ptzs_frame_from_conf(&fr, &ac);
    lim[0] = ac.pan_max;
    lim[1] = ac.tilt_max;
    cnt[0] = cp;
    cnt[1] = belief < 1 ? 1 : belief;
    rc = drv_init(p, lim, cnt, &fr);
    if (!rc) {
        x->cf = &ac;
        rc = ptzs_goto(x, cp, p->cf.tilt_min, p->cf.speed);
        x->cf = &p->cf;
        p->pos_valid = 0;
    }
    return rc ? rc : declare_final(p, cp, p->cf.tilt_min);
}

int hal_ptz_tilt_rehome(hal_ptz_t *p)
{
    struct ptzs_ctx *x = &p->x;
    struct ptzs_conf ac = p->cf;
    struct ptzs_frame fr;
    uint32_t lim[2] = { p->cf.pan_step_max, p->cf.tilt_step_max }, cnt[2], cp, ct, start, cur = 0, belief, walked;
    int rc, rc2, canceled, got, leg;
    if (p->fd < 0 || !p->trusted || !p->pos_valid || p->moving || p->cal_active)
        return HAL_PTZ_EREFUSED;
    ST(p->cancel, 0);
    ST(p->canceled, 0);
    if (p->marked) { /* like begin(): a cancel between the caller's dequeue (hal_ptz_mark) and now is not lost */
        p->marked = 0;
        if (__atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != p->mark_gen)
            return HAL_PTZ_ECANCEL;
    }
    cp = p->pan;
    ct = p->tilt;
    plog(p, "ptz: tilt re-home: down to the bottom stop whatever the slip (the full stroke; believed %u, pan %u untouched), then up to home %u", (unsigned)ct, (unsigned)cp, (unsigned)p->cf.tilt_home);
    p->cal_active = 1;
    /* The walk must reach the bottom stop from ANY real tilt: like the boot calibration, the counter is declared at the stock maximum (2160) and walked to 0, then declared 300 and walked again
     * (the real travel, about 2315, is longer than the declared maximum): the head always ends ON the stop, however large the slip (up to the whole stroke). */
    walked = 0;
    for (leg = 0; leg < 2; leg++) {
        start = leg ? TILT_EXTRA_STEPS : lim[1];
        cnt[0] = cp;
        cnt[1] = start;
        p->nocancel = 1; /* the driver inits (1 s each) are not interrupted: a cancel is taken by the first poll of the walk */
        rc = drv_init(p, lim, cnt, NULL);
        p->nocancel = 0;
        if (rc) { /* nothing moved, the frame is invalid: the position is unknown */
            p->cal_active = 0;
            p->pos_valid = 0;
            plog(p, "ptz: tilt re-home: the driver init failed (%d): the position is no longer trusted", rc);
            return rc;
        }
        rc = sweep(p, 1, start);
        if (rc)
            break;
        walked += start;
    }
    canceled = LD(p->canceled);
    if (rc) { /* interrupted (or failed) on the way down: the believed position of before, minus what was walked */
        p->nocancel = 1; /* the re-declare must finish whatever cancels arrive meanwhile (a person who stays in view cancels at every frame) */
        ST(p->cancel, 0);
        ST(p->canceled, 0);
        belief = 1;
        got = read_settled(p, 1, &cur);
        if (got == 0 && cur <= start) {
            walked += start - cur;
            belief = ct > walked ? ct - walked : 1;
        }
        rc2 = got ? HAL_PTZ_EIO : rehome_settle(p, cp, belief);
        p->cal_active = 0;
        p->nocancel = 0;
        if (rc2)
            p->pos_valid = 0;
        plog(p, "ptz: tilt re-home %s on the way down: the position stays the believed %u/%u (counter %u)", canceled ? "interrupted" : "FAILED", (unsigned)cp, (unsigned)belief, (unsigned)cur);
        if (rc2)
            plog(p, "ptz: tilt re-home: re-declaring failed (%d): the position is no longer trusted", rc2);
        return rc2 ? rc2 : canceled ? HAL_PTZ_ECANCEL : rc;
    }
    ac.pan_min = ac.tilt_min = 1;
    ac.pan_max = p->cf.pan_max;
    ac.tilt_max = p->cf.tilt_home + APPROACH_MARGIN;
    ptzs_frame_from_conf(&fr, &ac);
    lim[0] = ac.pan_max;
    lim[1] = ac.tilt_max;
    cnt[0] = cp;
    cnt[1] = 1; /* real 1: one step off the bottom stop (a raw 0 would read as "not initialised"), like a calibration */
    p->nocancel = 1;
    rc = drv_init(p, lim, cnt, &fr);
    p->nocancel = 0;
    if (!rc) {
        x->cf = &ac;
        rc = ptzs_goto(x, cp, p->cf.tilt_home, p->cf.speed);
        x->cf = &p->cf;
        p->pos_valid = 0;
    }
    canceled = LD(p->canceled);
    if (rc) { /* interrupted on the way up: settle where the head is (the counter is exact: it was declared at the stop) */
        p->nocancel = 1;
        ST(p->cancel, 0);
        ST(p->canceled, 0);
        belief = 1;
        got = read_settled(p, 1, &cur);
        if (got == 0)
            belief = cur;
        rc2 = got ? HAL_PTZ_EIO : rehome_settle(p, cp, belief);
        p->cal_active = 0;
        p->nocancel = 0;
        if (rc2)
            p->pos_valid = 0;
        plog(p, "ptz: tilt re-home %s on the way up (counter %u from the stop)", canceled ? "interrupted" : "FAILED", (unsigned)belief);
        if (rc2)
            plog(p, "ptz: tilt re-home: re-declaring failed (%d): the position is no longer trusted", rc2);
        return rc2 ? rc2 : canceled ? HAL_PTZ_ECANCEL : rc;
    }
    p->nocancel = 1;
    rc = declare_final(p, cp, p->cf.tilt_home);
    p->nocancel = 0;
    p->cal_active = 0;
    if (rc) {
        p->pos_valid = 0;
        plog(p, "ptz: tilt re-home FAILED (%d): the position stays unknown", rc);
        (void)frames_invalidate(p);
        return rc;
    }
    plog(p, "ptz: tilt re-homed: the tilt counter is the stop-derived one again, at home %u/%u", (unsigned)cp, (unsigned)p->cf.tilt_home);
    return 0;
}

unsigned hal_ptz_cal_count(const hal_ptz_t *p) { return p->cal_runs; }
unsigned long hal_ptz_travel(const hal_ptz_t *p) { return p->travel; }
unsigned long hal_ptz_reversals(const hal_ptz_t *p) { return p->reversals; }

unsigned hal_ptz_drift_cum_max(const hal_ptz_t *p) { return p->cf.drift_cum_max; }

void hal_ptz_px_steps(const hal_ptz_t *p, unsigned *pan_x10, unsigned *tilt_x10)
{
    *pan_x10 = p->cf.px_pan_x10;
    *tilt_x10 = p->cf.px_tilt_x10;
}

/* The scene check measured that the head is NOT where the counters say: the counters are re-declared to the measured position, no motion (the stock declare path:
 * init limits + init position, then the counters are read back; the frame files in /tmp are rewritten). A failure leaves the position unknown (untrusted). */
int hal_ptz_declare(hal_ptz_t *p, uint32_t pan, uint32_t tilt)
{
    int rc;
    if (p->fd < 0 || !p->trusted || !p->pos_valid || p->moving || p->cal_active)
        return HAL_PTZ_EREFUSED;
    if (pan < p->cf.pan_min || pan > p->cf.pan_max || tilt < p->cf.tilt_min || tilt > p->cf.tilt_max)
        return HAL_PTZ_EREFUSED;
    ST(p->cancel, 0);
    ST(p->canceled, 0);
    rc = declare_final(p, pan, tilt);
    if (rc) {
        plog(p, "ptz: re-declaring the position %u/%u failed (%d): the position is no longer trusted", (unsigned)pan, (unsigned)tilt, rc);
        p->trusted = 0;
        p->pos_valid = 0;
        (void)frames_invalidate(p);
    }
    return rc;
}

int hal_ptz_verify_pos(hal_ptz_t *p)
{
    uint32_t pan, tilt;
    if (p->fd < 0 || !p->trusted || !p->pos_valid || p->moving || p->cal_active)
        return -1;
    ST(p->cancel, 0); /* a stale cancel must not eat the read */
    if (ptzs_read_pos(&p->x, &pan, &tilt) || (p->x.raw[0] == 0 && p->x.raw[1] == 0))
        return -1;
    return (pan > p->pan ? pan - p->pan : p->pan - pan) <= VERIFY_TOL && (tilt > p->tilt ? tilt - p->tilt : p->tilt - tilt) <= VERIFY_TOL ? 0 : -1;
}

void hal_ptz_close(hal_ptz_t *p)
{
    if (p->fd >= 0)
        (void)ptzs_stop(&p->x);
}

/* ---- motions ---- */
static int begin(hal_ptz_t *p)
{
    if (p->fd < 0 || !p->trusted || p->cal_active) {
        plog(p, "ptz: motion refused: the position is not known (%s)", p->cal_active ? "calibrating" : "not initialised");
        return HAL_PTZ_EREFUSED;
    }
    if (p->marked) { /* a cancel that arrived between the caller's dequeue (hal_ptz_mark) and now is not lost */
        p->marked = 0;
        if (__atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST) != p->mark_gen) {
            ST(p->cancel, 0);
            return HAL_PTZ_ECANCEL;
        }
    }
    ST(p->cancel, 0);
    ST(p->canceled, 0);
    p->moving = 1;
    p->explicit_pose = 1;
    p->wpan = p->wtilt = 1;
    p->bpan = p->pan;
    p->btilt = p->tilt;
    return 0;
}

static int end_t(hal_ptz_t *p, int rc, long travel_d);
static int end(hal_ptz_t *p, int rc) { return end_t(p, rc, -1); }

/* travel_d >= 0: the steps travelled (a continuous session: reversals count), else the net displacement */
static int end_t(hal_ptz_t *p, int rc, long travel_d)
{
    uint32_t pan, tilt;
    char s[SAVED_MAX];
    int lost = 0;
    p->moving = 0;
    ST(p->cancel, 0); /* a cancel that came too late must not eat the final position read */
    if (!p->pos_valid && p->fd >= 0 && !ptzs_read_pos(&p->x, &pan, &tilt) && !(p->x.raw[0] == 0 && p->x.raw[1] == 0)) {
        snprintf(s, sizeof s, "pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt); /* a cancel can eat the final read */
        (void)store_pos(p, s);
    }
    if (p->pos_valid)
        {
            unsigned long d = travel_d >= 0 ? (unsigned long)travel_d : (unsigned long)(p->pan > p->bpan ? p->pan - p->bpan : p->bpan - p->pan) * p->wpan + (unsigned long)(p->tilt > p->btilt ? p->tilt - p->btilt : p->btilt - p->tilt) * p->wtilt;
            p->travel = p->travel > ~0ul - d ? ~0ul : p->travel + d; /* saturates: a counter that wrapped would read as a fresh calibration */
        }
    if (LD(p->canceled)) {
        ST(p->canceled, 0);
        return HAL_PTZ_ECANCEL;
    }
    if (rc == HAL_PTZ_ESTALL || (rc == HAL_PTZ_EIO && !p->pos_valid)) { /* an I/O error with a readable position changed nothing */
        plog(p, "ptz: motion failed (%d): the position is no longer trusted until the next start", rc);
        p->trusted = 0;
        p->pos_valid = 0;
        lost = 1;
        (void)frames_invalidate(p); /* a restart must not re-adopt a frame whose counters lost the head */
    }
    if (lost && p->scene_claim) /* the claim made by the scene check proved wrong (or the motor is stuck): the next cold boot must not repeat it */
        scene_forget(p, "the claimed position failed");
    return rc;
}

_Static_assert(HAL_PTZ_SPEED_MIN == PTZS_MIN_SPEED && HAL_PTZ_SPEED_MAX == PTZS_MAX_SPEED, "hal_ptz.h speed limits follow the safe layer");

/* the weighted travel of a goto / nudge: a move REQUESTED faster than divisor 3 (the fast intent that loses steps) counts like a continuous one at the divisor really used (the pan at sp, the tilt at
 * max(sp, tilt_min_div), the safe layer's floor); the ordinary moves (divisor >= 3: home / park returns, the default speed) keep counting 1 per step, as before */
static void set_weights(hal_ptz_t *p, unsigned sp)
{
    p->wpan = sp < 3 ? ptzs_cont_weight(sp) : 1;
    p->wtilt = sp < 3 ? ptzs_cont_weight(sp < p->cf.tilt_min_div ? p->cf.tilt_min_div : sp) : 1;
}

int hal_ptz_goto(hal_ptz_t *p, uint32_t pan, uint32_t tilt)
{
    int rc = begin(p);
    if (rc)
        return rc;
    set_weights(p, p->cf.speed);
    return end(p, ptzs_goto(&p->x, pan, tilt, p->cf.speed));
}

int hal_ptz_goto_sp(hal_ptz_t *p, uint32_t pan, uint32_t tilt, int speed)
{
    int rc;
    if (speed != 0 && (speed < HAL_PTZ_SPEED_MIN || speed > HAL_PTZ_SPEED_MAX))
        return HAL_PTZ_EREFUSED;
    rc = begin(p);
    if (rc)
        return rc;
    set_weights(p, speed > 0 ? (unsigned)speed : p->cf.speed);
    return end(p, ptzs_goto(&p->x, pan, tilt, speed > 0 ? (uint32_t)speed : p->cf.speed));
}

/* Home is always reached from BELOW (increasing counters), the direction of the last leg of a calibration: the final approach never carries the backlash of a reversal. A head above home on an axis first goes
 * HOME_APPROACH_STEPS under it (when the soft range allows), then up to home. An axis already within the goto tolerance of home is left alone. */
#define HOME_APPROACH_STEPS 10u
int hal_ptz_home(hal_ptz_t *p)
{
    uint32_t cp = 0, ct = 0, pp = p->d.pan_home, pt = p->d.tilt_home;
    int rc = begin(p), pre = 0;
    if (rc)
        return rc;
    set_weights(p, p->cf.speed);
    if (ptzs_read_pos(&p->x, &cp, &ct) == 0) {
        if (cp > pp + PTZS_TOL && pp >= p->d.pan_min + HOME_APPROACH_STEPS) {
            pp -= HOME_APPROACH_STEPS;
            pre = 1;
        }
        if (ct > pt + PTZS_TOL && pt >= p->d.tilt_min + HOME_APPROACH_STEPS) {
            pt -= HOME_APPROACH_STEPS;
            pre = 1;
        }
    }
    rc = pre ? ptzs_goto(&p->x, pp, pt, p->cf.speed) : 0;
    if (!rc)
        rc = ptzs_goto(&p->x, p->d.pan_home, p->d.tilt_home, p->cf.speed);
    return end(p, rc);
}
int hal_ptz_park(hal_ptz_t *p) { return hal_ptz_goto(p, p->d.pan_park, p->d.tilt_park); }

int hal_ptz_step(hal_ptz_t *p, int dir)
{
    unsigned ms = p->d.step * p->cf.speed;
    int rc;
    if (dir < 0 || dir >= HAL_PTZ_NDIR)
        return HAL_PTZ_EREFUSED;
    rc = begin(p);
    if (rc)
        return rc;
    {
        unsigned cmd = p->d.dir[dir], sp = p->cf.speed, spp = sp, spt = sp;
        if (cmd <= 7 && PTZS_TILT_SIGN[cmd] && spt < p->cf.tilt_min_div)
            spt = p->cf.tilt_min_div; /* a tilt step (and a diagonal: both axes) is never faster than tilt_min_div */
        if (cmd <= 7 && PTZS_TILT_SIGN[cmd] && PTZS_PAN_SIGN[cmd] && spp < p->cf.tilt_min_div)
            spp = p->cf.tilt_min_div;
        p->wpan = p->cf.speed < 3 ? ptzs_cont_weight(spp) : 1;
        p->wtilt = p->cf.speed < 3 ? ptzs_cont_weight(spt) : 1;
        ms = p->d.step * (spp > spt ? spp : spt); /* the same distance at the slower divisor */
        return end(p, ptzs_move(&p->x, cmd, ms > 3000 ? 3000 : ms, spp, spt));
    }
}

int hal_ptz_nudge(hal_ptz_t *p, int dx, int dy) { return hal_ptz_nudge_sp(p, dx, dy, 0); }

/* speed: a divisor 2..20 (the safe layer refuses anything else), 0 = the configured one */
int hal_ptz_nudge_sp(hal_ptz_t *p, int dx, int dy, int speed)
{
    int64_t pan, tilt;
    int rc = begin(p);
    if (rc)
        return rc;
    pan = (int64_t)p->pan + dx;
    tilt = (int64_t)p->tilt + dy;
    pan = pan < (int64_t)p->cf.pan_min ? p->cf.pan_min : pan > (int64_t)p->cf.pan_max ? p->cf.pan_max : pan;
    tilt = tilt < (int64_t)p->cf.tilt_min ? p->cf.tilt_min : tilt > (int64_t)p->cf.tilt_max ? p->cf.tilt_max : tilt;
    set_weights(p, speed > 0 ? (unsigned)speed : p->cf.speed);
    return end(p, ptzs_goto(&p->x, (uint32_t)pan, (uint32_t)tilt, speed > 0 ? (uint32_t)speed : p->cf.speed));
}

/* The command word of a logical command: 0 ok (*w, *a, *b filled), -1 stop / bad. Diagonals use the descriptor's words, or the faster axis alone when there are none. */
static int cont_word(const hal_ptz_t *p, const hal_ptz_cmd_t *m, unsigned *w, uint32_t *a, uint32_t *b)
{
    int sx = m->sx, sy = m->sy;
    if (sx < -1 || sx > 1 || sy < -1 || sy > 1 || m->dx < 0 || m->dy < 0 || (!sx && !sy))
        return -1;
    if (sx && sy && !p->d.diag_ok) { /* no diagonal command: the axis with the faster speed (smaller divisor) alone */
        if ((m->dx ? m->dx : (int)p->cf.speed) <= (m->dy ? m->dy : (int)p->cf.speed))
            sy = 0;
        else
            sx = 0;
    }
    if (sx && sy)
        *w = p->d.diag[sy > 0 ? (sx > 0 ? 0 : 3) : (sx > 0 ? 1 : 2)];
    else if (sy)
        *w = p->d.dir[sy > 0 ? HAL_PTZ_UP : HAL_PTZ_DOWN];
    else
        *w = p->d.dir[sx > 0 ? HAL_PTZ_RIGHT : HAL_PTZ_LEFT];
    *a = sx && m->dx ? (uint32_t)m->dx : 0;
    *b = sy && m->dy ? (uint32_t)m->dy : 0;
    return 0;
}

int hal_ptz_cont_run(hal_ptz_t *p, const hal_ptz_cmd_t *first, unsigned lease_ms, hal_ptz_cont_cb cb, void *ctx)
{
    struct ptzs_cont c;
    unsigned w;
    uint32_t a, b;
    int rc = begin(p);
    if (rc)
        return rc;
    memset(&c, 0, sizeof c);
    if (!first || !cb || cont_word(p, first, &w, &a, &b))
        return end(p, HAL_PTZ_EREFUSED);
    rc = ptzs_cont_begin(&p->x, &c, w, a, b, lease_ms);
    if (rc)
        return end(p, rc);
    while (!rc) {
        hal_ptz_cstate_t st;
        hal_ptz_cmd_t cmd;
        int k;
        rc = ptzs_cont_poll(&p->x, &c);
        if (rc)
            break;
        st.pan = c.pos[0];
        st.tilt = c.pos[1];
        st.t_ms = p->x.t_ms;
        memset(&cmd, 0, sizeof cmd);
        k = cb(ctx, &st, &cmd);
        if (k == HAL_PTZ_CONT_STOP) {
            rc = PTZS_CONT_DONE;
        } else if (k == HAL_PTZ_CONT_CMD) {
            if (cont_word(p, &cmd, &w, &a, &b)) /* a zero command = stop */
                rc = PTZS_CONT_DONE;
            else
                rc = ptzs_cont_set(&p->x, &c, w, a, b);
        }
    }
    rc = ptzs_cont_end(&p->x, &c, rc);
    p->reversals += c.nrev;
    return end_t(p, rc, (long)c.travel);
}

/* Async-signal-safe: one stop request on the already open node (the fatal-signal handler of the app). No allocation, no lock, no log. */
void hal_ptz_emergency_stop(void)
{
    hal_ptz_t *p = g_hp;
    if (p && p->fd >= 0 && p->ops.ioctl_fn) {
        (void)p->ops.ioctl_fn(p->fd, PTZ_REQ_STOP, NULL);
        (void)p->ops.ioctl_fn(p->fd, PTZ_REQ_STOP, NULL); /* a lost stop leaves the motor running: sent twice */
    }
}

int hal_ptz_oneshot_stop(const hal_ptz_ops_t *ops)
{
    int fd, i, r = -1;
    if (!ops || !ops->open_dev || !ops->ioctl_fn)
        return -1;
    fd = ops->open_dev();
    if (fd < 0)
        return -1;
    for (i = 0; i < 3 && r; i++)
        r = ops->ioctl_fn(fd, PTZ_REQ_STOP, NULL);
    return r;
}

void hal_ptz_cancel(hal_ptz_t *p)
{
    __atomic_add_fetch(&p->cancel_gen, 1u, __ATOMIC_SEQ_CST);
    ST(p->cancel, 1); /* taken at the next position poll; begin() clears a stale one */
}

void hal_ptz_mark(hal_ptz_t *p)
{
    p->mark_gen = __atomic_load_n(&p->cancel_gen, __ATOMIC_SEQ_CST);
    p->marked = 1;
}

static int near(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= STATE_TOL; }

int hal_ptz_state(const hal_ptz_t *p)
{
    if (p->cal_active)
        return HAL_PTZ_CALIBRATING;
    if (!p->trusted)
        return HAL_PTZ_UNKNOWN;
    if (p->moving || !p->pos_valid)
        return HAL_PTZ_MOVED;
    if (p->explicit_pose && near(p->pan, p->d.pan_park) && near(p->tilt, p->d.tilt_park))
        return HAL_PTZ_PARKED;
    if (near(p->pan, p->d.pan_home) && near(p->tilt, p->d.tilt_home))
        return HAL_PTZ_HOME;
    return HAL_PTZ_MOVED;
}

int hal_ptz_pos(const hal_ptz_t *p, uint32_t *pan, uint32_t *tilt)
{
    if (!p->trusted || !p->pos_valid)
        return 0;
    *pan = p->pan;
    *tilt = p->tilt;
    return 1;
}
