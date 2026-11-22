/* Capture and encode, Rotatable 2K own app (PoC-3). The call sequence is the hardware-validated one of PoC-2
 * (poc2_frame.c): sdk_init, mem queries (frame depth), vi_open, load_sensor_cfg, set_dev_attr, set_chn_attr for BOTH
 * channels, enable_dev, enable_chn for both, venc_open_ex per channel. The vendor ABI is our own transcription
 * (abi.h); no vendor header is used. Teardown is the reverse. This file opens no file and no device node of
 * its own: the vendor libraries open theirs. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "abi.h"
#include "cap.h"
#include "detect.h"
#include "isp.h"
#include "log.h"
#include "orient.h"
#include "video.h"
#include "vlive.h"
#include "isptest.h"
#include "rtp_clock.h"

#define NCHN 2
/* A channel that yields no frame (or no encoded data) for this long is dead: the caller shuts the app down. */
#define DEAD_MS 10000
/* The IDR requests of several connecting clients are merged into one per this many ms. */
#define IDR_MIN_GAP_MS 300

struct chan {
    int idx;
    int handle;                 /* encoder handle, -1 when closed */
    a2k_stream_t *s;
    pthread_t th;
    int started;
    struct vi_chn_attr ca;
    struct venc_param vp;
    int fps, acc;               /* encoded frame rate and its accumulator (the VI delivers VI_FPS: frames in between are not encoded) */
};
#define VI_FPS 15

static struct chan g_ch[NCHN];
static struct {
    int sdk, vi_open, dev_on, chn_on[NCHN], det_chn_on;
} g_st;
static volatile int g_failed;
static a2k_vlive_t g_live;
static int g_venc_test; /* --venc-test 1: the capture threads poll A2K_VENC_TEST_FILE */
static const a2k_vlive_ops_t g_live_ops = { ak_venc_set_fps, ak_venc_set_qp, ak_venc_get_attr, ak_venc_set_attr, ak_venc_set_rc_type,
                                            ak_venc_get_cur_qp, ak_venc_get_rc_stat };
static int g_jpeg = -1; /* handle of the JPEG encoder of the snapshots, -1 when there is none */

/* The JPEG encoder of the snapshots (evidence: stock-firmware-reference.md, "JPEG snapshot encoder"): the same
 * ak_venc_open_ex with enc_type 1 and profile 7 (Jpeg); the lib then requires min QP >= 1, max QP <= 100 and a JPEG
 * quality level (+28, the stock snap_jpeg_quality, default 1) of 0..3, width a multiple of 32 and height a multiple of 8
 * (640x360 is). It encodes the frame the sub capture thread already holds, one frame per request. */
#define SNAP_JPEG_QLEVEL 1

static void jpeg_defaults(struct venc_param *vp, const struct a2k_cap_cfg *cfg)
{
    memset(vp, 0, sizeof *vp);
    vp->width = A2K_SNAP_W;
    vp->height = A2K_SNAP_H;
    vp->fps = 15;
    vp->gop = (uint16_t)cfg->gop;
    vp->target_kbps = (uint16_t)cfg->sub_kbps; /* not used by a JPEG encoder; a valid value in case the lib checks it */
    vp->max_kbps = (uint16_t)(cfg->sub_kbps + cfg->sub_kbps / 4);
    vp->profile = 7; /* Jpeg */
    vp->rc_mode = 1;
    vp->init_qp = 50; /* the lib refuses an init QP outside 10..100 ("initqp error", seen on hardware), also for JPEG; 50 = mean of 1..100 */
    vp->min_qp = 1;
    vp->max_qp = 100;
    vp->jpeg_quality = SNAP_JPEG_QLEVEL;
    vp->chroma_mode = 1;
    vp->enc_type = 1; /* JPEG */
    vp->quality_lvl = 30;
}

/* One JPEG of the frame held by the sub capture thread. Runs in that thread, only when a snapshot was asked for. */
static void snap_encode(const struct vi_frame *fr)
{
    struct venc_stream st;
    int rc;
    memset(&st, 0, sizeof st);
    rc = ak_venc_encode_frame(g_jpeg, (const void *)(uintptr_t)fr->data, fr->len, (const void *)(uintptr_t)fr->mdinfo, &st);
    if (rc == 0 && st.data && st.len) {
        a2k_snap_submit(st.data, st.len); /* copies at most A2K_SNAP_MAX_JPEG bytes and returns */
        if (ak_venc_release_stream(g_jpeg, &st))
            a2k_log("snapshot: ak_venc_release_stream failed");
        return;
    }
    if (rc == 0)
        (void)ak_venc_release_stream(g_jpeg, &st);
    a2k_log("snapshot: ak_venc_encode_frame = %d (len %u)", rc, (unsigned)st.len);
    a2k_snap_failed();
}

/* The runtime video settings (video.h) over the built-in values of a channel's venc_param. */
static void vid_apply_t(int chn, struct venc_param *vp, const a2k_vid_t *vv)
{
    a2k_vid_t v = *vv;
    if (!a2k_vid_valid(chn, &v))
        return; /* not set up: the built-in values stay */
    vp->fps = (uint16_t)v.v[A2K_VP_FPS];
    vp->gop = (uint16_t)v.v[A2K_VP_GOP];
    vp->target_kbps = (uint16_t)v.v[A2K_VP_KBPS];
    vp->max_kbps = (uint16_t)a2k_vid_eff_max_kbps(&v);
    vp->min_qp = (uint16_t)v.v[A2K_VP_MINQP];
    vp->max_qp = (uint16_t)v.v[A2K_VP_MAXQP];
    vp->rc_mode = (uint32_t)v.v[A2K_VP_RC];
}

static void vid_apply(int chn, struct venc_param *vp)
{
    a2k_vid_t v;
    a2k_vid_get(chn, &v);
    vid_apply_t(chn, vp, &v);
}

static void chn_defaults(int chn, struct vi_chn_attr *ca, struct venc_param *vp, const struct a2k_cap_cfg *cfg)
{
    memset(ca, 0, sizeof *ca);
    memset(vp, 0, sizeof *vp);
    ca->chn = (uint32_t)chn;
    ca->fps = 15;
    vp->fps = 15;
    vp->gop = (uint16_t)cfg->gop;
    vp->jpeg_quality = 1;
    vp->chroma_mode = 1;
    vp->quality_lvl = 30;
    vp->min_qp = 26;
    vp->rc_mode = 1; /* VBR */
    if (chn == 0) {
        ca->width = vp->width = 2304;
        ca->height = vp->height = 1296;
        vp->profile = 0; /* Main */
        vp->init_qp = 100; /* stock main defaults, as PoC-2 ran them with -p main -k */
        vp->max_qp = 51;
        vp->target_kbps = (uint16_t)cfg->main_kbps;
        vp->max_kbps = (uint16_t)(cfg->main_kbps + cfg->main_kbps / 4);
    } else {
        ca->width = vp->width = 640;
        ca->height = vp->height = 360;
        vp->profile = 0; /* Main, as the main channel (Baseline is the retry in a2k_cap_start); 640x360 is the most this SoC's 3DNR / venc pool takes */
        vp->init_qp = 36;
        vp->max_qp = 44;
        vp->target_kbps = (uint16_t)cfg->sub_kbps;
        vp->max_kbps = (uint16_t)(cfg->sub_kbps + cfg->sub_kbps / 4);
    }
    vid_apply(chn, vp);
}

static int64_t now_ms(void) { return rtp_mono_us() / 1000; }

/* Opens the encoder of channel c with c->vp (the validated PoC-2 sequence). 0, or -1 with c->handle = -1. */
static int open_enc(struct chan *c)
{
    struct venc_ext ext;
    int h = -1, rc;
    memset(&ext, 0, sizeof ext);
    ext.b[24] = 1;
    rc = ak_venc_open_ex(&c->vp, &ext, &h);
    a2k_log("ak_venc_open_ex(chn %d: %ux%u, profile %u, rc %u, %u/%u kbit/s, %u fps, QP %u..%u, gop %u) = %d, handle %d", c->idx,
            (unsigned)c->vp.width, (unsigned)c->vp.height, (unsigned)c->vp.profile, (unsigned)c->vp.rc_mode,
            (unsigned)c->vp.target_kbps, (unsigned)c->vp.max_kbps, (unsigned)c->vp.fps, (unsigned)c->vp.min_qp,
            (unsigned)c->vp.max_qp, (unsigned)c->vp.gop, rc, h);
    if (rc || h == -1)
        return -1;
    c->handle = h;
    rc = ak_venc_strictlylimit_frameSize(h, 400);
    a2k_log("ak_venc_strictlylimit_frameSize(%d, 400) = %d", h, rc);
    if (c->vp.width > 1920 && c->vp.height > 1080 && c->vp.init_qp > 60) {
        rc = ak_venc_set_stream_buff(h, 1048576u);
        a2k_log("ak_venc_set_stream_buff(%d, 1 MiB) = %d", h, rc);
    }
    rc = ak_venc_request_idr(h);
    a2k_log("ak_venc_request_idr(%d) = %d", h, rc);
    c->fps = c->vp.fps;
    c->acc = 0;
    return 0;
}

/* The stream's own copy of the numbers (SDP, pacing of the RTSP senders) follows the encoder; the clock restarts with the new frame rate. */
static void stream_follow(struct chan *c)
{
    c->s->fps = c->vp.fps;
    c->s->gop = c->vp.gop;
    c->s->kbps = c->vp.target_kbps;
    rtp_vclock_init(&c->s->vclock, 1000000.0 / (c->vp.fps ? c->vp.fps : VI_FPS));
}

/* A runtime change of the video settings: only this channel's encoder is closed and opened again (about 1 to 2 s without video on this stream, the
 * keyframe request is part of the open). A refusal puts the previous settings back and opens with them; only when that fails too the channel is dead. */
static void reopen_enc(struct chan *c, const a2k_vid_t *want)
{
    int rc;
    if (c->handle >= 0) {
        rc = ak_venc_close(c->handle);
        a2k_log("video: stream %d re-open: ak_venc_close(%d) = %d", c->idx, c->handle, rc);
        c->handle = -1;
    }
    vid_apply_t(c->idx, &c->vp, want); /* exactly what a2k_vid_pending returned: a change made during the open stays pending */
    if (open_enc(c) == 0) {
        stream_follow(c);
        a2k_vid_applied(c->idx, 1, want);
        return;
    }
    a2k_vid_applied(c->idx, 0, want); /* back to the values that ran */
    vid_apply(c->idx, &c->vp);
    if (open_enc(c) == 0) {
        stream_follow(c);
        return;
    }
    a2k_log("video: stream %d: the encoder cannot be opened again, giving up", c->idx);
    g_failed = 1;
}

/* The live path of a pending change: 1 = applied without a re-open (the pacing and the stream numbers follow), 0 = re-open. */
static int live_enc(struct chan *c, const a2k_vid_t *want)
{
    if (!a2k_vlive_apply(&g_live, c->idx, c->handle, &c->vp, want))
        return 0;
    c->fps = c->vp.fps;
    c->acc = 0;
    stream_follow(c);
    a2k_vid_applied(c->idx, 1, want);
    return 1;
}

/* Debug hook of the hardware test: one command per file (vlive.h), live setters only, never a fallback. */
static void venc_test_poll(struct chan *c)
{
    a2k_vlive_cmd_t cmd;
    int rc;
    if (!a2k_vlive_poll(A2K_VENC_TEST_FILE, c->idx, &cmd))
        return;
    rc = a2k_vlive_run_cmd(&g_live, c->handle, &c->vp, &cmd);
    a2k_log("venc-test: stream %d command done, rc %d", c->idx, rc);
    c->fps = c->vp.fps;
    c->acc = 0;
    stream_follow(c);
}

static void *capture_thread(void *arg)
{
    struct chan *c = arg;
    int64_t last_ok = now_ms(), last_idr = 0;
    unsigned long enc_fail = 0;

    while (!a2k_stop) {
        struct vi_frame fr;
        struct venc_stream st;
        a2k_vid_t want;
        int rc, encode = 1;

        if (a2k_vid_pending(c->idx, &want)) {
            if (!live_enc(c, &want))
                reopen_enc(c, &want);
            last_ok = now_ms();
            if (g_failed)
                break;
        }
        if (g_venc_test)
            venc_test_poll(c);
        if (a2k_stream_take_idr(c->s)) {
            int64_t t = now_ms();
            if (t - last_idr >= IDR_MIN_GAP_MS) {
                last_idr = t;
                rc = ak_venc_request_idr(c->handle);
                a2k_log("%s: ak_venc_request_idr = %d", c->s->name, rc);
            }
        }
        memset(&fr, 0, sizeof fr);
        rc = ak_vi_get_frame(c->idx, &fr);
        if (rc) {
            if (now_ms() - last_ok > DEAD_MS) {
                a2k_log("%s: no frame for %d s (ak_vi_get_frame = %d), giving up", c->s->name, DEAD_MS / 1000, rc);
                g_failed = 1;
                break;
            }
            usleep(5000);
            continue;
        }
        memset(&st, 0, sizeof st);
        if (c->fps < VI_FPS) { /* a lower frame rate: encode every VI_FPS / fps-th frame (evenly spread), the snapshot below still sees every frame */
            c->acc += c->fps;
            if (c->acc < VI_FPS)
                encode = 0;
            else
                c->acc -= VI_FPS;
        }
        if (!encode) {
            rc = -1;
            last_ok = now_ms();
        } else {
            rc = ak_venc_encode_frame(c->handle, (const void *)(uintptr_t)fr.data, fr.len, (const void *)(uintptr_t)fr.mdinfo, &st);
        }
        if (!encode) {
            /* nothing to publish or release */
        } else if (rc == 0) {
            if (st.data && st.len) {
                (void)a2k_stream_publish(c->s, st.data, st.len);
                last_ok = now_ms();
            }
            if (ak_venc_release_stream(c->handle, &st))
                enc_fail++;
        } else {
            enc_fail++;
            if (enc_fail % 100 == 1)
                a2k_log("%s: ak_venc_encode_frame = %d (%lu failures)", c->s->name, rc, enc_fail);
        }
        if (c->idx == 1 && g_jpeg >= 0 && a2k_snap_pending())
            snap_encode(&fr);
        if (ak_vi_release_frame(c->idx, &fr))
            enc_fail++;
        if (now_ms() - last_ok > DEAD_MS) {
            a2k_log("%s: no encoded data for %d s, giving up", c->s->name, DEAD_MS / 1000);
            g_failed = 1;
            break;
        }
    }
    return NULL;
}

/* ---- ISP test hook (isptest.h, --isp-test 1): the vendor access of the hook, device 0 ---- */

static int it_effect_get(int type, int *value) { return ak_vpss_effect_get(0, type, value); }
static int it_get(int what, void *buf)
{
    switch (what) {
    case A2K_IT_YUV: return ak_vpss_get_yuv_effect_attr(0, buf);
    case A2K_IT_WBTYPE: return ak_vpss_get_wb_type(0, buf); /* one int at the start of the buffer */
    case A2K_IT_MWB: return ak_vpss_get_mwb_attr(0, buf);
    case A2K_IT_CCM: return ak_vpss_get_ccm_attr(0, buf);
    case A2K_IT_NR1: return ak_vpss_get_nr1_attr(0, buf);
    case A2K_IT_NR2: return ak_vpss_get_nr2_attr(0, buf);
    case A2K_IT_UVNR: return ak_vpss_get_uvnr_attr(0, buf);
    case A2K_IT_NR3D: return ak_vpss_get_3d_nr_attr(0, buf);
    case A2K_IT_AWB: return ak_vpss_get_awb_stat_info(0, buf);
    default: return -1;
    }
}
static int it_set(int what, const void *buf)
{
    switch (what) {
    case A2K_IT_YUV: return ak_vpss_set_yuv_effect_attr(0, buf);
    case A2K_IT_WBTYPE: return ak_vpss_set_wb_type(0, buf);
    case A2K_IT_MWB: return ak_vpss_set_mwb_attr(0, buf);
    case A2K_IT_CCM: return ak_vpss_set_ccm_attr(0, buf);
    case A2K_IT_NR1: return ak_vpss_set_nr1_attr(0, buf);
    case A2K_IT_NR2: return ak_vpss_set_nr2_attr(0, buf);
    case A2K_IT_UVNR: return ak_vpss_set_uvnr_attr(0, buf);
    case A2K_IT_NR3D: return ak_vpss_set_3d_nr_attr(0, buf);
    default: return -1;
    }
}
static int it_rgb(int *r, int *g, int *b) { return ak_vpss_get_rgb_average(0, r, g, b); }
static const a2k_isptest_ops_t g_it_ops = { a2k_isp_effect, it_effect_get, it_get, it_set, it_rgb };

static void teardown(void)
{
    int rc, c;
    a2k_set_step("teardown");
    a2k_detect_stop(); /* detection thread (svp2 destroy, npu deinit) before anything it reads is torn down */
    a2k_snap_stop();
    if (g_jpeg >= 0) {
        rc = ak_venc_close(g_jpeg);
        a2k_log("ak_venc_close(%d) = %d (JPEG)", g_jpeg, rc);
        g_jpeg = -1;
    }
    for (c = NCHN - 1; c >= 0; c--)
        if (g_ch[c].handle >= 0) {
            rc = ak_venc_close(g_ch[c].handle);
            a2k_log("ak_venc_close(%d) = %d", g_ch[c].handle, rc);
            g_ch[c].handle = -1;
        }
    if (g_st.det_chn_on && !a2k_detect_chn_disabled()) {
        rc = ak_vi_disable_chn(A2K_DET_CHN);
        a2k_log("ak_vi_disable_chn(%d) = %d", A2K_DET_CHN, rc);
        g_st.det_chn_on = 0;
    }
    for (c = NCHN - 1; c >= 0; c--)
        if (g_st.chn_on[c]) {
            rc = ak_vi_disable_chn(c);
            a2k_log("ak_vi_disable_chn(%d) = %d", c, rc);
            g_st.chn_on[c] = 0;
        }
    if (g_st.dev_on) {
        rc = ak_vi_disable_dev(0);
        a2k_log("ak_vi_disable_dev(0) = %d", rc);
        g_st.dev_on = 0;
    }
    if (g_st.vi_open) {
        rc = ak_vi_close(0);
        a2k_log("ak_vi_close(0) = %d", rc);
        g_st.vi_open = 0;
    }
    if (g_st.sdk) {
        rc = ak_sdk_exit();
        a2k_log("ak_sdk_exit() = %d", rc);
        g_st.sdk = 0;
    }
}

int a2k_cap_start(const struct a2k_cap_cfg *cfg, a2k_stream_t *s[2])
{
    struct vi_dev_attr da;
    struct venc_ext ext;
    struct sdk_param sp;
    uint32_t mem_total = 0, dma_reserved = 0, depth;
    struct vi_chn_attr da16;
    int rc, c, det_chn = 0;

    a2k_vlive_init(&g_live, &g_live_ops, cfg->video_live);
    g_venc_test = cfg->venc_test;

    memset(&g_st, 0, sizeof g_st);
    g_jpeg = -1;
    for (c = 0; c < NCHN; c++) {
        g_ch[c].idx = c;
        g_ch[c].handle = -1;
        g_ch[c].started = 0;
        chn_defaults(c, &g_ch[c].ca, &g_ch[c].vp, cfg);
        a2k_stream_init(s[c], c ? "stream1" : "stream0", (int)g_ch[c].ca.width, (int)g_ch[c].ca.height,
                        (int)g_ch[c].vp.fps, (int)g_ch[c].vp.gop, (int)g_ch[c].vp.target_kbps,
                        c ? A2K_MAX_FRAME_SUB : A2K_MAX_FRAME_MAIN);
        g_ch[c].s = s[c];
    }

    a2k_set_step("ak_sdk_init");
    memset(&sp, 0, sizeof sp);
    rc = ak_sdk_init(&sp);
    a2k_log("ak_sdk_init = %d", rc);
    if (rc)
        goto fail;
    g_st.sdk = 1;

    a2k_set_step("ak_mem_get_size");
    rc = ak_mem_get_total_size(&mem_total);
    a2k_log("ak_mem_get_total_size = %d (%u)", rc, (unsigned)mem_total);
    rc = ak_mem_dma_get_reserved_size(&dma_reserved);
    depth = (dma_reserved > 52428800u) ? 3u : 2u;
    a2k_log("ak_mem_dma_get_reserved_size = %d (%u bytes) -> frame depth %u", rc, (unsigned)dma_reserved, depth);
    g_ch[0].ca.depth = g_ch[1].ca.depth = depth;

    a2k_set_step("ak_vi_open");
    rc = ak_vi_open(0);
    a2k_log("ak_vi_open(0) = %d", rc);
    if (rc)
        goto fail;
    g_st.vi_open = 1;

    a2k_set_step("ak_vi_load_sensor_cfg");
    rc = ak_vi_load_sensor_cfg(0, SENSOR_CFG);
    a2k_log("ak_vi_load_sensor_cfg(0, %s) = %d", SENSOR_CFG, rc);
    if (rc)
        goto fail;

    a2k_set_step("ak_vi_set_dev_attr");
    memset(&da, 0, sizeof da);
    da.crop_w = 1280;
    da.crop_h = 720;
    da.max_w = 2304;
    da.max_h = 1296;
    da.fps = 15;
    da.sub_max_w = 640;
    da.sub_max_h = 360;
    rc = ak_vi_set_dev_attr(0, &da);
    a2k_log("ak_vi_set_dev_attr(0) = %d", rc);
    if (rc)
        goto fail;

    a2k_set_step("ak_vi_set_chn_attr");
    for (c = 0; c < NCHN; c++) {
        rc = ak_vi_set_chn_attr(c, &g_ch[c].ca);
        a2k_log("ak_vi_set_chn_attr(%d, %ux%u) = %d", c, (unsigned)g_ch[c].ca.width, (unsigned)g_ch[c].ca.height, rc);
        if (rc)
            goto fail;
    }

    if (cfg->det.enabled) {
        /* The stock third channel (id 16, 320x192, 20 fps, depth 2), set up before enable_dev like chn 0/1 (PoC-4). A
         * refusal only switches the detection off: the two streams go on. */
        memset(&da16, 0, sizeof da16);
        da16.chn = A2K_DET_CHN;
        da16.fps = 20;
        da16.width = A2K_DET_W;
        da16.height = A2K_DET_H;
        da16.depth = 2;
        rc = ak_vi_set_chn_attr(A2K_DET_CHN, &da16);
        a2k_log("ak_vi_set_chn_attr(%d, %ux%u) = %d", A2K_DET_CHN, A2K_DET_W, A2K_DET_H, rc);
        if (rc)
            a2k_log("detect: VI chn %d refused, detection off", A2K_DET_CHN);
        else
            det_chn = 1;
    }

    a2k_set_step("ak_vi_enable_dev");
    rc = ak_vi_enable_dev(0);
    a2k_log("ak_vi_enable_dev(0) = %d", rc);
    if (rc)
        goto fail;
    g_st.dev_on = 1;

    a2k_set_step("ak_vi_enable_chn");
    for (c = 0; c < NCHN; c++) {
        rc = ak_vi_enable_chn(c);
        a2k_log("ak_vi_enable_chn(%d) = %d", c, rc);
        if (rc)
            goto fail;
        g_st.chn_on[c] = 1;
    }

    if (det_chn) {
        rc = ak_vi_enable_chn(A2K_DET_CHN);
        a2k_log("ak_vi_enable_chn(%d) = %d", A2K_DET_CHN, rc);
        if (rc)
            a2k_log("detect: VI chn %d did not enable, detection off", A2K_DET_CHN);
        else
            g_st.det_chn_on = 1;
    }

    a2k_set_step("ak_venc_open_ex");
    for (c = 0; c < NCHN; c++) {
        rc = open_enc(&g_ch[c]);
        if (rc && c == 1 && g_ch[c].vp.profile == 0) {
            /* the sub asks for Main like the main channel; if the lib refuses it, the same stream in Baseline (what the sub always ran before) */
            a2k_log("sub stream: the encoder refused Main (rc %d), trying Baseline", rc);
            g_ch[c].vp.profile = 2;
            rc = open_enc(&g_ch[c]);
        }
        if (rc && a2k_vid_reset_runtime()) {
            /* the runtime video settings may be what the lib refuses: forget them and try the conf / default values once */
            for (int k = 0; k < NCHN; k++)
                vid_apply(k, &g_ch[k].vp);
            rc = open_enc(&g_ch[c]);
        }
        if (rc)
            goto fail;
        stream_follow(&g_ch[c]);
    }

    if (cfg->snap.enabled) {
        /* A failure here only switches the snapshots off: the two streams (and the detection) go on. */
        struct venc_param jp;
        int h = -1;
        a2k_set_step("ak_venc_open_ex jpeg");
        jpeg_defaults(&jp, cfg);
        memset(&ext, 0, sizeof ext);
        ext.b[24] = 1;
        rc = ak_venc_open_ex(&jp, &ext, &h);
        a2k_log("ak_venc_open_ex(JPEG %ux%u, qlevel %u) = %d, handle %d", (unsigned)jp.width, (unsigned)jp.height,
                (unsigned)jp.jpeg_quality, rc, h);
        if (rc || h == -1) {
            char why[96];
            /* 0x0D000003 is what the lib returned for "param is invalid" on hardware; anything else is not a parameter problem */
            snprintf(why, sizeof why, "ak_venc_open_ex(JPEG) failed, rc 0x%08x, %s", (unsigned)rc,
                     (unsigned)rc == 0x0D000003u ? "invalid parameter, see the vendor log line above" : "not a parameter error");
            a2k_snap_disable(why);
        } else {
            g_jpeg = h;
            if (a2k_snap_start(&cfg->snap) != 0) {
                (void)ak_venc_close(g_jpeg);
                g_jpeg = -1;
            }
        }
    }

    a2k_set_step("capture threads");
    for (c = 0; c < NCHN; c++) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 96 * 1024);
        rc = pthread_create(&g_ch[c].th, &at, capture_thread, &g_ch[c]);
        pthread_attr_destroy(&at);
        if (rc) {
            a2k_log("pthread_create(capture %d) = %d", c, rc);
            a2k_stop = 1;
            while (--c >= 0)
                pthread_join(g_ch[c].th, NULL);
            rc = -1;
            goto fail;
        }
        g_ch[c].started = 1;
    }
    if (g_st.det_chn_on)
        (void)a2k_detect_start(&cfg->det); /* failure is logged there and never affects the streams */
    if (cfg->isp_test && a2k_isptest_start(&g_it_ops) != 0)
        a2k_log("isp-test: the poll thread could not start (the app goes on without the hook)");
    return 0;

fail:
    a2k_log("capture bring-up FAILED (rc %d)", rc);
    teardown();
    for (c = 0; c < NCHN; c++)
        a2k_stream_destroy(s[c]);
    return -1;
}

int a2k_cap_failed(void) { return g_failed; }

void a2k_cap_stop(void)
{
    int c;
    a2k_detect_stop(); /* first: it destroys the svp2 channel and de-initialises the NPU */
    a2k_isptest_stop(); /* the hook thread leaves on a2k_stop; its ISP calls must be over before the teardown */
    a2k_set_step("capture join");
    for (c = 0; c < NCHN; c++)
        if (g_ch[c].started) {
            pthread_join(g_ch[c].th, NULL);
            g_ch[c].started = 0;
        }
    teardown();
}

/* ---- ISP: image effects and day/night (isp.h). The stock calls these with the VI/ISP device 0 while the streams run. */

int a2k_isp_effect(int type, int value)
{
    int rc = ak_vpss_effect_set(0, type, value);
    a2k_log("ak_vpss_effect_set(0, %d, %d) = %d", type, value, rc);
    return rc;
}

int a2k_isp_flip_mirror(int flip, int mirror)
{
    int rc;
    if ((flip != 0 && flip != 1) || (mirror != 0 && mirror != 1))
        return -1;
    rc = ak_vpss_switch_flip_mirror(0, flip, mirror);
    a2k_log("ak_vpss_switch_flip_mirror(0, %d, %d) = %d", flip, mirror, rc);
    return rc;
}

int a2k_isp_wdr(int on)
{
    return a2k_isp_effect(A2K_FX_WDR, on ? 40 : 0);
}

/* The values the stock yy_cam passes (yy_media_av130_soft_ps_alg_init, 0x2a123c); its two thresholds are the [dn] defaults
 * daytonight_threshold 15410000 and nighttoday_threshold 2359000 of param.conf. */
int a2k_isp_dn_init(void)
{
    struct vpss_dn_param p;
    int rc;
    memset(&p, 0, sizeof p);
    p.day_to_night_lum = 15410000u;
    p.night_to_day_lum = 2359000u;
    p.awb_night_dis_max = 4000;
    p.day_check_frame_num = 10;
    p.night_check_frame_num = 10;
    p.exp_stable_check_frame_num = 10;
    p.w28 = 10000;
    p.gain_r = 1280;
    p.w36 = 2048;
    p.gain_b = 1280;
    p.w44 = 2048;
    p.blk_check_flag = 1;
    p.low_lumi_th = 5;
    p.high_lumi_th = 245;
    p.non_infrared_ratio_th = 40;
    p.highlight_lumi_th = 170;
    p.highlight_blk_cnt_max = 45;
    rc = ak_vpss_set_soft_ps_param(0, &p);
    a2k_log("ak_vpss_set_soft_ps_param(0) = %d", rc);
    return rc;
}

int a2k_isp_dn_level(int cur_night, int *want_night)
{
    int want = 0, rc = ak_vpss_get_soft_ps_level(0, cur_night ? 1 : 0, &want);
    if (rc == 0)
        *want_night = want ? 1 : 0;
    return rc;
}

int a2k_isp_mode(int night)
{
    int rc, f, m;
    /* The vendor lib may reset the flip inside the mode switch (not verified on hardware): the frames between the switch and the replay can be unflipped, so the
     * settling window (scene check, tracker, zones) is open from before the switch until after the replay, whenever an orientation is on. */
    a2k_orient_isp_lock();
    a2k_orient_get(&f, &m);
    if (f || m)
        a2k_orient_hold();
    rc = ak_vi_switch_mode(0, night ? 1 : 0);
    a2k_log("ak_vi_switch_mode(0, %d) = %d", night ? 1 : 0, rc);
    if (rc == 0) { /* the stock does not replay the flip after a mode switch; the call is absolute (not a toggle), so replaying it is harmless when the lib kept it */
        a2k_orient_get(&f, &m);
        if (f || m) {
            (void)a2k_isp_flip_mirror(f, m);
            a2k_orient_hold();
        }
    }
    a2k_orient_isp_unlock();
    for (int c = 0; c < NCHN; c++)
        if (g_ch[c].started)
            a2k_stream_want_idr(g_ch[c].s);
    return rc;
}
