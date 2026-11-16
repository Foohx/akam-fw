/* akam_ptz_safe.h -- software-limited motion layer for the LSC Rotatable 2K motor (lsc-rotatable-2k-ak3918av130).
 *
 * Header-only on purpose (one translation unit, one build line, reusable by the app later: include it, fill a
 * struct ptz_ops and a struct ptzs_conf, call ptzs_move / ptzs_goto / ptzs_park). It talks to the motor only
 * through ops->ioctl_fn with the three request numbers below; the init requests live in akam_ptz.c alone.
 *
 * Why it exists: the motor is an open-loop stepper with NO limit switch. The driver counts steps and cannot tell
 * where the mechanical stops are; at a stop the motor silently skips steps. Software limits are the only guard, so:
 *   - every motion is bounded by a configurable soft range (defaults 300..3650 pan, 300..2100 tilt, measured on the
 *     unit: mechanical travel pan 0..~4000, tilt 0..~2315 (top stop between 2290 and 2340); the soft range keeps at least 300 steps from the
 *     pan stops and about 190 from the worst-case tilt top stop (the level-up view is at about 2104); see
 *     docs/porting/lsc-rotatable-2k/ptz-findings.md "Safe motion layer");
 *   - the position is polled every 50 ms during a motion; the motion stops at the target or when the next poll
 *     could cross a soft limit; a counter that does not change for more than 300 ms stops everything with an error;
 *   - the speed divisor is always explicit (2..20): "0 = keep the driver's previous speed" is never sent, because
 *     the previous speed is unknown (the stock app leaves speed 1, ~1000 steps/s);
 *   - a driver whose counters read 0/0 is considered uninitialised and refuses every motion;
 *   - the stop request is always sent last, and retried;
 *   - the DRIVER's own limits are the soft range (shifted frame, see struct ptzs_frame): even a tool killed by
 *     SIGKILL in the middle of a move cannot walk the motor past a soft limit, the driver stops it there itself.
 * Driver command 10 (absolute move) and the check sweeps (9, 12) are NOT used: timed directional moves (commands
 * 0..7, verified on the hardware) plus polling are.
 * The clock is virtual: the sum of the sleeps requested (ioctl latency is ignored, it only makes real time
 * slightly longer than counted, never shorter).
 */
#ifndef AKAM_PTZ_SAFE_H
#define AKAM_PTZ_SAFE_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PTZ_REQ_STOP 0x00000c00u
#define PTZ_REQ_POS 0x801c0c01u
#define PTZ_REQ_MOVE 0x401c0c02u
#define PTZ_ARG_BYTES 28
#define PTZ_CONF_PATH "/mnt/sdcard/akam/ptz.conf"
#define PTZ_POS_PATH "/tmp/akam_ptz.pos"
#define PTZ_FRAME_PATH "/tmp/akam_ptz.frame"
/* The frame the own APP may adopt after a process restart (same boot). The app writes it next to PTZ_FRAME_PATH; the tool
 * (akam_ptz) invalidates it on any motion or init, so a head moved by the tool is never re-adopted by the app. */
#define PTZ_APP_FRAME_PATH "/tmp/akam_ptz_app.frame"
/* The app's saved position (diagnostic only: a cold boot always calibrates); the tool writes "unknown" to it on any motion. */
#define PTZ_SAVED_POS_PATH "/config/akam/ptz.pos"
/* The app's scene-check reference (thumbnail + position, app/scene_check.h): the tool writes the invalid marker on any motion, so a head moved
 * by the tool can never be declared by a later cold boot (the image comparison would catch it too). */
#define PTZ_SCENE_PATH "/config/akam/ptz.scene"

/* the driver's own built-in limits (ty_driver g_motor_init): nothing above is accepted by init */
#define PTZS_DRV_PAN_MAX 6000u
#define PTZS_DRV_TILT_MAX 2200u
/* hard caps of the soft range (measured stops: tilt top 2290..2340, pan right >= 3932; margin for a wrong ptz.conf) */
#define PTZS_CAP_PAN_MAX 3700u
#define PTZS_CAP_TILT_MAX 2100u
/* stock values of this unit: ptz_pan_step_max / ptz_tilt_step_max code defaults, ptz_*_step_home of param.conf */
#define PTZS_DEF_PAN_MAX 4000u
#define PTZS_DEF_TILT_MAX 2160u
#define PTZS_DEF_PAN_HOME 1275u  /* level front view, measured 2026-11-15 (the stock home 816 tilt looks lower) */
#define PTZS_DEF_TILT_HOME 1300u
#define PTZS_DEF_PAN_MIN 300u    /* measured: left stop 0, right stop 3932..4151; tilt bottom stop 0, top 2290..2340 */
#define PTZS_DEF_PAN_SOFT_MAX 3650u
#define PTZS_DEF_TILT_MIN 300u
#define PTZS_DEF_TILT_SOFT_MAX 2100u
#define PTZS_DEF_SPEED 3u  /* ~333 steps/s, hardware verified 2026-11-15 */
#define PTZS_MIN_SPEED 2u  /* fastest allowed: ~500 steps/s, hardware verified */
#define PTZS_MAX_SPEED 20u
#define PTZS_MARGIN 50u    /* a soft limit keeps at least this many steps from the driver's 0 and max */
#define PTZS_MIN_SPAN 100u /* soft range width */
#define PTZS_POLL_MS 50u
#define PTZS_STALL_MS 300u
#define PTZS_TOL 3u        /* goto target tolerance, steps */
#define PTZS_ACCEPT 10u    /* goto accepted after the correction passes when this close (sleep granularity) */
#define PTZS_PASSES 5

enum { PTZS_OK = 0, PTZS_EIO = 1, PTZS_EREFUSED = 4, PTZS_ESTALL = 5, PTZS_ECONF = 6 };

struct ptz_ops {
    int (*open_dev)(void);
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);
    void (*sleep_ms)(unsigned ms);
    /* load_file: whole file into buf (NUL-terminated), returns its length, -1 with errno (ENOENT = absent) */
    int (*load_file)(const char *path, char *buf, unsigned cap);
    /* save_file: replace the file atomically (temp + rename); 0 ok */
    int (*save_file)(const char *path, const char *data);
};

struct ptzs_conf {
    uint32_t pan_min, pan_max, tilt_min, tilt_max; /* soft range */
    uint32_t pan_home, tilt_home;                  /* park target, default init position */
    uint32_t pan_step_max, tilt_step_max;          /* driver limits given to init */
    uint32_t speed;                                /* default speed divisor */
};

/* Coordinate frame. The tool and its users speak "real" positions (the stock numbering: home 1275/816). The driver
 * is told a SHIFTED frame at init so that its own limits coincide with the soft range:
 *     driver counter = real - off,  off = soft_min - 1,  driver step_max = soft_max - off = soft_max - soft_min + 1
 * Driver counter 0 is therefore real soft_min - 1 and the driver max is real soft_max: every directional move
 * (commands 0..7 walk the counter to 0 or step_max and stop there) ends at a soft limit by itself, whatever
 * happens to this process. The "+ 1" keeps every legitimate counter >= 1: the driver reads a 0 word of init as
 * "keep the current value", and a raw 0/0 pair stays recognisable as an uninitialised driver. The frame lives in
 * PTZ_FRAME_PATH (/tmp, same boot as the driver counters) and is only valid while the driver was initialised
 * with it: it is invalidated before init talks to the driver and written after init verified the counters. */
struct ptzs_frame {
    uint32_t mn[2], mx[2]; /* soft range of the frame: pan, tilt */
};

struct ptzs_ctx {
    const struct ptz_ops *ops;
    int fd;
    const struct ptzs_conf *cf;
    unsigned t_ms;         /* virtual clock */
    int have_frame;        /* 0: ptzs_read_axis returns raw driver counters (init, pos before init) */
    struct ptzs_frame fr;
    uint32_t raw[2];       /* last raw driver counters read by ptzs_read_axis */
};

static uint32_t ptzs_off(const struct ptzs_ctx *x, unsigned ax)
{
    return x->fr.mn[ax] - 1;
}

/* driver step_max of the frame (also the largest legal raw counter) */
static uint32_t ptzs_dmax(const struct ptzs_ctx *x, unsigned ax)
{
    return x->fr.mx[ax] - ptzs_off(x, ax);
}

static void ptzs_frame_from_conf(struct ptzs_frame *f, const struct ptzs_conf *c)
{
    f->mn[0] = c->pan_min;
    f->mx[0] = c->pan_max;
    f->mn[1] = c->tilt_min;
    f->mx[1] = c->tilt_max;
}

static int ptzs_frame_eq(const struct ptzs_frame *a, const struct ptzs_frame *b)
{
    return a->mn[0] == b->mn[0] && a->mx[0] == b->mx[0] && a->mn[1] == b->mn[1] && a->mx[1] == b->mx[1];
}

static int ptzs_frame_format(const struct ptzs_frame *f, char *s, size_t n)
{
    return snprintf(s, n, "pan_min=%u pan_max=%u tilt_min=%u tilt_max=%u\n", (unsigned)f->mn[0], (unsigned)f->mx[0],
                    (unsigned)f->mn[1], (unsigned)f->mx[1]);
}

/* ptzs_frame_parse: strict "pan_min=<n> pan_max=<n> tilt_min=<n> tilt_max=<n>\n", sane against the driver limits. */
static int ptzs_frame_parse(const char *text, struct ptzs_frame *f)
{
    static const char *const names[4] = {"pan_min=", "pan_max=", "tilt_min=", "tilt_max="};
    unsigned long v[4];
    unsigned i;
    const char *p = text;
    for (i = 0; i < 4; i++) {
        char *end;
        size_t l = strlen(names[i]);
        if (strncmp(p, names[i], l) || p[l] < '0' || p[l] > '9')
            return -1;
        errno = 0;
        v[i] = strtoul(p + l, &end, 10);
        if (errno || v[i] > 0xffffUL)
            return -1;
        p = end;
        if (i < 3) {
            if (*p != ' ')
                return -1;
            p++;
        }
    }
    if (*p != '\n' || p[1])
        return -1;
    if (v[0] < 1 || v[2] < 1 || v[1] <= v[0] || v[3] <= v[2] || v[1] - v[0] + 1 > PTZS_DRV_PAN_MAX ||
        v[3] - v[2] + 1 > PTZS_DRV_TILT_MAX)
        return -1;
    f->mn[0] = (uint32_t)v[0];
    f->mx[0] = (uint32_t)v[1];
    f->mn[1] = (uint32_t)v[2];
    f->mx[1] = (uint32_t)v[3];
    return 0;
}

/* ptzs_frame_load: the frame the driver was initialised with, 0 ok; -1 absent, invalidated or corrupt. */
static int ptzs_frame_load_from(const struct ptz_ops *ops, const char *path, struct ptzs_frame *f)
{
    char buf[128];
    if (ops->load_file(path, buf, sizeof(buf)) < 0)
        return -1;
    return ptzs_frame_parse(buf, f);
}
static int ptzs_frame_load(const struct ptz_ops *ops, struct ptzs_frame *f)
{
    return ptzs_frame_load_from(ops, PTZ_FRAME_PATH, f);
}

static void ptzs_conf_defaults(struct ptzs_conf *c)
{
    c->pan_min = PTZS_DEF_PAN_MIN;
    c->pan_max = PTZS_DEF_PAN_SOFT_MAX;
    c->tilt_min = PTZS_DEF_TILT_MIN;
    c->tilt_max = PTZS_DEF_TILT_SOFT_MAX;
    c->pan_home = PTZS_DEF_PAN_HOME;
    c->tilt_home = PTZS_DEF_TILT_HOME;
    c->pan_step_max = PTZS_DEF_PAN_MAX;
    c->tilt_step_max = PTZS_DEF_TILT_MAX;
    c->speed = PTZS_DEF_SPEED;
}

/* ptzs_num: whole-string unsigned decimal with an upper bound; 0 on success. */
static int ptzs_num(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long v;
    if (!s || *s < '0' || *s > '9')
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || *end || v > max)
        return -1;
    *out = v;
    return 0;
}

/* ptzs_conf_check: 0 when the configuration is safe, else -1 with a reason in err. */
static int ptzs_conf_check(const struct ptzs_conf *c, char *err, size_t n)
{
    const char *m = NULL;
    if (c->pan_step_max < 1 || c->pan_step_max > PTZS_DEF_PAN_MAX || c->tilt_step_max < 1 ||
        c->tilt_step_max > PTZS_DEF_TILT_MAX)
        m = "pan_step_max/tilt_step_max must be 1..4000 / 1..2160 (the stock numbering of this unit)";
    else if (c->pan_max > PTZS_CAP_PAN_MAX || c->tilt_max > PTZS_CAP_TILT_MAX)
        m = "pan_max/tilt_max must be at most 3700 / 2100 (measured mechanical stops)";
    else if (c->pan_min < PTZS_MARGIN || c->tilt_min < PTZS_MARGIN)
        m = "pan_min/tilt_min must be at least 50 steps from the driver's 0";
    else if (c->pan_max + PTZS_MARGIN > c->pan_step_max || c->tilt_max + PTZS_MARGIN > c->tilt_step_max)
        m = "pan_max/tilt_max must be at least 50 steps below pan_step_max/tilt_step_max";
    else if (c->pan_max < c->pan_min + PTZS_MIN_SPAN || c->tilt_max < c->tilt_min + PTZS_MIN_SPAN)
        m = "the soft range must be at least 100 steps wide";
    else if (c->pan_home < c->pan_min || c->pan_home > c->pan_max || c->tilt_home < c->tilt_min ||
             c->tilt_home > c->tilt_max)
        m = "pan_home/tilt_home must lie inside the soft range";
    else if (c->speed < PTZS_MIN_SPEED || c->speed > PTZS_MAX_SPEED)
        m = "speed must be 2..20";
    if (m && err)
        snprintf(err, n, "%s", m);
    return m ? -1 : 0;
}

/* ptzs_conf_parse: key=value lines, # comments, strict (unknown or repeated key, junk, long line = error). The
 * result is validated by ptzs_conf_check; on any error *c is unspecified and the caller must refuse to move. */
static int ptzs_conf_parse_over(const char *text, struct ptzs_conf *c, char *err, size_t n);
static int ptzs_conf_parse(const char *text, struct ptzs_conf *c, char *err, size_t n)
{
    ptzs_conf_defaults(c);
    return ptzs_conf_parse_over(text, c, err, n);
}

/* ptzs_conf_parse_over: the same on top of the values already in *c (a model's descriptor instead of the code defaults). */
static int ptzs_conf_parse_over(const char *text, struct ptzs_conf *c, char *err, size_t n)
{
    struct {
        const char *name;
        uint32_t *p;
    } keys[] = {{"pan_min", &c->pan_min},           {"pan_max", &c->pan_max},
                {"tilt_min", &c->tilt_min},         {"tilt_max", &c->tilt_max},
                {"pan_home", &c->pan_home},         {"tilt_home", &c->tilt_home},
                {"pan_step_max", &c->pan_step_max}, {"tilt_step_max", &c->tilt_step_max},
                {"speed", &c->speed}};
    const unsigned nk = sizeof(keys) / sizeof(keys[0]);
    unsigned seen = 0, i;
    while (*text) {
        char line[96], *s, *e, *eq;
        size_t len = strcspn(text, "\n");
        unsigned long v;
        if (len >= sizeof(line)) {
            snprintf(err, n, "line too long");
            return -1;
        }
        memcpy(line, text, len);
        line[len] = 0;
        text += len + (text[len] ? 1 : 0);
        s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            *--e = 0;
        if (!*s || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, n, "not a key=value line: %.40s", s);
            return -1;
        }
        *eq++ = 0;
        for (i = 0; i < nk && strcmp(s, keys[i].name); i++) {
        }
        if (i == nk) {
            snprintf(err, n, "unknown key: %.40s", s);
            return -1;
        }
        if (seen & (1u << i)) {
            snprintf(err, n, "repeated key: %s", keys[i].name);
            return -1;
        }
        seen |= 1u << i;
        if (ptzs_num(eq, 0xffffUL, &v)) {
            snprintf(err, n, "bad value for %s", keys[i].name);
            return -1;
        }
        *keys[i].p = (uint32_t)v;
    }
    return ptzs_conf_check(c, err, n);
}

/* ptzs_conf_load: PTZ_CONF_PATH through ops; an absent file means defaults; anything else wrong is an error. */
static int ptzs_conf_load(const struct ptz_ops *ops, struct ptzs_conf *c, char *err, size_t n)
{
    char buf[1024];
    int len;
    errno = 0;
    len = ops->load_file(PTZ_CONF_PATH, buf, sizeof(buf));
    if (len < 0) {
        if (errno == ENOENT) {
            ptzs_conf_defaults(c);
            return ptzs_conf_check(c, err, n);
        }
        snprintf(err, n, "cannot read %s: %s", PTZ_CONF_PATH, strerror(errno));
        return -1;
    }
    return ptzs_conf_parse(buf, c, err, n);
}

/* position file: "pan=<n> tilt=<n>\n" (the format `pos` prints); 0 ok */
static int ptzs_pos_parse(const char *text, uint32_t *pan, uint32_t *tilt)
{
    char *end;
    unsigned long a, b;
    if (strncmp(text, "pan=", 4) || text[4] < '0' || text[4] > '9')
        return -1;
    a = strtoul(text + 4, &end, 10);
    if (strncmp(end, " tilt=", 6) || end[6] < '0' || end[6] > '9')
        return -1;
    b = strtoul(end + 6, &end, 10);
    if ((*end && strcmp(end, "\n")) || a > 0xffffUL || b > 0xffffUL)
        return -1;
    *pan = (uint32_t)a;
    *tilt = (uint32_t)b;
    return 0;
}

static void ptzs_wait(struct ptzs_ctx *x, unsigned ms)
{
    x->ops->sleep_ms(ms);
    x->t_ms += ms;
}

/* ptzs_read_axis: position of axis 0 (pan) or 1 (tilt): selector in word 0, answer in word 1; 0 ok. With a frame
 * the answer is converted to real coordinates; a counter beyond the frame's driver max means the driver is not in
 * our frame (re-initialised by something else): error ERANGE. Without a frame the raw counter is returned. */
static int ptzs_read_axis(struct ptzs_ctx *x, unsigned axis, uint32_t *steps)
{
    uint32_t buf[PTZ_ARG_BYTES / 4];
    memset(buf, 0, sizeof(buf));
    buf[0] = axis;
    if (x->ops->ioctl_fn(x->fd, PTZ_REQ_POS, buf))
        return -1;
    x->raw[axis] = buf[1];
    if (x->have_frame) {
        if (buf[1] > ptzs_dmax(x, axis)) {
            errno = ERANGE;
            return -1;
        }
        *steps = buf[1] + ptzs_off(x, axis);
    } else {
        *steps = buf[1];
    }
    return 0;
}

static int ptzs_read_pos(struct ptzs_ctx *x, uint32_t *pan, uint32_t *tilt)
{
    return ptzs_read_axis(x, 0, pan) || ptzs_read_axis(x, 1, tilt) ? -1 : 0;
}

/* ptzs_stop: the stop request is retried (3 tries): a lost stop leaves the motor running. */
static int ptzs_stop(struct ptzs_ctx *x)
{
    int i, r = -1;
    for (i = 0; i < 3 && r; i++)
        r = x->ops->ioctl_fn(x->fd, PTZ_REQ_STOP, NULL);
    return r;
}

/* ptzs_save_pos: remember the position for this boot (best effort; the driver counters stay the authority). */
static void ptzs_save_pos(struct ptzs_ctx *x, uint32_t pan, uint32_t tilt)
{
    char s[48];
    snprintf(s, sizeof(s), "pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
    if (x->ops->save_file(PTZ_POS_PATH, s))
        fprintf(stderr, "akam_ptz: warning: cannot save %s: %s\n", PTZ_POS_PATH, strerror(errno));
}

/* ptzs_send_move: the only place that sends the move request. Only the directional commands 0..7 can leave here
 * (never 8..12: stop, absolute move, calibration sweeps). The position file is first marked unknown (fail closed):
 * a tool killed mid-move leaves no stale position that `restore` could later declare to the driver. */
static int ptzs_send_move(struct ptzs_ctx *x, unsigned dir, uint32_t a, uint32_t b)
{
    uint32_t buf[PTZ_ARG_BYTES / 4];
    if (dir > 7) {
        errno = EINVAL;
        return -1;
    }
    if (x->ops->save_file(PTZ_POS_PATH, "unknown\n"))
        return -1;
    memset(buf, 0, sizeof(buf));
    buf[0] = dir;
    buf[1] = a;
    buf[2] = b;
    return x->ops->ioctl_fn(x->fd, PTZ_REQ_MOVE, buf);
}

/* ptzs_finish: always stop, then read and persist the position. Returns the first error code of rc/stop/read. */
static int ptzs_finish(struct ptzs_ctx *x, int rc)
{
    uint32_t pan, tilt;
    if (ptzs_stop(x)) {
        fprintf(stderr, "akam_ptz: stop failed: %s\n", strerror(errno));
        if (!rc)
            rc = PTZS_EIO;
    }
    if (ptzs_read_pos(x, &pan, &tilt)) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return rc ? rc : PTZS_EIO;
    }
    ptzs_save_pos(x, pan, tilt);
#ifndef PTZS_QUIET
    printf("pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
#endif
    return rc;
}

/* ptzs_known: refuse a driver whose raw counters read 0/0 (cold counters: the position is a lie; with the shifted
 * frame a legitimate position never reads 0/0, only a diagonal cut at the very corner would). Call after a read. */
static int ptzs_known(const struct ptzs_ctx *x)
{
    if (x->raw[0] == 0 && x->raw[1] == 0) {
        fprintf(stderr, "akam_ptz: driver counters are 0/0 (not initialised): refusing to move. Run 'restore' "
                        "(same boot, camera not moved) or 'init --i-know [pan tilt]' (camera at that position)\n");
        return -1;
    }
    return 0;
}

/* direction table (0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left; pan + = right,
 * tilt + = up, hardware verified for 0, 2, 4, 6) */
static const int8_t PTZS_PAN_SIGN[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int8_t PTZS_TILT_SIGN[8] = {1, 1, 0, -1, -1, -1, 0, 1};

/* ptzs_room: steps left before the soft limit in the direction of travel (0 when at or beyond it). Outside the
 * range, travel toward the range has room, travel away from it has none. */
static uint32_t ptzs_room(uint32_t pos, uint32_t lo, uint32_t hi, int dir)
{
    if (dir > 0)
        return pos < hi ? hi - pos : 0;
    return pos > lo ? pos - lo : 0;
}

/* ptzs_move: timed directional move, polled, clipped at the soft limits, stall-protected.
 * a, b: speed divisors (0 = the configured default). Returns PTZS_* (0 ok, also when clipped by a limit). */
static int ptzs_move(struct ptzs_ctx *x, unsigned dir, unsigned ms, uint32_t a, uint32_t b)
{
    const struct ptzs_conf *cf = x->cf;
    int d[2];
    uint32_t lo[2] = {cf->pan_min, cf->tilt_min}, hi[2] = {cf->pan_max, cf->tilt_max}, sp[2], pos[2], last[2];
    unsigned still[2] = {0, 0}, el = 0, ax;
    int rc = 0, clipped = 0;

    if (dir > 7 || ms == 0)
        return PTZS_EREFUSED;
    sp[0] = a ? a : cf->speed;
    sp[1] = b ? b : cf->speed;
    if (sp[0] < PTZS_MIN_SPEED || sp[0] > PTZS_MAX_SPEED || sp[1] < PTZS_MIN_SPEED || sp[1] > PTZS_MAX_SPEED) {
        fprintf(stderr, "akam_ptz: speed divisors must be %u..%u\n", PTZS_MIN_SPEED, PTZS_MAX_SPEED);
        return PTZS_EREFUSED;
    }
    d[0] = PTZS_PAN_SIGN[dir];
    d[1] = PTZS_TILT_SIGN[dir];
    if (ptzs_read_pos(x, &pos[0], &pos[1])) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return PTZS_EIO;
    }
    if (ptzs_known(x))
        return PTZS_EREFUSED;
    for (ax = 0; ax < 2; ax++) {
        if (d[ax] && ptzs_room(pos[ax], lo[ax], hi[ax], d[ax]) <= PTZS_POLL_MS / sp[ax] + 3) {
            fprintf(stderr, "akam_ptz: refused: %s is at its soft limit (%u, range %u..%u)\n", ax ? "tilt" : "pan",
                    (unsigned)pos[ax], (unsigned)lo[ax], (unsigned)hi[ax]);
            return PTZS_EREFUSED;
        }
        last[ax] = pos[ax];
    }
    if (ptzs_send_move(x, dir, sp[0], sp[1])) {
        fprintf(stderr, "akam_ptz: move failed: %s\n", strerror(errno));
        return ptzs_finish(x, PTZS_EIO);
    }
    while (!rc && !clipped && el < ms) {
        unsigned step = ms - el < PTZS_POLL_MS ? ms - el : PTZS_POLL_MS;
        ptzs_wait(x, step);
        el += step;
        if (ptzs_read_pos(x, &pos[0], &pos[1])) {
            fprintf(stderr, "akam_ptz: pos failed during the move: %s\n", strerror(errno));
            rc = PTZS_EIO;
            break;
        }
        for (ax = 0; ax < 2 && !rc; ax++) {
            long delta = ((long)pos[ax] - (long)last[ax]) * d[ax];
            if (!d[ax])
                continue;
            if (delta > 0) {
                last[ax] = pos[ax];
                still[ax] = 0;
            } else {
                still[ax] += step;
                if (delta < -2) {
                    fprintf(stderr, "akam_ptz: %s moves the wrong way: stopping\n", ax ? "tilt" : "pan");
                    rc = PTZS_ESTALL;
                }
            }
            if (!rc && still[ax] > PTZS_STALL_MS) {
                fprintf(stderr, "akam_ptz: stall: %s position unchanged for %u ms: stopping\n", ax ? "tilt" : "pan",
                        still[ax]);
                rc = PTZS_ESTALL;
            }
            if (!rc && ptzs_room(pos[ax], lo[ax], hi[ax], d[ax]) <= PTZS_POLL_MS / sp[ax] + 3) {
                fprintf(stderr, "akam_ptz: clipped at the %s soft limit\n", ax ? "tilt" : "pan");
                clipped = 1;
            }
        }
    }
    return ptzs_finish(x, rc);
}

/* ptzs_axis_goto: bring one axis to target (inside the soft range, checked by the caller) with polled timed
 * moves; up to PTZS_PASSES passes, each pass one directional move that is stopped at the target. */
static int ptzs_axis_goto(struct ptzs_ctx *x, unsigned ax, uint32_t target, uint32_t sp)
{
    const struct ptzs_conf *cf = x->cf;
    uint32_t lo = ax ? cf->tilt_min : cf->pan_min, hi = ax ? cf->tilt_max : cf->pan_max;
    uint32_t pos;
    int pass;
    for (pass = 0; pass < PTZS_PASSES; pass++) {
        uint32_t last;
        long err;
        int dsign, rc = 0, done = 0;
        unsigned dir, el = 0, still = 0, timeout;
        if (ptzs_read_axis(x, ax, &pos)) {
            fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
            return PTZS_EIO;
        }
        err = (long)target - (long)pos;
        if (err <= (long)PTZS_TOL && err >= -(long)PTZS_TOL)
            return PTZS_OK;
        dsign = err > 0 ? 1 : -1;
        dir = ax ? (dsign > 0 ? 0 : 4) : (dsign > 0 ? 2 : 6);
        timeout = (unsigned)(err * dsign) * sp * 2 + 600;
        last = pos;
        if (ptzs_send_move(x, dir, sp, sp)) {
            fprintf(stderr, "akam_ptz: move failed: %s\n", strerror(errno));
            (void)ptzs_stop(x);
            return PTZS_EIO;
        }
        while (!rc && !done) {
            long rem = ((long)target - (long)pos) * dsign, delta;
            unsigned w = PTZS_POLL_MS;
            if (rem > 0 && (unsigned long)rem * sp < PTZS_POLL_MS)
                w = (unsigned)rem * sp;
            if (!w)
                w = 1;
            ptzs_wait(x, w);
            el += w;
            if (ptzs_read_axis(x, ax, &pos)) {
                fprintf(stderr, "akam_ptz: pos failed during goto: %s\n", strerror(errno));
                rc = PTZS_EIO;
                break;
            }
            delta = ((long)pos - (long)last) * dsign;
            if (delta > 0) {
                last = pos;
                still = 0;
            } else {
                still += w;
                if (delta < -2) {
                    fprintf(stderr, "akam_ptz: axis moves the wrong way: stopping\n");
                    rc = PTZS_ESTALL;
                }
            }
            if (!rc && still > PTZS_STALL_MS) {
                fprintf(stderr, "akam_ptz: stall: position unchanged for %u ms: stopping\n", still);
                rc = PTZS_ESTALL;
            } else if (!rc && ((dsign < 0 && pos < lo) || (dsign > 0 && pos > hi))) {
                fprintf(stderr, "akam_ptz: goto left the soft range (%u): stopping\n", (unsigned)pos);
                rc = PTZS_ESTALL;
            } else if (!rc && el > timeout) {
                fprintf(stderr, "akam_ptz: goto timeout: stopping\n");
                rc = PTZS_ESTALL;
            } else if (!rc && ((long)target - (long)pos) * dsign <= 0) {
                done = 1;
            }
        }
        if (ptzs_stop(x)) {
            fprintf(stderr, "akam_ptz: stop failed: %s\n", strerror(errno));
            return PTZS_EIO;
        }
        if (rc)
            return rc;
        ptzs_wait(x, 20);
    }
    if (ptzs_read_axis(x, ax, &pos))
        return PTZS_EIO;
    if ((pos > target ? pos - target : target - pos) > PTZS_ACCEPT) {
        fprintf(stderr, "akam_ptz: goto did not converge (at %u, wanted %u)\n", (unsigned)pos, (unsigned)target);
        return PTZS_ESTALL;
    }
    return PTZS_OK;
}

/* ptzs_goto: absolute move inside the soft range, pan first then tilt. A target outside the range is REFUSED
 * (not clipped: the caller asked for a place the camera must not go). speed 0 = the configured default. */
static int ptzs_goto(struct ptzs_ctx *x, uint32_t pan, uint32_t tilt, uint32_t speed)
{
    const struct ptzs_conf *cf = x->cf;
    uint32_t p, t;
    int rc;
    if (!speed)
        speed = cf->speed;
    if (speed < PTZS_MIN_SPEED || speed > PTZS_MAX_SPEED) {
        fprintf(stderr, "akam_ptz: speed divisor must be %u..%u\n", PTZS_MIN_SPEED, PTZS_MAX_SPEED);
        return PTZS_EREFUSED;
    }
    if (pan < cf->pan_min || pan > cf->pan_max || tilt < cf->tilt_min || tilt > cf->tilt_max) {
        fprintf(stderr, "akam_ptz: refused: target %u/%u is outside the soft range pan %u..%u tilt %u..%u\n",
                (unsigned)pan, (unsigned)tilt, (unsigned)cf->pan_min, (unsigned)cf->pan_max, (unsigned)cf->tilt_min,
                (unsigned)cf->tilt_max);
        return PTZS_EREFUSED;
    }
    if (ptzs_read_pos(x, &p, &t)) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return PTZS_EIO;
    }
    if (ptzs_known(x))
        return PTZS_EREFUSED;
    rc = ptzs_axis_goto(x, 0, pan, speed);
    if (!rc)
        rc = ptzs_axis_goto(x, 1, tilt, speed);
    return ptzs_finish(x, rc);
}

/* ptzs_park: back to the configured home (call before a clean shutdown/reboot: after a reboot `init` declares
 * home, which is only true when the camera was left there). */
static int ptzs_park(struct ptzs_ctx *x)
{
    return ptzs_goto(x, x->cf->pan_home, x->cf->tilt_home, 0);
}

#endif
