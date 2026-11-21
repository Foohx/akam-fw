/* PoC-2 of the Rotatable 2K own-app study: sensor -> VI -> H.264 encoder -> a file, N frames, then a clean teardown.
 *
 *   poc2_frame [-n FRAMES] [-c CHN] [-p base|main] [-k KBPS] [-t SECONDS] OUTFILE
 *
 * Defaults: 30 frames, channel 1 (sub, 640x360), Baseline, target 1000 kbit/s (max 1200), whole run limited to 20 s.
 * OUTFILE must be under /mnt/ or /tmp/ and must not contain ".."; the output is capped at 8 MiB. -c 0 selects the main channel (2304x1296, stock main defaults, AVBR).
 * BOTH VI channels are always configured and enabled, and the unused one is pulled and discarded frame by frame, as the
 * stock app does: hardware run 2 showed that ak_vi_enable_dev fails ("Channel [0] is not existed!") when only chn1 is
 * configured. The old -b option is accepted and ignored.
 *
 * No vendor header is used. Every prototype and struct below is OUR transcription of what the stock /appfs/bin/yy_cam
 * does (ARM disassembly, addresses are virtual addresses in that binary) and of the exports of the on-camera libs
 * (libplat_common, libplat_vi, libmpi_venc). Confidence: HIGH = read directly in instructions with the call
 * sites; MEDIUM = consistent inference; LOW = guess. Everything is UNVERIFIED until it runs on the camera.
 *
 * Call order (yy_media_av130_video_init_framesource 0x29d218 and the venc init 0x29dea0..0x29e7dc), HIGH. NOTE: the
 * stock sets the channel attributes BEFORE ak_vi_enable_dev (the feasibility doc listed enable_dev first):
 *   ak_sdk_init(&16 byte struct)                 0x29d060, call at 0x29d0f0
 *   ak_vi_open(dev)                              0x29d2fc
 *   ak_vi_load_sensor_cfg(dev, path)             0x29d41c
 *   ak_vi_set_dev_attr(dev, &56 bytes)           0x29d774
 *   ak_vi_set_chn_attr(chn, &20 bytes)           0x29da9c   (once per channel)
 *   ak_vi_enable_dev(dev)                        0x29db58
 *   ak_vi_enable_chn(chn)                        0x29dc78   (once per channel)
 *   ak_venc_open_ex(&64 bytes, &28 bytes, &handle) 0x29e6d4
 *   ak_venc_strictlylimit_frameSize(handle, 400) 0x29e76c
 *   loop: ak_vi_get_frame(chn, &frame) 0x29f9e8 (stock retries up to 3 times),
 *         ak_venc_encode_frame(handle, frame.w0, frame.w1, frame.w8, &stream) 0x29faa0,
 *         use stream.data / stream.len, ak_venc_release_stream(handle, &stream) 0x29fb60,
 *         ak_vi_release_frame(chn, &frame) 0x29fb70
 *   teardown: ak_venc_close, ak_vi_disable_chn, ak_vi_disable_dev, ak_vi_close, ak_sdk_exit.
 *
 * Safety: this program opens exactly one file of its own, OUTFILE (plus two /proc text files for the RSS and free
 * RAM figures). It never opens a device node, a flash node, nvram or the watchdog: the vendor libraries open their own
 * VI/ISP/venc nodes. A hang is bounded by alarm(): the handler writes the step and calls _exit, the kernel then
 * releases whatever the process held. A crash signal does the same. */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- struct transcriptions (sizes and offsets from the stock code) ----------------------------------------- */

/* 0x29d060: 16 zero bytes, only the word at +12 is ever set (1 enables the ISP tuning server: we never do). HIGH */
struct sdk_param { uint32_t w[4]; };

/* ak_vi_set_dev_attr, 56 bytes at ctx+900 (memset 56 at 0x29d368), copied from the config at 0x29d4b8..0x29d744.
 * Offsets HIGH; names of the four zero words (+4..+16: interface mode, fd?, data path, data type) are LOW, the stock
 * leaves them 0 with the default config. */
struct vi_dev_attr {
    uint32_t dev_id;                                   /* +0  sensor1_dev_id (0) */
    uint32_t zero4, zero8, zero12, zero16;             /* +4..+16 */
    uint32_t crop_left, crop_top, crop_w, crop_h;      /* +20..+32 defaults 0 0 1280 720 (kept as stock) */
    uint32_t max_w, max_h;                             /* +36,+40 2304 x 1296 */
    uint32_t fps;                                      /* +44 15 */
    uint32_t sub_max_w, sub_max_h;                     /* +48,+52 640 x 360 */
};

/* ak_vi_set_chn_attr, 20 bytes per channel at ctx+956 (memset 80 = 4 channels, 0x29d7e8). HIGH on offsets from the
 * stores at 0x29d8cc..0x29da18, names MEDIUM. */
struct vi_chn_attr {
    uint32_t chn;      /* +0  video_chnN_chn_id: 0 main, 1 sub */
    uint32_t fps;      /* +4  video_chnN_frame_rate (15) */
    uint32_t width;    /* +8 */
    uint32_t height;   /* +12 */
    uint32_t depth;    /* +16 frame depth, MUST be 1..4 (lib check, hardware run 1). yy_cam: video_chnN_depth, and when
                        * that is 0 (the default) 3 if the DMA reserved pool is > 50 MiB else 2 (0x29d294..0x29d2ac,
                        * used at 0x29d9cc..0x29da4c). HIGH */
};

/* ak_venc_open_ex first argument, 64 bytes (fp-104 at 0x29e6d4), filled at 0x29df50..0x29e684. HIGH on offsets. */
struct venc_param {
    uint16_t width, height;          /* +0,+2 */
    uint16_t fps, gop;               /* +4,+6 */
    uint16_t target_kbps, max_kbps;  /* +8,+10 */
    uint32_t profile;                /* +12 Main 0, High 1, Baseline 2 (strcmp table 0x29e0d0..0x29e2ac) */
    uint32_t rc_mode;                /* +16 CBR 0, VBR 1, CONST_QP 2, LOW_LATENCY 3, AVBR 4 (names MEDIUM) */
    uint16_t init_qp, min_qp, max_qp, pad26; /* +20,+22,+24 (+26 never written) */
    uint32_t jpeg_quality;           /* +28 video_chnN_snap_jpeg_quality (1) */
    uint32_t chroma_mode;            /* +32 (1) */
    uint32_t enc_type;               /* +36 H264 0 (MJPEG 1, H265 2: MEDIUM) */
    uint32_t buf_size;               /* +40 enc_buf_size (0) */
    uint32_t quality_lvl;            /* +44 (30) */
    uint32_t smart_mode;             /* +48 (0) */
    uint16_t smart_goplen, smart_q, smart_static, pad58; /* +52,+54 (not loaded: 0),+56 */
    uint32_t gdr_mode;               /* +60 (0) */
};

/* second argument of ak_venc_open_ex: 28 zero bytes, the byte at +24 set to 1 (meaning LOW), copied from stock. */
struct venc_ext { uint8_t b[28]; };

/* ak_vi_get_frame out struct: 40 bytes zeroed by the stock (0x29f96c). Only these are used by yy_cam: word 0 and
 * word 1 go to ak_venc_encode_frame as (data, len) (MEDIUM: consistent with the lib's encode_frame(handle, data, len,
 * mdinfo, stream)), the u64 at +8 is the timestamp in ms (HIGH, stock multiplies by 1000), word 8 (+32) is passed
 * through as the third encoder argument (probably the motion/ROI info pointer, LOW). */
struct vi_frame {
    uint32_t data;      /* +0  */
    uint32_t len;       /* +4  */
    uint64_t ts_ms;     /* +8  */
    uint32_t w16, w20, w24, w28; /* +16..+28 unknown (the stock reads +16 into its own record) */
    uint32_t mdinfo;    /* +32 */
    uint32_t w36;
};

/* ak_venc_encode_frame out struct, 24 bytes zeroed by the stock (0x29f980): data ptr +0, len +4 (HIGH), timestamp u64
 * +8 (HIGH), sequence +16 (MEDIUM), type +20 (MEDIUM: the stock maps ==1 to its own 'not I' flag). */
struct venc_stream {
    uint8_t *data;
    uint32_t len;
    uint64_t ts_ms;
    uint32_t seq;
    uint32_t type;
};

#define SZCHK(t, n) _Static_assert(sizeof(t) == (n), #t " size")
SZCHK(struct sdk_param, 16);
SZCHK(struct vi_dev_attr, 56);
SZCHK(struct vi_chn_attr, 20);
SZCHK(struct venc_param, 64);
SZCHK(struct venc_ext, 28);
SZCHK(struct vi_frame, 40);
SZCHK(struct venc_stream, 24);
_Static_assert(offsetof(struct vi_dev_attr, crop_left) == 20, "dev attr crop");
_Static_assert(offsetof(struct vi_dev_attr, sub_max_w) == 48, "dev attr sub");
_Static_assert(offsetof(struct venc_param, profile) == 12, "venc profile");
_Static_assert(offsetof(struct venc_param, init_qp) == 20, "venc qp");
_Static_assert(offsetof(struct venc_param, enc_type) == 36, "venc type");
_Static_assert(offsetof(struct venc_param, smart_goplen) == 52, "venc smart");
_Static_assert(offsetof(struct venc_param, gdr_mode) == 60, "venc gdr");
_Static_assert(offsetof(struct vi_frame, ts_ms) == 8, "frame ts");
_Static_assert(offsetof(struct vi_frame, mdinfo) == 32, "frame mdinfo");
_Static_assert(offsetof(struct venc_stream, ts_ms) == 8, "stream ts");
_Static_assert(offsetof(struct venc_stream, type) == 20, "stream type");

/* ---- vendor prototypes (all return 0 on success; HIGH from the call sites) ------------------------------- */
extern int ak_sdk_init(struct sdk_param *p);
extern int ak_sdk_exit(void);
extern int ak_mem_get_total_size(uint32_t *size);          /* 0x29d24c, out word, rc ignored by stock (MEDIUM) */
extern int ak_mem_dma_get_reserved_size(uint32_t *size);   /* 0x29d258 */
extern int ak_vi_open(int dev);
extern int ak_vi_close(int dev);
extern int ak_vi_load_sensor_cfg(int dev, const char *cfg_path);
extern int ak_vi_get_sensor_resolution(int dev, int res[2]); /* 8-byte out struct, 0x27c1c8 (MEDIUM) */
extern int ak_vi_set_dev_attr(int dev, const struct vi_dev_attr *attr);
extern int ak_vi_enable_dev(int dev);
extern int ak_vi_disable_dev(int dev);
extern int ak_vi_set_chn_attr(int chn, const struct vi_chn_attr *attr);
extern int ak_vi_enable_chn(int chn);
extern int ak_vi_disable_chn(int chn);
extern int ak_vi_get_frame(int chn, struct vi_frame *f);
extern int ak_vi_release_frame(int chn, struct vi_frame *f);
extern int ak_venc_open_ex(const struct venc_param *p, const struct venc_ext *ext, int *handle);
extern int ak_venc_close(int handle);
extern int ak_venc_strictlylimit_frameSize(int handle, int limit);
extern int ak_venc_set_stream_buff(int handle, unsigned size);
extern int ak_venc_request_idr(int handle);
extern int ak_venc_encode_frame(int handle, const void *data, unsigned len, const void *mdinfo,
                                struct venc_stream *out);
extern int ak_venc_release_stream(int handle, struct venc_stream *s);

#define MAX_OUT_BYTES (8u * 1024u * 1024u)
#define SENSOR_CFG "/appfs/conf/isp_sc1346_mipi_1lane_av130.bin"

/* ---- logging, time, guards ---------------------------------------------------------------------------- */
static struct timeval t0;
static volatile const char *volatile g_step = "start";

static long ms_since_start(void)
{
    struct timeval n;
    gettimeofday(&n, NULL);
    return (n.tv_sec - t0.tv_sec) * 1000L + (n.tv_usec - t0.tv_usec) / 1000L;
}

#define LOG(...) do { printf("[%6ldms] ", ms_since_start()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* Reads one "Key:   value kB" line of a /proc text file; -1 when absent. */
static long proc_kb(const char *file, const char *key)
{
    char line[160];
    long v = -1;
    size_t kl = strlen(key);
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            v = strtol(line + kl + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

static void mem_report(const char *tag)
{
    LOG("mem %s: VmRSS %ld kB, VmHWM %ld kB, MemFree %ld kB, MemAvailable %ld kB", tag,
        proc_kb("/proc/self/status", "VmRSS"), proc_kb("/proc/self/status", "VmHWM"),
        proc_kb("/proc/meminfo", "MemFree"), proc_kb("/proc/meminfo", "MemAvailable"));
}

/* Async-signal-safe: write() and _exit() only. */
static void say(const char *a, const char *b)
{
    if (write(1, a, strlen(a)) < 0) { }
    if (b && write(1, b, strlen(b)) < 0) { }
    if (write(1, "\n", 1) < 0) { }
}

static void on_alarm(int sig)
{
    (void)sig;
    say("poc2: TIMEOUT, whole-run guard fired in step: ", (const char *)g_step);
    _exit(124);
}

static void on_crash(int sig)
{
    (void)sig;
    say("poc2: CRASH signal in step: ", (const char *)g_step);
    _exit(2);
}

static volatile sig_atomic_t g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ---- Annex B scan (host-side check helper: which NAL types did the encoder emit) --------------------------- */
static void nal_scan(const uint8_t *d, uint32_t n, unsigned counts[32])
{
    uint32_t i;
    for (i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && i + 4 < n && d[i + 3] == 1))) {
            uint32_t h = (d[i + 2] == 1) ? i + 3 : i + 4;
            if (h < n)
                counts[d[h] & 0x1f]++;
            i = h - 1; /* resume after the start code: 00 00 00 01 must not match again as 00 00 01 at i+1 */
        }
}

/* ---- the run ------------------------------------------------------------------------------------------- */
struct opts {
    int frames, chn, main_profile, kbps, secs;
    const char *out;
};

struct state {
    int sdk, vi_open, dev_on, chn_on[2], venc;   /* venc: handle, -1 when closed */
};

static void cleanup(struct state *s)
{
    int rc, c;
    g_step = "teardown";
    if (s->venc >= 0) {
        rc = ak_venc_close(s->venc);
        LOG("ak_venc_close(%d) = %d", s->venc, rc);
        s->venc = -1;
    }
    for (c = 1; c >= 0; c--)
        if (s->chn_on[c]) {
            rc = ak_vi_disable_chn(c);
            LOG("ak_vi_disable_chn(%d) = %d", c, rc);
            s->chn_on[c] = 0;
        }
    if (s->dev_on) {
        rc = ak_vi_disable_dev(0);
        LOG("ak_vi_disable_dev(0) = %d", rc);
        s->dev_on = 0;
    }
    if (s->vi_open) {
        rc = ak_vi_close(0);
        LOG("ak_vi_close(0) = %d", rc);
        s->vi_open = 0;
    }
    if (s->sdk) {
        rc = ak_sdk_exit();
        LOG("ak_sdk_exit() = %d", rc);
        s->sdk = 0;
    }
}

/* Stock per-channel defaults (video_chnN_* keys, 0x29a8d8..0x29b520), HIGH. */
static void chn_defaults(int chn, struct vi_chn_attr *ca, struct venc_param *vp)
{
    memset(ca, 0, sizeof *ca);
    memset(vp, 0, sizeof *vp);
    ca->chn = (uint32_t)chn;
    ca->fps = 15;
    vp->fps = 15;
    vp->gop = 30;
    vp->profile = 2; /* Baseline */
    vp->jpeg_quality = 1;
    vp->chroma_mode = 1;
    vp->quality_lvl = 30;
    vp->min_qp = 32;
    if (chn == 0) {
        ca->width = vp->width = 2304;
        ca->height = vp->height = 1296;
        vp->rc_mode = 4; /* AVBR */
        vp->init_qp = 100;
        vp->max_qp = 51;
        vp->max_kbps = 800;
        vp->target_kbps = 630;
    } else {
        ca->width = vp->width = 640;
        ca->height = vp->height = 360;
        vp->rc_mode = 1; /* VBR */
        vp->init_qp = 41;
        vp->max_qp = 50;
        vp->max_kbps = 256;
        vp->target_kbps = 180;
    }
}

static int run(const struct opts *o, struct state *s)
{
    struct vi_dev_attr da;
    struct vi_chn_attr ca[2];
    struct venc_param vp[2];
    struct venc_ext ext;
    struct sdk_param sp;
    int res[2] = { 0, 0 };
    int rc, c, i, handle = -1, chn = o->chn, other = !o->chn;
    unsigned nal[32];
    unsigned long total = 0, got = 0;
    uint32_t mem_total = 0, dma_reserved = 0;
    uint32_t depth;
    FILE *out;

    chn_defaults(0, &ca[0], &vp[0]);
    chn_defaults(1, &ca[1], &vp[1]);
    if (o->main_profile)
        vp[chn].profile = 0;
    if (o->kbps > 0) {
        vp[chn].target_kbps = (uint16_t)o->kbps;
        vp[chn].max_kbps = (uint16_t)(o->kbps + o->kbps / 5);
        if (chn == 0)
            vp[chn].rc_mode = 1; /* VBR for a manual rate on the main channel too */
    }

    out = fopen(o->out, "wb");
    if (!out) {
        LOG("cannot open output %s: %s", o->out, strerror(errno));
        return 1;
    }
    LOG("output %s, %d frames, channel %d (%ux%u), profile %s, %u/%u kbit/s, guard %d s", o->out, o->frames, chn,
        (unsigned)ca[chn].width, (unsigned)ca[chn].height, o->main_profile ? "Main" : "Baseline",
        (unsigned)vp[chn].target_kbps, (unsigned)vp[chn].max_kbps, o->secs);
    mem_report("before init");

    g_step = "ak_sdk_init";
    memset(&sp, 0, sizeof sp);
    rc = ak_sdk_init(&sp);
    LOG("ak_sdk_init = %d", rc);
    if (rc) { fclose(out); return 1; }
    s->sdk = 1;

    /* The stock queries both sizes here and derives the default frame depth from the DMA pool (0x29d24c..0x29d2ac). */
    g_step = "ak_mem_get_size";
    rc = ak_mem_get_total_size(&mem_total);
    LOG("ak_mem_get_total_size = %d (%u)", rc, (unsigned)mem_total);
    rc = ak_mem_dma_get_reserved_size(&dma_reserved);
    depth = (dma_reserved > 52428800u) ? 3u : 2u;
    LOG("ak_mem_dma_get_reserved_size = %d (%u bytes) -> frame depth %u", rc, (unsigned)dma_reserved,
        (unsigned)depth);
    ca[0].depth = ca[1].depth = depth;

    g_step = "ak_vi_open";
    rc = ak_vi_open(0);
    LOG("ak_vi_open(0) = %d", rc);
    if (rc) goto fail;
    s->vi_open = 1;

    g_step = "ak_vi_load_sensor_cfg";
    rc = ak_vi_load_sensor_cfg(0, SENSOR_CFG);
    LOG("ak_vi_load_sensor_cfg(0, %s) = %d", SENSOR_CFG, rc);
    if (rc) goto fail;

    g_step = "ak_vi_set_dev_attr";
    memset(&da, 0, sizeof da);
    da.crop_w = 1280; da.crop_h = 720;
    da.max_w = 2304; da.max_h = 1296;
    da.fps = 15;
    da.sub_max_w = 640; da.sub_max_h = 360;
    rc = ak_vi_set_dev_attr(0, &da);
    LOG("ak_vi_set_dev_attr(0, max %ux%u, sub max %ux%u, %u fps) = %d", (unsigned)da.max_w, (unsigned)da.max_h,
        (unsigned)da.sub_max_w, (unsigned)da.sub_max_h, (unsigned)da.fps, rc);
    if (rc) goto fail;

    g_step = "ak_vi_set_chn_attr";
    for (c = 0; c < 2; c++) {
        rc = ak_vi_set_chn_attr(c, &ca[c]);
        LOG("ak_vi_set_chn_attr(%d, %ux%u, %u fps) = %d", c, (unsigned)ca[c].width, (unsigned)ca[c].height,
            (unsigned)ca[c].fps, rc);
        if (rc) goto fail;
    }

    g_step = "ak_vi_enable_dev";
    rc = ak_vi_enable_dev(0);
    LOG("ak_vi_enable_dev(0) = %d", rc);
    if (rc) goto fail;
    s->dev_on = 1;

    g_step = "ak_vi_get_sensor_resolution";
    rc = ak_vi_get_sensor_resolution(0, res);
    LOG("ak_vi_get_sensor_resolution(0) = %d (%d x %d)", rc, res[0], res[1]); /* informational */

    g_step = "ak_vi_enable_chn";
    for (c = 0; c < 2; c++) {
        rc = ak_vi_enable_chn(c);
        LOG("ak_vi_enable_chn(%d) = %d", c, rc);
        if (rc) goto fail;
        s->chn_on[c] = 1;
    }
    mem_report("after VI");

    g_step = "ak_venc_open_ex";
    memset(&ext, 0, sizeof ext);
    ext.b[24] = 1;
    rc = ak_venc_open_ex(&vp[chn], &ext, &handle);
    LOG("ak_venc_open_ex = %d, handle %d", rc, handle);
    if (rc || handle == -1) goto fail;
    s->venc = handle;
    rc = ak_venc_strictlylimit_frameSize(handle, 400);
    LOG("ak_venc_strictlylimit_frameSize(%d, 400) = %d", handle, rc);
    if (vp[chn].width > 1920 && vp[chn].height > 1080 && vp[chn].init_qp > 60) {
        rc = ak_venc_set_stream_buff(handle, 1048576u);
        LOG("ak_venc_set_stream_buff(%d, 1 MiB) = %d", handle, rc);
    }
    rc = ak_venc_request_idr(handle);
    LOG("ak_venc_request_idr(%d) = %d", handle, rc);
    mem_report("after venc");

    memset(nal, 0, sizeof nal);
    for (i = 0; i < o->frames && !g_stop; i++) {
        struct vi_frame fr, fo;
        struct venc_stream st;
        int tries;
        long t1 = ms_since_start();

        memset(&fr, 0, sizeof fr);
        memset(&st, 0, sizeof st);
        g_step = "ak_vi_get_frame";
        for (tries = 0; tries < 5; tries++) {
            rc = ak_vi_get_frame(chn, &fr);
            if (rc == 0)
                break;
            LOG("frame %d: ak_vi_get_frame(%d) = %d (try %d)", i, chn, rc, tries + 1);
            usleep(100000);
        }
        if (rc) goto fail;

        memset(&fo, 0, sizeof fo);
        if (ak_vi_get_frame(other, &fo) == 0)
            ak_vi_release_frame(other, &fo);

        g_step = "ak_venc_encode_frame";
        rc = ak_venc_encode_frame(handle, (const void *)(uintptr_t)fr.data, fr.len,
                                  (const void *)(uintptr_t)fr.mdinfo, &st);
        if (rc) {
            LOG("frame %d: ak_venc_encode_frame = %d (frame data %p len %u)", i, rc, (void *)(uintptr_t)fr.data,
                (unsigned)fr.len);
            ak_vi_release_frame(chn, &fr);
            goto fail;
        }
        if (st.data && st.len && st.len > MAX_OUT_BYTES - total) { /* also bounds a bogus length from the lib */
            LOG("frame %d: output cap of %u bytes reached, stopping", i, (unsigned)MAX_OUT_BYTES);
            ak_venc_release_stream(handle, &st);
            ak_vi_release_frame(chn, &fr);
            break;
        }
        if (st.data && st.len) {
            unsigned before[32];
            memcpy(before, nal, sizeof before);
            nal_scan(st.data, st.len, nal);
            if (fwrite(st.data, 1, st.len, out) != st.len) {
                LOG("frame %d: write error: %s", i, strerror(errno));
                ak_venc_release_stream(handle, &st);
                ak_vi_release_frame(chn, &fr);
                goto fail;
            }
            total += st.len;
            got++;
            LOG("frame %d: in %u bytes ts %llu ms, out %u bytes type %u seq %u, nal sps %u pps %u idr %u slice %u, %ld ms",
                i, (unsigned)fr.len, (unsigned long long)fr.ts_ms, (unsigned)st.len, (unsigned)st.type,
                (unsigned)st.seq, nal[7] - before[7], nal[8] - before[8], nal[5] - before[5], nal[1] - before[1],
                ms_since_start() - t1);
        } else {
            LOG("frame %d: encoder returned no data (len %u)", i, (unsigned)st.len);
        }
        g_step = "release";
        rc = ak_venc_release_stream(handle, &st);
        if (rc) LOG("frame %d: ak_venc_release_stream = %d", i, rc);
        rc = ak_vi_release_frame(chn, &fr);
        if (rc) LOG("frame %d: ak_vi_release_frame = %d", i, rc);
    }
    mem_report("after loop");
    LOG("done: %lu frames written, %lu bytes, NAL totals sps %u pps %u idr %u slice %u", got, total, nal[7], nal[8],
        nal[5], nal[1]);
    fclose(out);
    cleanup(s);
    mem_report("after teardown");
    return got ? 0 : 1;

fail:
    LOG("FAILED in step %s (rc %d), cleaning up", (const char *)g_step, rc);
    fclose(out);
    cleanup(s);
    return 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: poc2_frame [-n frames] [-c 0|1] [-p base|main] [-k kbps] [-t secs] OUTFILE\n");
    return 3;
}

/* Strict decimal parse: whole string, no overflow, within [lo, hi]; -1 on any error. */
static int parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o = { 30, 1, 0, 1000, 20, NULL };
    struct state s;
    int ch, rc;
    extern char *optarg;
    extern int optind;

    while ((ch = getopt(argc, argv, "n:c:bp:k:t:")) != -1) {
        switch (ch) {
        case 'n': if (parse_int(optarg, 1, 3000, &o.frames)) return usage(); break;
        case 'c': if (parse_int(optarg, 0, 1, &o.chn)) return usage(); break;
        case 'b': break; /* obsolete: both channels are always enabled now */
        case 'p':
            if (strcmp(optarg, "main") != 0 && strcmp(optarg, "base") != 0)
                return usage();
            o.main_profile = (strcmp(optarg, "main") == 0);
            break;
        case 'k': if (parse_int(optarg, 0, 50000, &o.kbps)) return usage(); break;
        case 't': if (parse_int(optarg, 5, 120, &o.secs)) return usage(); break;
        default: return usage();
        }
    }
    if (optind + 1 != argc)
        return usage();
    o.out = argv[optind];
    if (strstr(o.out, "..")) {
        fprintf(stderr, "poc2: OUTFILE must not contain ..\n");
        return 3;
    }
    if (strncmp(o.out, "/mnt/", 5) != 0 && strncmp(o.out, "/tmp/", 5) != 0) {
        fprintf(stderr, "poc2: OUTFILE must be under /mnt/ or /tmp/\n");
        return 3;
    }
    memset(&s, 0, sizeof s);
    s.venc = -1;
    gettimeofday(&t0, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_signals();
    alarm((unsigned)o.secs);
    rc = run(&o, &s);
    alarm(0);
    LOG("exit %d", rc);
    return rc;
}
