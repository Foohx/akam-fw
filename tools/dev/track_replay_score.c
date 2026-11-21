/* Open-loop replay and score of a recorded tracker session (host). Reads a raw recording (akam2k.conf track_debug=2, /tmp/akam2k_track.raw on the camera, format in
 * docs/porting/lsc-rotatable-2k/tracking.md "The raw recorder"), runs the CURRENT tl_update (track_logic.c) on the recorded C / I lines (exact inputs, so the camera pose is the
 * recorded one, not a simulated one: the decisions are scored, the motor loop is not) and prints one "name key=value ..." line like tools/dev/track_sim.
 * Usage: track_replay_score [--cfg k=v] [--frame WxH] [--emit] FILE...      --emit: also print "V <ms> pan_dir pan_div tilt_dir tilt_div" for every command of the current tracker
 *        (the input of tools/dev/motor_replay.sh)
 * Keys: ticks, dur_s, lock (% of the ticks, absences left out, with a person box (class 0, score > 0 = detector or relay)), err_mean / err_p95 (px, horizontal offset of the picked box from the
 * frame centre), rev_lat_mean / rev_lat_max (ms from the picked box changing side of the centre (>= 25 px, after 500 ms on the other side) to a pan command towards it, 4000 = never),
 * still_cmds (commands while the box has moved < 6 px for 3 s), flips (pan command direction reversals), cmds, starts (command runs from idle), reacq_mean / reacq_max (ms of the
 * gaps of >= 1 s without a box, until a box is back), relay_pct (% of the boxed ticks that carry score 0 = a relay box), pan_range / tilt_range (recorded steps), lat_mean_ms /
 * lat_slope_ms_min (now - cap: the video-to-decision latency and its growth per minute, least squares), lock_all (the lock over ALL ticks), absent_n / absent_s (absences: runs of >= 60 s without any
 * box, nobody was there; lock and reacq_* leave them out: a person who left for minutes is not a loss to reacquire, and idle time is not tracking time). */
#include "../../src/features/track/track_cfgkv.h"
#include "../../src/features/track/track_logic.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXT 400000
static int g_off[MAXT];
static long long g_ts[MAXT];
static signed char g_cmd[MAXT];
static char g_got[MAXT];
#define ABSENT_MS 60000 /* a run of ticks without any box this long is an absence (nobody there: the tracker went home), not a loss to reacquire */

static int cmp_i(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int parse_boxes(const char *s, const char **end, int n, struct tl_box *b)
{
    for (int i = 0; i < n; i++) {
        int c, sc, x1, y1, x2, y2, used = 0;
        while (*s == ' ') s++;
        if (sscanf(s, "{%d,%d,%d,%d,%d,%d}%n", &c, &sc, &x1, &y1, &x2, &y2, &used) != 6 || !used) return -1;
        b[i].cls = c; b[i].score = sc; b[i].x1 = x1; b[i].y1 = y1; b[i].x2 = x2; b[i].y2 = y2;
        s += used;
    }
    *end = s;
    return 0;
}

static void cfg_from_line(struct tl_cfg *c, const char *l)
{
    char tok[512];
    const char *p = l + 1;
    while (sscanf(p, " %511s", tok) == 1) {
        char *eq = strchr(tok, '=');
        p = strstr(p, tok) + strlen(tok);
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(tok, "lost")) { (void)tl_lost_parse(eq + 1, c); continue; }
        (void)tl_cfg_kv_set(c, tok, atoi(eq + 1));
    }
}

static int g_trace;
static int score(const char *path, int fw, int fh, char ov[][48], int nov, int emit, const char *name)
{
    FILE *f = fopen(path, "r");
    static char line[8192];
    tl_t t;
    struct tl_cfg c;
    int have = 0, n_t = 0, boxed = 0, relay = 0, i, flips = 0, lastd = 0, cmds = 0, starts = 0, prev_active = 0, still_cmds = 0;
    int pmin = 1 << 30, pmax = -(1 << 30), tmin = 1 << 30, tmax = -(1 << 30);
    double esum = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, latsum = 0;
    long long last_box_ms = -1, gap_start = -1;
    int ra_n = 0, ra_max = 0, rl_n = 0, rl_max = 0, side = 0, corr = 0;
    double ra_sum = 0, rl_sum = 0;
    int absent_n = 0, absent_ticks = 0;
    long long absent_ms = 0;
    static int pend_t[4096];
    int np = 0;
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    tl_cfg_defaults(&c);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'C' && line[1] == ' ') { cfg_from_line(&c, line); have = 0; continue; }
        if (line[0] == 'I' && line[1] == ' ') {
            long long now, cap, me = 0;
            struct tl_ptz p;
            struct tl_box in[TL_MAX_ALL * 2 + 4];
            int n = 0, off = 0, cr = 0, ob = 0, rb = 0, ct = 0, moving, rp, rt, pickoff = 0, got;
            const char *s, *end;
            struct tl_out o;
            memset(&p, 0, sizeof p);
            if (sscanf(line, "I %lld %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d%n", &now, &cap, &p.pan, &p.tilt, &p.has_cap, &p.cap_pan, &p.cap_tilt, &rp, &rt, &moving, &p.tilt_home,
                       &p.pan_min, &p.pan_max, &p.tilt_min, &p.tilt_max, &n, &off) != 16 || n < 0 || n > TL_MAX_ALL) continue;
            if (parse_boxes(line + off, &end, n, in)) continue;
            if ((s = strstr(end, " me=")) != NULL) sscanf(s, " me=%lld cr=%d ob=%d rb=%d ct=%d", &me, &cr, &ob, &rb, &ct);
            p.cap_ms = cap; p.cap_rate_pan = rp; p.cap_rate_tilt = rt; p.moving = moving; p.motion_end_ms = me; p.cap_rate_ok = cr; p.over_budget = ob; p.route_busy = rb; p.cont = ct;
            if (!have) {
                for (i = 0; i < nov; i++) { char k[64], *eq; snprintf(k, sizeof k, "%.63s", ov[i]); eq = strchr(k, '='); if (eq) { *eq = 0; (void)tl_cfg_kv_set(&c, k, atoi(eq + 1)); } }
                if (tl_init(&t, &c) != 0) { fclose(f); return 2; }
                have = 1;
            }
            tl_update(&t, now, in, n, fw, fh, &p, &o);
            if (n_t >= MAXT) break;
            g_ts[n_t] = now;
            if (g_trace) printf("T %lld n=%d pan=%d tilt=%d box=%d,%d,%d,%d rule=%d why=%s ex=%d ey=%d vdy=%d dist=%d\n", now, n, p.pan, p.tilt, t.dbg.box.x1, t.dbg.box.y1, t.dbg.box.x2, t.dbg.box.y2, t.dbg.rule, t.dbg.why ? t.dbg.why : "-", t.dbg.ex, t.dbg.ey, t.dbg.vdy, t.dist_mm);
            got = t.dbg.has_box;
            if (p.pan < pmin) pmin = p.pan;
            if (p.pan > pmax) pmax = p.pan;
            if (p.tilt < tmin) tmin = p.tilt;
            if (p.tilt > tmax) tmax = p.tilt;
            if (cap > 0 && now - cap < 5000) { double x = (double)now / 60000.0, y = (double)(now - cap); latsum += y; sx += x; sy += y; sxx += x * x; sxy += x * y; }
            {   /* the picked box: offset from the centre */
                int pb = 0;
                for (i = 0; i < n; i++) if (in[i].cls == 0 && in[i].score == 0) pb = 1;
                if (got) {
                    pickoff = (t.dbg.box.x1 + t.dbg.box.x2) / 2 - fw / 2;
                    boxed++;
                    if (pb && n == 1) relay++;
                }
            }
            g_off[n_t] = got ? pickoff : 0;
            g_got[n_t] = got ? 1 : 0;
            {   /* the commanded pan direction (image sense: + = the box is to the right) and the counters */
                int d = o.kind == TL_VEL ? o.pan_dir : o.kind == TL_NUDGE ? (o.dpan > 0 ? 1 : o.dpan < 0 ? -1 : 0) : 0;
                int active = (o.kind == TL_VEL && (o.pan_dir || o.tilt_dir)) || o.kind == TL_NUDGE;
                g_cmd[n_t] = (signed char)d;
                if (active) cmds++;
                if (active && !prev_active) starts++;
                prev_active = active;
                if (d && lastd && d != lastd) flips++;
                if (d) lastd = d;
                if (got && d) corr += (pickoff > 0 ? 1 : pickoff < 0 ? -1 : 0) * d;
                if (emit && (o.kind == TL_VEL || o.kind == TL_STOP))
                    printf("V %lld %d %d %d %d\n", now, o.kind == TL_VEL ? o.pan_dir : 0, o.kind == TL_VEL ? o.pan_div : 0, o.kind == TL_VEL ? o.tilt_dir : 0, o.kind == TL_VEL ? o.tilt_div : 0);
            }
            if (got) { /* reacquire gaps */
                if (gap_start >= 0 && now - gap_start >= ABSENT_MS) absent_n++;
                else if (gap_start >= 0 && now - gap_start >= 1000) { double g = (double)(now - gap_start); ra_sum += g; ra_n++; if (g > ra_max) ra_max = (int)g; }
                gap_start = -1; last_box_ms = now;
            } else if (gap_start < 0 && last_box_ms >= 0) gap_start = last_box_ms;
            n_t++;
        }
    }
    fclose(f);
    if (n_t < 2) { printf("%s ticks=%d\n", name, n_t); return 0; }
    {   /* the sign convention between the box side and the pan command, from the data (a motor sign depends on invert_pan) */
        int sgn = corr >= 0 ? 1 : -1, ne = 0;
        static int e[MAXT];
        long last_side_ms = 0;
        for (i = 0; i < n_t; i++) {
            if (g_off[i]) { e[ne++] = abs(g_off[i]); esum += abs(g_off[i]); }
            if (abs(g_off[i]) >= 25) {   /* reaction latency to a change of side */
                int s2 = g_off[i] > 0 ? 1 : -1;
                if (s2 != side) {
                    if (side && g_ts[i] - last_side_ms >= 500 && np < 4096) pend_t[np++] = i;
                    side = s2; last_side_ms = g_ts[i];
                }
            }
        }
        for (int k = 0; k < np; k++) {
            int lat = 4000;
            for (i = pend_t[k]; i < n_t && g_ts[i] - g_ts[pend_t[k]] < 4000; i++)
                if (g_cmd[i] * sgn == (g_off[pend_t[k]] > 0 ? 1 : -1)) { lat = (int)(g_ts[i] - g_ts[pend_t[k]]); break; }
            rl_sum += lat; rl_n++; if (lat > rl_max) rl_max = lat;
        }
        for (i = 0; i < n_t; i++) {   /* commands while the box is still: moved < 6 px over the last 3 s */
            int j = i, still = g_off[i] != 0;
            while (j > 0 && g_ts[i] - g_ts[j] < 3000) { j--; if (!g_off[j] || abs(g_off[j] - g_off[i]) >= 6) { still = 0; break; } }
            if (still && g_ts[i] - g_ts[0] > 3000 && g_cmd[i]) still_cmds++;
        }
        for (i = 0; i < n_t;) {   /* absences: runs of ticks without a box lasting >= ABSENT_MS (a trailing run counts too) */
            int j = i;
            if (g_got[i]) { i++; continue; }
            while (j < n_t && !g_got[j]) j++;
            if (g_ts[j - 1] - g_ts[i] >= ABSENT_MS) { absent_ticks += j - i; absent_ms += g_ts[j - 1] - g_ts[i]; if (j == n_t) absent_n++; }
            i = j;
        }
        if (ne) qsort(e, (size_t)ne, sizeof e[0], cmp_i);
        {
            double den = n_t * sxx - sx * sx;
            printf("%s ticks=%d dur_s=%.0f lock=%.1f err_mean=%.1f err_p95=%d rev_lat_mean=%.0f rev_lat_max=%d still_cmds=%d flips=%d cmds=%d starts=%d reacq_mean=%.0f reacq_max=%d relay_pct=%.1f pan_range=%d tilt_range=%d lat_mean_ms=%.0f lat_slope_ms_min=%.1f lock_all=%.1f absent_n=%d absent_s=%.0f\n",
                   name, n_t, (double)(g_ts[n_t - 1] - g_ts[0]) / 1000.0, (n_t - absent_ticks) > 0 ? 100.0 * boxed / (n_t - absent_ticks) : 0.0, ne ? esum / ne : 0.0, ne ? e[(long)ne * 95 / 100] : 0, rl_n ? rl_sum / rl_n : -1.0, rl_n ? rl_max : -1,
                   still_cmds, flips, cmds, starts, ra_n ? ra_sum / ra_n : -1.0, ra_n ? ra_max : -1, boxed ? 100.0 * relay / boxed : 0.0, pmax - pmin, tmax - tmin, latsum / n_t,
                   fabs(den) > 1e-9 ? (n_t * sxy - sx * sy) / den : 0.0, 100.0 * boxed / n_t, absent_n, (double)absent_ms / 1000.0);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    char ov[16][48];
    int nov = 0, fw = 320, fh = 192, emit = 0, rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cfg") && i + 1 < argc && nov < 16) snprintf(ov[nov++], sizeof ov[0], "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frame") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &fw, &fh) != 2) return 2; }
        else if (!strcmp(argv[i], "--emit")) emit = 1;
        else if (!strcmp(argv[i], "--trace")) g_trace = 1; /* one "T ..." line per tick: pose, picked box, rule, why, ex / ey, vdy, distance */
        else if (argv[i][0] != '-') {
            const char *b = strrchr(argv[i], '/');
            rc |= score(argv[i], fw, fh, ov, nov, emit, b ? b + 1 : argv[i]);
            any = 1;
        } else { fprintf(stderr, "usage: %s [--cfg k=v] [--frame WxH] [--emit] FILE...\n", argv[0]); return 2; }
    }
    if (!any) { fprintf(stderr, "no file\n"); return 2; }
    return rc;
}
