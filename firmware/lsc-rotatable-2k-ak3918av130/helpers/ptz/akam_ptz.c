/* akam_ptz -- tiny on-camera PTZ tool for the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
 *
 * Usage: akam_ptz stop | pos | limits | move <dir 0-7> <ms> [a b] | goto <pan> <tilt> [speed] | park | restore |
 *                 init --i-know [pan tilt]
 *
 * Talks to the stock motor driver node and nothing else. Request numbers and the argument layout come from
 * the offline analysis in docs/porting/lsc-rotatable-2k/ptz-findings.md; hardware verified 2026-11-15.
 * The motor is an open-loop stepper with NO limit switch: every motion goes through the software-limited layer of
 * akam_ptz_safe.h (soft range, 50 ms polling, stall detection, explicit speed), see "Safe motion layer" there.
 *   stop    : ioctl(fd, _IO(0x0c,0)) (works even with a bad ptz.conf)
 *   pos     : ioctl(fd, _IOR(0x0c,1,28), buf) twice (selector 0 pan, 1 tilt); prints "pan=<steps> tilt=<steps>" in
 *             REAL coordinates (the stock numbering); before init, or with counters 0/0, the raw driver counters
 *   limits  : prints the effective soft range / home / speed (akam/ptz.conf on the card, else defaults); no device
 *   move    : timed directional move (dirs 0 up, 2 right, 4 down, 6 left, odd = diagonals), at most 1500 ms, polled,
 *             clipped at the soft limits, stopped on a stall; speeds a b = 2..20 divisors (default: ptz.conf speed)
 *   goto    : absolute position inside the soft range (refused outside), timed moves + polling, pan then tilt
 *   park    : goto home (ptz.conf pan_home/tilt_home); run it before a clean shutdown or reboot
 *   init    : replays what the stock app sends once at start (ptz-findings.md, "Motor init without yy_cam"):
 *             _IOW(0x0c,3,16) {0, pan_dmax, tilt_dmax, 0}; 1 s; _IOW(0x0c,4,40) {pan_cnt, tilt_cnt, 0...}; 1 s; then
 *             pos. The values are in the SHIFTED frame of akam_ptz_safe.h: the driver's own limits are the soft range
 *             (counter = real - (soft_min - 1), step_max = soft_max - soft_min + 1), so even a tool killed mid-move
 *             cannot be walked past a soft limit by the driver. [pan tilt] (real coordinates, default: ptz.conf
 *             home) is WHERE THE CAMERA IS NOW; it moves nothing, and it refuses to run without --i-know because a
 *             wrong "where I am" makes later moves hit the mechanical end stop. Never run it while the stock app
 *             runs (it would reset its counters). The frame is kept in /tmp/akam_ptz.frame; a ptz.conf whose soft
 *             range differs from it refuses every motion until `init` is run again.
 *   restore : same boot, driver reloaded (counters 0/0): re-declare the position saved in /tmp/akam_ptz.pos with
 *             the frame of /tmp/akam_ptz.frame (refused when either is missing/unknown or ptz.conf changed)
 *   Any motion, init or restore first invalidates the app's frame file (/tmp/akam_ptz_app.frame) and writes "unknown" to
 *   /config/akam/ptz.pos: the app never re-adopts a head the tool moved (it calibrates at its next start).
 *   Every motion command stops the motor last, saves the position to /tmp/akam_ptz.pos and prints it.
 * SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGPIPE stop the motor (SIGTSTP is ignored); after SIGKILL the driver itself stops
 * the motor at the soft limit (its limits are the soft range).
 * Deliberately absent: driver absolute move (command 10), the calibration sweep (commands 9 and 12), raw access.
 * Exit codes: 0 ok, 1 device or ioctl error, 2 usage, 3 device busy, 4 refused (soft limit, uninitialised
 * counters, no or stale frame, target outside the range), 5 stall / timeout / no convergence (motor stopped), 6 bad ptz.conf.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "akam_ptz_safe.h"

#define PTZ_DEV "/dev/tymotor"
#define PTZ_REQ_INIT_LIMITS 0x40100c03u /* _IOW(0x0c,3,16) */
#define PTZ_REQ_INIT_POS 0x40280c04u /* _IOW(0x0c,4,40) */
#define PTZ_INIT_LIMITS_BYTES 16
#define PTZ_INIT_POS_BYTES 40
#define PTZ_MAX_MS 1500
#define PTZ_MAX_DIR 7

struct ptz_cmd {
    enum { PTZ_STOP, PTZ_POS, PTZ_LIMITS, PTZ_MOVE, PTZ_GOTO, PTZ_PARK, PTZ_RESTORE, PTZ_INIT } kind;
    unsigned dir, ms;
    uint32_t a, b;
    int init_explicit;         /* init: 1 = pan/tilt given (real position now), 0 = ptz.conf home */
    uint32_t pan, tilt, speed; /* goto target / init position */
};

static int g_fd = -1;
static const struct ptz_ops *g_ops;

/* ptz_parse: 0 ok, -1 usage error. The raw access is refused on purpose. */
int ptz_parse(int argc, char **argv, struct ptz_cmd *c)
{
    unsigned long v;
    memset(c, 0, sizeof(*c));
    if (argc < 2)
        return -1;
    if (!strcmp(argv[1], "stop") && argc == 2) {
        c->kind = PTZ_STOP;
        return 0;
    }
    if (!strcmp(argv[1], "pos") && argc == 2) {
        c->kind = PTZ_POS;
        return 0;
    }
    if (!strcmp(argv[1], "limits") && argc == 2) {
        c->kind = PTZ_LIMITS;
        return 0;
    }
    if (!strcmp(argv[1], "park") && argc == 2) {
        c->kind = PTZ_PARK;
        return 0;
    }
    if (!strcmp(argv[1], "restore") && argc == 2) {
        c->kind = PTZ_RESTORE;
        return 0;
    }
    if (!strcmp(argv[1], "raw")) {
        fprintf(stderr, "akam_ptz: raw is disabled in this build\n");
        return -1;
    }
    if (!strcmp(argv[1], "cal")) {
        fprintf(stderr, "akam_ptz: cal is not implemented on purpose: the motor has no limit switch, so a "
                        "software sweep cannot measure the range and can only grind at the mechanical stop. See "
                        "ptz-findings.md, \"Safe motion layer\", for the manual measurement procedure.\n");
        return -1;
    }
    if (!strcmp(argv[1], "goto") && (argc == 4 || argc == 5)) {
        c->kind = PTZ_GOTO;
        if (ptzs_num(argv[2], 0xffffUL, &v))
            return -1;
        c->pan = (uint32_t)v;
        if (ptzs_num(argv[3], 0xffffUL, &v))
            return -1;
        c->tilt = (uint32_t)v;
        if (argc == 5) {
            if (ptzs_num(argv[4], PTZS_MAX_SPEED, &v) || v < PTZS_MIN_SPEED) {
                fprintf(stderr, "akam_ptz: speed divisor must be %u..%u\n", PTZS_MIN_SPEED, PTZS_MAX_SPEED);
                return -1;
            }
            c->speed = (uint32_t)v;
        }
        return 0;
    }
    if (!strcmp(argv[1], "init")) {
        if (argc < 3 || strcmp(argv[2], "--i-know")) {
            fprintf(stderr, "akam_ptz: init tells the driver where the camera is (counters) and its step limits; a\n"
                            "wrong position makes later moves hit the end stop. Pass --i-know to run it.\n");
            return -1;
        }
        if (argc != 3 && argc != 5)
            return -1;
        c->kind = PTZ_INIT;
        c->init_explicit = argc == 5;
        if (argc == 5) {
            /* real coordinates; checked against the soft range of ptz.conf in do_init (0 is inside no range) */
            if (ptzs_num(argv[3], 0xffffUL, &v) || v < 1)
                return -1;
            c->pan = (uint32_t)v;
            if (ptzs_num(argv[4], 0xffffUL, &v) || v < 1)
                return -1;
            c->tilt = (uint32_t)v;
        }
        return 0;
    }
    if (!strcmp(argv[1], "move") && (argc == 4 || argc == 6)) {
        c->kind = PTZ_MOVE;
        if (ptzs_num(argv[2], PTZ_MAX_DIR, &v)) {
            fprintf(stderr, "akam_ptz: direction must be 0..%d\n", PTZ_MAX_DIR);
            return -1;
        }
        c->dir = (unsigned)v;
        if (ptzs_num(argv[3], PTZ_MAX_MS, &v) || v == 0) {
            fprintf(stderr, "akam_ptz: duration must be 1..%d ms\n", PTZ_MAX_MS);
            return -1;
        }
        c->ms = (unsigned)v;
        if (argc == 6) {
            /* 0 ("keep the previous speed") is refused: the previous speed is unknown */
            if (ptzs_num(argv[4], PTZS_MAX_SPEED, &v) || v < PTZS_MIN_SPEED) {
                fprintf(stderr, "akam_ptz: speed divisors must be %u..%u\n", PTZS_MIN_SPEED, PTZS_MAX_SPEED);
                return -1;
            }
            c->a = (uint32_t)v;
            if (ptzs_num(argv[5], PTZS_MAX_SPEED, &v) || v < PTZS_MIN_SPEED) {
                fprintf(stderr, "akam_ptz: speed divisors must be %u..%u\n", PTZS_MIN_SPEED, PTZS_MAX_SPEED);
                return -1;
            }
            c->b = (uint32_t)v;
        }
        return 0;
    }
    return -1;
}

/* ptz_init_limits_arg: 16 bytes {pan_roll_step = 0, pan_max_step, tilt_max_step, infi_rot_en = 0}, the driver's
 * limits of the frame (never 0: 0 would mean "keep the current value"). */
void ptz_init_limits_arg(const struct ptzs_ctx *x, uint32_t out[PTZ_INIT_LIMITS_BYTES / 4])
{
    memset(out, 0, PTZ_INIT_LIMITS_BYTES);
    out[1] = ptzs_dmax(x, 0);
    out[2] = ptzs_dmax(x, 1);
}

/* ptz_init_pos_arg: 40 bytes {current pan counter, current tilt counter, zeros}: the real position shifted into
 * the frame (>= 1 by construction). */
void ptz_init_pos_arg(const struct ptzs_ctx *x, uint32_t pan, uint32_t tilt, uint32_t out[PTZ_INIT_POS_BYTES / 4])
{
    memset(out, 0, PTZ_INIT_POS_BYTES);
    out[0] = pan - ptzs_off(x, 0);
    out[1] = tilt - ptzs_off(x, 1);
}

/* print_pos: "pan=<n> tilt=<n>" in real coordinates; the raw driver counters when there is no frame or they read
 * 0/0 (uninitialised: no real position exists); 0 ok. */
static int print_pos(struct ptzs_ctx *x)
{
    uint32_t pan = 0, tilt = 0;
    if (ptzs_read_pos(x, &pan, &tilt)) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return -1;
    }
    if (!x->have_frame || (x->raw[0] == 0 && x->raw[1] == 0)) {
        pan = x->raw[0];
        tilt = x->raw[1];
        fprintf(stderr, "akam_ptz: note: raw driver counters (no initialised position)\n");
    }
    printf("pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
    return 0;
}

/* do_init: declare the camera at real position pan/tilt in the frame of the soft range cf: limits, 1 s, counters,
 * 1 s (like the stock app), read back and verify, then record the frame and the position. The frame file is
 * invalidated first and written last, so a failure anywhere leaves "no frame" and every motion refused. */
static int do_init(struct ptzs_ctx *x, const struct ptzs_conf *cf, uint32_t pan, uint32_t tilt)
{
    uint32_t lim[PTZ_INIT_LIMITS_BYTES / 4], pos[PTZ_INIT_POS_BYTES / 4], rp, rt;
    struct ptzs_frame fr;
    char s[128];
    ptzs_frame_from_conf(&fr, cf);
    if (pan < fr.mn[0] || pan > fr.mx[0] || tilt < fr.mn[1] || tilt > fr.mx[1]) {
        fprintf(stderr, "akam_ptz: refused: init position %u/%u is outside the soft range pan %u..%u tilt %u..%u\n",
                (unsigned)pan, (unsigned)tilt, (unsigned)fr.mn[0], (unsigned)fr.mx[0], (unsigned)fr.mn[1],
                (unsigned)fr.mx[1]);
        return PTZS_EREFUSED;
    }
    if (x->ops->save_file(PTZ_FRAME_PATH, "invalid\n")) {
        fprintf(stderr, "akam_ptz: cannot write %s: %s\n", PTZ_FRAME_PATH, strerror(errno));
        return 1;
    }
    x->fr = fr;
    x->have_frame = 0;
    ptz_init_limits_arg(x, lim);
    ptz_init_pos_arg(x, pan, tilt, pos);
    if (x->ops->ioctl_fn(x->fd, PTZ_REQ_INIT_LIMITS, lim)) {
        fprintf(stderr, "akam_ptz: init (limits) failed: %s\n", strerror(errno));
        return 1;
    }
    x->ops->sleep_ms(1000);
    if (x->ops->ioctl_fn(x->fd, PTZ_REQ_INIT_POS, pos)) {
        fprintf(stderr, "akam_ptz: init (position) failed: %s\n", strerror(errno));
        return 1;
    }
    x->ops->sleep_ms(1000);
    if (ptzs_read_pos(x, &rp, &rt)) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return 1;
    }
    if (x->raw[0] != pos[0] || x->raw[1] != pos[1]) {
        fprintf(stderr, "akam_ptz: init: the driver reads %u/%u, expected %u/%u: frame not recorded, motion stays "
                        "refused\n", (unsigned)x->raw[0], (unsigned)x->raw[1], (unsigned)pos[0], (unsigned)pos[1]);
        return 1;
    }
    ptzs_frame_format(&fr, s, sizeof(s));
    if (x->ops->save_file(PTZ_FRAME_PATH, s)) {
        fprintf(stderr, "akam_ptz: cannot write %s: %s\n", PTZ_FRAME_PATH, strerror(errno));
        return 1;
    }
    x->have_frame = 1;
    printf("pan=%u tilt=%u\n", (unsigned)pan, (unsigned)tilt);
    ptzs_save_pos(x, pan, tilt);
    return 0;
}

/* do_restore: driver counters 0/0, a saved position and the frame of this boot: declare them again (no motion). */
static int do_restore(struct ptzs_ctx *x, const struct ptzs_conf *cf)
{
    char buf[64];
    uint32_t sp, st, p, t;
    struct ptzs_frame fr, want;
    int n;
    errno = 0;
    n = x->ops->load_file(PTZ_POS_PATH, buf, sizeof(buf));
    if (n < 0 || ptzs_pos_parse(buf, &sp, &st) || ptzs_frame_load(x->ops, &fr)) {
        fprintf(stderr, "akam_ptz: no usable saved position and frame in %s / %s (a reboot or a move cut short clears "
                        "them): use 'init --i-know' only if the camera is physically at that position\n",
                PTZ_POS_PATH, PTZ_FRAME_PATH);
        return PTZS_EREFUSED;
    }
    ptzs_frame_from_conf(&want, cf);
    if (!ptzs_frame_eq(&fr, &want)) {
        fprintf(stderr, "akam_ptz: ptz.conf changed since the last init: refusing to restore\n");
        return PTZS_EREFUSED;
    }
    x->fr = fr;
    x->have_frame = 1;
    if (ptzs_read_pos(x, &p, &t)) {
        fprintf(stderr, "akam_ptz: pos failed: %s\n", strerror(errno));
        return 1;
    }
    if (x->raw[0] || x->raw[1]) {
        fprintf(stderr, "akam_ptz: the driver already has a position (raw %u/%u): nothing to restore\n",
                (unsigned)x->raw[0], (unsigned)x->raw[1]);
        return PTZS_EREFUSED;
    }
    return do_init(x, cf, sp, st);
}

/* ptz_frame_needed: load the frame of this boot's init for a motion command: 0 ok, else the exit code. */
static int ptz_frame_needed(struct ptzs_ctx *x, const struct ptzs_conf *cf)
{
    struct ptzs_frame fr, want;
    if (ptzs_frame_load(x->ops, &fr)) {
        fprintf(stderr, "akam_ptz: no coordinate frame (%s): the driver was not initialised by this tool this boot. "
                        "Run 'init --i-know [pan tilt]' with the camera at that position\n", PTZ_FRAME_PATH);
        return PTZS_EREFUSED;
    }
    ptzs_frame_from_conf(&want, cf);
    if (!ptzs_frame_eq(&fr, &want)) {
        fprintf(stderr, "akam_ptz: ptz.conf soft range (pan %u..%u tilt %u..%u) differs from the one the driver was "
                        "initialised with (pan %u..%u tilt %u..%u): run 'pos', then 'init --i-know <pan> <tilt>' with "
                        "the position it printed\n", (unsigned)want.mn[0], (unsigned)want.mx[0], (unsigned)want.mn[1],
                (unsigned)want.mx[1], (unsigned)fr.mn[0], (unsigned)fr.mx[0], (unsigned)fr.mn[1], (unsigned)fr.mx[1]);
        return PTZS_EREFUSED;
    }
    x->fr = fr;
    x->have_frame = 1;
    return 0;
}

/* tool_invalidate: the head is about to move (or be re-declared) outside the app: the app must never trust its frame or its saved
 * position again (it calibrates at its next start). The app frame file is the one that matters (fail closed: refused when it cannot
 * be written). Nothing else is written: no flash write from the tool either (the home reference is keyed to home and needs no invalidation). */
static int tool_invalidate(struct ptzs_ctx *x)
{
    if (x->ops->save_file(PTZ_APP_FRAME_PATH, "invalid\n")) {
        fprintf(stderr, "akam_ptz: cannot write %s: %s\n", PTZ_APP_FRAME_PATH, strerror(errno));
        return 1;
    }
    return 0;
}

int ptz_run(const struct ptz_cmd *c, const struct ptz_ops *ops)
{
    struct ptzs_conf cf;
    struct ptzs_ctx x;
    char err[128];
    int rc = 0, have_conf = 0;

    g_ops = ops;
    /* motion commands need a valid configuration before the motor is touched; stop and pos never do */
    if (c->kind != PTZ_STOP && c->kind != PTZ_POS) {
        if (ptzs_conf_load(ops, &cf, err, sizeof(err))) {
            fprintf(stderr, "akam_ptz: bad %s: %s\n", PTZ_CONF_PATH, err);
            return PTZS_ECONF;
        }
        have_conf = 1;
    }
    if (c->kind == PTZ_LIMITS) {
        printf("pan=%u..%u tilt=%u..%u home=%u/%u step_max=%u/%u speed=%u\n", (unsigned)cf.pan_min,
               (unsigned)cf.pan_max, (unsigned)cf.tilt_min, (unsigned)cf.tilt_max, (unsigned)cf.pan_home,
               (unsigned)cf.tilt_home, (unsigned)cf.pan_step_max, (unsigned)cf.tilt_step_max, (unsigned)cf.speed);
        return 0;
    }
    g_fd = ops->open_dev();
    if (g_fd < 0) {
        if (errno == EBUSY)
            fprintf(stderr, "akam_ptz: %s is busy (EBUSY): another process holds the motor\n", PTZ_DEV);
        else
            fprintf(stderr, "akam_ptz: cannot open %s: %s\n", PTZ_DEV, strerror(errno));
        return errno == EBUSY ? 3 : 1;
    }
    memset(&x, 0, sizeof(x));
    x.ops = ops;
    x.fd = g_fd;
    x.cf = have_conf ? &cf : NULL;
    if (c->kind == PTZ_MOVE || c->kind == PTZ_GOTO || c->kind == PTZ_PARK) {
        rc = ptz_frame_needed(&x, &cf);
        if (rc)
            return rc;
        rc = tool_invalidate(&x);
        if (rc)
            return rc;
    } else if (c->kind == PTZ_POS && !ptzs_frame_load(ops, &x.fr)) {
        x.have_frame = 1; /* pos prints real coordinates in the frame of the last init */
    }
    if (c->kind == PTZ_INIT || c->kind == PTZ_RESTORE) {
        rc = tool_invalidate(&x);
        if (rc)
            return rc;
    }
    switch (c->kind) {
    case PTZ_STOP:
        if (ptzs_stop(&x)) {
            fprintf(stderr, "akam_ptz: stop failed: %s\n", strerror(errno));
            rc = 1;
        }
        break;
    case PTZ_POS:
        if (print_pos(&x))
            rc = 1;
        break;
    case PTZ_INIT:
        rc = do_init(&x, &cf, c->init_explicit ? c->pan : cf.pan_home, c->init_explicit ? c->tilt : cf.tilt_home);
        break;
    case PTZ_RESTORE:
        rc = do_restore(&x, &cf);
        break;
    case PTZ_MOVE:
        rc = ptzs_move(&x, c->dir, c->ms, c->a, c->b);
        break;
    case PTZ_GOTO:
        rc = ptzs_goto(&x, c->pan, c->tilt, c->speed);
        break;
    case PTZ_PARK:
        rc = ptzs_park(&x);
        break;
    case PTZ_LIMITS:
        break;
    }
    return rc;
}

#ifndef AKAM_PTZ_NO_MAIN
static int real_open(void)
{
    return open(PTZ_DEV, O_RDWR);
}

static int real_ioctl(int fd, unsigned long req, void *arg)
{
    return ioctl(fd, req, arg);
}

static void real_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

static int real_load(const char *path, char *buf, unsigned cap)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f) || !feof(f)) { /* error, or larger than the buffer: fail closed */
        int e = ferror(f) ? EIO : EFBIG;
        fclose(f);
        errno = e;
        return -1;
    }
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static int real_save(const char *path, const char *data)
{
    char tmp[96];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    if (fputs(data, f) == EOF || fclose(f) == EOF) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path);
}

static void on_signal(int sig)
{
    (void)sig;
    if (g_fd >= 0) {
        int i, r = -1;
        for (i = 0; i < 3 && r; i++)
            r = g_ops->ioctl_fn(g_fd, PTZ_REQ_STOP, NULL);
    }
    _exit(130);
}

int main(int argc, char **argv)
{
    static const struct ptz_ops ops = {real_open, real_ioctl, real_sleep_ms, real_load, real_save};
    struct ptz_cmd c;
    struct sigaction sa;

    if (ptz_parse(argc, argv, &c)) {
        fprintf(stderr, "usage: akam_ptz stop | pos | limits | move <dir 0-7> <ms 1-%d> [a b] | goto <pan> <tilt> "
                        "[speed] | park | restore | init --i-know [pan tilt]\n",
                PTZ_MAX_MS);
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    /* a job-control stop (^Z) would freeze the process while the motor runs: ignore it */
    signal(SIGTSTP, SIG_IGN);
    return ptz_run(&c, &ops);
}
#endif
