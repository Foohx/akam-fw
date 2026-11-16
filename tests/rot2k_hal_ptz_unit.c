/* Host unit test of hal_ptz (app/hal_ptz.c + the safe-motion layer ptz/akam_ptz_safe.h) and of the PTZ controller thread
 * (app/akam2k_ptz.c) against a simulated motor: a step-counting driver (commands 0..7, stop, counter reads, the two init requests)
 * on top of a "mechanical" position with hard stops, plus in-memory files. Run with a fake clock: sleeping advances the model.
 * Covers the calibration path, the skip path (trusted saved position), cal off, adoption after a process restart, park / home,
 * soft-limit clipping, stall, cancel, the flash-write budget, the thread (queue, nudge sum, cancel, refusal while calibrating, privacy)
 * and the parsers. A driver command word other than 0..7 or a move past the soft range after the calibration fails the test. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#include "../firmware/lsc-rotatable-2k-ak3918av130/ptz/akam_ptz_safe.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_hal.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_ptz.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/app/hal_ptz.h"

#define REQ_LIM 0x40100c03u
#define REQ_IPOS 0x40280c04u
#define POSF "/config/akam/ptz.pos"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ------------------------------------------------------------------------------------------------ the simulated motor */
static struct {
    uint32_t cnt[2], tgt[2], spd[2], drvmax[2];
    int running[2], mech[2], lo[2], hi[2], stuck[2];
    unsigned acc[2];
    unsigned long grind, clock;
    int bad_cmd, nmove, nstop, ninit1, ninit2, open_fail, cmds[256], ncmd;
    unsigned dirs;
    uint32_t first_ipos[2], last_lim[4], last_ipos[10];
    struct { char path[96], data[128]; int present, fail; unsigned writes; } f[6];
    char log[8192];
    pthread_mutex_t gm;
    pthread_cond_t gc;
    int gate, blocked;
    int cancel_at;           /* >0: call the cancel hook when this many sleeps remain... (counted down per sleep) */
    hal_ptz_t *cancel_hp;
} M = { .gm = PTHREAD_MUTEX_INITIALIZER, .gc = PTHREAD_COND_INITIALIZER };

static void reset(int pan, int tilt)
{
    pthread_mutex_lock(&M.gm);
    memset(M.cnt, 0, sizeof M.cnt);
    memset(M.tgt, 0, sizeof M.tgt);
    memset(M.running, 0, sizeof M.running);
    memset(M.stuck, 0, sizeof M.stuck);
    memset(M.acc, 0, sizeof M.acc);
    M.grind = M.clock = 0;
    M.bad_cmd = M.nmove = M.nstop = M.ninit1 = M.ninit2 = M.open_fail = M.ncmd = 0;
    M.dirs = 0;
    memset(M.f, 0, sizeof M.f);
    M.log[0] = 0;
    M.gate = M.blocked = M.cancel_at = 0;
    M.cancel_hp = NULL;
    M.drvmax[0] = 6000;
    M.drvmax[1] = 2200;
    M.spd[0] = 5;
    M.spd[1] = 4;
    M.lo[0] = M.lo[1] = 0;
    M.hi[0] = 4000;
    M.hi[1] = 2300; /* the top stop lies between 2290 and 2340 (measured) */
    M.mech[0] = pan;
    M.mech[1] = tilt;
    pthread_mutex_unlock(&M.gm);
}

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        M.clock++;
        for (int ax = 0; ax < 2; ax++) {
            int step;
            if (!M.running[ax] || M.cnt[ax] == M.tgt[ax]) {
                M.running[ax] = 0;
                continue;
            }
            if (++M.acc[ax] < M.spd[ax])
                continue;
            M.acc[ax] = 0;
            if (M.stuck[ax])
                continue;
            step = M.tgt[ax] > M.cnt[ax] ? 1 : -1;
            M.cnt[ax] += (uint32_t)step;
            M.mech[ax] += step;
            if (M.mech[ax] < M.lo[ax] || M.mech[ax] > M.hi[ax]) { /* the motor skips steps against a stop */
                M.mech[ax] -= step;
                M.grind++;
            }
        }
    }
}

static int s_open(void)
{
    if (M.open_fail) {
        errno = EBUSY;
        return -1;
    }
    return 3;
}

static int s_ioctl(int fd, unsigned long req, void *arg)
{
    uint32_t *w = arg;
    (void)fd;
    if (req == PTZ_REQ_STOP) {
        M.running[0] = M.running[1] = 0;
        M.nstop++;
        return 0;
    }
    if (req == PTZ_REQ_POS) {
        if (w[0] > 1) {
            errno = EINVAL;
            return -1;
        }
        w[1] = M.cnt[w[0]];
        return 0;
    }
    if (req == PTZ_REQ_MOVE) {
        unsigned cmd = w[0];
        if (cmd > 7) {
            M.bad_cmd++;
            errno = EINVAL;
            return -1;
        }
        M.nmove++;
        M.dirs |= 1u << cmd;
        if (M.ncmd < 256)
            M.cmds[M.ncmd++] = (int)cmd;
        if (w[1])
            M.spd[0] = w[1];
        if (w[2])
            M.spd[1] = w[2];
        if (PTZS_PAN_SIGN[cmd]) {
            M.tgt[0] = PTZS_PAN_SIGN[cmd] > 0 ? M.drvmax[0] : 0;
            M.running[0] = 1;
        }
        if (PTZS_TILT_SIGN[cmd]) {
            M.tgt[1] = PTZS_TILT_SIGN[cmd] > 0 ? M.drvmax[1] : 0;
            M.running[1] = 1;
        }
        return 0;
    }
    if (req == REQ_LIM) {
        M.ninit1++;
        memcpy(M.last_lim, w, 16);
        if (w[1])
            M.drvmax[0] = w[1];
        if (w[2])
            M.drvmax[1] = w[2];
        return 0;
    }
    if (req == REQ_IPOS) {
        if (++M.ninit2 == 1) {
            M.first_ipos[0] = w[0];
            M.first_ipos[1] = w[1];
        }
        memcpy(M.last_ipos, w, 40);
        if (w[0])
            M.cnt[0] = w[0];
        if (w[1])
            M.cnt[1] = w[1];
        return 0;
    }
    M.bad_cmd++;
    errno = ENOTTY;
    return -1;
}

static void s_sleep(unsigned ms)
{
    if (M.cancel_hp && M.cancel_at > 0 && --M.cancel_at == 0)
        hal_ptz_cancel(M.cancel_hp);
    advance(ms);
    pthread_mutex_lock(&M.gm);
    if (M.gate) {
        M.blocked = 1;
        pthread_cond_broadcast(&M.gc);
        while (M.gate)
            pthread_cond_wait(&M.gc, &M.gm);
        M.blocked = 0;
    }
    pthread_mutex_unlock(&M.gm);
}

static int file_idx(const char *path, int create)
{
    for (int i = 0; i < 6; i++)
        if (!strcmp(M.f[i].path, path))
            return i;
    if (create)
        for (int i = 0; i < 6; i++)
            if (!M.f[i].path[0]) {
                snprintf(M.f[i].path, sizeof M.f[i].path, "%s", path);
                return i;
            }
    return -1;
}

static int s_load(const char *path, char *buf, unsigned cap)
{
    int i, n = -1;
    pthread_mutex_lock(&M.gm);
    i = file_idx(path, 0);
    if (i < 0 || !M.f[i].present) {
        errno = ENOENT;
    } else {
        snprintf(buf, cap, "%s", M.f[i].data);
        n = (int)strlen(buf);
    }
    pthread_mutex_unlock(&M.gm);
    return n;
}

static int s_save(const char *path, const char *data)
{
    int i, rc = 0;
    pthread_mutex_lock(&M.gm);
    i = file_idx(path, 1);
    if (i < 0 || M.f[i].fail) {
        errno = EIO;
        rc = -1;
    } else {
        snprintf(M.f[i].data, sizeof M.f[i].data, "%s", data);
        M.f[i].present = 1;
        M.f[i].writes++;
    }
    pthread_mutex_unlock(&M.gm);
    return rc;
}

static void put_file(const char *path, const char *data)
{
    int i = file_idx(path, 1);
    snprintf(M.f[i].data, sizeof M.f[i].data, "%s", data);
    M.f[i].present = 1;
    M.f[i].writes = 0;
}

static const char *get_file(const char *path)
{
    int i = file_idx(path, 0);
    return i >= 0 && M.f[i].present ? M.f[i].data : "";
}

static unsigned writes_of(const char *path)
{
    int i = file_idx(path, 0);
    return i < 0 ? 0 : M.f[i].writes;
}

static void log_locked(const char *line)
{
    size_t n = strlen(M.log);
    snprintf(M.log + n, sizeof M.log - n, "%s\n", line);
}

static void s_log(const char *line)
{
    pthread_mutex_lock(&M.gm);
    log_locked(line);
    pthread_mutex_unlock(&M.gm);
}

void a2k_log(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s_log(line);
}

static const hal_ptz_ops_t OPS = { s_open, s_ioctl, s_sleep, s_load, s_save, s_log, NULL, NULL };

static hal_ptz_t *mk(const char *conf)
{
    char err[96] = "";
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_t *p = hal_ptz_new(&d, &OPS, conf, POSF, err, sizeof err);
    if (!p)
        printf("  (hal_ptz_new: %s)\n", err);
    return p;
}

static int near_mech(int pan, int tilt, int tol)
{
    return abs(M.mech[0] - pan) <= tol && abs(M.mech[1] - tilt) <= tol;
}

static int pos_is(hal_ptz_t *p, uint32_t pan, uint32_t tilt, uint32_t tol)
{
    uint32_t a, b;
    return hal_ptz_pos(p, &a, &b) && (a > pan ? a - pan : pan - a) <= tol && (b > tilt ? b - tilt : tilt - b) <= tol;
}

/* a booted camera: calibrated at home (counters 976 / 1001, frame file written), flushed */
static hal_ptz_t *booted(int pan, int tilt)
{
    hal_ptz_t *p;
    reset(pan, tilt);
    p = mk(NULL);
    if (!p || hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0)
        return NULL;
    hal_ptz_flush(p);
    return p;
}

/* ------------------------------------------------------------------------------------------------ hal_ptz */
static void test_desc(void)
{
    char err[96];
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_t *p;
    printf("== descriptor and ptz.conf\n");
    CHECK(!strcmp(d.dev, "/dev/tymotor") && d.dir[HAL_PTZ_UP] == 0 && d.dir[HAL_PTZ_RIGHT] == 2 && d.dir[HAL_PTZ_DOWN] == 4 && d.dir[HAL_PTZ_LEFT] == 6,
          "the 2K descriptor: /dev/tymotor, up 0, right 2, down 4, left 6");
    CHECK(d.pan_min == 300 && d.pan_max == 3650 && d.tilt_min == 300 && d.tilt_max == 2100 && d.pan_home == 1275 && d.tilt_home == 1300 &&
          d.pan_park == 1275 && d.tilt_park == 300 && d.pan_step_max == 4000 && d.tilt_step_max == 2160 && d.speed == 3,
          "measured defaults: pan 300..3650, tilt 300..2100, home 1275/1300, park 1275/300 (down), step_max 4000/2160, speed 3");
    {
        struct ptzs_conf c;
        ptzs_conf_defaults(&c);
        CHECK(c.pan_min == d.pan_min && c.pan_max == d.pan_max && c.tilt_min == d.tilt_min && c.tilt_max == d.tilt_max && c.pan_home == d.pan_home &&
              c.tilt_home == d.tilt_home && c.speed == d.speed, "the tool's defaults (akam_ptz_safe.h) are the descriptor's");
    }
    reset(0, 0);
    p = mk("pan_min=400\ntilt_max=1700\nspeed=4\n");
    CHECK(p && hal_ptz_desc(p)->pan_min == 400 && hal_ptz_desc(p)->tilt_max == 1700 && hal_ptz_desc(p)->speed == 4 && hal_ptz_desc(p)->pan_max == 3650,
          "ptz.conf overrides the descriptor, the rest stays");
    CHECK(hal_ptz_desc(p)->tilt_park == 300, "the default park (1275/300) is inside the soft range and stays");
    p = mk("tilt_min=400\n");
    CHECK(p && hal_ptz_desc(p)->tilt_park == 400, "a park outside the soft range is pulled in");
    CHECK(mk("pan_min=10\n") == NULL && mk("bogus=1\n") == NULL && mk("pan_min=400\npan_min=500\n") == NULL && mk("speed=1\n") == NULL,
          "a bad ptz.conf is refused (range margin, unknown key, repeated key, speed 1)");
    d.dir[0] = 9;
    CHECK(hal_ptz_new(&d, &OPS, NULL, POSF, err, sizeof err) == NULL, "a direction word above 7 is refused");
    {
        hal_ptz_desc_t dd = a2k_ptz_desc;
        dd.pan_park = 100;
        dd.tilt_park = 9999;
        p = hal_ptz_new(&dd, &OPS, NULL, POSF, err, sizeof err);
        CHECK(p && hal_ptz_desc(p)->pan_park == 300 && hal_ptz_desc(p)->tilt_park == 2100, "park pulled into the soft range");
    }
}

static void test_calibration(void)
{
    hal_ptz_t *p;
    unsigned w0;
    printf("== boot calibration (no saved position)\n");
    reset(2000, 700); /* the camera is somewhere, the driver is cold (0/0), no saved file */
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "start ok");
    CHECK(M.first_ipos[0] == 4000 && M.first_ipos[1] == 2160, "first init: the counters are declared at the stock maximum 4000/2160");
    CHECK(M.cmds[0] == 4 && M.cmds[1] == 4 && M.cmds[2] == 6, "tilt DOWN (4), tilt DOWN again (the real travel is longer than the declared maximum), then pan LEFT (6)");
    CHECK(M.bad_cmd == 0 && (M.dirs & ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6))) == 0, "only the four single-axis commands 0 2 4 6 are ever sent (never 9, 10, 12)");
    CHECK(M.ninit1 == 4 && M.ninit2 == 4, "four inits: stock frame, tilt re-declared, approach frame, final frame");
    CHECK(M.last_lim[1] == 3351 && M.last_lim[2] == 1801 && M.last_ipos[0] == 976 && M.last_ipos[1] == 1001,
          "final frame: driver limits = the soft range {3351,1801}, counters = home - (soft_min - 1) = 976/1001");
    CHECK(near_mech(1275, 1300, 12), "the camera is at home (mechanically)");
    CHECK(hal_ptz_state(p) == HAL_PTZ_HOME && pos_is(p, 1275, 1300, 0), "state home, position 1275/1300");
    CHECK(strstr(M.log, "calibrating") && strstr(M.log, "calibrated, at home"), "logged");
    CHECK(!strcmp(get_file("/tmp/akam_ptz.frame"), "pan_min=300 pan_max=3650 tilt_min=300 tilt_max=2100\n"), "the frame file is written");
    CHECK(!strcmp(get_file(POSF), "unknown\n") && hal_ptz_dirty(p), "the saved file says unknown until the flush");
    hal_ptz_flush(p);
    CHECK(!strcmp(get_file(POSF), "pan=1275 tilt=1300\n") && writes_of(POSF) == 2, "flush: the position, two writes in all (unknown, position)");
    w0 = writes_of(POSF);
    hal_ptz_flush(p);
    CHECK(writes_of(POSF) == w0, "flushing again writes nothing");
    {
        unsigned long g = M.grind;
        CHECK(hal_ptz_goto(p, 3000, 1000) == 0 && hal_ptz_home(p) == 0 && M.grind == g, "after the calibration no motion ever reaches a stop");
    }

    printf("== calibration variants\n");
    reset(300, 100);
    put_file(POSF, "unknown\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.nmove > 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "a saved 'unknown' (killed mid-move): calibrates");
    reset(3000, 1900);
    put_file(POSF, "pan=100 tilt=900\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.nmove > 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "a saved position outside the soft range: calibrates");
    reset(1000, 1000);
    put_file(POSF, "garbage\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.nmove > 0, "a corrupt saved file: calibrates");
    reset(1000, 1000);
    M.stuck[1] = 1;
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN && strstr(M.log, "FAILED"), "a dead tilt axis: the calibration fails, state unknown");
    CHECK(hal_ptz_goto(p, 1275, 1300) == HAL_PTZ_EREFUSED, "...and every motion is refused");
    reset(1000, 1000);
    M.open_fail = 1;
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && M.nmove == 0 && hal_ptz_step(p, HAL_PTZ_UP) == HAL_PTZ_EREFUSED, "the node is busy: no start, no motion");
}

static void test_skip_and_off(void)
{
    hal_ptz_t *p;
    printf("== cold boot with a saved position: ALWAYS calibrates (the head may have moved outside the app)\n");
    reset(2000, 900); /* a valid saved position, and the head really is there: still not trusted */
    put_file(POSF, "pan=2000 tilt=900\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "start ok");
    CHECK(M.ninit1 == 4 && M.nmove > 0 && M.last_lim[1] == 3351 && M.last_ipos[0] == 1275 - 299 && M.last_ipos[1] == 1300 - 299,
          "the calibration runs (4 inits), the final frame is declared at HOME, not at the saved position");
    CHECK(pos_is(p, 1275, 1300, 0) && hal_ptz_state(p) == HAL_PTZ_HOME && near_mech(1275, 1300, 15), "calibrated at home");
    CHECK(strstr(M.log, "saved position is not trusted") != NULL, "logged");
    reset(1275, 1750); /* the head was moved by hand while the saved file says home */
    put_file(POSF, "pan=1275 tilt=1300\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && near_mech(1275, 1300, 15),
          "a saved home with the head really at the top: calibration finds the truth, ends at home");
    reset(1275, 1750);
    put_file(POSF, "pan=1275 tilt=1750\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "a saved park position: calibrates and ends at home, not parked");

    printf("== ptz_cal=off\n");
    reset(2000, 900);
    put_file(POSF, "pan=2000 tilt=900\n");
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_OFF) == 0 && M.ninit1 == 0 && M.ninit2 == 0 && M.nmove == 0, "off: the driver is not initialised, nothing moves");
    CHECK(hal_ptz_state(p) == HAL_PTZ_UNKNOWN && !pos_is(p, 2000, 900, 0), "state unknown, no position");
    CHECK(hal_ptz_goto(p, 1275, 1300) == HAL_PTZ_EREFUSED && hal_ptz_step(p, HAL_PTZ_LEFT) == HAL_PTZ_EREFUSED && hal_ptz_park(p) == HAL_PTZ_EREFUSED && M.nmove == 0,
          "motion refused until an init (akam_ptz init)");

    printf("== process restart in the same boot: the driver holds our frame\n");
    p = booted(2500, 800);
    CHECK(p != NULL, "booted");
    hal_ptz_close(p);
    {
        int n1 = M.ninit1, nm = M.nmove;
        p = mk(NULL);
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.ninit1 == n1 && M.nmove == nm && pos_is(p, 1275, 1300, 15), "frame file + counters: adopted, no init, no calibration");
        p = mk("pan_min=400\n");
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.ninit1 > n1 + 1 && M.nmove > nm, "a ptz.conf that changed the frame: not adopted, calibrates");
    }
    printf("== a stall, or the tool, forbids the adoption\n");
    p = booted(1275, 1300);
    CHECK(p && hal_ptz_goto(p, 2000, 1300) == 0, "booted and moved");
    M.stuck[0] = 1; /* the pan axis stops counting */
    CHECK(hal_ptz_goto(p, 2600, 1300) == HAL_PTZ_ESTALL && hal_ptz_state(p) == HAL_PTZ_UNKNOWN, "stall: position no longer trusted");
    CHECK(!strcmp(get_file("/tmp/akam_ptz_app.frame"), "invalid\n") && !strcmp(get_file("/tmp/akam_ptz.frame"), "invalid\n"), "...both frame files are invalidated");
    hal_ptz_close(p);
    M.stuck[0] = 0;
    {
        int n1 = M.ninit1;
        p = mk(NULL);
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.ninit1 == n1 + 4 && hal_ptz_state(p) == HAL_PTZ_HOME && near_mech(1275, 1300, 15),
              "restart after a stall: NOT adopted, recalibrates");
    }
    reset(1275, 1300);
    p = booted(1275, 1300);
    hal_ptz_close(p);
    put_file("/tmp/akam_ptz_app.frame", "invalid\n"); /* what akam_ptz does on any motion or init */
    {
        int n1 = M.ninit1;
        p = mk(NULL);
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.ninit1 == n1 + 4, "restart after the tool invalidated the app frame: recalibrates");
        p = mk(NULL);
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.ninit1 == n1 + 4 && pos_is(p, 1275, 1300, 15), "...and the restart after that adopts again");
    }
}

static void test_motion(void)
{
    hal_ptz_t *p;
    unsigned long g;
    uint32_t a, b;
    printf("== park / home\n");
    p = booted(1275, 1300);
    CHECK(p && hal_ptz_park(p) == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && pos_is(p, 1275, 300, 4) && near_mech(1275, 300, 14), "park: pan home, tilt at the soft min 300 (down), state parked");
    CHECK(hal_ptz_home(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && pos_is(p, 1275, 1300, 4), "home: state home");
    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        char err[64];
        d.pan_park = 800;
        d.tilt_park = 900;
        reset(1275, 1300);
        p = hal_ptz_new(&d, &OPS, NULL, POSF, err, sizeof err);
        CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_park(p) == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && pos_is(p, 800, 900, 4), "a configured park position (800:900)");
    }
    printf("== soft limits\n");
    p = booted(1275, 1300);
    g = M.grind;
    CHECK(hal_ptz_goto(p, 1275, 2100) == 0, "at the top (2100)");
    {
        int nm = M.nmove;
        CHECK(hal_ptz_step(p, HAL_PTZ_UP) == HAL_PTZ_EREFUSED && M.nmove == nm, "up at the soft max: refused, nothing sent to the driver");
    }
    CHECK(hal_ptz_goto(p, 1275, 2000) == 0 && hal_ptz_step(p, HAL_PTZ_UP) == 0 && hal_ptz_pos(p, &a, &b) && b <= 2100 && b >= 2050, "a step up near the top is clipped at 2100");
    CHECK(hal_ptz_nudge(p, 100000, -100000) == 0 && hal_ptz_pos(p, &a, &b) && abs((int)a - 3650) <= 10 && abs((int)b - 300) <= 10 && a <= 3650 + 3 && b >= 300 - 3,
          "a huge nudge is clipped to the soft range corner (3650/300)");
    CHECK(hal_ptz_nudge(p, -100000, 100000) == 0 && hal_ptz_pos(p, &a, &b) && abs((int)a - 300) <= 10 && abs((int)b - 2100) <= 10, "...and the opposite corner");
    CHECK(hal_ptz_goto(p, 299, 1000) == HAL_PTZ_EREFUSED && hal_ptz_goto(p, 3651, 1000) == HAL_PTZ_EREFUSED && hal_ptz_goto(p, 1000, 299) == HAL_PTZ_EREFUSED &&
          hal_ptz_goto(p, 1000, 2101) == HAL_PTZ_EREFUSED, "goto outside the soft range is refused, not clipped");
    CHECK(hal_ptz_goto(p, 1275, 1300) == 0 && hal_ptz_nudge(p, 120, -80) == 0 && pos_is(p, 1395, 1220, 5), "a small nudge is exact to a few steps");
    CHECK(hal_ptz_step(p, HAL_PTZ_LEFT) == 0 && pos_is(p, 1195, 1220, 8) && hal_ptz_step(p, HAL_PTZ_DOWN) == 0 && pos_is(p, 1195, 1020, 8) &&
          hal_ptz_step(p, HAL_PTZ_RIGHT) == 0 && pos_is(p, 1395, 1020, 8) && hal_ptz_step(p, HAL_PTZ_UP) == 0 && pos_is(p, 1395, 1220, 8), "one button step = about 200 steps in each direction");
    CHECK(M.grind == g && M.bad_cmd == 0, "no step was ever made against a mechanical stop, no forbidden command word");

    printf("== stall\n");
    p = booted(1275, 1300);
    M.stuck[0] = 1;
    CHECK(hal_ptz_step(p, HAL_PTZ_RIGHT) == HAL_PTZ_ESTALL && M.running[0] == 0, "a stuck axis: stall, motor stopped");
    CHECK(hal_ptz_state(p) == HAL_PTZ_UNKNOWN && hal_ptz_step(p, HAL_PTZ_LEFT) == HAL_PTZ_EREFUSED, "state unknown, further motion refused");
    hal_ptz_flush(p);
    CHECK(!strcmp(get_file(POSF), "unknown\n"), "the saved position stays 'unknown': the next boot calibrates");

    printf("== cancel\n");
    p = booted(1275, 1300);
    M.cancel_hp = p;
    M.cancel_at = 6; /* about 300 ms into the goto */
    CHECK(hal_ptz_goto(p, 3500, 1300) == HAL_PTZ_ECANCEL, "cancel: the goto returns ECANCEL");
    CHECK(M.running[0] == 0 && M.mech[0] > 1275 && M.mech[0] < 1600 && hal_ptz_state(p) != HAL_PTZ_UNKNOWN && pos_is(p, (uint32_t)M.mech[0], 1300, 12),
          "motor stopped within a poll, position known and right (trusted)");
    M.cancel_hp = NULL;
    CHECK(hal_ptz_home(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "a command after a cancel works");
    {
        int nm;
        hal_ptz_cancel(p); /* idle cancel, then the caller takes a command: the old cancel is dropped */
        hal_ptz_mark(p);
        nm = M.nmove;
        CHECK(hal_ptz_goto(p, 1500, 1300) == 0 && M.nmove > nm, "a cancel BEFORE the dequeue mark is dropped (nothing was running)");
        hal_ptz_mark(p); /* dequeued ... */
        hal_ptz_cancel(p); /* ... a cancel arrives before begin() */
        nm = M.nmove;
        CHECK(hal_ptz_goto(p, 1275, 1300) == HAL_PTZ_ECANCEL && M.nmove == nm, "a cancel between dequeue and begin() is not lost: ECANCEL, no motion");
        CHECK(hal_ptz_state(p) == HAL_PTZ_HOME || hal_ptz_state(p) == HAL_PTZ_MOVED, "...and the camera stays controllable");
        CHECK(hal_ptz_goto(p, 1275, 1300) == 0, "the next command (no mark) runs normally");
    }
}

static void test_persistence(void)
{
    hal_ptz_t *p;
    unsigned w;
    printf("== saved position: atomic and few writes\n");
    p = booted(1275, 1300);
    w = writes_of(POSF);
    for (int i = 0; i < 10; i++)
        (void)hal_ptz_step(p, i & 1 ? HAL_PTZ_LEFT : HAL_PTZ_RIGHT);
    CHECK(writes_of(POSF) == w + 1 && !strcmp(get_file(POSF), "unknown\n"), "ten moves in a burst: ONE write ('unknown' once), none in between");
    hal_ptz_flush(p);
    CHECK(writes_of(POSF) == w + 2 && !strncmp(get_file(POSF), "pan=", 4), "the flush writes the final position once");
    M.f[file_idx(POSF, 0)].fail = 1;
    (void)hal_ptz_step(p, HAL_PTZ_RIGHT);
    CHECK(hal_ptz_state(p) != HAL_PTZ_UNKNOWN, "a failing 'unknown' write on a stale position refuses the move");
    M.f[file_idx(POSF, 0)].fail = 0;
    reset(1275, 1300);
    M.f[file_idx(POSF, 1)].fail = 1; /* the directory does not exist: no saved file at all */
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && hal_ptz_step(p, HAL_PTZ_RIGHT) == 0 && strstr(M.log, "cannot save the position"),
          "no writable /config/akam: still calibrates and moves, warns once");
}

static void test_real_files(void)
{
    char dir[] = "/tmp/rot2k_hal_ptz_XXXXXX", path[160], buf[64], cmd[200];
    const hal_ptz_ops_t *o;
    printf("== the Linux driver: atomic file replace\n");
    if (!mkdtemp(dir)) {
        printf("  FAIL - mkdtemp\n");
        fails++;
        return;
    }
    o = hal_ptz_linux_ops("/dev/null-not-opened");
    snprintf(path, sizeof path, "%s/ptz.pos", dir);
    CHECK(o->save_file(path, "pan=1 tilt=2\n") == 0 && o->load_file(path, buf, sizeof buf) == 13 && !strcmp(buf, "pan=1 tilt=2\n"), "save then load");
    CHECK(o->save_file(path, "unknown\n") == 0 && o->load_file(path, buf, sizeof buf) == 8 && !strcmp(buf, "unknown\n"), "replace");
    snprintf(cmd, sizeof cmd, "ls %s | wc -l | grep -qx 1", dir);
    CHECK(system(cmd) == 0, "no temp file left behind");
    snprintf(path, sizeof path, "%s/nodir/ptz.pos", dir);
    errno = 0;
    CHECK(o->save_file(path, "x\n") == -1 && o->load_file(path, buf, sizeof buf) == -1 && errno == ENOENT, "a missing directory: save fails, load says ENOENT (nothing is created)");
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd)) { }
}

/* ------------------------------------------------------------------------------------------------ the controller thread */
static a2k_ptz_info_t g_notified;
static int g_nnotify;
static void on_notify(const a2k_ptz_info_t *i)
{
    pthread_mutex_lock(&M.gm);
    g_notified = *i;
    g_nnotify++;
    pthread_mutex_unlock(&M.gm);
}

static int wait_state(int st, int ms)
{
    a2k_ptz_info_t i;
    for (int t = 0; t < ms; t += 5) {
        a2k_ptz_info(&i);
        if (i.state == st)
            return 1;
        usleep(5000);
    }
    return 0;
}

static void set_gate(int on)
{
    pthread_mutex_lock(&M.gm);
    M.gate = on;
    pthread_cond_broadcast(&M.gc);
    pthread_mutex_unlock(&M.gm);
}

static int wait_blocked(void)
{
    int ok = 0;
    pthread_mutex_lock(&M.gm);
    for (int t = 0; t < 400 && !M.blocked; t++) {
        pthread_mutex_unlock(&M.gm);
        usleep(5000);
        pthread_mutex_lock(&M.gm);
    }
    ok = M.blocked;
    pthread_mutex_unlock(&M.gm);
    return ok;
}

static void test_thread(void)
{
    a2k_ptz_cfg_t c;
    a2k_ptz_info_t i;
    printf("== controller thread (akam2k_ptz.c)\n");
    CHECK(a2k_ptz_parse_cal("auto") == HAL_PTZ_CAL_AUTO && a2k_ptz_parse_cal("off") == HAL_PTZ_CAL_OFF && a2k_ptz_parse_cal("on") < 0 && a2k_ptz_parse_cal("") < 0,
          "ptz_cal parses auto|off only");
    {
        int pan = -1, tilt = -1;
        CHECK(a2k_ptz_parse_park("1275:1750", &pan, &tilt) == 0 && pan == 1275 && tilt == 1750 && a2k_ptz_parse_park("0:0", &pan, &tilt) == 0 &&
              a2k_ptz_parse_park("1275", &pan, &tilt) < 0 && a2k_ptz_parse_park("1:2:3", &pan, &tilt) < 0 && a2k_ptz_parse_park(":5", &pan, &tilt) < 0 &&
              a2k_ptz_parse_park("5:", &pan, &tilt) < 0 && a2k_ptz_parse_park("a:5", &pan, &tilt) < 0 && a2k_ptz_parse_park("-1:5", &pan, &tilt) < 0 &&
              a2k_ptz_parse_park("70000:5", &pan, &tilt) < 0 && a2k_ptz_parse_park("5:5 ", &pan, &tilt) < 0, "ptz_park parses PAN:TILT, digits only");
    }
    CHECK(!strcmp(a2k_ptz_state_name(HAL_PTZ_HOME), "home") && !strcmp(a2k_ptz_state_name(HAL_PTZ_PARKED), "parked") && !strcmp(a2k_ptz_state_name(HAL_PTZ_MOVED), "moved") &&
          !strcmp(a2k_ptz_state_name(HAL_PTZ_CALIBRATING), "calibrating") && !strcmp(a2k_ptz_state_name(HAL_PTZ_UNKNOWN), "unknown"), "state names");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == -1 && a2k_ptz_nudge(1, 1) == -1 && a2k_ptz_toggle_park() == -1 && !a2k_ptz_privacy_active(), "not started: commands refused");

    /* calibration in progress: commands refused */
    reset(2000, 700);
    set_gate(1);
    a2k_ptz_defaults(&c);
    c.privacy = 1;
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf", POSF, 60);
    a2k_ptz_set_notify(on_notify);
    CHECK(a2k_ptz_start(&c) == 0, "start");
    CHECK(wait_blocked(), "the thread is inside the calibration");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_CALIBRATING && a2k_ptz_command(A2K_PTZ_PARK) == -1 && a2k_ptz_nudge(5, 5) == -1 && a2k_ptz_toggle_park() == -1,
          "state calibrating, every command refused meanwhile");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_HOME, 5000) && near_mech(1275, 1300, 12), "calibrated: state home");
    a2k_ptz_info(&i);
    CHECK(i.known && i.pan == 1275 && i.tilt == 1300 && !a2k_ptz_privacy_active(), "position published, privacy not active at home");
    {
        int ok = 0;
        for (int t = 0; t < 400 && !ok; t++) {
            pthread_mutex_lock(&M.gm);
            ok = !strcmp(get_file(POSF), "pan=1275 tilt=1300\n");
            pthread_mutex_unlock(&M.gm);
            usleep(5000);
        }
        CHECK(ok, "the position is written after the motor has been quiet (flush delay)");
    }

    /* park / home toggle and privacy */
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_privacy_active(), "toggle at home: parked, privacy active (video withheld)");
    a2k_ptz_info(&i);
    CHECK(i.pan >= 1270 && i.pan <= 1280 && i.tilt >= 295 && i.tilt <= 310, "parked at pan home, tilt 300 (down)");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_HOME, 5000) && !a2k_ptz_privacy_active(), "toggle when parked: home, privacy off");
    CHECK(g_nnotify >= 6 && !strcmp(a2k_ptz_state_name(g_notified.state), "home"), "the state changes were notified");

    /* a nudge while moving is queued and summed; a new command cancels */
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_blocked(), "a park motion is running (held)");
    CHECK(a2k_ptz_nudge(10, 0) == 0 && a2k_ptz_nudge(10, 0) == 0 && a2k_ptz_nudge(10, 0) == 0, "three nudges queued meanwhile");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_MOVED, 5000), "after the park: the nudge ran (30 steps right of the park: moved)");
    {
        int ok = 0;
        for (int t = 0; t < 600 && !ok; t++) {
            a2k_ptz_info(&i);
            ok = i.state == HAL_PTZ_MOVED && i.known && i.pan >= 1300 && i.pan <= 1310;
            usleep(5000);
        }
        CHECK(ok, "the three nudges were added up: pan 1275 + 30");
    }
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_blocked(), "a home motion is running (held)");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0, "a new command while moving");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_privacy_active(), "it cancelled the running motion and replaced the pending one: parked");
    CHECK(a2k_ptz_command(99) == -1 && a2k_ptz_command(-1) == -1, "an unknown command is refused");
    CHECK(a2k_ptz_command(A2K_PTZ_DOWN) == 0 && wait_state(HAL_PTZ_PARKED, 2000), "a button step at the bottom is refused by the soft limit, still parked");
    a2k_ptz_stop();
    CHECK(!a2k_ptz_privacy_active() && !strncmp(get_file(POSF), "pan=1275 tilt=", 14) && atoi(get_file(POSF) + 14) <= 310, "stop: motor stopped, the position written");
    CHECK(M.bad_cmd == 0, "no forbidden driver command word during the whole thread test");
    a2k_ptz_set_notify(NULL);

    printf("== controller thread: privacy off, cal off\n");
    reset(1275, 1300);
    put_file(POSF, "pan=1275 tilt=1750\n");
    a2k_ptz_defaults(&c);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 60000) && !a2k_ptz_privacy_active(), "saved park position, privacy 0: a cold boot calibrates to home, the video is NOT withheld");
    CHECK(M.nmove > 0, "...it did calibrate");
    a2k_ptz_stop();
    reset(1275, 1300);
    c.cal = HAL_PTZ_CAL_OFF;
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_UNKNOWN, 2000), "cal off: state unknown");
    usleep(50000);
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0, "the command is queued...");
    usleep(100000);
    CHECK(M.nmove == 0 && M.ninit1 == 0, "...and refused by the engine: no motion, no init");
    a2k_ptz_stop();
    reset(1275, 1300);
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf", POSF, 60);
    put_file("/config/akam/ptz.conf", "pan_min=5\n");
    a2k_ptz_defaults(&c);
    CHECK(a2k_ptz_start(&c) == -1 && M.ninit1 == 0 && M.nmove == 0 && strstr(M.log, "bad /config/akam/ptz.conf"), "a bad ptz.conf: PTZ off, the motor is never touched");
    c.park_pan = 1000;
    c.park_tilt = 1500;
    put_file("/config/akam/ptz.conf", "pan_min=400\n");
    put_file(POSF, "pan=1275 tilt=1300\n");
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 5000) && a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_state(HAL_PTZ_PARKED, 5000), "ptz.conf and ptz_park honoured");
    {
        a2k_ptz_info_t j;
        a2k_ptz_info(&j);
        CHECK(j.pan >= 995 && j.pan <= 1005 && j.tilt >= 1495 && j.tilt <= 1505 && M.last_lim[1] == 3650 - 399, "park 1000:1500, soft pan_min 400 (driver limit 3251)");
    }
    a2k_ptz_stop();
}

/* ------------------------------------------------------------------------------------------------ scene check wiring */
static volatile int sc_verdict, sc_nboot, sc_ncap, sc_nsave, sc_ndrop, sc_cap_rc, sc_commit_rc, sc_drop_rc, sc_last_fresh, sc_ncommit;
static volatile unsigned sc_pan, sc_tilt, sc_save_pan, sc_save_tilt;
static volatile int sc_nmove_at_boot = -1;
static int sc_boot(uint32_t *pan, uint32_t *tilt, int (*cancelled)(void))
{
    (void)cancelled;
    sc_nboot++;
    sc_nmove_at_boot = M.nmove; /* no motor command may have been sent before the check */
    *pan = sc_pan;
    *tilt = sc_tilt;
    return sc_verdict;
}
static int sc_capture(void)
{
    sc_ncap++;
    return sc_cap_rc;
}
static int sc_commit(uint32_t pan, uint32_t tilt, int fresh)
{
    sc_ncommit++;
    sc_save_pan = pan;
    sc_save_tilt = tilt;
    sc_last_fresh = fresh;
    if (sc_commit_rc == 0)
        sc_nsave++;
    return sc_commit_rc;
}
static int sc_drop(void) { sc_ndrop++; return sc_drop_rc; }
static const a2k_ptz_scene_t SCENE_HOOKS = { sc_boot, sc_capture, sc_commit, sc_drop };
static int hk_check(uint32_t *pan, uint32_t *tilt) { sc_nboot++; sc_nmove_at_boot = M.nmove; *pan = sc_pan; *tilt = sc_tilt; return sc_verdict; }
static int hk_drop(void) { sc_ndrop++; return sc_drop_rc; }
static hal_ptz_t *g_cancel_hp;
static int hk_drop_cancel(void) { sc_ndrop++; if (g_cancel_hp) hal_ptz_cancel(g_cancel_hp); return 0; }

static void sc_reset(int verdict, unsigned pan, unsigned tilt)
{
    sc_verdict = verdict;
    sc_pan = pan;
    sc_tilt = tilt;
    sc_nboot = sc_ncap = sc_nsave = sc_ndrop = sc_cap_rc = sc_commit_rc = sc_drop_rc = sc_ncommit = 0;
    sc_last_fresh = -1;
    sc_nmove_at_boot = -1;
}

static int wait_count(volatile int *v, int n, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        if (*v >= n)
            return 1;
        usleep(5000);
    }
    return 0;
}

static void test_scene(void)
{
    hal_ptz_ops_t ops = OPS;
    hal_ptz_t *p;
    a2k_ptz_cfg_t c;
    a2k_ptz_info_t i;
    char err[64];
    uint32_t a, b;
    printf("== scene check: hal_ptz boot path\n");
    ops.cold_check = hk_check;
    ops.scene_drop = hk_drop;
    sc_reset(1, 1500, 900);
    reset(1500, 900); /* the head is where the saved reference says */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "match: start ok");
    CHECK(sc_nboot == 1 && sc_nmove_at_boot == 0 && M.nmove == 0 && M.grind == 0 && sc_ndrop == 0, "match: checked before any motor command, no motion, no calibration, nothing dropped");
    CHECK(hal_ptz_pos(p, &a, &b) && a == 1500 && b == 900 && hal_ptz_state(p) == HAL_PTZ_MOVED, "match: the saved position is declared (trusted, not home)");
    CHECK(hal_ptz_verify_pos(p) == 0, "the counters agree with the declared position");
    CHECK(hal_ptz_home(p) == 0 && near_mech(1275, 1300, 12), "match: the declared frame is right, home reaches the real home");
    CHECK(M.bad_cmd == 0 && strstr(M.log, "scene check passed"), "match: logged, only commands 0..7");
    hal_ptz_close(p);
    hal_ptz_free(p);

    sc_reset(0, 1500, 900);
    reset(2500, 700); /* the head was turned while off */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && M.grind > 0, "mismatch: the long calibration runs as before");
    CHECK(sc_nboot == 1 && sc_nmove_at_boot == 0 && sc_ndrop == 1, "mismatch: checked first, the reference dropped before the calibration");
    hal_ptz_close(p);
    hal_ptz_free(p);

    sc_reset(1, 100, 900); /* a match whose position is outside the soft range is not believed */
    reset(1500, 900);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && M.grind > 0, "a saved position outside the soft range: calibrates");
    hal_ptz_close(p);
    hal_ptz_free(p);

    sc_reset(1, 1275, 300); /* the saved position is the park position: never declared */
    reset(1275, 300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && sc_nboot == 1 && M.grind > 0 && near_mech(1275, 1300, 12), "a saved position at the PARK position: the scene check is skipped, full calibration");
    hal_ptz_close(p);
    hal_ptz_free(p);
    sc_reset(1, 1275 + 14, 300 + 14); /* within the state tolerance of the park */
    reset(1289, 314);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0, "...also within the park tolerance (15 steps)");
    hal_ptz_close(p);
    hal_ptz_free(p);

    sc_reset(1, 1500, 900);
    reset(1500, 900);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_OFF) == 0 && sc_nboot == 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN && M.nmove == 0, "cal off: no check, the motor untouched");
    CHECK(hal_ptz_verify_pos(p) == -1, "an untrusted position cannot be verified");
    CHECK(hal_ptz_recalibrate(p) == 0 && near_mech(1275, 1300, 12) && sc_ndrop == 1 && hal_ptz_state(p) == HAL_PTZ_HOME, "recalibrate: the long calibration now, the reference dropped, at home");
    {
        unsigned long g = M.grind;
        M.cnt[0] += 60; /* the driver counters moved (a lost step burst, a hand): the cached position is no longer the real one */
        CHECK(hal_ptz_verify_pos(p) == -1, "counters differing from the cached position: verify refuses");
        M.cnt[0] -= 60;
        CHECK(hal_ptz_verify_pos(p) == 0 && M.grind == g, "...and agrees again");
    }
    /* a cancel that arrived between the dequeue (mark) and the recalibration is not lost (L3) */
    {
        int nm = M.nmove, ni = M.ninit1;
        hal_ptz_mark(p);
        hal_ptz_cancel(p);
        CHECK(hal_ptz_recalibrate(p) == HAL_PTZ_ECANCEL && M.nmove == nm && M.ninit1 == ni, "recalibrate: a cancel between the dequeue and the start is honoured, nothing is moved");
        CHECK(hal_ptz_recalibrate(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "...and the next one runs");
    }
    hal_ptz_close(p);
    hal_ptz_free(p);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_recalibrate(p) == HAL_PTZ_EREFUSED, "recalibrate with the motor not open: refused");
    hal_ptz_free(p);

    /* L3 residual: a shutdown cancel that arrives while the reference is being invalidated must not be wiped */
    {
        hal_ptz_ops_t o3 = OPS;
        o3.scene_drop = hk_drop_cancel;
        sc_reset(0, 1500, 900);
        reset(1500, 900);
        p = hal_ptz_new(&a2k_ptz_desc, &o3, NULL, POSF, err, sizeof err);
        g_cancel_hp = p;
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_OFF) == 0, "motor open, cal off");
        {
            int nm = M.nmove;
            int rc = hal_ptz_recalibrate(p);
            CHECK(rc != 0 && hal_ptz_state(p) != HAL_PTZ_HOME && hal_ptz_state(p) != HAL_PTZ_CALIBRATING, "a cancel during the drop aborts the recalibration (not wiped): position unknown");
            CHECK(M.nmove == nm && !near_mech(1275, 1300, 12), "...the head was not walked to home");
        }
        g_cancel_hp = NULL;
        hal_ptz_close(p);
        hal_ptz_free(p);
    }

    /* L2: the reference cannot be invalidated: fail closed */
    sc_reset(0, 1500, 900);
    sc_drop_rc = -1;
    reset(2500, 700);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, POSF, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && M.nmove == 0 && M.grind == 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN && strstr(M.log, "cannot be invalidated"),
          "drop fails at a cold boot: no calibration, the motor untouched, position unknown");
    hal_ptz_free(p);

    printf("== calibration: the second tilt sweep\n");
    {
        hal_ptz_ops_t o2 = OPS;
        reset(1900, 2300); /* the head at the top stop: the real travel (about 2300) is longer than the declared maximum 2160 */
        p = hal_ptz_new(&a2k_ptz_desc, &o2, NULL, POSF, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12), "from the top stop (tilt 2300): calibrated, the head really is at home (the bottom stop was reached)");
        CHECK(M.ninit1 == 4 && M.ninit2 == 4 && M.cmds[0] == 4 && M.cmds[1] == 4 && M.cmds[2] == 6, "four inits (stock, tilt re-declared, approach, final) and sweeps tilt down, tilt down again, pan left");
        CHECK(M.bad_cmd == 0, "only commands 0..7");
        hal_ptz_close(p);
        hal_ptz_free(p);
    }

    printf("== scene check: controller (hooks, capture, confirm, drop, recalibrate)\n");
    a2k_ptz_defaults(&c);
    CHECK(c.scene_check == 1, "ptz_scene_check defaults to 1");
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf", POSF, 60);
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    a2k_ptz_set_scene(&SCENE_HOOKS);
    /* match: no motion, no save (the reference on disk is still good) */
    sc_reset(1, 1500, 900);
    reset(1500, 900);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_MOVED, 5000), "controller: boot with a matching scene");
    usleep(400000);
    a2k_ptz_info(&i);
    CHECK(M.nmove == 0 && i.known && i.pan == 1500 && i.tilt == 900 && sc_nsave == 0 && sc_ncap == 0 && sc_ndrop == 0, "no motion, position 1500/900 published, no new reference");
    /* a command: the reference is dropped at its start, captured when quiet, written after the confirmation */
    CHECK(a2k_ptz_nudge(100, 0) == 0 && wait_count(&sc_ndrop, 1, 3000), "a nudge: the reference is dropped when the motion starts");
    CHECK(wait_count(&sc_nsave, 1, 3000) && sc_ncap == 1 && sc_save_pan >= 1595 && sc_save_pan <= 1605 && sc_save_tilt >= 895 && sc_save_tilt <= 905,
          "quiet again: candidate captured, then the position written with it");
    CHECK(sc_last_fresh == 0, "...not marked fresh (no calibration since the last write)");
    /* M1: a motion during the confirmation window: nothing is written for the old candidate */
    {
        int ns = sc_nsave, nc = sc_ncap;
        CHECK(a2k_ptz_nudge(100, 0) == 0 && wait_count(&sc_ncap, nc + 1, 3000), "another nudge: a new candidate");
        CHECK(a2k_ptz_nudge(-60, 0) == 0, "a motion inside the confirmation window");
        CHECK(wait_count(&sc_nsave, ns + 1, 3000) && sc_save_pan >= 1635 && sc_save_pan <= 1645, "the candidate of the old position is dropped; the write carries the NEW position (recaptured after the motion)");
    }
    /* M1: the view changed during the window: the scene module refuses the commit; nothing counts, retried then given up */
    {
        int ns = sc_nsave, nc = sc_ncommit;
        sc_commit_rc = -1;
        CHECK(a2k_ptz_nudge(40, 0) == 0 && wait_count(&sc_ncommit, nc + 1, 3000), "view changed during the window: commit attempted and refused");
        usleep(800000);
        CHECK(sc_nsave == ns && sc_ncommit - nc <= 3, "...no reference written, at most 3 attempts (no endless loop)");
        sc_commit_rc = 0;
    }
    /* M1: the driver counters moved after the capture: the thread re-reads them and refuses to write */
    {
        int ns = sc_nsave, nc = sc_ncap, ncm = sc_ncommit;
        CHECK(a2k_ptz_nudge(30, 0) == 0 && wait_count(&sc_ncap, nc + 1, 3000), "a nudge, the candidate captured");
        pthread_mutex_lock(&M.gm);
        M.cnt[1] += 80; /* the counters no longer agree with the cached position */
        pthread_mutex_unlock(&M.gm);
        usleep(600000);
        CHECK(sc_nsave == ns && sc_ncommit == ncm, "counters differ from the position: the commit is not even attempted, nothing written");
        pthread_mutex_lock(&M.gm);
        M.cnt[1] -= 80;
        pthread_mutex_unlock(&M.gm);
    }
    /* recalibrate button */
    {
        int g0 = (int)M.grind, ns = sc_nsave;
        CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0, "Recalibrate PTZ: queued");
        CHECK(wait_state(HAL_PTZ_HOME, 20000) && near_mech(1275, 1300, 12) && (int)M.grind > g0, "...the long calibration ran, at home");
        CHECK(wait_count(&sc_nsave, ns + 1, 4000) && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1, "...and saved at home once quiet, marked fresh (the matched-boots counter restarts)");
    }
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0 && wait_blocked(), "recalibration held in its sweep");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_CALIBRATING && a2k_ptz_command(A2K_PTZ_PARK) == -1 && a2k_ptz_nudge(5, 5) == -1 && a2k_ptz_command(A2K_PTZ_RECAL) == -1,
          "calibrating: every command (and a second recalibration) refused, none cancels it");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_HOME, 20000), "it completes");
    a2k_ptz_stop();
    CHECK(M.bad_cmd == 0, "only commands 0..7");

    /* an app restart in the same boot (adoption, no calibration): the reference is written NOT fresh (the on-disk count is kept) */
    sc_reset(0, 1500, 900);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&sc_nsave, 1, 4000) && sc_last_fresh == 1, "boot with a calibration: written fresh");
    a2k_ptz_stop();
    {
        int g0 = (int)M.grind;
        sc_reset(0, 1500, 900); /* the driver keeps its frame: the next start adopts it */
        CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 5000) && (int)M.grind == g0 && sc_nboot == 0, "restart in the same boot: adopted, no calibration");
        CHECK(a2k_ptz_nudge(60, 0) == 0 && wait_count(&sc_nsave, 1, 4000) && sc_last_fresh == 0, "after an adoption the reference is written NOT fresh (the matched-boots count is kept)");
        a2k_ptz_stop();
    }
    /* L2 at the controller: the invalidation fails, the motion is refused and nothing moves */
    sc_reset(1, 1500, 900);
    reset(1500, 900);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_MOVED, 5000), "boot (match) for the fail-closed test");
    sc_drop_rc = -1;
    {
        int nm = M.nmove;
        CHECK(a2k_ptz_nudge(100, 0) == 0, "a nudge is queued");
        usleep(400000);
        a2k_ptz_info(&i);
        CHECK(M.nmove == nm && i.known && i.pan == 1500 && sc_ndrop >= 1, "the reference cannot be invalidated: the motion is refused, the head did not move");
    }
    sc_drop_rc = 0;
    CHECK(a2k_ptz_nudge(100, 0) == 0 && wait_count(&sc_ndrop, 2, 3000), "when the invalidation works again the motion runs");
    a2k_ptz_stop();

    /* mismatch at boot: calibrate, then a reference is captured and written, fresh */
    sc_reset(0, 1500, 900);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && near_mech(1275, 1300, 12) && sc_nboot == 1 && sc_nmove_at_boot == 0, "mismatch: checked first, then calibrated");
    CHECK(sc_ndrop >= 1 && wait_count(&sc_nsave, 1, 4000) && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1, "...and the calibrated position gets a new reference (fresh)");
    a2k_ptz_stop();
    /* a view with too little detail: nothing to write, no write, no retry */
    sc_reset(0, 1500, 900);
    sc_cap_rc = 1;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start (low detail view)");
    usleep(600000);
    CHECK(sc_ncap == 1 && sc_ncommit == 0 && sc_nsave == 0, "low detail: captured once, no commit, no write");
    a2k_ptz_stop();
    /* a failing capture (no fresh frame): retried a few times, then given up */
    sc_reset(0, 1500, 900);
    sc_cap_rc = -1;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start (capture failing)");
    usleep(900000);
    CHECK(sc_nsave == 0 && sc_ncap == 3, "no reference when the capture fails (3 attempts, no endless loop)");
    a2k_ptz_stop();
    /* shutdown commit: the candidate is settled (> 3 s old), the write waits for the confirmation delay (long) */
    a2k_ptz_test_set_scene_timing(60, 60000, 0);
    sc_reset(0, 1500, 900);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start for the shutdown commit");
    usleep(3400000);
    CHECK(sc_ncap == 1 && sc_nsave == 0, "candidate captured, the confirmation is still far away (60 s)");
    a2k_ptz_stop();
    CHECK(sc_nsave == 1 && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1, "app shutdown: the reference is written (on the PTZ thread) before it ends");
    sc_reset(0, 1500, 900);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start again");
    a2k_ptz_stop();
    CHECK(sc_nsave == 0 && sc_ncap == 0, "a stop right after the calibration (no settled candidate): nothing written");
    /* shutdown with the counters moved: not written */
    sc_reset(0, 1500, 900);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start for the shutdown / counters test");
    usleep(3400000);
    pthread_mutex_lock(&M.gm);
    M.cnt[0] += 90;
    pthread_mutex_unlock(&M.gm);
    a2k_ptz_stop();
    CHECK(sc_nsave == 0 && sc_ncommit == 0, "shutdown with the counters off the cached position: nothing written");
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    /* ptz_scene_check = 0: no check, no capture, the drop still protects an old file */
    c.scene_check = 0;
    sc_reset(1, 1500, 900);
    reset(1500, 900);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && sc_nboot == 0 && M.grind > 0, "ptz_scene_check=0: always calibrates, the check is not even run");
    usleep(500000);
    CHECK(sc_nsave == 0 && sc_ncap == 0 && sc_ndrop >= 1, "...no reference is written, a stale one is dropped");
    a2k_ptz_stop();
    c.scene_check = 1;
    /* ptz_cal=off: the recalibrate button never touches the motor */
    c.cal = HAL_PTZ_CAL_OFF;
    sc_reset(1, 1500, 900);
    reset(1500, 900);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_UNKNOWN, 2000) && sc_nboot == 0 && a2k_ptz_command(A2K_PTZ_RECAL) == -1 && M.nmove == 0, "ptz_cal=off: no check, Recalibrate refused");
    a2k_ptz_stop();
    a2k_ptz_set_scene(NULL);
    a2k_ptz_test_set_scene_timing(0, 0, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_desc();
    test_calibration();
    test_skip_and_off();
    test_motion();
    test_persistence();
    test_real_files();
    test_thread();
    test_scene();
    printf("rot2k hal_ptz unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
