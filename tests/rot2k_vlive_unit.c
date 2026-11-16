/* Host unit test of the live video apply (akam2k_vlive.c) with a fake encoder: live success per setter, a refusal falls back (returns 0) and switches
 * that setter off (logged once), the disabled flag, the video_live=0 switch, avbr = re-open, set_attr keeps width / height / profile / enc_type,
 * and the parsing and running of the hardware-test hook commands. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "akam2k_vlive.h"

static int fails;
static int logs;
void a2k_log(const char *fmt, ...) { (void)fmt; logs++; }
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static struct { int fps, qp, get, set, rct, cur, stat; int rc_fps, rc_qp, rc_get, rc_set, rc_rct; struct venc_param attr; int last_fps, last_min, last_max, last_rct; } F;
static int f_set_fps(int h, int fps) { (void)h; F.fps++; F.last_fps = fps; return F.rc_fps; }
static int f_set_qp(int h, int a, int b) { (void)h; F.qp++; F.last_min = a; F.last_max = b; return F.rc_qp; }
static int f_get_attr(int h, struct venc_param *o) { (void)h; F.get++; *o = F.attr; return F.rc_get; }
static int f_set_attr(int h, const struct venc_param *p) { (void)h; F.set++; if (!F.rc_set) F.attr = *p; return F.rc_set; }
static int f_set_rct(int h, int t) { (void)h; F.rct++; F.last_rct = t; return F.rc_rct; }
static int f_cur(int h, void *o) { (void)h; (void)o; F.cur++; return 0; }
static int f_stat(int h, void *o) { (void)h; (void)o; F.stat++; return 0; }
static const a2k_vlive_ops_t OPS = { f_set_fps, f_set_qp, f_get_attr, f_set_attr, f_set_rct, f_cur, f_stat };

static void setup(struct venc_param *vp, a2k_vid_t *want, a2k_vlive_t *v, int enabled)
{
    memset(&F, 0, sizeof F);
    memset(vp, 0, sizeof *vp);
    a2k_vid_defaults(1, 512, 30, want);
    vp->width = 640; vp->height = 360; vp->profile = 2; vp->enc_type = 0;
    vp->fps = (uint16_t)want->v[A2K_VP_FPS]; vp->gop = (uint16_t)want->v[A2K_VP_GOP];
    vp->target_kbps = (uint16_t)want->v[A2K_VP_KBPS]; vp->max_kbps = (uint16_t)a2k_vid_eff_max_kbps(want);
    vp->min_qp = (uint16_t)want->v[A2K_VP_MINQP]; vp->max_qp = (uint16_t)want->v[A2K_VP_MAXQP]; vp->rc_mode = (uint32_t)want->v[A2K_VP_RC];
    F.attr = *vp;
    a2k_vlive_init(v, &OPS, enabled);
}

static void test_apply(void)
{
    struct venc_param vp;
    a2k_vid_t w;
    a2k_vlive_t v;
    printf("== live apply\n");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_FPS] = 10;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 1 && F.fps == 1 && F.last_fps == 10 && !F.qp && !F.get && !F.set && !F.rct && vp.fps == 10, "fps -> set_fps only");
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.fps == 1, "nothing differs: no call");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_MINQP] = 35; w.v[A2K_VP_MAXQP] = 45;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 1 && F.qp == 1 && F.last_min == 35 && F.last_max == 45 && !F.set && vp.min_qp == 35 && vp.max_qp == 45, "qp -> set_qp only");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_KBPS] = 300; w.v[A2K_VP_MAXKBPS] = 400; w.v[A2K_VP_GOP] = 15;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 1 && F.get == 1 && F.set == 1 && !F.fps && !F.qp, "kbps / max / gop -> get_attr + set_attr");
    CHECK(F.attr.target_kbps == 300 && F.attr.max_kbps == 400 && F.attr.gop == 15 && F.attr.width == 640 && F.attr.height == 360 && F.attr.profile == 2 && F.attr.enc_type == 0,
          "set_attr carries the new rates and leaves width, height, profile, enc_type");
    CHECK(vp.target_kbps == 300 && vp.max_kbps == 400 && vp.gop == 15, "the running copy follows");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_RC] = A2K_RC_CBR;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 1 && F.rct == 1 && F.last_rct == 0 && vp.rc_mode == 0, "vbr -> cbr: set_rc_type(0)");
    w.v[A2K_VP_RC] = A2K_RC_AVBR;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.rct == 1, "-> avbr: re-open, no call");
    setup(&vp, &w, &v, 1);
    vp.rc_mode = A2K_RC_AVBR; w.v[A2K_VP_RC] = A2K_RC_VBR;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.rct == 0, "avbr -> vbr: re-open, no call");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_FPS] = 10; w.v[A2K_VP_MINQP] = 35; w.v[A2K_VP_KBPS] = 300; w.v[A2K_VP_RC] = A2K_RC_CBR;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 1 && F.fps && F.qp && F.rct && F.set, "several changes: all setters");
    CHECK(F.attr.fps == 10 && F.attr.min_qp == 35 && F.attr.rc_mode == 0, "set_attr keeps the fps / QP / rc the other setters just set");
}

static void test_fail(void)
{
    struct venc_param vp;
    a2k_vid_t w;
    a2k_vlive_t v;
    int before;
    printf("== refusal, disabled flag, switch\n");
    setup(&vp, &w, &v, 1);
    F.rc_fps = -1;
    w.v[A2K_VP_FPS] = 10;
    before = logs;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.fps == 1 && v.off[A2K_VL_FPS] && logs > before, "set_fps refused: 0 (the caller re-opens with the same want), setter off");
    CHECK(vp.fps == 15, "a refused setter does not change the running copy");
    F.rc_fps = 0;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.fps == 1, "disabled setter is not retried");
    w.v[A2K_VP_MINQP] = 35;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.qp == 0, "a change needing a disabled setter calls none of the others (one re-open does all)");
    setup(&vp, &w, &v, 1);
    F.rc_set = 0x0d000012;
    w.v[A2K_VP_GOP] = 15;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && v.off[A2K_VL_ATTR] && !v.off[A2K_VL_FPS], "set_attr refused: only that setter off");
    CHECK(vp.gop == 30, "refused set_attr leaves the running copy");
    setup(&vp, &w, &v, 1);
    F.rc_get = -1;
    w.v[A2K_VP_GOP] = 15;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.set == 0 && v.off[A2K_VL_ATTR], "get_attr failure: no set_attr, fallback");
    setup(&vp, &w, &v, 0);
    w.v[A2K_VP_FPS] = 10;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.fps == 0, "video_live=0: never live");
    setup(&vp, &w, &v, 1);
    w.v[A2K_VP_FPS] = 10;
    CHECK(a2k_vlive_apply(&v, 1, -1, &vp, &w) == 0 && F.fps == 0, "no encoder handle: no call");
    w.v[A2K_VP_MINQP] = 50; w.v[A2K_VP_MAXQP] = 40;
    CHECK(a2k_vlive_apply(&v, 1, 3, &vp, &w) == 0 && F.fps == 0, "an invalid want is never sent");
}

static void test_hook(void)
{
    a2k_vlive_cmd_t c;
    struct venc_param vp;
    a2k_vid_t w;
    a2k_vlive_t v;
    printf("== debug hook\n");
    CHECK(a2k_vlive_parse_cmd("sub fps 10\n", &c) == 0 && c.chn == 1 && c.kind == A2K_VC_FPS && c.a == 10, "sub fps 10");
    CHECK(a2k_vlive_parse_cmd("main qp 32 45", &c) == 0 && c.chn == 0 && c.kind == A2K_VC_QP && c.a == 32 && c.b == 45, "main qp 32 45");
    CHECK(a2k_vlive_parse_cmd("sub kbps 300 400\n", &c) == 0 && c.kind == A2K_VC_KBPS && c.a == 300 && c.b == 400, "sub kbps 300 400");
    CHECK(a2k_vlive_parse_cmd("sub gop 30", &c) == 0 && c.kind == A2K_VC_GOP && c.a == 30, "sub gop 30");
    CHECK(a2k_vlive_parse_cmd("sub rc cbr", &c) == 0 && c.kind == A2K_VC_RC && c.a == 0, "sub rc cbr");
    CHECK(a2k_vlive_parse_cmd("sub rc vbr\n", &c) == 0 && c.a == 1, "sub rc vbr");
    CHECK(a2k_vlive_parse_cmd("sub attr\n", &c) == 0 && c.kind == A2K_VC_ATTR, "sub attr");
    CHECK(a2k_vlive_parse_cmd("sub fps", &c) && a2k_vlive_parse_cmd("sub fps 1x", &c) && a2k_vlive_parse_cmd("sub fps -1", &c) && a2k_vlive_parse_cmd("sub fps 10 5", &c) &&
              a2k_vlive_parse_cmd("sub qp 32", &c) && a2k_vlive_parse_cmd("sub rc avbr", &c) && a2k_vlive_parse_cmd("both fps 10", &c) && a2k_vlive_parse_cmd("subfps 10", &c) &&
              a2k_vlive_parse_cmd("sub attrx", &c) && a2k_vlive_parse_cmd("", &c) && a2k_vlive_parse_cmd("sub fps 1234567", &c) && a2k_vlive_parse_cmd(NULL, &c),
          "bad commands are refused");
    {
        char path[] = "/tmp/rot2k_vlive_XXXXXX";
        int fd = mkstemp(path);
        FILE *f;
        a2k_vlive_cmd_t got;
        if (fd >= 0) {
            close(fd);
            f = fopen(path, "w"); fputs("sub fps 10\n", f); fclose(f);
            CHECK(a2k_vlive_poll(path, 0, &got) == 0 && access(path, F_OK) == 0, "poll: a command for the other stream stays");
            CHECK(a2k_vlive_poll(path, 1, &got) == 1 && got.kind == A2K_VC_FPS && got.a == 10 && access(path, F_OK) != 0, "poll: own command returned and deleted");
            CHECK(a2k_vlive_poll(path, 1, &got) == 0, "poll: no file, nothing");
            f = fopen(path, "w"); fputs("sub bogus\n", f); fclose(f);
            CHECK(a2k_vlive_poll(path, 1, &got) == 0 && access(path, F_OK) == 0, "poll: a bad one stays for the main thread");
            CHECK(a2k_vlive_poll(path, 0, &got) == 0 && access(path, F_OK) != 0, "poll: the main thread deletes a bad one");
            unlink(path);
        }
    }
    setup(&vp, &w, &v, 1);
    a2k_vlive_parse_cmd("sub fps 10", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.fps == 1 && vp.fps == 10, "run: fps, running copy follows");
    F.rc_fps = -1;
    a2k_vlive_parse_cmd("sub fps 5", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == -1 && vp.fps == 10 && !v.off[A2K_VL_FPS], "run: the rc is returned, no fallback, no disabled flag");
    v.off[A2K_VL_QP] = 1;
    a2k_vlive_parse_cmd("sub qp 32 45", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.qp == 1 && F.last_min == 32, "run: ignores the disabled flags");
    a2k_vlive_parse_cmd("sub kbps 200 250", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.attr.target_kbps == 200 && F.attr.max_kbps == 250 && F.attr.gop == 30, "run: kbps keeps the gop");
    a2k_vlive_parse_cmd("sub gop 15", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.attr.gop == 15 && F.attr.target_kbps == 200, "run: gop keeps the rates");
    a2k_vlive_parse_cmd("sub rc cbr", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.last_rct == 0, "run: rc");
    a2k_vlive_parse_cmd("sub attr", &c);
    CHECK(a2k_vlive_run_cmd(&v, 3, &vp, &c) == 0 && F.cur == 1 && F.stat == 1, "run: attr dumps get_attr, get_cur_qp, get_rc_stat");
}

int main(void)
{
    test_apply();
    test_fail();
    test_hook();
    printf("rot2k vlive unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
