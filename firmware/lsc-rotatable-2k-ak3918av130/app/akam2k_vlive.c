/* Live apply of the video settings and the hardware-test hook, see akam2k_vlive.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_log.h"
#include "akam2k_vlive.h"

void a2k_vlive_init(a2k_vlive_t *v, const a2k_vlive_ops_t *ops, int enabled)
{
    memset(v, 0, sizeof *v);
    v->ops = ops;
    v->enabled = enabled;
}

static void fail(a2k_vlive_t *v, int setter, const char *name, int chn, int rc)
{
    if (!v->off[setter])
        a2k_log("video: live %s refused (rc 0x%08x, stream %d): switched off, re-opening instead", name, (unsigned)rc, chn);
    v->off[setter] = 1;
}

/* get_attr, edit the rate fields, set_attr. fps / QP / rc of the cache come from *vp (the truth: their own setters may not refresh the lib's cache,
 * and set_attr writes the whole struct). Width, height, profile, enc_type stay as get_attr returned them. */
static int attr_step(const a2k_vlive_ops_t *o, int h, struct venc_param *vp, int kbps, int maxk, int gop)
{
    struct venc_param t;
    int rc;
    memset(&t, 0, sizeof t);
    rc = o->get_attr(h, &t);
    if (rc)
        return rc;
    t.fps = vp->fps;
    t.min_qp = vp->min_qp;
    t.max_qp = vp->max_qp;
    t.rc_mode = vp->rc_mode;
    t.target_kbps = (uint16_t)kbps;
    t.max_kbps = (uint16_t)maxk;
    t.gop = (uint16_t)gop;
    rc = o->set_attr(h, &t);
    if (rc)
        return rc;
    vp->target_kbps = t.target_kbps;
    vp->max_kbps = t.max_kbps;
    vp->gop = t.gop;
    return 0;
}

int a2k_vlive_apply(a2k_vlive_t *v, int chn, int h, struct venc_param *vp, const a2k_vid_t *want)
{
    const a2k_vlive_ops_t *o = v->ops;
    int fps, qp, attr, rcd, rc, mx;
    a2k_vid_t w = *want;

    if (!v->enabled || !o || h < 0 || !a2k_vid_valid(chn, &w))
        return 0;
    mx = a2k_vid_eff_max_kbps(&w);
    fps = vp->fps != (uint16_t)w.v[A2K_VP_FPS];
    qp = vp->min_qp != (uint16_t)w.v[A2K_VP_MINQP] || vp->max_qp != (uint16_t)w.v[A2K_VP_MAXQP];
    attr = vp->target_kbps != (uint16_t)w.v[A2K_VP_KBPS] || vp->max_kbps != (uint16_t)mx || vp->gop != (uint16_t)w.v[A2K_VP_GOP];
    rcd = vp->rc_mode != (uint32_t)w.v[A2K_VP_RC];
    if (rcd && (w.v[A2K_VP_RC] == A2K_RC_AVBR || vp->rc_mode == (uint32_t)A2K_RC_AVBR))
        return 0; /* avbr needs a re-open */
    if (!fps && !qp && !attr && !rcd)
        return 0; /* nothing differs: the caller settles the pending change with a re-open (harmless, rare) */
    if ((fps && v->off[A2K_VL_FPS]) || (qp && v->off[A2K_VL_QP]) || (attr && v->off[A2K_VL_ATTR]) || (rcd && v->off[A2K_VL_RC]))
        return 0;
    if (fps) {
        rc = o->set_fps(h, w.v[A2K_VP_FPS]);
        a2k_log("video: stream %d live ak_venc_set_fps(%d, %d) = %d", chn, h, w.v[A2K_VP_FPS], rc);
        if (rc) {
            fail(v, A2K_VL_FPS, "set_fps", chn, rc);
            return 0;
        }
        vp->fps = (uint16_t)w.v[A2K_VP_FPS];
    }
    if (qp) {
        rc = o->set_qp(h, w.v[A2K_VP_MINQP], w.v[A2K_VP_MAXQP]);
        a2k_log("video: stream %d live ak_venc_set_qp(%d, %d, %d) = %d", chn, h, w.v[A2K_VP_MINQP], w.v[A2K_VP_MAXQP], rc);
        if (rc) {
            fail(v, A2K_VL_QP, "set_qp", chn, rc);
            return 0;
        }
        vp->min_qp = (uint16_t)w.v[A2K_VP_MINQP];
        vp->max_qp = (uint16_t)w.v[A2K_VP_MAXQP];
    }
    if (rcd) {
        rc = o->set_rc_type(h, w.v[A2K_VP_RC]);
        a2k_log("video: stream %d live ak_venc_set_rc_type(%d, %d) = %d", chn, h, w.v[A2K_VP_RC], rc);
        if (rc) {
            fail(v, A2K_VL_RC, "set_rc_type", chn, rc);
            return 0;
        }
        vp->rc_mode = (uint32_t)w.v[A2K_VP_RC];
    }
    if (attr) {
        rc = attr_step(o, h, vp, w.v[A2K_VP_KBPS], mx, w.v[A2K_VP_GOP]);
        a2k_log("video: stream %d live get_attr/set_attr (%d kbit/s, peak %d, gop %d) = %d", chn, w.v[A2K_VP_KBPS], mx, w.v[A2K_VP_GOP], rc);
        if (rc) {
            fail(v, A2K_VL_ATTR, "set_attr", chn, rc);
            return 0;
        }
    }
    return 1;
}

/* ---- debug hook ---- */

static int num(const char **p, int *out)
{
    const char *s = *p;
    long n = 0;
    int d = 0;
    while (*s == ' ')
        s++;
    while ((*s >= '0' && *s <= '9') && d < 6) {
        n = n * 10 + (*s++ - '0');
        d++;
    }
    if (!d || (*s && *s != ' ' && *s != '\n'))
        return -1;
    *out = (int)n;
    *p = s;
    return 0;
}

static int word(const char **p, const char *w)
{
    size_t n = strlen(w);
    while (**p == ' ')
        (*p)++;
    if (strncmp(*p, w, n) || ((*p)[n] && (*p)[n] != ' ' && (*p)[n] != '\n'))
        return 0;
    *p += n;
    return 1;
}

int a2k_vlive_parse_cmd(const char *text, a2k_vlive_cmd_t *out)
{
    const char *p = text;
    a2k_vlive_cmd_t c = { 0, 0, 0, 0 };
    if (!text)
        return -1;
    if (word(&p, "main"))
        c.chn = 0;
    else if (word(&p, "sub"))
        c.chn = 1;
    else
        return -1;
    if (word(&p, "fps")) {
        c.kind = A2K_VC_FPS;
        if (num(&p, &c.a))
            return -1;
    } else if (word(&p, "qp")) {
        c.kind = A2K_VC_QP;
        if (num(&p, &c.a) || num(&p, &c.b))
            return -1;
    } else if (word(&p, "kbps")) {
        c.kind = A2K_VC_KBPS;
        if (num(&p, &c.a) || num(&p, &c.b))
            return -1;
    } else if (word(&p, "gop")) {
        c.kind = A2K_VC_GOP;
        if (num(&p, &c.a))
            return -1;
    } else if (word(&p, "rc")) {
        c.kind = A2K_VC_RC;
        if (word(&p, "cbr"))
            c.a = A2K_RC_CBR;
        else if (word(&p, "vbr"))
            c.a = A2K_RC_VBR;
        else
            return -1;
    } else if (word(&p, "attr")) {
        c.kind = A2K_VC_ATTR;
    } else {
        return -1;
    }
    while (*p == ' ' || *p == '\n')
        p++;
    if (*p)
        return -1;
    *out = c;
    return 0;
}

int a2k_vlive_poll(const char *file, int chn, a2k_vlive_cmd_t *out)
{
    char buf[96];
    FILE *f = fopen(file, "r");
    size_t n;
    a2k_vlive_cmd_t c;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (a2k_vlive_parse_cmd(buf, &c)) {
        if (chn == 0) {
            a2k_log("venc-test: bad command, deleted");
            (void)unlink(file);
        }
        return 0;
    }
    if (c.chn != chn)
        return 0;
    (void)unlink(file);
    *out = c;
    return 1;
}

int a2k_vlive_run_cmd(const a2k_vlive_t *v, int h, struct venc_param *vp, const a2k_vlive_cmd_t *c)
{
    const a2k_vlive_ops_t *o = v->ops;
    int rc = 0;
    switch (c->kind) {
    case A2K_VC_FPS:
        rc = o->set_fps(h, c->a);
        a2k_log("venc-test: stream %d ak_venc_set_fps(%d, %d) = %d", c->chn, h, c->a, rc);
        if (!rc)
            vp->fps = (uint16_t)c->a;
        break;
    case A2K_VC_QP:
        rc = o->set_qp(h, c->a, c->b);
        a2k_log("venc-test: stream %d ak_venc_set_qp(%d, %d, %d) = %d", c->chn, h, c->a, c->b, rc);
        if (!rc) {
            vp->min_qp = (uint16_t)c->a;
            vp->max_qp = (uint16_t)c->b;
        }
        break;
    case A2K_VC_KBPS:
        rc = attr_step(o, h, vp, c->a, c->b, vp->gop);
        a2k_log("venc-test: stream %d get_attr/set_attr (%d kbit/s, peak %d) = %d", c->chn, c->a, c->b, rc);
        break;
    case A2K_VC_GOP:
        rc = attr_step(o, h, vp, vp->target_kbps, vp->max_kbps, c->a);
        a2k_log("venc-test: stream %d get_attr/set_attr (gop %d) = %d", c->chn, c->a, rc);
        break;
    case A2K_VC_RC:
        rc = o->set_rc_type(h, c->a);
        a2k_log("venc-test: stream %d ak_venc_set_rc_type(%d, %d) = %d", c->chn, h, c->a, rc);
        if (!rc)
            vp->rc_mode = (uint32_t)c->a;
        break;
    case A2K_VC_ATTR: {
        struct venc_param t;
        unsigned w[A2K_VL_RAW / 4];
        int r2;
        memset(&t, 0, sizeof t);
        rc = o->get_attr(h, &t);
        a2k_log("venc-test: stream %d get_attr = %d: %ux%u fps %u gop %u kbps %u/%u profile %u rc %u qp init %u %u..%u type %u", c->chn, rc,
                (unsigned)t.width, (unsigned)t.height, (unsigned)t.fps, (unsigned)t.gop, (unsigned)t.target_kbps, (unsigned)t.max_kbps,
                (unsigned)t.profile, (unsigned)t.rc_mode, (unsigned)t.init_qp, (unsigned)t.min_qp, (unsigned)t.max_qp, (unsigned)t.enc_type);
        memset(w, 0, sizeof w);
        r2 = o->get_cur_qp(h, w);
        a2k_log("venc-test: stream %d get_cur_qp = %d: %08x %08x %08x %08x", c->chn, r2, w[0], w[1], w[2], w[3]);
        if (!rc)
            rc = r2;
        memset(w, 0, sizeof w);
        r2 = o->get_rc_stat(h, w);
        a2k_log("venc-test: stream %d get_rc_stat = %d: %08x %08x %08x %08x", c->chn, r2, w[0], w[1], w[2], w[3]);
        if (!rc)
            rc = r2;
        break;
    }
    default:
        return -1;
    }
    return rc;
}
