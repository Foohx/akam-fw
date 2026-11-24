/* Host test of the raw recorder (akam.conf track_debug=2, track.c): the live tracker is fed a walker with head boxes, the recorder writes /tmp/akam2k_track.raw (C, I, R, D lines),
 * and tests/rot2k_track_replay.c replays the I lines: the replay must make the SAME decision as the live tracker on every tick (kind, directions, divisors). Also the line formats, the
 * rolling bound of the file, and that level 1 / 0 write no raw file. Build: tests/rot2k_overlay_test.sh. Prints "rot2k track rec unit: N failed". */
#define REPLAY_LIB 1
#include "rot2k_track_replay.c"
#include "track.h"
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)
void a2k_log(const char *fmt, ...) { (void)fmt; }

static int g_vel_calls, g_vsx, g_vsy, g_vdx, g_vdy;
static void f_info(a2k_ptz_info_t *o) { memset(o, 0, sizeof *o); o->state = HAL_PTZ_HOME; o->known = 1; o->pan = 1275; o->tilt = 1300; }
static int f_range(int r[4]) { r[0] = 300; r[1] = 3650; r[2] = 300; r[3] = 2100; return 0; }
static int f_priv(void) { return 0; }
static int f_nudge(int dx, int dy, int sp) { (void)dx; (void)dy; (void)sp; return 0; }
static int f_home(void) { return 0; }
static int f_vel(int sx, int sy, int dx, int dy) { g_vel_calls++; g_vsx = sx; g_vsy = sy; g_vdx = dx; g_vdy = dy; return 0; }
static int f_pos_at(int64_t t, int *pan, int *tilt) { *pan = 1275 + (int)((t / 10) % 200); *tilt = 1300 + (int)((t / 30) % 40); return 0; }

#define RAW "/tmp/akam2k_track.raw"

static int lines_with(const char *prefix)
{
    FILE *f = fopen(RAW, "r");
    char l[4096];
    int n = 0;
    if (!f) return 0;
    while (fgets(l, sizeof l, f)) n += !strncmp(l, prefix, strlen(prefix));
    fclose(f);
    return n;
}

int main(void)
{
    static a2k_track_t t;
    a2k_track_ops_t ops = { f_info, f_range, f_nudge, f_home, f_priv, NULL, NULL, NULL, f_vel, f_pos_at, NULL, NULL, NULL, NULL, NULL };
    struct tl_cfg c;
    int live_kind[200], live_sx[200], live_sy[200], live_dx[200], live_dy[200], k, nt = 0;
    FILE *o;
    char path[] = "/tmp/akam2k_track_rec_unit.out";
    struct replay_opt op;
    struct replay_stats st;
    char line[4096];

    printf("== the raw recorder\n");
    remove(RAW);
    a2k_track_init(&t, &ops, NULL);
    tl_cfg_defaults(&c);
    c.motion = TL_MOTION_CONT; c.invert_pan = 1; c.tilt_enable = 1; c.return_home = 0;
    (void)a2k_track_set_cfg(&t, &c);
    a2k_track_set_enabled(&t, 1);
    a2k_track_set_debug(&t, 1);
    for (k = 0; k < 10; k++) { struct tl_box b = { 200, 40, 250, 150, 0, 700 }; a2k_track_rec_ring(k, 1, 2); a2k_track_tick_ts(&t, 1000 + k * 200, 900 + k * 200, &b, 1, 320, 192); }
    CHECK(access(RAW, F_OK) != 0, "track_debug=1: no raw file, R lines are dropped");
    (void)a2k_track_set_cfg(&t, &c); /* a clean policy state: the replay starts from one too */
    a2k_track_set_debug(&t, 2);
    for (k = 0; k < 60; k++) {
        int64_t T = 5000 + k * 200;
        struct tl_box in[3] = { { 40 + (k * 3) % 220, 30, 90 + (k * 3) % 220, 160, TL_CLS_PERSON, 800 }, { 50 + (k * 3) % 220, 30, 70 + (k * 3) % 220, 50, TL_CLS_HEAD, 500 }, { 0, 0, 20, 20, TL_CLS_HEAD, 330 } };
        int n = k % 9 == 8 ? 0 : k % 4 == 1 ? 3 : 2;
        for (int j = 0; j < 4; j++) a2k_track_rec_ring(T - 200 + j * 50, 1275 + j, 1300);
        g_vel_calls = 0; g_vsx = g_vsy = g_vdx = g_vdy = 0;
        a2k_track_tick_ts(&t, T, T - 100, in, n, 320, 192);
        live_kind[nt] = g_vel_calls; live_sx[nt] = g_vsx; live_sy[nt] = g_vsy; live_dx[nt] = g_vdx; live_dy[nt] = g_vdy; nt++;
    }
    a2k_track_set_debug(&t, 0);
    CHECK(lines_with("C ") == 1, "one C (config) line");
    CHECK(lines_with("I ") == 60, "one I line per tl_update input (60)");
    CHECK(lines_with("R ") == 240, "one R line per position-ring push of the session (240)");
    CHECK(lines_with("D ") >= 10, "D lines (decisions) are in the file");
    {   /* the I line format: now cap pan tilt has_cap cap_pan cap_tilt rate_p rate_t moving tilt_home pmin pmax tmin tmax n {cls,score,x1,y1,x2,y2}... */
        FILE *f = fopen(RAW, "r");
        int seen = 0, d = 0;
        while (f && fgets(line, sizeof line, f)) {
            if (!strncmp(line, "I 5000 4900 ", 12)) {
                long long a, b2;
                int v[13], n2 = 0;
                seen = sscanf(line, "I %lld %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &a, &b2, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &n2) == 16 &&
                       n2 == 2 && strstr(line, "{0,800,40,30,90,160}") && strstr(line, "{1,500,50,30,70,50}");
            }
            if (!strncmp(line, "D ", 2) && strstr(line, "rule=") && strstr(line, "dcue=") && strstr(line, "inn=") && strstr(line, "bud="))
                d++;
        }
        if (f) fclose(f);
        CHECK(seen, "I line: the documented columns, the person {0,800,...} and the head {1,500,...} boxes");
        CHECK(d >= 10, "D lines carry rule, distance cue, innovation and budgets");
    }
    /* the replay of the recording decides exactly like the live tracker */
    memset(&op, 0, sizeof op);
    op.fw = 320;
    op.fh = 192;
    o = fopen(path, "w");
    CHECK(replay_file(RAW, &op, o, &st, NULL, 0) == 0 && st.ticks == 60, "replay: all 60 I lines run");
    fclose(o);
    o = fopen(path, "r");
    {
        int i = 0, same = 0;
        while (o && fgets(line, sizeof line, o) && i < nt) {
            long long now;
            int kind, pd, pv, td, tv;
            if (sscanf(line, "O %lld %d pan=%d/%d tilt=%d/%d", &now, &kind, &pd, &pv, &td, &tv) == 6) {
                int ok = live_kind[i] ? (kind == TL_VEL ? (pd == live_sx[i] && pv == (live_sx[i] ? live_dx[i] : 0) && td == -live_sy[i] && tv == (live_sy[i] ? live_dy[i] : 0)) :
                                                         (kind == TL_STOP && !live_sx[i] && !live_sy[i])) : kind == TL_NONE;
                same += ok;
                if (!ok) printf("    tick %d live calls %d (%d,%d,%d,%d) replay kind %d pan %d/%d tilt %d/%d\n", i, live_kind[i], live_sx[i], live_sy[i], live_dx[i], live_dy[i], kind, pd, pv, td, tv);
                i++;
            }
        }
        if (o) fclose(o);
        CHECK(i == 60 && same == 60, "replay of the recording: the same decision as the live tracker on all 60 ticks");
    }
    remove(path);
    /* the file is bounded: rolling, the C line kept */
    a2k_track_set_debug(&t, 2);
    for (k = 0; k < 24000; k++) { /* 24000 ticks write well over 1 MB: the file rolls */
        struct tl_box b = { 100, 30, 160, 170, 0, 700 };
        a2k_track_tick_ts(&t, 200000 + k * 200, 199900 + k * 200, &b, 1, 320, 192);
        a2k_track_rec_ring(200000 + k * 200, 1, 2);
    }
    {
        struct stat sb;
        FILE *f;
        int first_c = 0;
        CHECK(stat(RAW, &sb) == 0 && sb.st_size <= 1048576 && sb.st_size > 262144, "the raw file never exceeds 1 MB (rolling) and kept its newest half");
        f = fopen(RAW, "r");
        if (f && fgets(line, sizeof line, f))
            first_c = line[0] == 'C';
        if (f) fclose(f);
        CHECK(first_c, "after a roll the file starts with the C line again (a replay still has its config)");
    }
    { /* M1: a low crop (rule 7) is logged as low-crop with the box top as its reference row */
        FILE *f;
        int seen = 0;
        (void)a2k_track_set_cfg(&t, &c);
        a2k_track_set_debug(&t, 2);
        for (k = 0; k < 6; k++) { struct tl_box b = { 100, 150, 220, 191, 0, 700 }; a2k_track_tick_ts(&t, 400000 + k * 200, 399900 + k * 200, &b, 1, 320, 192); a2k_track_rec_ring(400000 + k * 200, 1, 2); }
        a2k_track_set_debug(&t, 0);
        f = fopen(RAW, "r");
        while (f && fgets(line, sizeof line, f))
            { if (getenv("TL_TRACE") && !strncmp(line, "D ", 2)) printf("%s", line); if (!strncmp(line, "D ", 2) && strstr(line, "rule=low-crop") && strstr(line, "ref=160,150")) seen++; }
        if (f) fclose(f);
        printf("   low-crop D lines: %d\n", seen);
        CHECK(seen >= 3, "a rule-7 box is logged as rule=low-crop with ref = (centre, box top)");
    }
    a2k_track_set_debug(&t, 0);
    a2k_track_rec_ring(1, 1, 1);
    remove(RAW);
    a2k_track_destroy(&t);
    printf("rot2k track rec unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
