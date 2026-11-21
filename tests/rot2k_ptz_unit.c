/* Host unit test of akam_ptz.c + akam_ptz_safe.h against a simulated motor: a step-counting driver (commands 0..7,
 * stop, selector reads, init requests) on top of a "mechanical" position with hard stops. The test fails when the
 * simulated mechanism ever reaches a stop (hit_stop) or the code sends a driver command other than 0..7. */
#define AKAM_PTZ_NO_MAIN
#include "../firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz.c"
#include <setjmp.h>

static FILE *out;
static int fails;
#define CHECK(c, msg) do { if (c) fprintf(out, "  ok   - %s\n", msg); else { fprintf(out, "  FAIL - %s\n", msg); fails++; } } while (0)

struct model {
    uint32_t cnt[2], tgt[2], spd[2], drvmax[2];
    int running[2];
    int mech_lo[2], mech_hi[2], real[2];
    int hit_stop, bad_cmd, nmove, nstop, nreq, ninit1, ninit2, npos;
    int stuck[2], reverse[2];
    unsigned slow, jitter;
    unsigned acc[2];
    int fail_pos_after;     /* pos reads fail from this count on (-1 never) */
    int stop_fail, move_fail, open_errno;
    uint32_t last_lim[4], last_ipos[10];
    unsigned long clock;
    unsigned long stop_at;  /* clock of the last stop */
    unsigned long move_at;  /* clock of the last move request */
    unsigned dirs;          /* bit mask of the commands seen */
    unsigned last_speed[2];
    int conf_present, conf_err, pos_present, save_fail;
    char conf[1024], posfile[64];
    int frame_present, frame_fail;
    char scene[16];
    char appframe[64], savedpos[64]; /* the app's frame file and ptz.pos, as the tool leaves them */
    char frame[128];
    int off[2];             /* physical truth: real position - driver counter, learnt when the driver is declared */
    int init_fail, init_ignore_pos;
    long kill_ms;           /* >= 0: the tool is SIGKILLed (longjmp) when this many more ms have been slept */
} M;
static jmp_buf killjb;

static void model_reset(void)
{
    memset(&M, 0, sizeof(M));
    M.drvmax[0] = 4000; M.drvmax[1] = 2160;
    M.spd[0] = 5; M.spd[1] = 4;
    M.mech_lo[0] = 150; M.mech_hi[0] = 3850;   /* the mechanical stops lie inside 0..max: worst case */
    M.mech_lo[1] = 100; M.mech_hi[1] = 2320;
    M.real[0] = 1275; M.real[1] = 816;
    M.slow = 1; M.fail_pos_after = -1; M.kill_ms = -1;
    M.cnt[0] = M.cnt[1] = 0;   /* cold driver: counters 0/0, camera physically at the stock home */
    /* the cases below were written for the first (unmeasured) range; they keep it through ptz.conf, the CODE defaults are checked in test_conf */
    M.conf_present = 1;
    snprintf(M.conf, sizeof(M.conf), "pan_min=300\npan_max=3700\ntilt_min=200\ntilt_max=1800\npan_home=1275\ntilt_home=816\n");
}

static void advance(unsigned ms)
{
    unsigned i, ax;
    for (i = 0; i < ms; i++) {
        M.clock++;
        for (ax = 0; ax < 2; ax++) {
            int step;
            if (!M.running[ax] || M.cnt[ax] == M.tgt[ax]) { M.running[ax] = 0; continue; }
            if (++M.acc[ax] < M.spd[ax] * M.slow) continue;
            M.acc[ax] = 0;
            if (M.stuck[ax]) continue;
            step = M.tgt[ax] > M.cnt[ax] ? 1 : -1;
            if (M.reverse[ax]) step = -step;
            if (step < 0 && M.cnt[ax] == 0) continue;
            M.cnt[ax] += step;
            M.real[ax] += step;
            if (M.real[ax] < M.mech_lo[ax]) { M.real[ax] = M.mech_lo[ax]; M.hit_stop++; }
            if (M.real[ax] > M.mech_hi[ax]) { M.real[ax] = M.mech_hi[ax]; M.hit_stop++; }
        }
    }
}

static int s_open(void) { if (M.open_errno) { errno = M.open_errno; return -1; } return 5; }
static int s_ioctl(int fd, unsigned long req, void *arg)
{
    uint32_t *w = arg;
    if (fd != 5) return -1;
    M.nreq++;
    if (req == PTZ_REQ_STOP) {
        M.nstop++; M.stop_at = M.clock;
        if (M.stop_fail > 0) { M.stop_fail--; errno = EIO; return -1; }
        M.running[0] = M.running[1] = 0;
        return 0;
    }
    if (req == PTZ_REQ_POS) {
        M.npos++;
        if (M.fail_pos_after >= 0 && M.npos > M.fail_pos_after) { errno = EIO; return -1; }
        w[1] = w[0] > 1 ? 0 : M.cnt[w[0]];
        return 0;
    }
    if (req == PTZ_REQ_MOVE) {
        unsigned cmd = w[0];
        if (M.move_fail) { errno = EIO; return -1; }
        if (cmd > 7) { M.bad_cmd++; return 0; }  /* 8..13: stop, move_to, check sweeps: must never be sent */
        M.nmove++; M.move_at = M.clock; M.dirs |= 1u << cmd;
        if (w[1]) M.spd[0] = w[1];
        if (w[2]) M.spd[1] = w[2];
        M.last_speed[0] = w[1]; M.last_speed[1] = w[2];
        if (PTZS_PAN_SIGN[cmd]) { M.tgt[0] = PTZS_PAN_SIGN[cmd] > 0 ? M.drvmax[0] : 0; M.running[0] = 1; }
        if (PTZS_TILT_SIGN[cmd]) { M.tgt[1] = PTZS_TILT_SIGN[cmd] > 0 ? M.drvmax[1] : 0; M.running[1] = 1; }
        return 0;
    }
    if (req == PTZ_REQ_INIT_LIMITS) {
        M.ninit1++; memcpy(M.last_lim, w, 16);
        if (M.init_fail) { errno = EIO; return -1; }
        if (w[1]) M.drvmax[0] = w[1];
        if (w[2]) M.drvmax[1] = w[2];
        return 0;
    }
    if (req == PTZ_REQ_INIT_POS) {
        M.ninit2++; memcpy(M.last_ipos, w, 40);
        if (M.init_ignore_pos) return 0;
        if (w[0]) { M.cnt[0] = w[0]; M.off[0] = M.real[0] - (int)w[0]; }
        if (w[1]) { M.cnt[1] = w[1]; M.off[1] = M.real[1] - (int)w[1]; }
        return 0;
    }
    M.bad_cmd++;
    return -1;
}
static void s_sleep(unsigned ms)
{
    if (M.kill_ms >= 0) {
        if ((long)ms >= M.kill_ms) {
            advance((unsigned)M.kill_ms);
            M.kill_ms = -1;
            longjmp(killjb, 1);   /* SIGKILL: no handler, no stop request, the driver keeps going */
        }
        M.kill_ms -= ms;
    }
    advance(ms + M.jitter);
}
static int s_load(const char *path, char *buf, unsigned cap)
{
    const char *src = NULL;
    if (!strcmp(path, PTZ_CONF_PATH)) {
        if (M.conf_err) { errno = EACCES; return -1; }
        if (M.conf_present) src = M.conf;
    } else if (!strcmp(path, PTZ_POS_PATH)) {
        if (M.pos_present) src = M.posfile;
    } else if (!strcmp(path, PTZ_FRAME_PATH)) {
        if (M.frame_present) src = M.frame;
    }
    if (!src) { errno = ENOENT; return -1; }
    if (strlen(src) >= cap) { errno = EFBIG; return -1; }
    strcpy(buf, src);
    return (int)strlen(src);
}
static int s_save(const char *path, const char *data)
{
    if (!strcmp(path, PTZ_FRAME_PATH)) {
        if (M.frame_fail) { errno = EROFS; return -1; }
        snprintf(M.frame, sizeof(M.frame), "%s", data);
        M.frame_present = 1;
        return 0;
    }
    if (!strcmp(path, PTZ_APP_FRAME_PATH)) { snprintf(M.appframe, sizeof(M.appframe), "%s", data); return 0; }
    if (!strcmp(path, PTZ_SAVED_POS_PATH)) { snprintf(M.savedpos, sizeof(M.savedpos), "%s", data); return 0; }
    if (!strcmp(path, PTZ_SCENE_PATH)) { snprintf(M.scene, sizeof(M.scene), "%s", data); return 0; } /* the app's scene reference: the tool only invalidates it */
    if (strcmp(path, PTZ_POS_PATH)) { M.bad_cmd++; return -1; }  /* nothing else may ever be written */
    if (M.save_fail) { errno = EROFS; return -1; }
    snprintf(M.posfile, sizeof(M.posfile), "%s", data);
    M.pos_present = 1;
    return 0;
}
static const struct ptz_ops ops = {s_open, s_ioctl, s_sleep, s_load, s_save};

static int parse(const char *a1, const char *a2, const char *a3, const char *a4, const char *a5, struct ptz_cmd *c)
{
    char *v[7] = {"akam_ptz", (char *)a1, (char *)a2, (char *)a3, (char *)a4, (char *)a5, NULL};
    int n = 1;
    while (n < 6 && v[n]) n++;
    return ptz_parse(n, v, c);
}
static int go(const char *a1, const char *a2, const char *a3, const char *a4, const char *a5)
{
    struct ptz_cmd c;
    if (parse(a1, a2, a3, a4, a5, &c)) return -100;
    return ptz_run(&c, &ops);
}
static void set_conf(const char *text) { M.conf_present = 1; snprintf(M.conf, sizeof(M.conf), "%s", text); }
/* the camera is at the stock home and the tool declares it (default or current ptz.conf frame) */
static void model_ready_conf(const char *conf)
{
    model_reset();
    if (conf) set_conf(conf);
    go("init", "--i-know", 0, 0, 0);
    M.nreq = M.nmove = M.nstop = M.npos = M.ninit1 = M.ninit2 = 0;
}
static void model_ready(void) { model_ready_conf(NULL); }
/* put the camera physically at real position p/t; the driver counters follow through the declared offset */
static void place(int p, int t)
{
    M.real[0] = p; M.real[1] = t;
    M.cnt[0] = (uint32_t)(p - M.off[0]); M.cnt[1] = (uint32_t)(t - M.off[1]);
}
/* the tool's stdout of one run, for the checks of what `pos` prints */
static char cap[256];
static int go_cap(const char *a1, const char *a2, const char *a3, const char *a4, const char *a5)
{
    int rc, saved;
    FILE *t = tmpfile();
    size_t n;
    fflush(stdout);
    saved = dup(1);
    dup2(fileno(t), 1);
    rc = go(a1, a2, a3, a4, a5);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(t);
    n = fread(cap, 1, sizeof(cap) - 1, t);
    cap[n] = 0;
    fclose(t);
    return rc;
}

static void test_parse(void)
{
    struct ptz_cmd c;
    CHECK(PTZ_REQ_STOP == 0x0c00 && PTZ_REQ_POS == 0x801c0c01u && PTZ_REQ_MOVE == 0x401c0c02u, "request numbers");
    CHECK(PTZ_REQ_MOVE == (1u << 30 | 28u << 16 | 0x0cu << 8 | 2u), "move request is _IOW(0x0c,2,28)");
    CHECK(PTZ_REQ_POS == (2u << 30 | 28u << 16 | 0x0cu << 8 | 1u), "pos request is _IOR(0x0c,1,28)");
    CHECK(PTZ_REQ_INIT_LIMITS == (1u << 30 | 16u << 16 | 0x0cu << 8 | 3u), "init limits request is _IOW(0x0c,3,16)");
    CHECK(PTZ_REQ_INIT_POS == (1u << 30 | 40u << 16 | 0x0cu << 8 | 4u), "init position request is _IOW(0x0c,4,40)");
    CHECK(parse("stop", 0, 0, 0, 0, &c) == 0 && c.kind == PTZ_STOP, "parse stop");
    CHECK(parse("pos", 0, 0, 0, 0, &c) == 0 && c.kind == PTZ_POS, "parse pos");
    CHECK(parse("limits", 0, 0, 0, 0, &c) == 0 && c.kind == PTZ_LIMITS, "parse limits");
    CHECK(parse("park", 0, 0, 0, 0, &c) == 0 && c.kind == PTZ_PARK, "parse park");
    CHECK(parse("restore", 0, 0, 0, 0, &c) == 0 && c.kind == PTZ_RESTORE, "parse restore");
    CHECK(parse("move", "3", "300", 0, 0, &c) == 0 && c.dir == 3 && c.ms == 300 && !c.a && !c.b, "parse move");
    CHECK(parse("move", "7", "1500", "10", "20", &c) == 0 && c.a == 10 && c.b == 20, "parse move with a b");
    CHECK(parse("move", "8", "300", 0, 0, &c) < 0, "dir 8 refused");
    CHECK(parse("move", "-1", "300", 0, 0, &c) < 0, "negative dir refused");
    CHECK(parse("move", "0", "1501", 0, 0, &c) < 0, "over the 1500 ms cap refused");
    CHECK(parse("move", "0", "0", 0, 0, &c) < 0, "0 ms refused");
    CHECK(parse("move", "0", "3x", 0, 0, &c) < 0, "trailing junk refused");
    CHECK(parse("move", "0", "300", "3", 0, &c) < 0, "lone a refused");
    CHECK(parse("move", "0", "300", "21", "3", &c) < 0 && parse("move", "0", "300", "3", "21", &c) < 0, "speed divisor above 20 refused");
    CHECK(parse("move", "0", "300", "0", "5", &c) < 0 && parse("move", "0", "300", "1", "5", &c) < 0, "speed 0 (keep) and 1 (~1000 steps/s) refused");
    CHECK(parse("move", "0", "300", "2", "20", &c) == 0 && c.a == 2 && c.b == 20, "speed 2 and 20 accepted");
    CHECK(parse("goto", "1300", "800", 0, 0, &c) == 0 && c.kind == PTZ_GOTO && c.pan == 1300 && c.tilt == 800 && !c.speed, "parse goto");
    CHECK(parse("goto", "1300", "800", "4", 0, &c) == 0 && c.speed == 4, "parse goto with speed");
    CHECK(parse("goto", "1300", "800", "1", 0, &c) < 0 && parse("goto", "1300", "800", "0", 0, &c) < 0, "goto speed below 2 refused");
    CHECK(parse("goto", "1300", 0, 0, 0, &c) < 0 && parse("goto", "-1", "5", 0, 0, &c) < 0 && parse("goto", "7x", "5", 0, 0, &c) < 0, "malformed goto refused");
    CHECK(parse("raw", "401c0c03", 0, 0, 0, &c) < 0, "raw refused");
    CHECK(parse("cal", 0, 0, 0, 0, &c) < 0 && parse("cal", "--i-know", 0, 0, 0, &c) < 0, "cal is refused (not implemented on purpose)");
    CHECK(parse("move_to", "1", "1", 0, 0, &c) < 0, "unknown command refused");
    CHECK(parse("stop", "x", 0, 0, 0, &c) < 0, "stop with extra argument refused");
    CHECK(parse("init", 0, 0, 0, 0, &c) < 0, "init without --i-know refused");
    CHECK(parse("init", "--i-know", "1", 0, 0, &c) < 0, "init with a partial value list refused");
    CHECK(parse("init", "--know", 0, 0, 0, &c) < 0, "init with a wrong flag refused");
    CHECK(parse("init", "--i-know", 0, 0, 0, &c) == 0 && c.kind == PTZ_INIT && !c.init_explicit, "init without values: position from ptz.conf home");
    CHECK(parse("init", "--i-know", "1300", "800", 0, &c) == 0 && c.init_explicit && c.pan == 1300 && c.tilt == 800, "init with the real position now");
    {
        char *v[8] = {"akam_ptz", "init", "--i-know", "4000", "2160", "1300", "800", NULL};
        CHECK(ptz_parse(7, v, &c) < 0, "the old 4-value init form is refused (limits are derived now)");
    }
    CHECK(parse("init", "--i-know", "0", "800", 0, &c) < 0 && parse("init", "--i-know", "1300", "0", 0, &c) < 0, "zero position refused");
    CHECK(parse("init", "--i-know", "70000", "800", 0, &c) < 0 && parse("init", "--i-know", "8x", "800", 0, &c) < 0 && parse("init", "--i-know", "-1", "800", 0, &c) < 0,
          "huge, junk and negative positions refused");
}

static void test_conf(void)
{
    struct ptzs_conf c;
    char err[128];
    ptzs_conf_defaults(&c);
    CHECK(ptzs_conf_check(&c, err, sizeof(err)) == 0 && c.pan_min == 300 && c.pan_max == 3650 && c.tilt_min == 300 && c.tilt_max == 2100 &&
          c.pan_home == 1275 && c.tilt_home == 1300 && c.pan_step_max == 4000 && c.tilt_step_max == 2160 && c.speed == 3,
          "defaults are valid and measured: pan 300..3650, tilt 300..2100, home 1275/1300, step_max 4000/2160, speed 3");
    CHECK(ptzs_conf_parse("", &c, err, sizeof(err)) == 0 && c.pan_home == 1275 && c.tilt_home == 1300, "empty conf = defaults");
    CHECK(ptzs_conf_parse("# c\r\npan_min=400\r\n  tilt_max = 1700 \n\nspeed=5\n", &c, err, sizeof(err)) != 0,
          "spaces around = are refused (strict key=value)");
    CHECK(ptzs_conf_parse("# c\r\npan_min=400\r\ntilt_max=1700\n\nspeed=5\n", &c, err, sizeof(err)) == 0 && c.pan_min == 400 && c.tilt_max == 1700 &&
          c.speed == 5 && c.pan_max == 3650, "comments, CRLF, blank lines, partial override");
    CHECK(ptzs_conf_parse("pan_min=50\ntilt_min=50\npan_max=3700\ntilt_max=2100\n", &c, err, sizeof(err)) == 0, "widest allowed range (the caps 3700 / 2100)");
    CHECK(ptzs_conf_parse("pan_min=49\n", &c, err, sizeof(err)) < 0, "pan_min below the 50 step margin refused");
    CHECK(ptzs_conf_parse("pan_max=3701\n", &c, err, sizeof(err)) < 0, "pan_max above the 3700 cap refused");
    CHECK(ptzs_conf_parse("tilt_max=2101\n", &c, err, sizeof(err)) < 0, "tilt_max above the 2100 cap refused");
    CHECK(ptzs_conf_parse("tilt_max=2111\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("pan_max=3950\n", &c, err, sizeof(err)) < 0, "the old step_max-50 ceilings are refused too");
    CHECK(ptzs_conf_parse("pan_step_max=3740\npan_max=3700\n", &c, err, sizeof(err)) < 0, "pan_max must stay 50 below pan_step_max");
    CHECK(ptzs_conf_parse("pan_min=3000\npan_max=3050\npan_home=3020\n", &c, err, sizeof(err)) < 0, "range narrower than 100 refused");
    CHECK(ptzs_conf_parse("pan_home=299\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("tilt_home=2101\n", &c, err, sizeof(err)) < 0, "home outside the soft range refused");
    CHECK(ptzs_conf_parse("speed=1\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("speed=21\n", &c, err, sizeof(err)) < 0, "speed outside 2..20 refused");
    CHECK(ptzs_conf_parse("pan_step_max=4001\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("tilt_step_max=2161\n", &c, err, sizeof(err)) < 0, "step_max above the stock 4000 / 2160 refused");
    CHECK(ptzs_conf_parse("pan_step_max=5000\n", &c, err, sizeof(err)) < 0, "a larger step_max is refused (stock numbering)");
    CHECK(ptzs_conf_parse("pan_min=400\npan_min=500\n", &c, err, sizeof(err)) < 0, "repeated key refused");
    CHECK(ptzs_conf_parse("pan_mid=400\n", &c, err, sizeof(err)) < 0, "unknown key refused");
    CHECK(ptzs_conf_parse("pan_min=4x\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("pan_min=-4\n", &c, err, sizeof(err)) < 0 &&
          ptzs_conf_parse("pan_min=\n", &c, err, sizeof(err)) < 0 && ptzs_conf_parse("pan_min\n", &c, err, sizeof(err)) < 0, "junk values refused");
    CHECK(ptzs_conf_parse("pan_min=99999999999\n", &c, err, sizeof(err)) < 0, "huge value refused");
    {
        char big[200];
        memset(big, 'a', sizeof(big)); big[199] = 0;
        CHECK(ptzs_conf_parse(big, &c, err, sizeof(err)) < 0, "over-long line refused");
    }

    /* through the tool: absent file = defaults, bad file = exit 6 and the motor is never touched, stop still works */
    model_reset();
    CHECK(go("limits", 0, 0, 0, 0) == 0 && M.nreq == 0, "limits prints and never opens the motor");
    set_conf("pan_min=10\n");
    M.nreq = 0;
    CHECK(go("move", "2", "100", 0, 0) == PTZS_ECONF && M.nreq == 0, "bad ptz.conf: move refused with exit 6, no request sent");
    CHECK(go("park", 0, 0, 0, 0) == PTZS_ECONF && go("init", "--i-know", 0, 0, 0) == PTZS_ECONF && go("limits", 0, 0, 0, 0) == PTZS_ECONF && M.nreq == 0,
          "bad ptz.conf: park, init, limits refused too");
    CHECK(go("stop", 0, 0, 0, 0) == 0 && M.nstop == 1, "stop works with a bad ptz.conf");
    CHECK(go("pos", 0, 0, 0, 0) == 0, "pos works with a bad ptz.conf");
    M.conf_present = 0; M.conf_err = 1; M.nreq = 0;
    CHECK(go("move", "2", "100", 0, 0) == PTZS_ECONF && M.nreq == 0, "unreadable ptz.conf (not ENOENT): fail closed");
}

static void test_move(void)
{
    int rc;
    model_ready();
    rc = go("move", "2", "300", "3", "3");
    CHECK(rc == 0 && M.real[0] == 1275 + 100 && M.real[1] == 816, "move right 300 ms at divisor 3: pan +100");
    CHECK(M.dirs == 1u << 2 && M.nmove == 1 && M.bad_cmd == 0 && M.hit_stop == 0, "only command 2 sent, never an invalid one, no stop hit");
    CHECK(M.nstop >= 1 && M.stop_at >= M.move_at + 300, "the stop is sent after the full duration");
    CHECK(M.npos >= 2 * 7, "position polled every 50 ms during the move (6 polls + before/after)");
    CHECK(!strcmp(M.posfile, "pan=1375 tilt=816\n"), "position persisted after the move");
    CHECK(M.last_speed[0] == 3 && M.last_speed[1] == 3, "explicit speed sent");
    go("move", "2", "100", 0, 0);
    CHECK(M.last_speed[0] == 3 && M.last_speed[1] == 3, "default speed from the config is sent (never 0 = keep)");

    /* soft limit: refusal when at the limit, clip on the way */
    model_ready(); place(3695, M.real[1]);
    CHECK(go("move", "2", "300", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "move right at 3695 (limit 3700): refused, nothing sent");
    model_ready(); place(3600, M.real[1]);
    rc = go("move", "2", "1500", "3", "3");
    CHECK(rc == 0 && M.real[0] <= 3700 && M.real[0] >= 3650 && M.hit_stop == 0, "long move right from 3600 is clipped at the soft limit");
    CHECK(M.stop_at - M.move_at < 400, "the clip stops the motion early");
    model_ready(); place(3600, M.real[1]);
    rc = go("move", "2", "1500", "2", "2");
    CHECK(rc == 0 && M.real[0] <= 3700 && M.hit_stop == 0, "clip also at the fastest allowed speed (divisor 2)");
    model_ready(); place(400, M.real[1]);
    rc = go("move", "6", "1500", "2", "2");
    CHECK(rc == 0 && M.real[0] >= 300 && M.real[0] <= 330 && M.hit_stop == 0, "long move left from 400 clipped at pan_min");
    model_ready(); place(M.real[0], 1700);
    rc = go("move", "0", "1500", "2", "2");
    CHECK(rc == 0 && M.real[1] <= 1800 && M.hit_stop == 0, "long move up clipped at tilt_max");
    model_ready(); place(M.real[0], 260);
    rc = go("move", "4", "1500", "2", "2");
    CHECK(rc == 0 && M.real[1] >= 200 && M.hit_stop == 0, "long move down clipped at tilt_min");
    model_ready(); place(M.real[0], 1795);
    CHECK(go("move", "0", "300", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "tilt up at its limit refused");
    model_ready(); place(3600, 1000);
    rc = go("move", "3", "1500", "2", "2");   /* diagonal down-right: pan hits its limit first, both stop */
    CHECK(rc == 0 && M.real[0] <= 3700 && M.real[1] < 1000 && M.real[1] >= 200 && M.hit_stop == 0 && M.dirs == 1u << 3, "diagonal clipped by the first axis to arrive, both axes stopped");
    model_ready(); place(3695, 1000);
    CHECK(go("move", "1", "300", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "diagonal refused when one of its axes is at the limit");
    model_ready(); place(3695, M.real[1]);
    CHECK(go("move", "6", "300", "3", "3") == 0 && M.real[0] < 3695, "moving away from a limit is allowed");

    /* one step outside the range (the driver's own 0, where a cut move ends): inward only */
    model_ready(); place(299, M.real[1]);
    CHECK(M.cnt[0] == 0 && go("move", "6", "300", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "outside the range (driver counter 0): further out refused");
    rc = go("move", "2", "300", "3", "3");
    CHECK(rc == 0 && M.real[0] == 399, "outside the range: inward allowed");

    /* uninitialised driver */
    model_ready(); M.cnt[0] = M.cnt[1] = 0; M.real[0] = M.real[1] = 0;
    CHECK(go("move", "2", "300", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "counters 0/0: refused");
    CHECK(go("goto", "1300", "800", 0, 0) == PTZS_EREFUSED && go("park", 0, 0, 0, 0) == PTZS_EREFUSED && M.nmove == 0, "counters 0/0: goto and park refused");

    /* stall: the pan counter never changes */
    model_ready(); M.stuck[0] = 1;
    rc = go("move", "2", "1500", "3", "3");
    CHECK(rc == PTZS_ESTALL, "stall: error 5");
    CHECK(M.stop_at - M.move_at > 300 && M.stop_at - M.move_at <= 400 && M.nstop >= 1 && !M.running[0], "stall: motor stopped after 300..400 ms");
    model_ready(); M.stuck[1] = 1; place(1275, M.real[1]);
    rc = go("move", "1", "1500", "3", "3");
    CHECK(rc == PTZS_ESTALL, "stall on one axis of a diagonal is detected too");
    model_ready(); M.reverse[0] = 1;
    CHECK(go("move", "2", "1500", "3", "3") == PTZS_ESTALL && M.stop_at - M.move_at <= 150, "counter moving the wrong way: stopped quickly");
    model_ready(); M.slow = 1; M.fail_pos_after = 4;
    CHECK(go("move", "2", "1500", "3", "3") == 1 && M.nstop >= 1 && !M.running[0], "pos read failure during the move: stop sent, exit 1");
    model_ready(); M.move_fail = 1;
    CHECK(go("move", "2", "300", "3", "3") == 1 && M.nstop >= 1, "failed move request still stops");
    model_ready(); M.stop_fail = 2;
    CHECK(go("move", "2", "100", "3", "3") == 0, "stop is retried (2 failures tolerated)");
    model_ready(); M.stop_fail = 3;
    CHECK(go("move", "2", "100", "3", "3") == 1 && M.nstop >= 3, "stop failing 3 times is reported");
    model_ready(); M.save_fail = 1;
    CHECK(go("move", "2", "100", "3", "3") == 1 && M.nmove == 0 && !M.running[0], "no position file to record in: the move is refused before anything is sent (fail closed)");

    /* a narrower configured range */
    model_ready_conf("pan_min=1000\npan_max=1500\npan_home=1275\n");
    rc = go("move", "2", "1500", "2", "2");
    CHECK(rc == 0 && M.real[0] <= 1500 && M.real[0] >= 1400, "configured range honoured");
    model_ready(); M.open_errno = EBUSY;
    CHECK(go("move", "2", "100", 0, 0) == 3 && M.nreq == 0, "EBUSY reported as 3, nothing sent");
    M.open_errno = ENOENT;
    CHECK(go("pos", 0, 0, 0, 0) == 1, "open failure reported as 1");
    model_ready();
    CHECK(go("pos", 0, 0, 0, 0) == 0 && M.nreq == 2, "pos issues two read requests (pan, tilt)");
    CHECK(go("stop", 0, 0, 0, 0) == 0 && M.nstop == 1, "stop");
}

static void test_goto(void)
{
    int rc;
    model_ready();
    rc = go("goto", "2000", "1500", 0, 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 2000) <= (int)PTZS_TOL && abs((int)M.real[1] - 1500) <= (int)PTZS_TOL, "goto converges (pan 2000, tilt 1500)");
    CHECK(M.hit_stop == 0 && M.bad_cmd == 0 && (M.dirs & ~((1u << 0) | (1u << 2) | (1u << 4) | (1u << 6))) == 0, "goto uses only the verified single-axis moves");
    {
        uint32_t p, t;
        CHECK(!ptzs_pos_parse(M.posfile, &p, &t) && (int)p == M.real[0] && (int)t == M.real[1] && M.cnt[0] == p - 299 && M.cnt[1] == t - 199, "the saved position is the real position, counters are shifted by soft_min - 1");
    }
    rc = go("goto", "1275", "816", "2", 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 1275) <= 3 && abs((int)M.real[1] - 816) <= 3, "goto back at speed 2");
    rc = go("goto", "1275", "816", "20", 0);
    model_ready(); M.nmove = 0;
    rc = go("goto", "1277", "814", "20", 0);
    CHECK(rc == 0 && M.nmove == 0, "goto within the tolerance sends no move");
    model_ready();
    rc = go("goto", "3700", "1800", "2", 0);
    CHECK(rc == 0 && M.real[0] <= 3700 + 3 && M.real[1] <= 1800 + 3 && M.real[0] >= 3690 && M.hit_stop == 0, "goto to the far soft corner");
    rc = go("goto", "300", "200", "2", 0);
    CHECK(rc == 0 && M.real[0] >= 300 - 3 && M.real[1] >= 200 - 3 && M.real[0] <= 310 && M.hit_stop == 0, "goto to the near soft corner");
    model_ready(); M.nreq = 0;
    CHECK(go("goto", "299", "816", 0, 0) == PTZS_EREFUSED && go("goto", "3701", "816", 0, 0) == PTZS_EREFUSED &&
          go("goto", "1275", "199", 0, 0) == PTZS_EREFUSED && go("goto", "1275", "1801", 0, 0) == PTZS_EREFUSED && go("goto", "0", "0", 0, 0) == PTZS_EREFUSED &&
          go("goto", "65535", "65535", 0, 0) == PTZS_EREFUSED, "goto outside the soft range refused (not clipped)");
    CHECK(M.nreq == 0, "a refused goto sends nothing at all");
    model_ready(); M.jitter = 7;
    rc = go("goto", "2500", "1200", 0, 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 2500) <= (int)PTZS_ACCEPT && abs((int)M.real[1] - 1200) <= (int)PTZS_ACCEPT && M.hit_stop == 0,
          "goto converges with a sloppy 7 ms sleep overshoot (correction passes)");
    model_ready(); place(299, M.real[1]);
    rc = go("goto", "800", "816", 0, 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 800) <= 3, "goto from one step outside the range back inside");
    model_ready(); M.stuck[0] = 1;
    CHECK(go("goto", "2000", "816", 0, 0) == PTZS_ESTALL && !M.running[0] && M.stop_at - M.move_at <= 400, "goto stall: stopped, error 5");
    model_ready(); M.slow = 4;
    CHECK(go("goto", "2200", "816", 0, 0) == PTZS_ESTALL && !M.running[0], "goto much slower than expected: timeout, stopped");
    model_ready(); M.reverse[1] = 1;
    CHECK(go("goto", "1275", "1500", 0, 0) == PTZS_ESTALL && !M.running[1], "goto: counter moving the wrong way: stopped");
    model_ready(); M.fail_pos_after = 6;
    CHECK(go("goto", "2000", "816", 0, 0) == 1 && !M.running[0] && M.nstop >= 1, "goto pos read failure: stopped, exit 1");
    model_ready(); M.drvmax[0] = 1300;  /* a driver with a smaller max walks to 1300 and stops: the target is not reached */
    {
        int r2 = go("goto", "2000", "816", 0, 0);
        CHECK(r2 == PTZS_ESTALL && M.hit_stop == 0, "goto never reaching its target ends in an error, not an endless walk");
    }
}

static void test_park_persist(void)
{
    int rc;
    uint32_t p, t;
    model_ready();
    go("goto", "3000", "1800", 0, 0);
    rc = go("park", 0, 0, 0, 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 1275) <= 3 && abs((int)M.real[1] - 816) <= 3 && M.hit_stop == 0, "park returns to the stock home 1275/816");
    CHECK(!ptzs_pos_parse(M.posfile, &p, &t) && (int)p == M.real[0] && (int)t == M.real[1], "park persists the position");
    model_reset(); set_conf("pan_home=2000\ntilt_home=1000\n"); go("init", "--i-know", "1275", "816", 0);
    rc = go("park", 0, 0, 0, 0);
    CHECK(rc == 0 && abs((int)M.real[0] - 2000) <= 3 && abs((int)M.real[1] - 1000) <= 3, "park uses pan_home/tilt_home of ptz.conf");
    model_ready(); M.stuck[1] = 1; place(M.real[0], 1500);
    CHECK(go("park", 0, 0, 0, 0) == PTZS_ESTALL && !M.running[0] && !M.running[1], "park with a stalled axis: error, motor stopped");

    /* init: shifted frame, persisted; restore re-declares after a driver reload */
    model_reset();
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore without a saved position refused");
    rc = go("init", "--i-know", 0, 0, 0);
    CHECK(rc == 0 && M.real[0] == 1275 && M.real[1] == 816 && !strcmp(M.posfile, "pan=1275 tilt=816\n"), "init declares home and persists the REAL position");
    CHECK(M.ninit1 == 1 && M.ninit2 == 1 && M.nmove == 0 && M.nstop == 0, "init sends two requests, nothing else");
    CHECK(M.last_lim[0] == 0 && M.last_lim[1] == 3401 && M.last_lim[2] == 1601 && M.last_lim[3] == 0,
          "init limits {0, soft_max-soft_min+1, ..., 0} = {0,3401,1601,0}: the driver's limits are the soft range");
    CHECK(M.last_ipos[0] == 976 && M.last_ipos[1] == 617 && !M.last_ipos[2] && !M.last_ipos[9],
          "init counters {home-(soft_min-1)} = {976,617,0...}, never 0 (0 would mean keep)");
    CHECK(M.frame_present && !strcmp(M.frame, "pan_min=300 pan_max=3700 tilt_min=200 tilt_max=1800\n"), "the frame is recorded after init");
    CHECK(go_cap("pos", 0, 0, 0, 0) == 0 && !strcmp(cap, "pan=1275 tilt=816\n"), "pos prints REAL coordinates (counters are 976/617)");
    go("goto", "2000", "1500", 0, 0);
    CHECK(go_cap("pos", 0, 0, 0, 0) == 0 && abs(atoi(cap + 4) - 2000) <= 3, "pos after goto: real coordinates");
    M.cnt[0] = M.cnt[1] = 0;   /* driver reloaded: counters lost, camera did not move */
    CHECK(go_cap("pos", 0, 0, 0, 0) == 0 && !strcmp(cap, "pan=0 tilt=0\n"), "pos with counters 0/0 prints the raw counters");
    M.nmove = 0;
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && M.nmove == 0, "after the counter loss motion is refused");
    M.ninit1 = M.ninit2 = 0;
    rc = go("restore", 0, 0, 0, 0);
    CHECK(rc == 0 && M.ninit1 == 1 && M.ninit2 == 1 && abs(M.real[0] - 2000) <= 3 && M.real[1] > 1400 && (int)M.cnt[0] == M.real[0] - 299,
          "restore re-declares the saved position (no motion, shifted)");
    M.ninit1 = 0;
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refused when the driver already has a position");
    M.cnt[0] = 0; M.cnt[1] = 5;
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refused when only one counter is 0");
    M.cnt[0] = M.cnt[1] = 0;
    strcpy(M.posfile, "pan=oops\n");
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refuses a corrupt position file");
    strcpy(M.posfile, "unknown\n");
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refuses the unknown marker");
    strcpy(M.posfile, "pan=0 tilt=816\n");
    CHECK(go("restore", 0, 0, 0, 0) == 4, "restore refuses 0 (outside the soft range)");
    strcpy(M.posfile, "pan=3701 tilt=816\n");
    CHECK(go("restore", 0, 0, 0, 0) == 4, "restore refuses a position above the soft range");
    strcpy(M.posfile, "pan=1500 tilt=800\n");
    M.frame_present = 0;
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refuses without a frame");
    strcpy(M.frame, "pan_min=300 pan_max=3600 tilt_min=200 tilt_max=1800\n"); M.frame_present = 1;
    CHECK(go("restore", 0, 0, 0, 0) == 4 && M.ninit1 == 0, "restore refuses when ptz.conf differs from the frame");
    CHECK(ptzs_pos_parse("pan=1 tilt=2", &p, &t) == 0 && ptzs_pos_parse("pan=1 tilt=2x", &p, &t) < 0 && ptzs_pos_parse("pan= tilt=2", &p, &t) < 0 &&
          ptzs_pos_parse("pan=1 tilt=2\n\n", &p, &t) < 0 && ptzs_pos_parse("1 2", &p, &t) < 0, "position file parser is strict");

    /* the frame follows ptz.conf; init position given explicitly is real and range-checked */
    model_reset(); set_conf("pan_min=500\npan_max=2500\ntilt_min=300\ntilt_max=1500\npan_home=1500\ntilt_home=900\n");
    rc = go("init", "--i-know", 0, 0, 0);
    CHECK(rc == 0 && M.last_lim[1] == 2001 && M.last_lim[2] == 1201 && M.last_ipos[0] == 1500 - 499 && M.last_ipos[1] == 900 - 299,
          "init frame derived from ptz.conf, position = its home");
    model_reset(); M.real[0] = 1300; M.real[1] = 800;
    CHECK(go("init", "--i-know", "1300", "800", 0) == 0 && M.last_ipos[0] == 1300 - 299 && M.last_ipos[1] == 800 - 199 && M.off[0] == 299 && M.off[1] == 199,
          "explicit real position wins over the ptz.conf home");
    model_reset(); M.nreq = 0;
    CHECK(go("init", "--i-know", "299", "800", 0) == 4 && go("init", "--i-know", "3701", "800", 0) == 4 && go("init", "--i-know", "1300", "199", 0) == 4 &&
          go("init", "--i-know", "1300", "1801", 0) == 4 && M.nreq == 0 && !M.frame_present, "init outside the soft range refused, nothing sent, no frame");

    /* init failure paths leave NO valid frame: every motion stays refused */
    model_reset(); M.init_fail = 1;
    CHECK(go("init", "--i-know", 0, 0, 0) == 1 && go("move", "2", "100", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "init: limits request failed: no valid frame, motion refused");
    model_reset(); M.init_ignore_pos = 1;
    CHECK(go("init", "--i-know", 0, 0, 0) == 1 && go("move", "2", "100", "3", "3") == PTZS_EREFUSED && M.nmove == 0, "init: driver did not take the counters: no valid frame, motion refused");
    model_reset(); M.frame_fail = 1;
    CHECK(go("init", "--i-know", 0, 0, 0) == 1 && M.ninit1 == 0, "init: cannot invalidate the frame file: nothing sent to the driver");
}

/* The app must never trust a frame or a saved position after the tool touched the head: any motion or init invalidates both. */
static void test_app_invalidation(void)
{
    const char *const good = "pan_min=300 pan_max=3650 tilt_min=300 tilt_max=1750\n";
    printf("== the tool invalidates the app's frame (and writes nothing to the flash)\n");
    model_ready();
    CHECK(!strcmp(M.appframe, "invalid\n") && M.savedpos[0] == 0, "init invalidates the app frame file and writes NOTHING to the flash (no ptz.pos)");
    snprintf(M.appframe, sizeof(M.appframe), "%s", good); snprintf(M.savedpos, sizeof(M.savedpos), "pan=1275 tilt=1300\n");
    CHECK(M.scene[0] == 0, "...nor to the app's scene reference (keyed to home: no flash write from the tool)");
    strcpy(M.scene, "scene1 x");
    CHECK(go("pos", 0, 0, 0, 0) == 0 && !strcmp(M.scene, "scene1 x"), "pos (read only) leaves the scene reference alone");
    CHECK(go("pos", 0, 0, 0, 0) == 0 && !strcmp(M.appframe, good) && !strcmp(M.savedpos, "pan=1275 tilt=1300\n"), "pos (read only) leaves them alone");
    CHECK(go("move", "2", "200", "3", "3") == 0 && !strcmp(M.appframe, "invalid\n") && !strcmp(M.savedpos, "pan=1275 tilt=1300\n") && !strcmp(M.scene, "scene1 x"), "move invalidates the app frame (RAM) only: ptz.pos and ptz.scene untouched");
    snprintf(M.appframe, sizeof(M.appframe), "%s", good); snprintf(M.savedpos, sizeof(M.savedpos), "pan=1275 tilt=1300\n");
    CHECK(go("goto", "1300", "816", 0, 0) == 0 && !strcmp(M.appframe, "invalid\n") && !strcmp(M.savedpos, "pan=1275 tilt=1300\n"), "goto invalidates the app frame only");
    snprintf(M.appframe, sizeof(M.appframe), "%s", good); snprintf(M.savedpos, sizeof(M.savedpos), "pan=1275 tilt=1300\n");
    CHECK(go("park", 0, 0, 0, 0) == 0 && !strcmp(M.appframe, "invalid\n") && !strcmp(M.savedpos, "pan=1275 tilt=1300\n"), "park invalidates the app frame only");
}

static void test_frame(void)
{
    int rc;
    /* no frame: a driver that holds a position but was not initialised by this tool (stock app, older tool) */
    model_reset(); M.cnt[0] = 1275; M.cnt[1] = 816;
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && go("goto", "1300", "800", 0, 0) == PTZS_EREFUSED && go("park", 0, 0, 0, 0) == PTZS_EREFUSED && M.nmove == 0 && M.nreq == 0,
          "no frame: move, goto, park refused before the driver is touched");
    CHECK(go_cap("pos", 0, 0, 0, 0) == 0 && !strcmp(cap, "pan=1275 tilt=816\n"), "no frame: pos prints the raw counters");
    CHECK(go("stop", 0, 0, 0, 0) == 0 && M.nstop == 1, "no frame: stop still works");
    /* corrupt / invalidated frame */
    model_ready(); strcpy(M.frame, "invalid\n");
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && M.nmove == 0, "an invalidated frame refuses motion");
    strcpy(M.frame, "pan_min=300 pan_max=3700 tilt_min=200 tilt_max=1800 \n");
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && M.nmove == 0, "a frame with trailing junk refuses motion");
    strcpy(M.frame, "pan_min=0 pan_max=3700 tilt_min=200 tilt_max=1800\n");
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && M.nmove == 0, "a frame with min 0 refuses motion");
    strcpy(M.frame, "pan_min=300 pan_max=3700 tilt_min=200 tilt_max=1800\n");
    CHECK(go("move", "2", "100", "3", "3") == 0 && M.nmove == 1, "the valid frame works again");
    /* ptz.conf edited after init */
    model_ready(); set_conf("pan_min=400\n");
    CHECK(go("move", "2", "100", 0, 0) == PTZS_EREFUSED && go("goto", "1300", "800", 0, 0) == PTZS_EREFUSED && go("park", 0, 0, 0, 0) == PTZS_EREFUSED && M.nmove == 0,
          "ptz.conf soft range changed since init: every motion refused");
    CHECK(go_cap("pos", 0, 0, 0, 0) == 0 && !strcmp(cap, "pan=1275 tilt=816\n"), "...and pos still prints real coordinates of the init frame");
    CHECK(go("init", "--i-know", "1275", "816", 0) == 0 && go("move", "2", "100", "3", "3") == 0 && M.last_lim[1] == 3251, "...re-init with the printed position adopts the new frame");
    /* a counter beyond the frame (re-initialised by something else) is an error, not a position */
    model_ready(); M.cnt[0] = 5000;
    rc = go("move", "2", "100", "3", "3");
    CHECK(rc == 1 && M.nmove == 0, "counter beyond the driver max of the frame: refused (exit 1), nothing moved");
}

/* ---- the key property: a tool killed by SIGKILL in the middle of anything can never be walked past a soft limit
 * by the driver, because the driver's own limits are the soft range. The mechanics here are the WORST CASE: the
 * hard stops sit exactly one step outside the soft range (real 299 / 3700, 199 / 1800). */
static void kill_setup(int sp, int st, long kill_ms)
{
    model_ready();
    M.mech_lo[0] = 299; M.mech_hi[0] = 3700; M.mech_lo[1] = 199; M.mech_hi[1] = 1800;
    place(sp, st);
    M.kill_ms = kill_ms;
}

static int kill_ok(void)
{
    advance(40000);   /* nobody stops the motor: the driver walks until its own limit */
    return !M.hit_stop && M.real[0] >= 299 && M.real[0] <= 3700 && M.real[1] >= 199 && M.real[1] <= 1800;
}

static void test_kill(void)
{
    static const int sps[][2] = {{300, 200}, {310, 210}, {1275, 816}, {2000, 1000}, {3690, 1790}, {3700, 1800}, {300, 1800}, {3700, 200}};
    static const long kills[] = {1, 20, 51, 120, 333, 700, 1400, 2500};
    static const char *const spd[] = {"2", "3", "20"};
    static const char *const gt[][2] = {{"3700", "1800"}, {"300", "200"}, {"3700", "200"}, {"300", "1800"}};
    int bad = 0, n = 0, d, i, k, v, g;
    for (d = 0; d < 8; d++)
        for (i = 0; i < 8; i++)
            for (k = 0; k < 8; k++)
                for (v = 0; v < 3; v++) {
                    struct ptz_cmd c;
                    char dir[16], *av[7];
                    snprintf(dir, sizeof(dir), "%d", d);
                    av[0] = "akam_ptz"; av[1] = "move"; av[2] = dir; av[3] = "1500"; av[4] = (char *)spd[v]; av[5] = (char *)spd[v]; av[6] = NULL;
                    kill_setup(sps[i][0], sps[i][1], kills[k]);
                    if (ptz_parse(6, av, &c)) { bad++; continue; }
                    if (setjmp(killjb) == 0) {
                        ptz_run(&c, &ops);   /* refused or finished before the kill time: nothing to check */
                        M.kill_ms = -1;
                        continue;
                    }
                    n++;
                    if (!kill_ok() || strcmp(M.posfile, "unknown\n")) {
                        fprintf(out, "    kill move %d speed %s from %d/%d after %ld ms ends at %d/%d (hit %d, file %s)\n", d, spd[v], sps[i][0], sps[i][1], kills[k],
                                M.real[0], M.real[1], M.hit_stop, M.posfile);
                        bad++;
                    }
                }
    CHECK(bad == 0 && n > 500, "move killed mid-flight (8 dirs x 8 starts incl. all corners x 8 kill times x 3 speeds): the driver itself stops at the soft limit; position file says 'unknown'");
    bad = 0; n = 0;
    for (i = 0; i < 8; i++)
        for (k = 0; k < 8; k++)
            for (g = 0; g < 4; g++) {
                kill_setup(sps[i][0], sps[i][1], kills[k] * 4);
                if (setjmp(killjb) == 0) {
                    go("goto", gt[g][0], gt[g][1], "2", 0);
                    M.kill_ms = -1;
                    continue;
                }
                n++;
                if (!kill_ok()) bad++;
            }
    CHECK(bad == 0 && n > 100, "goto killed mid-flight toward every soft corner: never reaches a hard stop");
    kill_setup(3000, 1500, 500);
    if (setjmp(killjb) == 0) go("park", 0, 0, 0, 0);
    CHECK(kill_ok(), "park killed mid-flight: never reaches a hard stop");
    /* control: the stock frame (driver limits 0..4000 / 0..2160, no shift) DOES walk into the stop when killed: the test can fail */
    model_reset();
    M.mech_lo[0] = 299; M.mech_hi[0] = 3700; M.mech_lo[1] = 199; M.mech_hi[1] = 1800;
    M.cnt[0] = M.real[0] = 1275; M.cnt[1] = M.real[1] = 816;
    {
        uint32_t w[7] = {2, 3, 3, 0, 0, 0, 0};
        s_ioctl(5, PTZ_REQ_MOVE, w);
        advance(40000);
    }
    CHECK(M.hit_stop > 0, "control: with the stock unshifted limits a killed move grinds against the stop (the kill test would catch it)");
}

static void test_never_unsafe(void)
{
    model_ready();
    go("move", "2", "1500", "2", "2"); go("move", "6", "1500", "2", "2"); go("move", "0", "1500", "2", "2"); go("move", "4", "1500", "2", "2");
    go("move", "1", "1500", "2", "2"); go("move", "5", "1500", "2", "2"); go("goto", "3700", "1800", 0, 0); go("goto", "300", "200", 0, 0);
    CHECK(M.hit_stop == 0 && M.bad_cmd == 0, "a long mixed session never reaches a mechanical stop nor sends command 8..13");
    CHECK(M.real[0] >= 300 - 3 && M.real[0] <= 3700 + 3 && M.real[1] >= 200 - 3 && M.real[1] <= 1800 + 3, "counters stay inside the soft range (+- tolerance)");
}

int main(void)
{
    out = fdopen(dup(1), "w");
    freopen("/dev/null", "w", stderr);
    freopen("/dev/null", "w", stdout);
    test_parse();
    test_conf();
    test_move();
    test_goto();
    test_park_persist();
    test_frame();
    test_app_invalidation();
    test_kill();
    test_never_unsafe();
    fprintf(out, "rot2k ptz unit: %d failed\n", fails);
    fflush(out);
    return fails != 0;
}
