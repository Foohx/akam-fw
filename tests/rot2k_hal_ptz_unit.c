/* Host unit test of hal_ptz (app/hal_ptz.c + the safe-motion layer ptz/akam_ptz_safe.h) and of the PTZ controller thread
 * (app/ptz.c) against a simulated motor: a step-counting driver (commands 0..7, stop, counter reads, the two init requests)
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
#include "../firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/hal.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/patrol.h"
#include "../firmware/lsc-rotatable-2k-ak3918av130/ptz.h"
#include "../src/hal/hal_ptz.h"

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
    struct { char path[96], data[128]; int present, fail; unsigned writes; } f[12];
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

static int file_idx(const char *path, int create);
static int file_writes(const char *path);
static int file_idx(const char *path, int create)
{
    for (int i = 0; i < 12; i++)
        if (!strcmp(M.f[i].path, path))
            return i;
    if (create)
        for (int i = 0; i < 12; i++)
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

static int file_writes(const char *path)
{
    int i, n = 0;
    pthread_mutex_lock(&M.gm);
    i = file_idx(path, 0);
    if (i >= 0)
        n = M.f[i].writes;
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

static const hal_ptz_ops_t OPS = { s_open, s_ioctl, s_sleep, s_load, s_save, s_log, NULL, NULL, NULL, NULL, NULL };

static hal_ptz_t *mk(const char *conf)
{
    char err[96] = "";
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_t *p = hal_ptz_new(&d, &OPS, conf, err, sizeof err);
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
          d.pan_park == 1275 && d.tilt_park == 80 && d.pan_step_max == 4000 && d.tilt_step_max == 2160 && d.speed == 3,
          "measured defaults: pan 300..3650, tilt 300..2100, home 1275/1300, park 1275/80 (the closed pose, below the soft range), step_max 4000/2160, speed 3");
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
    CHECK(hal_ptz_desc(p)->tilt_park == 80, "the default park (1275/80, the closed pose) lies below the soft range and stays (only a park may)");
    p = mk("tilt_min=400\n");
    CHECK(p && hal_ptz_desc(p)->tilt_park == 80, "a park below the soft range stays whatever the soft minimum is");
    {
        hal_ptz_desc_t dd = a2k_ptz_desc;
        dd.tilt_park = 10;
        reset(0, 0);
        p = hal_ptz_new(&dd, &OPS, NULL, err, sizeof err);
        CHECK(p && hal_ptz_desc(p)->tilt_park == 50, "a park closer than 50 steps to the bottom stop is pulled up to 50 (never a stall against the stop)");
        dd.tilt_park = 300;
        p = hal_ptz_new(&dd, &OPS, NULL, err, sizeof err);
        CHECK(p && hal_ptz_desc(p)->tilt_park == 300, "an explicit old-style park (1275/300) still works");
    }
    CHECK(mk("pan_min=10\n") == NULL && mk("bogus=1\n") == NULL && mk("pan_min=400\npan_min=500\n") == NULL && mk("speed=1\n") == NULL,
          "a bad ptz.conf is refused (range margin, unknown key, repeated key, speed 1)");
    d.dir[0] = 9;
    CHECK(hal_ptz_new(&d, &OPS, NULL, err, sizeof err) == NULL, "a direction word above 7 is refused");
    {
        hal_ptz_desc_t dd = a2k_ptz_desc;
        dd.pan_park = 100;
        dd.tilt_park = 9999;
        p = hal_ptz_new(&dd, &OPS, NULL, err, sizeof err);
        CHECK(p && hal_ptz_desc(p)->pan_park == 300 && hal_ptz_desc(p)->tilt_park == 2100, "park pulled into the soft range");
    }
}

static void test_calibration(void)
{
    hal_ptz_t *p;
    printf("== boot calibration (no saved position)\n");
    reset(2000, 700); /* the camera is somewhere, the driver is cold (0/0), no saved file */
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "start ok");
    CHECK(M.first_ipos[0] == 4000 && M.first_ipos[1] == 2160, "first init: the counters are declared at the stock maximum 4000/2160");
    CHECK(M.cmds[0] == 4 && M.cmds[1] == 4 && M.cmds[2] == 6, "tilt DOWN (4), tilt DOWN again (the real travel is longer than the declared maximum), then pan LEFT (6)");
    CHECK(M.bad_cmd == 0 && (M.dirs & ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6))) == 0, "only the four single-axis commands 0 2 4 6 are ever sent (never 9, 10, 12)");
    CHECK(M.ninit1 == 4 && M.ninit2 == 4, "four inits: stock frame, tilt re-declared, approach frame, final frame");
    CHECK(M.last_lim[1] == 3351 && M.last_lim[2] == 2021 && M.last_ipos[0] == 976 && M.last_ipos[1] == 1221,
          "final frame: driver limits = pan soft range / tilt park floor..soft max {3351,2021}, counters = home - (min - 1) = 976/1221 (the tilt frame reaches down to the closed pose, 80)");
    CHECK(near_mech(1275, 1300, 12), "the camera is at home (mechanically)");
    CHECK(hal_ptz_state(p) == HAL_PTZ_HOME && pos_is(p, 1275, 1300, 0), "state home, position 1275/1300");
    CHECK(strstr(M.log, "calibrating") && strstr(M.log, "calibrated, at home"), "logged");
    CHECK(!strcmp(get_file("/tmp/akam_ptz.frame"), "pan_min=300 pan_max=3650 tilt_min=80 tilt_max=2100\n"), "the frame file is written (the tilt frame starts at the park floor 80)");
    CHECK(writes_of(POSF) == 0 && !strcmp(get_file(POSF), ""), "no ptz.pos on the flash: the position lives in RAM, not one write for the calibration");
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
    CHECK(M.ninit1 == 4 && M.nmove > 0 && M.last_lim[1] == 3351 && M.last_ipos[0] == 1275 - 299 && M.last_ipos[1] == 1300 - 79,
          "the calibration runs (4 inits), the final frame is declared at HOME, not at the saved position");
    CHECK(pos_is(p, 1275, 1300, 0) && hal_ptz_state(p) == HAL_PTZ_HOME && near_mech(1275, 1300, 15), "calibrated at home");
    CHECK(strstr(M.log, "home view is not proven") != NULL, "logged");
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
    CHECK(p && hal_ptz_park(p) == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && pos_is(p, 1275, 80, 4) && near_mech(1275, 80, 14), "park: pan home, tilt at the closed pose 80 (below the soft min 300), state parked");
    CHECK(hal_ptz_home(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && pos_is(p, 1275, 1300, 4), "home: state home");
    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        char err[64];
        d.pan_park = 800;
        d.tilt_park = 900;
        reset(1275, 1300);
        p = hal_ptz_new(&d, &OPS, NULL, err, sizeof err);
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
    CHECK(hal_ptz_nudge_sp(p, 100, 0, 2) == 0 && pos_is(p, 1495, 1220, 5) && hal_ptz_nudge_sp(p, -100, 0, 0) == 0 && pos_is(p, 1395, 1220, 5),
          "a nudge at speed divisor 2 and at the default (0) is exact to a few steps");
    CHECK(hal_ptz_nudge_sp(p, 100, 0, 1) != 0 && hal_ptz_nudge_sp(p, 100, 0, 21) != 0 && pos_is(p, 1395, 1220, 5),
          "a nudge at speed divisor 1 or 21 is refused by the safe layer, nothing moves");
    CHECK(hal_ptz_step(p, HAL_PTZ_LEFT) == 0 && pos_is(p, 1195, 1220, 8) && hal_ptz_step(p, HAL_PTZ_DOWN) == 0 && pos_is(p, 1195, 1020, 8) &&
          hal_ptz_step(p, HAL_PTZ_RIGHT) == 0 && pos_is(p, 1395, 1020, 8) && hal_ptz_step(p, HAL_PTZ_UP) == 0 && pos_is(p, 1395, 1220, 8), "one button step = about 200 steps in each direction");
    CHECK(M.grind == g && M.bad_cmd == 0, "no step was ever made against a mechanical stop, no forbidden command word");

    printf("== stall\n");
    p = booted(1275, 1300);
    M.stuck[0] = 1;
    CHECK(hal_ptz_step(p, HAL_PTZ_RIGHT) == HAL_PTZ_ESTALL && M.running[0] == 0, "a stuck axis: stall, motor stopped");
    CHECK(hal_ptz_state(p) == HAL_PTZ_UNKNOWN && hal_ptz_step(p, HAL_PTZ_LEFT) == HAL_PTZ_EREFUSED, "state unknown, further motion refused");
    CHECK(writes_of(POSF) == 0, "a stall writes nothing to the flash either (the next cold boot calibrates unless the home view is proven)");

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
    int fail_before = fails;
    printf("== the position is never written to the flash (RAM only)\n");
    p = booted(1275, 1300);
    for (int i = 0; i < 20; i++)
        (void)hal_ptz_step(p, i & 1 ? HAL_PTZ_LEFT : HAL_PTZ_RIGHT);
    (void)hal_ptz_nudge(p, 30, -20);
    (void)hal_ptz_goto(p, 2000, 1000);
    (void)hal_ptz_home(p);
    hal_ptz_close(p);
    CHECK(writes_of(POSF) == 0 && file_writes("/config/akam/ptz.pos") == 0, "calibration, 20 steps, a nudge, goto, home, close: zero ptz.pos writes");
    CHECK(file_writes("/config/akam/ptz.scene") == 0, "...and the engine never touches the scene reference by itself");
    reset(1275, 1300);
    M.f[file_idx(POSF, 1)].fail = 1; /* no writable /config/akam at all: nothing depends on it */
    p = mk(NULL);
    CHECK(hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && hal_ptz_step(p, HAL_PTZ_RIGHT) == 0,
          "an unwritable /config/akam: still calibrates and moves (nothing is written there)");
    (void)fail_before;
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

/* Waits until the simulated motor has been commanded more than `n0` times (a command is counted when it reaches the driver, never undone). */
static int wait_nmove_above(int n0, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        pthread_mutex_lock(&M.gm);
        int n = M.nmove;
        pthread_mutex_unlock(&M.gm);
        if (n > n0)
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
    printf("== controller thread (ptz.c)\n");
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
    CHECK(a2k_ptz_nudge_sp(1, 1, 1) == -1 && a2k_ptz_nudge_sp(1, 1, 21) == -1 && a2k_ptz_nudge_sp(0, 0, 99) == 0, "a nudge speed outside 2..20 is refused before anything is queued (0 steps is a no-op)");

    /* calibration in progress: commands refused */
    reset(2000, 700);
    set_gate(1);
    a2k_ptz_defaults(&c);
    c.privacy = 1;
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
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
    CHECK(file_writes(POSF) == 0, "the position is published from RAM: no ptz.pos write");

    /* park / home toggle and privacy */
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_privacy_active(), "toggle at home: parked, privacy active (video withheld)");
    a2k_ptz_info(&i);
    CHECK(i.pan >= 1270 && i.pan <= 1280 && i.tilt >= 75 && i.tilt <= 90, "parked at pan home, tilt 80 (the closed pose)");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_HOME, 5000) && !a2k_ptz_privacy_active(), "toggle when parked: home, privacy off");
    CHECK(g_nnotify >= 6 && !strcmp(a2k_ptz_state_name(g_notified.state), "home"), "the state changes were notified");

    /* a nudge (tracking) is accepted only while idle: never queued behind a park / home (no un-park), never replacing one */
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_blocked(), "a park motion is running (held)");
    CHECK(a2k_ptz_nudge(10, 0) == -1 && a2k_ptz_nudge_sp(10, 0, 2) == -1, "a nudge while a command runs is refused, not queued");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_PARKED, 5000), "the park ended parked: no nudge ran after it (no un-park)");
    usleep(300000);
    /* flash wear, NEW policy: the position lives in RAM. 12 crossings of 6 nudges, a quiet time after each: ZERO ptz.pos writes (and no scene write: a
     * tracking move never makes the home reference due, and nothing is dropped before a move). */
    {
        int total = 0, k, j2;
        unsigned long t0 = a2k_ptz_travel();
        int w0 = file_writes(POSF);
        for (k = 0; k < 12; k++) {
            for (j2 = 0; j2 < 6; j2++) {
                int tries;
                for (tries = 0; tries < 800 && a2k_ptz_nudge(j2 & 1 ? -25 : 25, 0) != 0; tries++)
                    usleep(5000);
            }
            usleep(100000);
        }
        total = file_writes(POSF) - w0;
        printf("  info - ptz.pos writes for 12 crossings: %d\n", total);
        CHECK(total == 0, "tracking bursts: zero ptz.pos writes across 12 crossings");
        CHECK(a2k_ptz_travel() >= t0 + 12 * 3 * 25 && a2k_ptz_travel() < t0 + 20000, "the travel counter (steps since the calibration) grows with the motions");
    }
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_blocked(), "a home motion is running (held)");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0, "a new command while moving");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_privacy_active(), "it cancelled the running motion and replaced the pending one: parked");
    CHECK(a2k_ptz_command(99) == -1 && a2k_ptz_command(-1) == -1, "an unknown command is refused");
    CHECK(a2k_ptz_command(A2K_PTZ_DOWN) == 0 && wait_state(HAL_PTZ_PARKED, 2000), "a button step at the bottom is refused by the soft limit, still parked");
    a2k_ptz_stop();
    CHECK(!a2k_ptz_privacy_active() && file_writes(POSF) == 0, "stop: motor stopped, nothing written to ptz.pos");
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
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
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
static const a2k_ptz_scene_t SCENE_HOOKS = { sc_boot, sc_capture, sc_commit, sc_drop, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static int hk_check(uint32_t *pan, uint32_t *tilt) { sc_nboot++; sc_nmove_at_boot = M.nmove; *pan = sc_pan; *tilt = sc_tilt; return sc_verdict; }
static int hk_drop(void) { sc_ndrop++; return sc_drop_rc; }
static int hk_check_mech(uint32_t *pan, uint32_t *tilt) { (void)pan; (void)tilt; sc_nboot++; sc_nmove_at_boot = M.nmove; return near_mech(1275, 1300, 15) ? 1 : 0; }

/* "very likely home": the first call answers 3, the second (the proof after the short calibration) answers from the MECHANICS: 1 only when the head really is at home */
static int g_likely_calls, g_likely_proof, g_proof_files_invalid = -1, g_likely_cancel;
static hal_ptz_t *g_likely_hp;
static const char *get_file(const char *path);
static int hk_check_likely(uint32_t *pan, uint32_t *tilt)
{
    (void)pan; (void)tilt;
    sc_nboot++;
    sc_nmove_at_boot = M.nmove;
    if (++g_likely_calls == 1)
        return 3;
    /* the proof: the frame files must still be INVALID (a restart now must never adopt an unproven home) */
    g_proof_files_invalid = strncmp(get_file(PTZ_FRAME_PATH), "pan_min", 7) != 0 && strncmp(get_file(PTZ_APP_FRAME_PATH), "pan_min", 7) != 0;
    if (g_likely_cancel && g_likely_hp)
        hal_ptz_cancel(g_likely_hp);
    return (g_likely_proof && near_mech(1275, 1300, 15)) ? 1 : 0;
}

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
    printf("== scene check: hal_ptz boot path (the claim is HOME)\n");
    ops.cold_check = hk_check;
    ops.scene_drop = hk_drop;
    sc_reset(1, 1275, 1300);
    reset(1275, 1300); /* the head is at home, the home reference matches */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "match: start ok");
    CHECK(sc_nboot == 1 && sc_nmove_at_boot == 0 && M.nmove == 0 && M.grind == 0 && sc_ndrop == 0, "match: checked before any motor command, no motion, no calibration, nothing dropped");
    CHECK(hal_ptz_pos(p, &a, &b) && a == 1275 && b == 1300 && hal_ptz_state(p) == HAL_PTZ_HOME, "match: HOME is declared (trusted, state home)");
    CHECK(hal_ptz_verify_pos(p) == 0, "the counters agree with the declared position");
    CHECK(hal_ptz_goto(p, 1800, 900) == 0 && hal_ptz_home(p) == 0 && near_mech(1275, 1300, 12), "match: the declared frame is right, a move and home reach the real home");
    CHECK(M.bad_cmd == 0 && strstr(M.log, "scene check passed"), "match: logged, only commands 0..7");
    CHECK(writes_of(POSF) == 0 && sc_ndrop == 0, "no ptz.pos write, the reference is not dropped by moves");
    hal_ptz_close(p);
    hal_ptz_free(p);

    sc_reset(0, 1275, 1300);
    reset(2500, 700); /* the head was turned while off (or the power cut came while it was away from home) */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && M.grind > 0, "mismatch: the long calibration runs as before");
    CHECK(sc_nboot == 1 && sc_nmove_at_boot == 0 && sc_ndrop == 0, "mismatch: checked first; the reference is NOT dropped (it is keyed to home: it stays valid)");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* power cut while the head is AWAY from home: the "view" is the mechanical position, the claim home is only proven when the head is there */
    {
        hal_ptz_ops_t om = ops;
        om.cold_check = hk_check_mech;
        sc_reset(1, 1275, 1300);
        reset(1275, 1300);
        p = hal_ptz_new(&a2k_ptz_desc, &om, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind == 0 && M.nmove == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "cold boot with the head at home: claim home proven by the view, no calibration");
        hal_ptz_free(p);
        reset(2200, 800); /* the head had been moved away (tracking, a button) when the power was cut: RAM and the driver counters are gone */
        p = hal_ptz_new(&a2k_ptz_desc, &om, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0 && near_mech(1275, 1300, 12) && hal_ptz_state(p) == HAL_PTZ_HOME && sc_ndrop == 0,
              "cold boot with the head away from home (power cut away from home): the view does not prove home, FULL CALIBRATION, the reference is kept");
        hal_ptz_free(p);
    }

    /* very likely home (answer 3): the SHORT calibration, then the proof; the full one when the proof fails */
    {
        hal_ptz_ops_t ol = ops;
        long g0, t_short, t_full;
        ol.cold_check = hk_check_likely;
        g_likely_calls = 0; g_likely_proof = 1;
        sc_reset(1, 1275, 1300);
        reset(1275, 1300);
        p = hal_ptz_new(&a2k_ptz_desc, &ol, NULL, err, sizeof err);
        g0 = (long)M.clock;
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && g_likely_calls == 2 && hal_ptz_state(p) == HAL_PTZ_HOME && near_mech(1275, 1300, 12), "very likely home + proof: short calibration, then verified, state home");
        CHECK(M.first_ipos[0] == 1275 + 400 && M.first_ipos[1] == 1300 + 300, "the counters were declared at home + 400 / + 300, not at the stock maxima");
        t_short = (long)M.clock - g0;
        CHECK(strstr(M.log, "SHORT calibration") && strstr(M.log, "verified by the scene check"), "the short calibration and its proof are logged");
        CHECK(g_proof_files_invalid == 1, "at proof time the frame files were INVALID (not adoptable) ...");
        CHECK(!strncmp(get_file(PTZ_APP_FRAME_PATH), "pan_min", 7) && !strncmp(get_file(PTZ_FRAME_PATH), "pan_min", 7), "... and valid once the strict check proved the home");
        CHECK(hal_ptz_cal_count(p) == 0 && hal_ptz_travel(p) == 0 && hal_ptz_verify_pos(p) == 0 && M.bad_cmd == 0 && sc_ndrop == 0, "a verified short calibration is not a full one (the periodic counter keeps counting), the counters agree, only commands 0..7, the reference is kept");
        hal_ptz_close(p);
        hal_ptz_free(p);
        /* a cancel (shutdown) / kill during the proof: nothing adoptable is left behind */
        g_likely_calls = 0; g_likely_proof = 1; g_likely_cancel = 1; g_proof_files_invalid = -1;
        reset(3000, 1900);
        p = hal_ptz_new(&a2k_ptz_desc, &ol, NULL, err, sizeof err);
        g_likely_hp = p;
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == HAL_PTZ_ECANCEL && g_proof_files_invalid == 1, "a cancel during the proof: the start is cancelled, the files were invalid");
        CHECK(strncmp(get_file(PTZ_FRAME_PATH), "pan_min", 7) != 0 && strncmp(get_file(PTZ_APP_FRAME_PATH), "pan_min", 7) != 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN, "... and nothing is adoptable: the frame files are invalid, the position untrusted");
        hal_ptz_free(p);
        g_likely_cancel = 0; g_likely_hp = NULL;
        {   /* a restart in the same boot (the driver holds the declared counters, the files are the in-memory ones of this test) must calibrate, not adopt */
            hal_ptz_ops_t o2 = ops;
            hal_ptz_t *p2;
            unsigned long g1 = M.grind;
            o2.cold_check = NULL;
            p2 = hal_ptz_new(&a2k_ptz_desc, &o2, NULL, err, sizeof err);
            CHECK(p2 && hal_ptz_start(p2, HAL_PTZ_CAL_AUTO) == 0 && !strstr(M.log, "already holds our frame") && M.grind > g1 && near_mech(1275, 1300, 12), "the next start does NOT adopt the unproven home: it calibrates and ends at the real home");
            hal_ptz_free(p2);
        }
        g_likely_calls = 0; g_likely_proof = 0;
        reset(1275, 1300);
        /* the proof fails: the full calibration follows */
        g_likely_calls = 0; g_likely_proof = 0;
        reset(1275, 1300);
        p = hal_ptz_new(&a2k_ptz_desc, &ol, NULL, err, sizeof err);
        g0 = (long)M.clock;
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && g_likely_calls == 2 && strstr(M.log, "not proven afterwards: full calibration") && near_mech(1275, 1300, 12) && hal_ptz_state(p) == HAL_PTZ_HOME, "the proof fails: the full calibration runs and ends at home");
        t_full = (long)M.clock - g0;
        printf("   virtual time: short calibration + proof %ld ms, failed short + full calibration %ld ms\n", t_short, t_full);
        CHECK(t_short * 10 < t_full * 7, "the short calibration takes under 70 percent of a (failed short +) full calibration");
        CHECK(M.ninit1 >= 4, "... with the two sweeps of the full calibration (4 driver inits instead of 3)");
        hal_ptz_free(p);
        /* the head was really far away (more than the margin): the short sweep ends short, the proof (the mechanics) fails, the full calibration puts it right */
        g_likely_calls = 0; g_likely_proof = 1;
        reset(3000, 1900);
        p = hal_ptz_new(&a2k_ptz_desc, &ol, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && hal_ptz_state(p) == HAL_PTZ_HOME, "a head far from home: the short sweep cannot prove it, the full calibration corrects it (never trusted unproven)");
        hal_ptz_free(p);
    }

    sc_reset(1, 1500, 900); /* a hook that answers another position than home: never believed */
    reset(1500, 900);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && M.grind > 0 && strstr(M.log, "not the home position"), "a claim that is not the home position: calibrates");
    hal_ptz_close(p);
    hal_ptz_free(p);

    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        d.tilt_park = d.tilt_home; /* home equal to the park position: the parked view is the least informative one, never claimed */
        sc_reset(1, 1275, 1300);
        reset(1275, 1300);
        p = hal_ptz_new(&d, &ops, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0, "home == park: the claim is refused, full calibration");
        hal_ptz_close(p);
        hal_ptz_free(p);
    }

    sc_reset(1, 1275, 1300);
    reset(1275, 1300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_OFF) == 0 && sc_nboot == 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN && M.nmove == 0, "cal off: no check, the motor untouched");
    CHECK(hal_ptz_verify_pos(p) == -1, "an untrusted position cannot be verified");
    CHECK(hal_ptz_recalibrate(p) == 0 && near_mech(1275, 1300, 12) && sc_ndrop == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "recalibrate: the long calibration now, the home reference is kept, at home");
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
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_recalibrate(p) == HAL_PTZ_EREFUSED, "recalibrate with the motor not open: refused");
    hal_ptz_free(p);

    /* the reference is dropped (rarely, best effort) when a calibration FAILS, or when a position claimed by the scene check is lost */
    sc_reset(0, 1275, 1300);
    reset(1000, 1000);
    M.stuck[1] = 1;
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && hal_ptz_state(p) == HAL_PTZ_UNKNOWN && sc_ndrop == 1, "a calibration that fails (dead tilt axis): the home reference is dropped once");
    hal_ptz_free(p);
    sc_reset(0, 1275, 1300);
    sc_drop_rc = -1;
    reset(1000, 1000);
    M.stuck[1] = 1;
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && sc_ndrop == 1 && strstr(M.log, "could not be invalidated"), "...a drop that fails is logged, nothing else is refused or retried");
    hal_ptz_free(p);
    {
        sc_reset(0, 1275, 1300);
        reset(2500, 700);
        p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
        M.cancel_hp = p;
        M.cancel_at = 6; /* a shutdown cancel in the middle of the calibration */
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) != 0 && sc_ndrop == 0, "a calibration cancelled by a shutdown: the reference is NOT dropped");
        hal_ptz_free(p);
    }
    sc_reset(1, 1275, 1300);
    reset(1275, 1300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && sc_ndrop == 0, "claimed home (scene check)");
    M.stuck[0] = 1;
    CHECK(hal_ptz_step(p, HAL_PTZ_RIGHT) == HAL_PTZ_ESTALL && sc_ndrop == 1, "the claimed position is lost (stall): proven wrong, the reference is dropped (once)");
    hal_ptz_free(p);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "calibrated start");
    M.stuck[0] = 1;
    CHECK(hal_ptz_step(p, HAL_PTZ_RIGHT) == HAL_PTZ_ESTALL && sc_ndrop == 0, "a stall in a calibrated run says nothing about the reference: not dropped");
    hal_ptz_free(p);

    printf("== calibration: the second tilt sweep\n");
    {
        hal_ptz_ops_t o2 = OPS;
        reset(1900, 2300); /* the head at the top stop: the real travel (about 2300) is longer than the declared maximum 2160 */
        p = hal_ptz_new(&a2k_ptz_desc, &o2, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12), "from the top stop (tilt 2300): calibrated, the head really is at home (the bottom stop was reached)");
        CHECK(M.ninit1 == 4 && M.ninit2 == 4 && M.cmds[0] == 4 && M.cmds[1] == 4 && M.cmds[2] == 6, "four inits (stock, tilt re-declared, approach, final) and sweeps tilt down, tilt down again, pan left");
        CHECK(M.bad_cmd == 0, "only commands 0..7");
        hal_ptz_close(p);
        hal_ptz_free(p);
    }

    printf("== scene check: controller (home reference after a calibration only; moves never touch it)\n");
    a2k_ptz_defaults(&c);
    CHECK(c.scene_check == 1, "ptz_scene_check defaults to 1");
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    a2k_ptz_set_scene(&SCENE_HOOKS);
    /* a match: the head is at home, the reference is good: nothing is captured, nothing written, whatever moves follow */
    sc_reset(1, 1275, 1300);
    reset(1275, 1300);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 5000), "controller: boot with a matching home scene");
    usleep(400000);
    a2k_ptz_info(&i);
    CHECK(M.nmove == 0 && i.known && i.pan == 1275 && i.tilt == 1300 && sc_nsave == 0 && sc_ncap == 0 && sc_ndrop == 0, "no motion, position 1275/1300 published, no capture, no write, no drop");
    CHECK(a2k_ptz_nudge(100, 0) == 0 && wait_state(HAL_PTZ_MOVED, 3000), "a nudge of 100 steps");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "...and home again");
    usleep(500000);
    CHECK(sc_ndrop == 0 && sc_ncap == 0 && sc_nsave == 0, "moves: no drop before the motion, no capture after it, no reference write (it is keyed to home, it survives)");
    sc_drop_rc = -1; /* even a drop that cannot be done would not refuse anything: it is not called */
    CHECK(a2k_ptz_nudge(60, 0) == 0 && wait_state(HAL_PTZ_MOVED, 3000) && sc_ndrop == 0, "a motion with a failing drop hook runs (the hook is not used for moves)");
    sc_drop_rc = 0;
    a2k_ptz_stop();
    /* travel budget: moves between the calibration and the (late) capture exceed it: no reference check */
    a2k_ptz_test_set_scene_timing(500, 150, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    c.travel_budget = 100;
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "controller: calibrated start, travel budget 100 steps");
    CHECK(a2k_ptz_command(A2K_PTZ_RIGHT) == 0 && wait_state(HAL_PTZ_MOVED, 3000), "a button step away (200 steps)");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "...and home again");
    usleep(1500000);
    CHECK(a2k_ptz_travel() >= 150 && sc_ncap == 0 && sc_nsave == 0, "over the budget: no scene reference captured nor saved");
    a2k_ptz_stop();
    c.travel_budget = 0;
    /* L2: a lot of travel since the calibration (lost steps are possible): no reference is written in this run, whatever the budget option says */
    a2k_ptz_test_set_scene_timing(500, 150, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "calibrated start (default budget)");
    for (int k = 0; k < 3; k++) {
        CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_state(HAL_PTZ_PARKED, 8000) && a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 8000), "park and home (about 2000 steps)");
    }
    usleep(1500000);
    CHECK(a2k_ptz_travel() > A2K_PTZ_REF_TRAVEL && sc_ncap == 0 && sc_nsave == 0, "over 3000 steps since the calibration: no reference captured nor written");
    a2k_ptz_stop();
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    /* never during / just after tracking: a tracking nudge defers the reference by the quiet period (10 min) */
    a2k_ptz_test_set_scene_timing(500, 150, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && a2k_ptz_nudge(4, 0) == 0, "calibrated; a tracking nudge arrives before the capture");
    usleep(300000);
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "...the head is at home again");
    usleep(1500000);
    CHECK(sc_ncap == 0 && sc_nsave == 0, "no capture, no write while a tracker is active or was just now (the reference stays due)");
    a2k_ptz_stop();
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    /* mismatch at boot: calibrate, then the home reference is captured and written, fresh; later moves leave it alone */
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && near_mech(1275, 1300, 12) && sc_nboot == 1 && sc_nmove_at_boot == 0, "mismatch: checked first, then calibrated");
    CHECK(wait_count(&sc_nsave, 1, 4000) && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1 && sc_ndrop == 0 && sc_ncap == 1,
          "...and the home view is captured once and written fresh (nothing dropped)");
    {
        int nc = sc_ncap, ns = sc_nsave;
        CHECK(a2k_ptz_command(A2K_PTZ_RIGHT) == 0 && wait_state(HAL_PTZ_MOVED, 3000), "a move away (a button step)");
        CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "back home");
        usleep(600000);
        CHECK(sc_ncap == nc && sc_nsave == ns && sc_ndrop == 0, "after the write: no new capture, no new write, no drop, however the head moves (the reference survives moves)");
    }
    /* the Recalibrate button: the reference is checked again (fresh: the matched-boots counter restarts) */
    {
        int g0 = (int)M.grind, ns = sc_nsave;
        CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0, "Recalibrate PTZ: queued");
        for (int t = 0; t < 4000 && (int)M.grind == g0; t += 5)
            usleep(5000);
        CHECK((int)M.grind > g0 && wait_state(HAL_PTZ_HOME, 20000) && near_mech(1275, 1300, 12), "...the long calibration ran, at home");
        CHECK(wait_count(&sc_nsave, ns + 1, 4000) && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1 && sc_ndrop == 0,
              "...checked again at home once quiet, marked fresh, nothing dropped");
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

    /* a motion inside the confirmation window voids the candidate; away from home nothing is captured; back at home the reference is retried */
    a2k_ptz_test_set_scene_timing(60, 600, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&sc_ncap, 1, 3000), "calibrated, the home candidate is captured");
    CHECK(a2k_ptz_command(A2K_PTZ_RIGHT) == 0 && wait_state(HAL_PTZ_MOVED, 3000), "a motion (a button step) inside the confirmation window");
    usleep(1000000);
    CHECK(sc_nsave == 0 && sc_ncap == 1, "the candidate is void, the head is away from home: nothing written, nothing captured");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000) && wait_count(&sc_nsave, 1, 4000) && sc_ncap == 2 && sc_last_fresh == 1,
          "back at home: the reference is still due (a calibration ran, none was written): captured again and written");
    a2k_ptz_stop();
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    /* the view changed during the window: the scene module refuses the commit; at most 3 attempts */
    sc_reset(0, 1275, 1300);
    sc_commit_rc = -1;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&sc_ncommit, 1, 4000), "view changed during the window: commit attempted and refused");
    usleep(1200000);
    CHECK(sc_nsave == 0 && sc_ncommit <= 3, "...no reference written, at most 3 attempts (no endless loop)");
    a2k_ptz_stop();
    sc_commit_rc = 0;
    /* the driver counters moved after the capture: the thread re-reads them and refuses to write */
    a2k_ptz_test_set_scene_timing(60, 400, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&sc_ncap, 1, 3000), "calibrated, the candidate captured");
    pthread_mutex_lock(&M.gm);
    M.cnt[1] += 80; /* the counters no longer agree with the cached position */
    pthread_mutex_unlock(&M.gm);
    usleep(700000);
    CHECK(sc_nsave == 0 && sc_ncommit == 0, "counters differ from the position: the commit is not even attempted, nothing written");
    pthread_mutex_lock(&M.gm);
    M.cnt[1] -= 80;
    pthread_mutex_unlock(&M.gm);
    a2k_ptz_stop();
    a2k_ptz_test_set_scene_timing(60, 150, 0);

    /* an app restart in the same boot (adoption, no calibration): nothing is due, nothing is written */
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&sc_nsave, 1, 4000) && sc_last_fresh == 1, "boot with a calibration: written fresh");
    a2k_ptz_stop();
    {
        int g0 = (int)M.grind;
        sc_reset(0, 1275, 1300); /* the driver keeps its frame: the next start adopts it */
        CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 5000) && (int)M.grind == g0 && sc_nboot == 0, "restart in the same boot: adopted, no calibration");
        CHECK(a2k_ptz_nudge(60, 0) == 0 && a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "a move and home again");
        usleep(500000);
        CHECK(sc_ncap == 0 && sc_nsave == 0 && sc_ndrop == 0, "after an adoption nothing is captured, written or dropped");
        a2k_ptz_stop();
    }

    /* a view with too little detail: nothing to write, no write, no retry */
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 1;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start (low detail view)");
    usleep(600000);
    CHECK(sc_ncap == 1 && sc_ncommit == 0 && sc_nsave == 0, "low detail: captured once, no commit, no write");
    a2k_ptz_stop();
    /* a failing capture (no fresh frame): retried a few times, then given up */
    sc_reset(0, 1275, 1300);
    sc_cap_rc = -1;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start (capture failing)");
    usleep(900000);
    CHECK(sc_nsave == 0 && sc_ncap == 3, "no reference when the capture fails (3 attempts, no endless loop)");
    a2k_ptz_stop();
    /* shutdown commit: the candidate is settled (> 3 s old), the write waits for the confirmation delay (long) */
    a2k_ptz_test_set_scene_timing(60, 60000, 0);
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start for the shutdown commit");
    usleep(3400000);
    CHECK(sc_ncap == 1 && sc_nsave == 0, "candidate captured, the confirmation is still far away (60 s)");
    a2k_ptz_stop();
    CHECK(sc_nsave == 1 && sc_save_pan == 1275 && sc_save_tilt == 1300 && sc_last_fresh == 1, "app shutdown: the reference is written (on the PTZ thread) before it ends");
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start again");
    a2k_ptz_stop();
    CHECK(sc_nsave == 0 && sc_ncap == 0, "a stop right after the calibration (no settled candidate): nothing written");
    /* shutdown with the counters moved: not written */
    sc_reset(0, 1275, 1300);
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start for the shutdown / counters test");
    usleep(3400000);
    pthread_mutex_lock(&M.gm);
    M.cnt[0] += 90;
    pthread_mutex_unlock(&M.gm);
    a2k_ptz_stop();
    CHECK(sc_nsave == 0 && sc_ncommit == 0, "shutdown with the counters off the cached position: nothing written");
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    /* ptz_scene_check = 0: no check, no capture, no write (an old reference on the flash is left alone: it is keyed to home) */
    c.scene_check = 0;
    sc_reset(1, 1275, 1300);
    reset(1275, 1300);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && sc_nboot == 0 && M.grind > 0, "ptz_scene_check=0: always calibrates, the check is not even run");
    usleep(500000);
    CHECK(sc_nsave == 0 && sc_ncap == 0 && sc_ndrop == 0, "...no reference is written, nothing is dropped");
    a2k_ptz_stop();
    c.scene_check = 1;
    /* ptz_cal=off: the recalibrate button never touches the motor */
    c.cal = HAL_PTZ_CAL_OFF;
    sc_reset(1, 1275, 1300);
    reset(1275, 1300);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_UNKNOWN, 2000) && sc_nboot == 0 && a2k_ptz_command(A2K_PTZ_RECAL) == -1 && M.nmove == 0, "ptz_cal=off: no check, Recalibrate refused");
    a2k_ptz_stop();
    a2k_ptz_set_scene(NULL);
    a2k_ptz_test_set_scene_timing(0, 0, 0);
}

/* ------------------------------------------------------------------------------------------------ multi-view (featureless rest view) */
static int mv_tab[8][2] = { { 150, 0 }, { -150, 0 }, { 0, -100 } };
static int mv_n = 3;
static int mv_ref_pan, mv_ref_tilt;       /* where the head REALLY was when the reference was taken (the "view" is the mechanical position) */
static volatile int mv_probe_calls, mv_grind_at_probe, mv_probe_hits;
static int mv_probe_log[8];
static int mv_alt_rc[8];
static volatile int mv_alt_calls, mv_commit_multi_calls, mv_confirm_calls;
static volatile unsigned mv_mask, mv_cm_pan, mv_cm_tilt;
static int mv_alt_pos[8][2];
static volatile int mv_frames_valid_during, mv_nmove_log[8], mv_cancel_at; /* mv_cancel_at: the probe call number (1 based) that answers -1 */

/* both frame files of the safe layer hold a valid frame (the driver "holds our frame": an app restart would adopt it) */
static int frame_files_valid(void)
{
    int n = 0, i;
    for (i = 0; i < 12; i++)
        if ((!strcmp(M.f[i].path, PTZ_FRAME_PATH) || !strcmp(M.f[i].path, PTZ_APP_FRAME_PATH)) && M.f[i].present) {
            struct ptzs_frame fr;
            if (ptzs_frame_parse(M.f[i].data, &fr) == 0)
                n++;
        }
    return n == 2;
}

static int mv_off(unsigned i, int *dp, int *dt)
{
    if ((int)i >= mv_n)
        return -1;
    *dp = mv_tab[i][0];
    *dt = mv_tab[i][1];
    return 0;
}

/* the live view equals the stored view of offset i exactly when the head really is at (reference + offset) */
static int mv_probe(unsigned i)
{
    int dp = 0, dt = 0, hit;
    if (mv_probe_calls < 8) {
        mv_probe_log[mv_probe_calls] = (int)i;
        mv_nmove_log[mv_probe_calls] = M.nmove;
    }
    mv_probe_calls++;
    if (frame_files_valid())
        mv_frames_valid_during = 1; /* the driver must not be adoptable while the position is only claimed */
    if (mv_cancel_at && mv_probe_calls == mv_cancel_at)
        return -1;
    mv_grind_at_probe = (int)M.grind; /* no grinding (a stop hit) before / during the probe */
    mv_off(i, &dp, &dt);
    hit = near_mech(mv_ref_pan + dp, mv_ref_tilt + dt, 15);
    if (hit)
        mv_probe_hits++;
    return hit;
}

static int mv_alt_capture(unsigned i)
{
    if (i < 8) {
        mv_alt_pos[i][0] = M.mech[0];
        mv_alt_pos[i][1] = M.mech[1];
    }
    mv_alt_calls++;
    return i < 8 ? mv_alt_rc[i] : -1;
}
static int mv_alt_confirm(unsigned i) { (void)i; mv_confirm_calls++; return 0; }
static int mv_commit_multi(uint32_t pan, uint32_t tilt, int fresh, unsigned mask)
{
    (void)fresh;
    mv_cm_pan = pan;
    mv_cm_tilt = tilt;
    mv_mask = mask;
    mv_commit_multi_calls++;
    return 0;
}
static const a2k_ptz_scene_t MV_HOOKS = { sc_boot, sc_capture, sc_commit, sc_drop, mv_off, mv_probe, mv_off, mv_alt_capture, mv_alt_confirm, mv_commit_multi, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static void mv_reset(void)
{
    mv_n = 3;
    mv_tab[0][0] = 150; mv_tab[0][1] = 0;
    mv_tab[1][0] = -150; mv_tab[1][1] = 0;
    mv_tab[2][0] = 0; mv_tab[2][1] = -100;
    mv_probe_calls = mv_grind_at_probe = mv_probe_hits = mv_frames_valid_during = mv_cancel_at = 0;
    memset((void *)mv_nmove_log, 0, sizeof mv_nmove_log);
    memset(mv_probe_log, -1, sizeof mv_probe_log);
    memset(mv_alt_rc, 0, sizeof mv_alt_rc);
    mv_alt_calls = mv_commit_multi_calls = mv_confirm_calls = 0;
    mv_mask = mv_cm_pan = mv_cm_tilt = 0;
}

static void test_multiview(void)
{
    hal_ptz_ops_t ops = OPS;
    hal_ptz_t *p;
    a2k_ptz_cfg_t c;
    a2k_ptz_info_t i;
    char err[64];
    uint32_t a, b;
    printf("== multi-view: boot path (hal_ptz), declared position, small offset moves, probe\n");
    ops.cold_check = hk_check;
    ops.scene_drop = hk_drop;
    ops.cold_offset = mv_off;
    ops.cold_probe = mv_probe;

    /* the head is where the reference says: the first offset matches, back at the claimed position, trusted, no calibration */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(1275, 1300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "probe: start ok");
    CHECK(sc_nboot == 1 && sc_nmove_at_boot == 0 && mv_probe_calls == 1 && mv_probe_hits == 1 && mv_probe_log[0] == 0, "the check ran before any motor command, the first offset was probed and matched");
    CHECK(M.grind == 0 && M.cnt[0] != 0 && sc_ndrop == 0 && near_mech(1275, 1300, 12), "no calibration (no grinding), nothing dropped, the head is back at home");
    CHECK(hal_ptz_pos(p, &a, &b) && a == 1275 && b == 1300 && hal_ptz_verify_pos(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME, "home is trusted and the counters agree");
    CHECK(strstr(M.log, "multi-view") && strstr(M.log, "scene check passed (multi-view)") && M.bad_cmd == 0, "logged, only commands 0..7");
    CHECK(hal_ptz_goto(p, 1900, 900) == 0 && hal_ptz_home(p) == 0 && near_mech(1275, 1300, 12), "the declared frame is right: a move and home reach the real home");
    CHECK(mv_frames_valid_during == 0 && frame_files_valid(), "the frame files were INVALID during the probe (a hard kill there cannot make a restart adopt the claim) and are written once it is proven");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* the head was turned by hand: no offset matches, the long calibration runs; the offsets were small, nothing ground before it */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(2500, 700);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && near_mech(1275, 1300, 12) && M.grind > 0, "head turned: all offsets probed, none matches, full calibration, at home");
    CHECK(mv_probe_calls == 3 && mv_probe_log[0] == 0 && mv_probe_log[1] == 1 && mv_probe_log[2] == 2 && sc_ndrop == 0, "the three offsets were probed in order; the reference is not dropped (keyed to home)");
    CHECK(mv_frames_valid_during == 0, "failed probe: frame files invalid throughout");
    CHECK(mv_nmove_log[1] - mv_nmove_log[0] >= 2 && mv_nmove_log[2] - mv_nmove_log[1] >= 2, "between two offsets the head goes back to the claimed position (never offset to offset: the travel from the real position stays one D)");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* a shutdown (the probe answers -1): nothing else is moved and no calibration starts */
    mv_reset();
    mv_cancel_at = 1;
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(1275, 1300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == HAL_PTZ_ECANCEL && M.grind == 0 && mv_probe_calls == 1 && sc_ndrop == 0 && !frame_files_valid(), "cancelled during the probe: no calibration, no further move, the position stays unproven, frame files invalid");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* WORST CASE: the claim is wrong and the head sits at the edge of the soft range (pan 3650 / tilt 300): the probe moves (+150, -150, tilt -100)
     * stay clear of the stops (the model's hard stops are at 4000 and 0): no click before the calibration starts */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(3650, 300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls >= 1, "wrong claim, head at pan 3650 / tilt 300: probed, refused");
    CHECK(mv_grind_at_probe == 0, "the offset moves did not click against a stop, even from the worst corner");
    hal_ptz_close(p);
    hal_ptz_free(p);
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(300, 300); /* the other corner of the soft range */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 3 && mv_grind_at_probe == 0, "wrong claim, head at pan 300 / tilt 300: three offsets (pan -150 -> real 150, tilt -100 -> real 200) never reach a stop");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* an offset outside the soft range is skipped, an excessive one is refused whatever the hook says */
    mv_reset();
    sc_reset(2, 3600, 1300);
    mv_ref_pan = 3600;
    mv_ref_tilt = 1300;
    reset(3600, 1300);
    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        d.pan_home = 3600; /* a home configured near the right end of the soft range */
        p = hal_ptz_new(&d, &ops, NULL, err, sizeof err);
    }
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_hits == 1 && mv_probe_calls == 1 && mv_probe_log[0] == 1 && near_mech(3600, 1300, 12) && M.grind == 0,
          "home pan 3600: offset +150 is outside the soft range (3650): skipped; the offset -150 matches");
    hal_ptz_close(p);
    hal_ptz_free(p);
    mv_reset();
    mv_tab[0][0] = 900; /* a buggy hook asks for a long move */
    mv_tab[1][0] = 0;
    mv_tab[1][1] = 250;
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(1275, 1300);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 1 && mv_probe_log[0] == 2 && mv_probe_hits == 1 && M.grind == 0, "offsets of 900 / 250 steps are never moved (limit 200 per axis); only the sane third offset is probed");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* home equal to the park position: never declared, even for a probe */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(1275, 1300);
    {
        hal_ptz_desc_t d = a2k_ptz_desc;
        d.tilt_park = d.tilt_home;
        p = hal_ptz_new(&d, &ops, NULL, err, sizeof err);
    }
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 0 && M.grind > 0, "home == park: no probe, full calibration");
    hal_ptz_close(p);
    hal_ptz_free(p);

    /* no probe hooks (the 1080p-like caller, or an old controller): an answer 2 is just "no" */
    mv_reset();
    {
        hal_ptz_ops_t o2 = OPS;
        o2.cold_check = hk_check;
        o2.scene_drop = hk_drop;
        sc_reset(2, 1275, 1300);
        reset(1275, 1300);
        p = hal_ptz_new(&a2k_ptz_desc, &o2, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0 && mv_probe_calls == 0, "answer 2 without probe hooks: full calibration");
        hal_ptz_close(p);
        hal_ptz_free(p);
    }

    /* R7: the probe attempts are bounded per boot (a RAM marker): a crash loop does not walk the head by an offset at every restart */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(2500, 700); /* the claim is wrong: no match, the calibration runs */
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 3, "attempt 1: three offsets probed");
    hal_ptz_free(p);
    {
        /* an app restart that does not find our frame (a hard kill before the calibration ended is modelled by invalidating it) */
        put_file("/tmp/akam_ptz_app.frame", "invalid\n");
        put_file("/tmp/akam_ptz.frame", "invalid\n");
        mv_probe_calls = 0;
        M.mech[0] = 2500;
        M.mech[1] = 700;
        p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 3, "attempt 2: probed again");
        hal_ptz_free(p);
        put_file("/tmp/akam_ptz_app.frame", "invalid\n");
        put_file("/tmp/akam_ptz.frame", "invalid\n");
        mv_probe_calls = 0;
        M.mech[0] = 2500;
        M.mech[1] = 700;
        p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
        CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && mv_probe_calls == 0 && M.grind > 0 && strstr(M.log, "attempts already made"), "attempt 3 in the same boot: no probe move, the calibration decides");
        hal_ptz_free(p);
    }
    /* R6: a cancel that arrives between two probe moves is not lost to begin() clearing the flag */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(2500, 700);
    p = hal_ptz_new(&a2k_ptz_desc, &ops, NULL, err, sizeof err);
    M.cancel_hp = p; /* ... fired by the first sleep of the probe's first move */
    M.cancel_at = 1;
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == HAL_PTZ_ECANCEL && M.grind == 0 && mv_probe_calls <= 1, "a shutdown cancel during the start / probe: ECANCEL, no calibration, no further offset");
    hal_ptz_free(p);

    printf("== multi-view: controller (boot probe, reference built at home, abort)\n");
    a2k_ptz_defaults(&c);
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    a2k_ptz_set_scene(&MV_HOOKS);
    /* boot probe through the controller: matched, the position is published, no reference is rewritten */
    mv_reset();
    sc_reset(2, 1275, 1300);
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(1275, 1300);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 5000), "controller: boot with a featureless saved view");
    usleep(500000);
    a2k_ptz_info(&i);
    CHECK(mv_probe_hits == 1 && M.grind == 0 && i.known && i.pan == 1275 && i.tilt == 1300 && sc_ncap == 0 && sc_nsave == 0 && mv_commit_multi_calls == 0, "probe matched: home declared, no calibration, nothing re-captured (the stored views are still valid)");
    a2k_ptz_stop();
    /* boot probe fails -> calibration -> home -> the (featureless) rest view asks for the multi-view reference */
    mv_reset();
    sc_reset(2, 1275, 1300);
    sc_cap_rc = 2; /* the rest view is featureless */
    mv_ref_pan = 1275;
    mv_ref_tilt = 1300;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && near_mech(1275, 1300, 12), "controller: probe fails, calibrated, at home");
    CHECK(wait_count(&mv_commit_multi_calls, 1, 6000), "the featureless rest view: the multi-view reference is built");
    CHECK(mv_alt_calls == 3 && near_mech(1275, 1300, 12) && mv_mask == 7 && mv_cm_pan == 1275 && mv_cm_tilt == 1300, "three offset views kept, the head is back at home, written with the three views (mask 7)");
    CHECK(mv_alt_pos[0][0] >= 1275 + 140 && mv_alt_pos[0][0] <= 1275 + 160 && mv_alt_pos[1][0] >= 1275 - 160 && mv_alt_pos[1][0] <= 1275 - 140 &&
          mv_alt_pos[2][1] >= 1300 - 110 && mv_alt_pos[2][1] <= 1300 - 90 && mv_alt_pos[2][0] >= 1265 && mv_alt_pos[2][0] <= 1285, "views were kept at pan +150, pan -150, tilt -100 of the rest position");
    CHECK(sc_ncommit == 0 && sc_nsave == 0 && sc_ndrop == 0 && M.bad_cmd == 0, "the plain commit is not used; the old reference is not touched before the new one is complete; only commands 0..7");
    {
        int n = mv_alt_calls;
        usleep(700000);
        CHECK(mv_alt_calls == n && mv_commit_multi_calls == 1, "built once, not again");
        CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "park, then home again");
        usleep(1200000);
        CHECK(sc_ncap == 1 && mv_alt_calls == n && mv_commit_multi_calls == 1, "back at home: not captured again, not rebuilt (no wiggle, no flash write at every return home)");
        CHECK(a2k_ptz_command(A2K_PTZ_RECAL) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&mv_commit_multi_calls, 2, 8000), "after a recalibration (fresh reference) it is rebuilt");
    }
    a2k_ptz_stop();
    /* a view that is featureless at an offset too is skipped (mask without it) */
    mv_reset();
    mv_alt_rc[1] = 1;
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 2;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&mv_commit_multi_calls, 1, 6000) && mv_mask == 5 && mv_alt_calls == 3, "a featureless offset view is not stored: mask 101");
    a2k_ptz_stop();
    /* nothing usable at any offset: nothing written, not retried */
    mv_reset();
    mv_alt_rc[0] = mv_alt_rc[1] = mv_alt_rc[2] = 1;
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 2;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&mv_alt_calls, 3, 6000), "all three offset views are featureless too");
    usleep(500000);
    CHECK(mv_commit_multi_calls == 0 && sc_nsave == 0 && near_mech(1275, 1300, 12), "nothing is written; the head is back at home");
    {
        int k;
        for (k = 0; k < 5; k++) {
            a2k_ptz_command(A2K_PTZ_PARK);
            wait_state(HAL_PTZ_PARKED, 5000);
            a2k_ptz_command(A2K_PTZ_HOME);
            wait_state(HAL_PTZ_HOME, 5000);
            usleep(700000);
        }
        CHECK(mv_alt_calls == 3 && mv_commit_multi_calls == 0, "a real failure (no usable view) is not retried at every return home (only a new calibration asks again, 3 tries per run at most)");
    }
    a2k_ptz_stop();
    /* a command during the build abandons it: nothing written, the command runs */
    mv_reset();
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 2;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && wait_count(&mv_alt_calls, 1, 6000), "the build is under way (first offset view kept)");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0, "the user parks the head");
    usleep(1200000);
    CHECK(mv_commit_multi_calls == 0 && wait_state(HAL_PTZ_PARKED, 5000), "the build was abandoned, the park command won, nothing written");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000) && wait_count(&mv_commit_multi_calls, 1, 8000), "an interruption is not a failure: the reference stays due, back at home the build runs again and completes");
    a2k_ptz_stop();
    /* a tracker is active (a nudge): no excursion */
    mv_reset();
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 2;
    reset(2500, 700);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && a2k_ptz_nudge(5, 0) == 0, "calibrated; a tracking nudge arrives");
    usleep(300000);
    a2k_ptz_command(A2K_PTZ_HOME);
    usleep(1200000);
    CHECK(mv_alt_calls == 0 && mv_commit_multi_calls == 0, "no multi-view excursion while a tracker moves the head");
    a2k_ptz_stop();
    /* not at home (the head was nudged away before the capture): no capture, no excursion for a featureless view */
    mv_reset();
    sc_reset(0, 1275, 1300);
    sc_cap_rc = 2;
    reset(2500, 700);
    a2k_ptz_test_set_scene_timing(500, 150, 0);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && a2k_ptz_nudge(80, 0) == 0, "calibrated; a nudge before the capture");
    usleep(1200000);
    CHECK(sc_ncap == 0 && mv_alt_calls == 0 && mv_commit_multi_calls == 0, "away from home: nothing captured, no excursion, nothing stored");
    a2k_ptz_stop();
    a2k_ptz_test_set_scene_timing(60, 150, 0);
    a2k_ptz_set_scene(NULL);
    a2k_ptz_test_set_scene_timing(0, 0, 0);
}

/* ------------------------------------------------------------------------------------------------ PTZ patrol on the real controller */
static volatile int pc_hook_n, pc_hook_last;
static void pc_hook(int kind) { pc_hook_last = kind; pc_hook_n++; }

static char pc_ev[32][128];
static int pc_nev;
static pthread_mutex_t pc_mu = PTHREAD_MUTEX_INITIALIZER;
static char pc_file[PP_TEXT_MAX + 1];
static int pc_file_len = -1, pc_saves;

static int pc_limits(pp_limits_t *l)
{
    uint32_t a, b, c, d;
    int smin, smax, sdef;
    if (a2k_ptz_range(&a, &b, &c, &d) != 0 || a2k_ptz_speeds(&smin, &smax, &sdef) != 0)
        return -1;
    l->pan_min = (int)a;
    l->pan_max = (int)b;
    l->tilt_min = (int)c;
    l->tilt_max = (int)d;
    l->speed_min = sdef;
    (void)smin;
    if (a2k_ptz_park(&l->park_pan, &l->park_tilt) != 0)
        return -1;
    l->park_tol = A2K_PTZ_PARK_TOL;
    l->speed_max = smax;
    l->speed_def = sdef;
    return 0;
}
static int pc_move(int pan, int tilt, int speed, unsigned *ticket)
{
    if (ticket)
        return a2k_ptz_patrol_goto(pan, tilt, speed, ticket);
    return a2k_ptz_goto(pan, tilt, speed) == 0 ? 0 : -1;
}
static void pc_event(const char *type, const char *json)
{
    pthread_mutex_lock(&pc_mu);
    if (!strcmp(type, "patrol") && pc_nev < 32)
        snprintf(pc_ev[pc_nev++], sizeof pc_ev[0], "%s", json);
    pthread_mutex_unlock(&pc_mu);
}
static int pc_has(const char *needle)
{
    int i, f = 0;
    pthread_mutex_lock(&pc_mu);
    for (i = 0; i < pc_nev; i++)
        f |= strstr(pc_ev[i], needle) != NULL;
    pthread_mutex_unlock(&pc_mu);
    return f;
}
static int pc_load(char *buf, size_t cap) { (void)buf; (void)cap; return pc_file_len; }
static int pc_save(const char *text, size_t len)
{
    memcpy(pc_file, text, len);
    pc_file_len = (int)len;
    pc_saves++;
    return 1;
}
static int64_t pc_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int pc_wait_patrol(const char *want, int ms)
{
    char st[16], rt[17];
    int step, t;
    for (t = 0; t < ms; t += 10) {
        a2k_patrol_state(st, sizeof st, rt, sizeof rt, &step);
        if (!strcmp(st, want))
            return 1;
        usleep(10000);
    }
    return 0;
}
static int pc_ticket_done(unsigned tk, int *rc, int ms)
{
    int t;
    for (t = 0; t < ms; t += 5) {
        if (a2k_ptz_patrol_result(tk, rc))
            return 1;
        usleep(5000);
    }
    return 0;
}

static void test_patrol_controller(void)
{
    static const a2k_patrol_ops_t POPS = { a2k_ptz_info, pc_limits, pc_move, a2k_ptz_patrol_result, a2k_ptz_patrol_cancel, a2k_ptz_privacy_active, pc_event, pc_load, pc_save, pc_now, a2k_ptz_scene_pending, a2k_ptz_travel };
    a2k_ptz_cfg_t c;
    unsigned tk = 0, tk2 = 0;
    int rc = -1, smin = 0, smax = 0, sdef = 0, n;
    printf("== PTZ patrol on the real controller (ptz.c + hal_ptz.c on the simulated motor)\n");
    {
        uint32_t a, b, cc, d;
        CHECK(a2k_ptz_speeds(&smin, &smax, &sdef) == -1 && a2k_ptz_range(&a, &b, &cc, &d) == -1 && a2k_ptz_patrol_goto(900, 1200, 3, &tk) == -1 && a2k_ptz_goto(900, 1200, 3) == -1,
              "not started: speeds, range, goto and patrol goto are refused");
    }
    reset(2000, 700);
    a2k_ptz_defaults(&c);
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
    a2k_ptz_set_cmd_hook(pc_hook);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 60000), "started and calibrated");
    CHECK(a2k_ptz_speeds(&smin, &smax, &sdef) == 0 && smin == 2 && smax == 20 && sdef == 3, "the descriptor's allowed speeds: 2..20, default 3");
    pc_hook_n = 0;
    CHECK(a2k_ptz_patrol_goto(900, 1200, 25, &tk) == -1 && a2k_ptz_patrol_goto(900, 1200, 1, &tk) == -1 && a2k_ptz_patrol_goto(100, 1200, 3, &tk) == -1 && a2k_ptz_patrol_goto(900, 5000, 3, &tk) == -1 &&
          a2k_ptz_goto(4000, 1200, 3) == -1 && a2k_ptz_goto(900, 1200, 40) == -1 && pc_hook_n == 0, "a bad speed or a position outside the soft range is refused before anything is queued (no hook)");
    CHECK(a2k_ptz_patrol_goto(900, 1200, 5, &tk) == 0 && tk > 0, "a patrol move is queued (a ticket)");
    CHECK(pc_ticket_done(tk, &rc, 5000) && rc == HAL_PTZ_OK && near_mech(900, 1200, 12) && pc_hook_n == 0, "it ran through the safe move layer to the target; the patrol move does not call the manual hook");
    CHECK(M.spd[0] == 5, "...at the speed asked for (divisor 5)");
    CHECK(a2k_ptz_patrol_result(tk, &rc) == 1 && rc == HAL_PTZ_OK && a2k_ptz_patrol_result(tk + 7, &rc) == 1 && rc == HAL_PTZ_ECANCEL, "the result stays readable; an unknown ticket reads as cancelled");
    /* the park position is never a patrol target; a tracker home does not call the manual hook */
    pc_hook_n = 0;
    {
        int pp = 0, pt = 0;
        CHECK(a2k_ptz_park(&pp, &pt) == 0 && pp == 1275 && pt == 80, "the controller reports the park position");
        CHECK(a2k_ptz_patrol_goto(1275, 80, 3, &tk) == -1 && a2k_ptz_patrol_goto(1288, 92, 3, &tk) == -1 && a2k_ptz_patrol_goto(1291, 300, 3, &tk) == 0, "a patrol move to the park position (below the soft range: refused) is refused, the soft minimum 300 is an ordinary target now");
        CHECK(pc_ticket_done(tk, &rc, 5000), "(it ran)");
        pc_hook_n = 0;
        /* Race-free: the head is MOVED (away from home) before the call, so waiting for the state MOVED proves nothing and, on a loaded host, the
         * whole home move can finish between two polls of the transient state. What "queued" means is that the motor was commanded: count it. */
        {
            int nm0;
            pthread_mutex_lock(&M.gm);
            nm0 = M.nmove;
            pthread_mutex_unlock(&M.gm);
            CHECK(a2k_ptz_home_tracker() == 0 && wait_nmove_above(nm0, 5000), "the tracker's home is queued (the motor is commanded)");
        }
        CHECK(wait_state(HAL_PTZ_HOME, 5000) && pc_hook_n == 0, "...it goes home and never calls the manual-command hook");
        CHECK(a2k_ptz_goto(900, 1200, 0) == 0 && wait_state(HAL_PTZ_MOVED, 5000) && (usleep(200000), near_mech(900, 1200, 12)), "(away from home again)");
        pc_hook_n = 0;
    }
    /* a patrol move waits for an idle controller: it never preempts */
    set_gate(1);
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_blocked() && pc_hook_n == 1 && pc_hook_last == A2K_PTZ_HOME, "a manual home runs (held); the hook saw A2K_PTZ_HOME");
    CHECK(a2k_ptz_patrol_goto(1500, 1300, 0, &tk2) == 1, "a patrol move while a command runs: busy (1), nothing queued");
    CHECK(a2k_ptz_nudge_sp(10, 0, 3) == -1 && pc_hook_n == 1, "(a tracker nudge is refused while busy and never calls the hook)");
    a2k_ptz_patrol_cancel();
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_HOME, 5000) && near_mech(1275, 1300, 12), "...a patrol cancel does not touch another command's motion: home completed");
    /* cancel and replace */
    set_gate(1);
    CHECK(a2k_ptz_patrol_goto(2500, 1300, 0, &tk) == 0 && wait_blocked(), "a patrol move is running (held)");
    a2k_ptz_patrol_cancel();
    set_gate(0);
    CHECK(pc_ticket_done(tk, &rc, 5000) && rc == HAL_PTZ_ECANCEL && !near_mech(2500, 1300, 40), "a patrol cancel stops its own move (ECANCEL), the target was not reached");
    pc_hook_n = 0;
    set_gate(1);
    CHECK(a2k_ptz_patrol_goto(2500, 1300, 0, &tk) == 0 && wait_blocked(), "again a patrol move runs (held)");
    CHECK(a2k_ptz_command(A2K_PTZ_LEFT) == 0 && pc_hook_n == 1 && pc_hook_last == A2K_PTZ_LEFT, "a manual step replaces it and calls the hook");
    set_gate(0);
    CHECK(pc_ticket_done(tk, &rc, 5000) && rc == HAL_PTZ_ECANCEL, "the replaced patrol move reads as cancelled");
    CHECK(wait_state(HAL_PTZ_MOVED, 5000), "(the step ran)");
    usleep(150000);
    pc_hook_n = 0;
    CHECK(a2k_ptz_nudge(20, 0) == 0 && pc_hook_n == 1 && pc_hook_last == A2K_PTZ_HOOK_NUDGE, "a manual nudge calls the hook (HOOK_NUDGE)");
    usleep(150000);
    CHECK(a2k_ptz_nudge_sp(20, 0, 3) == 0 && pc_hook_n == 1, "a tracker nudge does not");
    usleep(150000);
    CHECK(a2k_ptz_goto(1200, 1250, 0) == 0 && pc_hook_n == 2 && pc_hook_last == A2K_PTZ_HOOK_GOTO, "a manual goto calls the hook (HOOK_GOTO)");
    CHECK(wait_state(HAL_PTZ_MOVED, 5000) && (usleep(150000), near_mech(1200, 1250, 12)), "...and reaches the target");

    /* the patrol core on this controller */
    printf("== PTZ patrol core on the real controller: route, tracking pause, manual stop, flash\n");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "(home)");
    pc_nev = 0;
    pc_saves = 0;
    pc_file_len = -1;
    a2k_patrol_init(&POPS);
    a2k_ptz_set_cmd_hook(a2k_patrol_note_cmd); /* the real hook: a manual command stops the patrol */
    {
        char out[2048];
        unsigned retry = 0;
        const char *r1 = "{\"name\":\"yard\",\"mode\":\"loop\",\"steps\":[{\"pan\":900,\"tilt\":1200,\"speed\":4,\"dwell_ms\":300},{\"pan\":1800,\"tilt\":1500,\"speed\":3,\"dwell_ms\":300},{\"pan\":2400,\"tilt\":900,\"dwell_ms\":300}]}";
        CHECK(a2k_patrol_http("POST", "routes", r1, strlen(r1), out, sizeof out, &retry) == 200 && pc_saves == 1, "a route is stored: one write");
    }
    CHECK(a2k_patrol_thread_start() == 0, "the patrol thread starts");
    n = file_writes("/config/akam/ptz.pos") + file_writes("/config/akam/ptz.scene");
    CHECK(a2k_patrol_start_route("yard") == PP_OK, "start the route");
    CHECK(pc_wait_patrol("running", 1000), "running");
    {
        int seen[3] = { 0, 0, 0 }, t;
        for (t = 0; t < 2400 && !(seen[0] && seen[1] && seen[2]); t++) {
            seen[0] |= near_mech(900, 1200, 15);
            seen[1] |= near_mech(1800, 1500, 15);
            seen[2] |= near_mech(2400, 900, 15);
            usleep(5000);
        }
        CHECK(seen[0] && seen[1] && seen[2], "the head visits the three steps in the real motor");
    }
    CHECK(pc_has("\"event\":\"started\"") && pc_has("\"event\":\"step\""), "events");
    {
        int t, ok = 1;
        for (t = 0; t < 200; t++) { /* the soft range is never left, whatever the route */
            if (M.mech[0] < 300 || M.mech[0] > 3650 || M.mech[1] < 300 || M.mech[1] > 2100)
                ok = 0;
            usleep(2000);
        }
        CHECK(ok && M.bad_cmd == 0, "the mechanical position stays inside the soft range during the patrol, no forbidden driver command");
    }
    a2k_patrol_note_person(1, 600);
    CHECK(pc_wait_patrol("paused", 1000), "a tracked person pauses it");
    CHECK(pc_has("\"event\":\"interrupted\"") && pc_has("\"reason\":\"tracking\""), "event interrupted");
    {
        int before = M.nmove;
        usleep(300000);
        CHECK(M.nmove == before, "the head does not move while paused");
        CHECK(a2k_ptz_nudge_sp(30, 0, 3) == 0, "the tracker can move it (idle controller)");
    }
    CHECK(pc_wait_patrol("running", 8000) && pc_has("\"event\":\"resumed\""), "it resumes after the return time");
    CHECK(a2k_ptz_command(A2K_PTZ_RIGHT) == 0 && pc_wait_patrol("idle", 2000) && pc_has("\"reason\":\"manual\""), "a manual command (a step) stops it");
    CHECK(a2k_patrol_start_route("yard") == PP_OK && pc_wait_patrol("running", 500), "start again");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && pc_wait_patrol("idle", 2000) && pc_has("\"reason\":\"park\""), "the park command stops it (reason park)");
    CHECK(wait_state(HAL_PTZ_PARKED, 5000), "...and the head parks");
    CHECK(a2k_ptz_command(A2K_PTZ_HOME) == 0 && wait_state(HAL_PTZ_HOME, 5000), "(home)");
    CHECK(a2k_patrol_start_route("yard") == PP_OK && pc_wait_patrol("running", 500), "start once more");
    usleep(300000);
    {
        int r1 = a2k_ptz_command(A2K_PTZ_RECAL), r2 = pc_wait_patrol("idle", 2000), r3 = pc_has("\"reason\":\"calibration\"");
        if (!(r1 == 0 && r2 && r3))
            printf("    recal: %d %d %d, events %d: %s | %s\n", r1, r2, r3, pc_nev, pc_nev > 0 ? pc_ev[pc_nev - 1] : "", pc_nev > 1 ? pc_ev[pc_nev - 2] : "");
        CHECK(r1 == 0 && r2 && r3, "a recalibration stops it (reason calibration)");
    }
    CHECK(wait_state(HAL_PTZ_HOME, 60000), "(calibrated)");
    CHECK(a2k_patrol_start_route("yard") == PP_OK, "and starts again");
    usleep(300000);
    a2k_patrol_stop();
    CHECK(pc_has("\"reason\":\"shutdown\"") && !a2k_patrol_active(), "shutdown cancels the patrol before the motor stops");
    CHECK(pc_saves == 1 && file_writes("/config/akam/ptz.pos") + file_writes("/config/akam/ptz.scene") == n, "zero flash writes while patrolling: one config save in total, no motor file written");
    a2k_ptz_stop();
    a2k_ptz_set_cmd_hook(NULL);
    CHECK(M.bad_cmd == 0, "no forbidden driver command word during the whole patrol test");
}

/* ---- park = the closed pose (below the soft range, never against the stop), the boot that skips the calibration, the bounded re-home, the counters */
static int g_boot_parked;
static int bp_hook(uint32_t *pan, uint32_t *tilt)
{
    *pan = 1275;
    *tilt = 80;
    return g_boot_parked;
}

static void test_park_closed(void)
{
    hal_ptz_t *p;
    hal_ptz_desc_t d = a2k_ptz_desc;
    hal_ptz_ops_t bo = OPS;
    char err[96] = "";
    unsigned long g, st0, st1;
    printf("== park: the closed pose, the margin to the stop, the boot after a park, the bounded re-home\n");
    /* park from home: below the soft range (300), 80 steps above the stop, not one step against it; home again; the soft range still holds for everything else */
    p = booted(1275, 1300);
    g = M.grind;
    CHECK(p && hal_ptz_park(p) == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && near_mech(1275, 80, 4) && pos_is(p, 1275, 80, 3), "park goes to 1275/80: tilt below the soft minimum 300, 80 steps above the bottom stop");
    CHECK(M.grind == g && M.mech[1] > 50, "...without a single step against the stop (no stall, no grinding)");
    CHECK(hal_ptz_goto(p, 1275, 200) == HAL_PTZ_EREFUSED && near_mech(1275, 80, 4), "a goto below the soft range is refused even while parked (only park may go there)");
    CHECK(hal_ptz_nudge(p, 0, -100) == 0 && M.grind == g && M.mech[1] >= 79, "a nudge down from the closed pose does not go lower");
    CHECK(hal_ptz_park(p) == 0 && M.grind == g && near_mech(1275, 80, 4), "park again while parked: stays, no stall");
    CHECK(hal_ptz_home(p) == 0 && hal_ptz_state(p) == HAL_PTZ_HOME && pos_is(p, 1275, 1300, 4) && M.grind == g, "open: back to home 1275/1300, trusted, no stall");
    CHECK(hal_ptz_goto(p, 1275, 299) == HAL_PTZ_EREFUSED && hal_ptz_goto(p, 1275, 300) == 0 && near_mech(1275, 300, 5) && M.grind == g, "the soft range of every other motion is unchanged: 299 refused, 300 reached");
    CHECK(hal_ptz_nudge(p, 0, -900) == 0 && M.mech[1] >= 296 && hal_ptz_step(p, HAL_PTZ_DOWN) == HAL_PTZ_EREFUSED && M.mech[1] >= 296 && M.grind == g, "a nudge down stops at the soft minimum 300, a step down from it is refused (nothing moves)");
    CHECK(hal_ptz_park(p) == 0 && near_mech(1275, 80, 4) && hal_ptz_home(p) == 0 && near_mech(1275, 1300, 4) && M.grind == g, "park from the soft minimum and back");
    /* ten park / open cycles: the counters stay equal to the mechanics (no step lost, the position stays trusted) */
    for (int k = 0; k < 10; k++)
        if (hal_ptz_park(p) || hal_ptz_home(p))
            break;
    CHECK(M.grind == g && near_mech(1275, 1300, 6) && hal_ptz_verify_pos(p) == 0, "10 park / open cycles: no stall, the driver counters agree with the believed position");
    /* the old explicit override keeps working (the default changed, the override did not) */
    d.pan_park = 1275;
    d.tilt_park = 300;
    reset(1275, 1300);
    p = hal_ptz_new(&d, &OPS, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && hal_ptz_park(p) == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && near_mech(1275, 300, 5) && hal_ptz_home(p) == 0, "ptz_park 1275:300 (the old default) still parks at 300 and opens");

    /* the boot after a park: declared at the park pose, no calibration, no stall; a different configured park falls back to the usual start */
    bo.boot_pose = bp_hook;
    g_boot_parked = 1;
    reset(1275, 80);
    d = a2k_ptz_desc;
    p = hal_ptz_new(&d, &bo, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.nmove == 0 && M.grind == 0 && hal_ptz_state(p) == HAL_PTZ_PARKED && pos_is(p, 1275, 80, 0) && strstr(M.log, "last left parked"),
          "boot after a park: position declared, no motor command at all, no calibration, state parked");
    CHECK(hal_ptz_home(p) == 0 && near_mech(1275, 1300, 4) && M.grind == 0 && hal_ptz_verify_pos(p) == 0, "...and the first open goes home from the closed pose, trusted");
    d.tilt_park = 300;
    reset(1275, 300);
    p = hal_ptz_new(&d, &bo, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0 && !strstr(M.log, "last left parked"), "a record made for another park position is ignored: the usual start (calibration)");
    g_boot_parked = 0;
    reset(1275, 1300);
    d = a2k_ptz_desc;
    p = hal_ptz_new(&d, &bo, NULL, err, sizeof err);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0 && M.grind > 0, "no park record: the usual start");

    /* full stroke ONCE when the position is unknown (before: every boot); the estimate and the counters */
    reset(1275, 1300);
    p = mk(NULL);
    CHECK(p && hal_ptz_start(p, HAL_PTZ_CAL_AUTO) == 0, "cold boot, position unknown: the full-stroke calibration");
    hal_ptz_stall_stats(p, &st0, NULL, NULL, NULL);
    printf("  info: full calibration from home: real skipped steps %lu, estimated %lu\n", M.grind, st0);
    CHECK(st0 > 0 && M.grind > 0, "the full calibration is counted as skipped steps (estimate > 0)");
    {
        unsigned cf = 9, cs = 9, rh = 9;
        hal_ptz_stall_stats(p, NULL, &cf, &cs, &rh);
        CHECK(cf == 1 && cs == 0 && rh == 0, "counters: one full calibration, no short one, no re-home");
    }

    /* the tilt re-home: bounded (believed distance + 250) against the full stroke (before) */
    g = M.grind;
    st0 = 0;
    hal_ptz_stall_stats(p, &st0, NULL, NULL, NULL);
    CHECK(hal_ptz_tilt_rehome(p) == 0 && near_mech(1275, 1300, 6), "re-home, full stroke (what every re-home did before)");
    {
        unsigned long full_real = M.grind - g, full_est;
        hal_ptz_stall_stats(p, &full_est, NULL, NULL, NULL);
        full_est -= st0;
        g = M.grind;
        st0 += full_est;
        CHECK(hal_ptz_tilt_rehome_bounded(p) == 0 && near_mech(1275, 1300, 6) && pos_is(p, 1275, 1300, 0), "re-home, bounded: at home again, trusted");
        hal_ptz_stall_stats(p, &st1, NULL, NULL, NULL);
        printf("  info: re-home from home: full stroke skipped %lu real / %lu est., bounded %lu real / %lu est.\n", full_real, full_est, M.grind - g, st1 - st0);
        CHECK(M.grind - g <= 260 && M.grind - g > 0 && (M.grind - g) * 3 < full_real, "the bounded re-home skips at most 260 steps against the stop, under a third of the full stroke");
        CHECK(st1 - st0 <= 260 && st1 - st0 >= 200, "its estimate is the margin (about 250)");
        CHECK(hal_ptz_verify_pos(p) == 0 && M.mech[1] >= 1295 && M.mech[1] <= 1305, "after it the counters agree with the head");
    }
    /* a slip inside the margin is corrected by the bounded re-home: the head is really 150 steps lower than believed */
    g = M.grind;
    M.mech[1] -= 150;
    CHECK(hal_ptz_tilt_rehome_bounded(p) == 0 && near_mech(1275, 1300, 8), "a real slip of 150 steps (inside the margin): the bounded re-home puts the tilt right");
    /* a believed tilt too high for the margin falls back to the full stroke */
    CHECK(hal_ptz_goto(p, 1275, 2000) == 0, "go up to 2000");
    g = M.grind;
    {
        size_t l0 = strlen(M.log);
        CHECK(hal_ptz_tilt_rehome_bounded(p) == 0 && near_mech(1275, 1300, 8) && strstr(M.log + l0, "tilt re-homed (full stroke)"), "a believed tilt above 1910: the margin does not fit below the stock maximum, the full stroke runs");
    }
}

/* ---- the controller: long press (toggle) = park / open, a toggle while moving, the park record (one flash write per change), the boot after a park */
static void test_park_controller(void)
{
    static const char *const SP = "/config/akam/ptz.conf.state";
    a2k_ptz_cfg_t c;
    a2k_ptz_info_t i;
    unsigned long grind0, est = 0;
    unsigned cf = 0, cs = 0, rh = 0;
    char rec[64];
    printf("== controller: long press = closed pose / back home, toggle while moving, the park record, the boot after a park\n");
    reset(1275, 1300);
    a2k_ptz_defaults(&c);
    c.privacy = 1;
    a2k_ptz_test_set(&OPS, "/config/akam/ptz.conf");
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000), "start: calibrated at home");
    grind0 = M.grind;
    a2k_ptz_stall_info(&est, &cf, &cs, &rh);
    CHECK(cf == 1 && est > 0 && file_writes(SP) == 0, "counters: one full calibration, estimated skipped steps > 0; no park record written by a boot");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_PARKED, 5000) && a2k_ptz_privacy_active(), "long press at home: parked, video withheld");
    a2k_ptz_info(&i);
    CHECK(i.tilt >= 77 && i.tilt <= 83 && near_mech(1275, 80, 5) && M.grind == grind0, "the lens is at the closed pose (tilt 80), not against the stop");
    CHECK(file_writes(SP) == 1 && !strncmp(get_file(SP), "parked 1275 80", 14), "one record written: parked 1275 80");
    CHECK(a2k_ptz_command(A2K_PTZ_PARK) == 0 && usleep(300000) == 0 && file_writes(SP) == 1, "parking again writes nothing");
    /* a toggle while the head is moving: the open is interrupted, the next long press parks again, ends parked, no stall */
    set_gate(1);
    CHECK(a2k_ptz_toggle_park() == 0 && wait_blocked(), "long press when parked: opening, the motor thread is inside the move");
    a2k_ptz_info(&i);
    CHECK(i.state == HAL_PTZ_MOVED && file_writes(SP) == 2 && !strncmp(get_file(SP), "open", 4), "while moving: state moved; the record already says open (before the head left)");
    CHECK(a2k_ptz_toggle_park() == 0, "a long press while moving is accepted (it replaces the running move)");
    set_gate(0);
    CHECK(wait_state(HAL_PTZ_PARKED, 8000) && near_mech(1275, 80, 5) && M.grind == grind0, "...and ends parked at the closed pose, no stall");
    usleep(100000);
    CHECK(file_writes(SP) == 3 && !strncmp(get_file(SP), "parked 1275 80", 14), "the record says parked again (3 writes in all: park, open, park)");
    CHECK(a2k_ptz_nudge(0, -50) < 0, "a manual move while parked is refused");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_HOME, 8000) && near_mech(1275, 1300, 6) && !a2k_ptz_privacy_active(), "long press again: back home, privacy off");
    CHECK(file_writes(SP) == 4 && !strncmp(get_file(SP), "open", 4), "the record says open (written before the head left)");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_PARKED, 5000), "parked once more for the boot test");
    snprintf(rec, sizeof rec, "%s", get_file(SP));
    a2k_ptz_stop();
    /* power cycle: the driver and the files are gone, the record stays; the head is where it was left */
    reset(1275, 80);
    put_file(SP, rec);
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_PARKED, 5000) && M.nmove == 0 && M.grind == 0 && file_writes(SP) == 0, "boot after a park: parked at once, no motor command, no calibration, no write");
    a2k_ptz_stall_info(&est, &cf, &cs, &rh);
    CHECK(est == 0 && cf == 0 && cs == 0, "counters of that boot: 0 estimated skipped steps, 0 calibrations");
    CHECK(a2k_ptz_toggle_park() == 0 && wait_state(HAL_PTZ_HOME, 8000) && near_mech(1275, 1300, 6) && M.grind == 0 && file_writes(SP) == 1, "open: home, no stall, the record cleared with one write");
    a2k_ptz_stop();
    /* a record that says open (the open record was written, the power failed during the open): the usual start */
    reset(1275, 1300);
    put_file(SP, "open\n");
    CHECK(a2k_ptz_start(&c) == 0 && wait_state(HAL_PTZ_HOME, 20000) && M.grind > 0, "record 'open': the usual start (calibration)");
    a2k_ptz_stop();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_desc();
    test_calibration();
    test_skip_and_off();
    test_motion();
    test_park_closed();
    test_persistence();
    test_real_files();
    test_thread();
    test_park_controller();
    test_scene();
    test_multiview();
    test_patrol_controller();
    printf("rot2k hal_ptz unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
