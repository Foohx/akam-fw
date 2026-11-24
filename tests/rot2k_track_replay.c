/* Replay of a raw tracker recording (akam.conf track_debug=2, /tmp/akam2k_track.raw on the camera; docs/porting/lsc-rotatable-2k/tracking.md "Hardware calibration
 * experiments"): reads the C (config), I (one tl_update input) and ignores the R / D / # lines, runs tl_update with the configuration and the exact inputs, and prints one
 * decision line per I line:
 *   O <now> <kind> pan=<dir>/<div> tilt=<dir>/<div> nudge=<dpan>/<dtilt> why=<why> rule=<n> dist=<mm> cue=<h|w|carried|none|->
 * Usage: rot2k_track_replay [options] FILE
 *   --cfg key=value   override a tl_cfg field (any name of track_cfgkv.h) after the C line, e.g. --cfg lag_ms=100
 *   --frame WxH       detector frame (default 320x192)
 *   --twice           replay twice and fail unless both outputs are identical (determinism)
 *   --heads           replay in lockstep a second tracker whose input also carries head boxes (class 1) in the frame corners, belonging to nobody, and fail on the first different decision (rule 6 uses only a head associated with the target)
 *   -q                no decision lines, only the summary
 * A fixture may carry "# expect NAME<op>N" lines (op = == <= >=): names tilt_down tilt_up pan_pos pan_neg moves searches ticks; each is checked against the replay and the exit
 * code is 1 when one fails. The summary line "S ..." counts them.
 * As a library (REPLAY_LIB defined, tests/rot2k_track_rec_unit.c): replay_file() is exported and main is not built. */
#include "../src/features/track/track_cfgkv.h"
#include "../src/features/track/track_logic.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct replay_stats { int ticks, moves, tilt_down, tilt_up, pan_pos, pan_neg, searches, diff; };

struct replay_opt { int fw, fh, heads; int nov; char ov[16][48]; };

static struct tl_box *parse_boxes(const char *s, const char **end, int n, struct tl_box *b)
{
    int i;
    for (i = 0; i < n; i++) {
        int c, sc, x1, y1, x2, y2, used = 0;
        while (*s == ' ') s++;
        if (sscanf(s, "{%d,%d,%d,%d,%d,%d}%n", &c, &sc, &x1, &y1, &x2, &y2, &used) != 6 || !used) return NULL;
        b[i].cls = c; b[i].score = sc; b[i].x1 = x1; b[i].y1 = y1; b[i].x2 = x2; b[i].y2 = y2;
        s += used;
    }
    *end = s;
    return b;
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

static void count(struct replay_stats *st, const tl_t *t, const struct tl_out *o)
{
    st->ticks++;
    if (o->kind == TL_NUDGE || o->kind == TL_VEL) st->moves++;
    if ((o->kind == TL_NUDGE && o->dtilt > 0) || (o->kind == TL_VEL && o->tilt_dir > 0)) st->tilt_down++;
    if ((o->kind == TL_NUDGE && o->dtilt < 0) || (o->kind == TL_VEL && o->tilt_dir < 0)) st->tilt_up++;
    if ((o->kind == TL_NUDGE && o->dpan > 0) || (o->kind == TL_VEL && o->pan_dir > 0)) st->pan_pos++;
    if ((o->kind == TL_NUDGE && o->dpan < 0) || (o->kind == TL_VEL && o->pan_dir < 0)) st->pan_neg++;
    if (t->dbg.why && !strcmp(t->dbg.why, "exit-search")) st->searches++;
}

static int same_out(const struct tl_out *a, const struct tl_out *b)
{
    return a->kind == b->kind && a->dpan == b->dpan && a->dtilt == b->dtilt && a->speed == b->speed && a->pan_dir == b->pan_dir && a->pan_div == b->pan_div && a->tilt_dir == b->tilt_dir &&
           a->tilt_div == b->tilt_div && a->route_op == b->route_op;
}

/* One pass over the file. out may be NULL. Returns 0, or -1 when the file cannot be read. */
static int replay_file(const char *path, const struct replay_opt *op, FILE *out, struct replay_stats *st, char *expect, size_t expcap)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    tl_t t, th;
    struct tl_cfg c;
    int have = 0;
    memset(st, 0, sizeof *st);
    if (expect && expcap) expect[0] = 0;
    if (!f) return -1;
    tl_cfg_defaults(&c);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' && expect && !strncmp(line, "# expect ", 9)) {
            size_t k = strlen(expect);
            snprintf(expect + k, expcap - k, "%s", line + 9);
        } else if (line[0] == 'C' && line[1] == ' ') {
            cfg_from_line(&c, line);
            have = 0;
        } else if (line[0] == 'I' && line[1] == ' ') {
            long long now, cap, me = 0;
            struct tl_ptz p;
            struct tl_box in[TL_MAX_ALL * 2 + 4], hin[TL_MAX_ALL * 2 + 4];
            int n = 0, hn, i, off = 0, cr = 0, ob = 0, rb = 0, ct = 0, moving, rp, rt;
            const char *s, *end;
            struct tl_out o, oh;
            memset(&p, 0, sizeof p);
            if (sscanf(line, "I %lld %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d%n", &now, &cap, &p.pan, &p.tilt, &p.has_cap, &p.cap_pan, &p.cap_tilt, &rp, &rt, &moving, &p.tilt_home,
                       &p.pan_min, &p.pan_max, &p.tilt_min, &p.tilt_max, &n, &off) != 16 || n < 0 || n > TL_MAX_ALL) continue;
            if (!parse_boxes(line + off, &end, n, in)) continue;
            s = end;
            if ((s = strstr(s, " me=")) != NULL) sscanf(s, " me=%lld cr=%d ob=%d rb=%d ct=%d", &me, &cr, &ob, &rb, &ct);
            p.cap_ms = cap; p.cap_rate_pan = rp; p.cap_rate_tilt = rt; p.moving = moving; p.motion_end_ms = me; p.cap_rate_ok = cr; p.over_budget = ob; p.route_busy = rb; p.cont = ct;
            if (!have) {
                for (i = 0; i < op->nov; i++) { char k[48], *eq; snprintf(k, sizeof k, "%s", op->ov[i]); eq = strchr(k, '='); if (eq) { *eq = 0; (void)tl_cfg_kv_set(&c, k, atoi(eq + 1)); } }
                if (tl_init(&t, &c) != 0) { fclose(f); return -1; }
                if (op->heads) (void)tl_init(&th, &c);
                have = 1;
            }
            tl_update(&t, now, in, n, op->fw, op->fh, &p, &o);
            count(st, &t, &o);
            if (out)
                fprintf(out, "O %lld %d pan=%d/%d tilt=%d/%d nudge=%d/%d why=%s rule=%d dist=%d cue=%s\n", now, (int)o.kind, o.kind == TL_VEL ? o.pan_dir : 0, o.kind == TL_VEL ? o.pan_div : 0,
                        o.kind == TL_VEL ? o.tilt_dir : 0, o.kind == TL_VEL ? o.tilt_div : 0, o.kind == TL_NUDGE ? o.dpan : 0, o.kind == TL_NUDGE ? o.dtilt : 0, t.dbg.why ? t.dbg.why : "?",
                        t.dbg.rule, t.dist_mm, t.dbg.dcue ? t.dbg.dcue : "-");
            if (op->heads) { /* the same frame with heads that belong to nobody (the lower corners of the frame (below the upper 45 % of any box), far from every person box and from the predicted target): the policy must not use them (rule 6 associates only a head of the target) */
                struct tl_box h0 = { 280, 150, 312, 188, TL_CLS_HEAD, 420 }, h1 = { 4, 150, 30, 188, TL_CLS_HEAD, 610 };
                hn = 0;
                hin[hn++] = h0;
                for (i = 0; i < n && hn < TL_MAX_ALL * 2 - 1; i++) hin[hn++] = in[i];
                hin[hn++] = h1;
                memset(&oh, 0, sizeof oh);
                tl_update(&th, now, hin, hn, op->fw, op->fh, &p, &oh);
                if (!same_out(&o, &oh) || strcmp(t.dbg.why ? t.dbg.why : "", th.dbg.why ? th.dbg.why : "")) st->diff++;
            }
        }
    }
    fclose(f);
    return 0;
}

__attribute__((unused)) static int expect_ok(const struct replay_stats *st, const char *expect)
{
    char buf[1024], *l, *sv = NULL;
    int bad = 0;
    snprintf(buf, sizeof buf, "%s", expect);
    for (l = strtok_r(buf, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
        char name[32], op[3] = { 0 };
        int v, got;
        const char *q = l;
        size_t k = strspn(q, " ");
        q += k;
        k = strcspn(q, "<>=");
        if (k == 0 || k >= sizeof name) continue;
        memcpy(name, q, k); name[k] = 0;
        q += k;
        op[0] = q[0]; if (q[1] == '=') { op[1] = '='; q += 2; } else q += 1;
        v = atoi(q);
        got = !strcmp(name, "tilt_down") ? st->tilt_down : !strcmp(name, "tilt_up") ? st->tilt_up : !strcmp(name, "pan_pos") ? st->pan_pos : !strcmp(name, "pan_neg") ? st->pan_neg :
              !strcmp(name, "moves") ? st->moves : !strcmp(name, "searches") ? st->searches : !strcmp(name, "ticks") ? st->ticks : -1;
        if (got < 0 || (!strcmp(op, "=") && got != v) || (!strcmp(op, "==") && got != v) || (!strcmp(op, "<=") && got > v) || (!strcmp(op, ">=") && got < v)) {
            printf("EXPECT FAILED: %s%s%d (got %d)\n", name, op, v, got);
            bad++;
        }
    }
    return bad;
}

#ifndef REPLAY_LIB
int main(int argc, char **argv)
{
    struct replay_opt op;
    struct replay_stats st, st2;
    char expect[1024];
    int twice = 0, quiet = 0, i, bad = 0;
    const char *path = NULL;
    memset(&op, 0, sizeof op);
    op.fw = 320; op.fh = 192;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cfg") && i + 1 < argc && op.nov < 16) snprintf(op.ov[op.nov++], sizeof op.ov[0], "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frame") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &op.fw, &op.fh) != 2) return 2; }
        else if (!strcmp(argv[i], "--twice")) twice = 1;
        else if (!strcmp(argv[i], "--heads")) op.heads = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] != '-') path = argv[i];
        else { fprintf(stderr, "usage: %s [--cfg k=v] [--frame WxH] [--twice] [--heads] [-q] FILE\n", argv[0]); return 2; }
    }
    if (!path) { fprintf(stderr, "no file\n"); return 2; }
    if (twice) { /* the same replay into two buffers: byte for byte equal */
        char *a = NULL, *b = NULL; size_t as = 0, bs = 0;
        FILE *fa = open_memstream(&a, &as), *fb = open_memstream(&b, &bs);
        if (!fa || !fb || replay_file(path, &op, fa, &st, expect, sizeof expect) || replay_file(path, &op, fb, &st2, NULL, 0)) { fprintf(stderr, "cannot read %s\n", path); return 2; }
        fclose(fa); fclose(fb);
        if (as != bs || memcmp(a, b, as)) { printf("NOT DETERMINISTIC\n"); bad++; }
        if (!quiet) fwrite(a, 1, as, stdout);
        free(a); free(b);
    } else if (replay_file(path, &op, quiet ? NULL : stdout, &st, expect, sizeof expect)) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    printf("S ticks=%d moves=%d tilt_down=%d tilt_up=%d pan_pos=%d pan_neg=%d searches=%d heads_diff=%d\n", st.ticks, st.moves, st.tilt_down, st.tilt_up, st.pan_pos, st.pan_neg, st.searches, st.diff);
    if (op.heads && st.diff) { printf("HEADS CHANGED %d DECISIONS\n", st.diff); bad++; }
    bad += expect_ok(&st, expect);
    return bad ? 1 : 0;
}
#endif
