/* PoC-4 of the Rotatable 2K own-app study: run the stock "hd" (human detection) model on the NPU from OUR program.
 *
 *   poc4_detect [-n INFERENCES=50] [-t SECONDS=30] [-S STAGE=4] [-m MODEL] [-c CLASSIFY=700] [-i IOU=3]
 *               [-T TARGET=0] [-p PERIOD_MS=100] [-M]
 *
 * Sequence (each step is logged with a millisecond clock and RSS):
 *   VI bring-up exactly as PoC-2 (dev attr, chn 0 and chn 1) PLUS the stock's third VI channel (id 16, 320x192, depth 2,
 *   it is what the stock "hd" thread reads), then
 *   ak_npu_init(), ak_svp2_create_chn(handle 0, attr, conf{model path}) [the model file is read by the vendor lib from
 *   /appfs/resource/model/..., never copied or shipped], then N times
 *   ak_vi_get_frame(16) -> ak_mem_dma_vaddr2paddr -> ak_svp2_process -> print results -> ak_svp2_release ->
 *   ak_vi_release_frame(16), then ak_svp2_destroy_chn, ak_npu_deinit and the VI teardown.
 * The stock thread sleeps 100 ms after every inference (-p keeps that default). Chn 0 and chn 1 frames are pulled and
 * discarded once per loop (the stock has encoders draining them).
 *
 * STAGE limits how far the run goes, so the coordinator can bring the risky parts up one by one:
 *   1 VI with the third channel, pull and print 3 frames of channel 16 (no NPU)
 *   2 + ak_npu_init / ak_npu_deinit
 *   3 + ak_svp2_create_chn (loads the model) / destroy
 *   4 + the inference loop (default)
 * -M also polls the VPSS motion statistics (ak_vpss_md_get_stat) every loop and prints a summary (cheap fallback).
 *
 * No vendor header is used. Every prototype and struct is OUR transcription of what the stock /appfs/bin/yy_cam does
 * (addresses are virtual addresses in that binary) and of the exports of the on-camera libs (libplat_vi, libplat_npu,
 * libmpi_svp2, libplat_mem, libplat_vpss). Confidence: HIGH = read in instructions; MEDIUM = consistent inference;
 * LOW = guess. Everything is UNVERIFIED until it runs on the camera; see docs/porting/lsc-rotatable-2k/ai-own-app.md.
 *
 * Safety: this program opens no file of its own except two /proc text files (RSS, free RAM). It never opens a device
 * node, a flash node, nvram or the watchdog: the vendor libraries open theirs. A hang is bounded by alarm(): the
 * handler prints the step and calls _exit, the kernel then releases whatever the process held. The loop itself stops
 * 4 s before the guard so the normal path tears down cleanly. */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- struct transcriptions (sizes and offsets from the stock code and the vendor libs) --------------------- */

/* 0x29d060: 16 zero bytes, only the word at +12 is ever set (never by us). HIGH (PoC-2 hardware) */
struct sdk_param { uint32_t w[4]; };

/* 56 bytes, same as PoC-2 (hardware verified). */
struct vi_dev_attr {
    uint32_t dev_id;
    uint32_t zero4, zero8, zero12, zero16;
    uint32_t crop_left, crop_top, crop_w, crop_h;
    uint32_t max_w, max_h;
    uint32_t fps;
    uint32_t sub_max_w, sub_max_h;
};

/* 20 bytes per channel (PoC-2 hardware verified); the stock third channel has chn id 16 (video_chn3_chn_id default,
 * 0x29a8d8..0x29b520), 320 x 192, 20 fps, depth 2 (video_chn3_depth default, non-zero so the DMA-pool substitute
 * is not used). HIGH on the values, MEDIUM that the lib accepts id 16 for a third channel ("vi_set_third_chn_attr"). */
struct vi_chn_attr {
    uint32_t chn, fps, width, height, depth;
};

/* ak_vi_get_frame out struct, 40 bytes (same as PoC-2). Offsets used by yy_cam's snap_nv12 (0x2a0974) and the "hd"
 * thread: +0 virtual address of the NV12 data (the Y plane, UV follows), +4 length in bytes, +8 u64 timestamp ms,
 * +24 physical address (HIGH that the stock passes this word as svp2 input phy_addr; we recompute it anyway). */
struct vi_frame {
    uint32_t data, len;
    uint64_t ts_ms;
    uint32_t w16, w20, phys, w28, w32, w36;
};

/* ak_svp2_create_chn attr (0x27e240 read_conf_input_param): model_num then one 20-byte entry per model; the lib checks
 * model_num > 0, env_mode 0..1 (day/night), model_type <= 47, classify 0..1000, IoU 0..10, target_type 0..2 (all from
 * the lib's own error strings). The stock uses 1 model: env 0, type 10, classify 700, IoU 3, target 0. HIGH */
struct svp2_model { uint32_t env_mode, model_type, classify_thr, iou_thr, target_type; };
struct svp2_attr { uint32_t model_num; struct svp2_model m[1]; };

/* conf_info (ctx+0x4e0 in the stock): +0 model path (char *), +4 conf_mode (0 = file path, 1 = in-memory buffer),
 * +8 pReadFunc (NULL = default parser). 12 bytes read by the lib; we pass 16 zeroed. HIGH */
struct svp2_conf { const char *path; uint32_t mode; void *read_fn; uint32_t pad; };

/* ak_svp2_process input, 40 bytes at fp-92 in 0x2801bc: +0 0, +4 img_type (0 = YUV420SP, 1 = RGB line, lib check),
 * +8 width, +12 height (1..16384), +16 left, +20 top, +24 width, +28 height of the region of interest (the stock uses
 * the whole image), +32 physical address (must equal ak_mem_dma_vaddr2paddr(+36)), +36 virtual address. HIGH */
struct svp2_input {
    uint32_t zero0, img_type, width, height, roi_left, roi_top, roi_w, roi_h, phys, virt;
};

/* ak_svp2_process out, 16 bytes at fp-108: +0 count, +4 pointer to the results (valid until ak_svp2_release), +8 result
 * type (lib internal: 3, 4, 16, 23 are known), +12 a second word. HIGH for +0/+4 (the stock reads exactly these). */
struct svp2_output { uint32_t count; const void *res; uint32_t type, w12; };

/* One result, stride 24 (0x2803a0: i*3*8). +0 class (the stock only handles class 0), +4..+16 box as two corners
 * x1,y1,x2,y2 in pixels of the input image (result_cvt 0x27fd8c divides by width-1 / height-1), +20 score 0..1000. HIGH
 * on the geometry, MEDIUM on the class meaning (see the doc). */
struct svp2_result { int32_t cls, x1, y1, x2, y2, score; };

#define SZCHK(t, n) _Static_assert(sizeof(t) == (n), #t " size")
SZCHK(struct sdk_param, 16);
SZCHK(struct vi_dev_attr, 56);
SZCHK(struct vi_chn_attr, 20);
SZCHK(struct vi_frame, 40);
SZCHK(struct svp2_attr, 24);
SZCHK(struct svp2_conf, 16);
SZCHK(struct svp2_input, 40);
SZCHK(struct svp2_output, 16);
SZCHK(struct svp2_result, 24);
_Static_assert(offsetof(struct vi_frame, phys) == 24, "frame phys");
_Static_assert(offsetof(struct svp2_input, phys) == 32, "svp2 input phys");
_Static_assert(offsetof(struct svp2_input, virt) == 36, "svp2 input virt");
_Static_assert(offsetof(struct svp2_input, img_type) == 4, "svp2 input img_type");
_Static_assert(offsetof(struct svp2_conf, read_fn) == 8, "svp2 conf read_fn");

/* ---- vendor prototypes (all return 0 on success unless noted) ------------------------------------------- */
extern int ak_sdk_init(struct sdk_param *p);
extern int ak_sdk_exit(void);
extern int ak_mem_get_total_size(uint32_t *size);
extern int ak_mem_dma_get_reserved_size(uint32_t *size);
extern int ak_mem_dma_vaddr2paddr(const void *vaddr, uint32_t *paddr); /* libplat_mem; called by libmpi_svp2 the same way */
extern int ak_vi_open(int dev);
extern int ak_vi_close(int dev);
extern int ak_vi_load_sensor_cfg(int dev, const char *cfg_path);
extern int ak_vi_set_dev_attr(int dev, const struct vi_dev_attr *attr);
extern int ak_vi_enable_dev(int dev);
extern int ak_vi_disable_dev(int dev);
extern int ak_vi_set_chn_attr(int chn, const struct vi_chn_attr *attr);
extern int ak_vi_enable_chn(int chn);
extern int ak_vi_disable_chn(int chn);
extern int ak_vi_get_frame(int chn, struct vi_frame *f);
extern int ak_vi_release_frame(int chn, struct vi_frame *f);
extern int ak_npu_init(void);                                  /* libplat_npu: no argument (the stock call at 0x27fa40) */
extern int ak_npu_deinit(void);
extern void ak_npu_reset_hw(void);                             /* the stock calls it after a failed process, rc unused */
extern int ak_svp2_create_chn(int handle, const struct svp2_attr *attr, const struct svp2_conf *conf);
extern int ak_svp2_destroy_chn(int handle);
extern int ak_svp2_process(int handle, const struct svp2_input *in, struct svp2_output *out);
extern int ak_svp2_release(struct svp2_output *out);
extern int ak_vpss_md_get_stat(int dev, void *stat);           /* stock: dev 0, 1536-byte zeroed buffer (0x27b7f4) */

#define SENSOR_CFG "/appfs/conf/isp_sc1346_mipi_1lane_av130.bin"
#define DEFAULT_MODEL "/appfs/resource/model/model_0x0A00000D_V1.00.01.bin"
#define MODEL_DIR "/appfs/resource/model/"
#define HD_CHN 16
#define HD_W 320u
#define HD_H 192u
#define MD_STAT_BYTES 1536u
#define MAX_RESULTS 64u
#define MIN_AVAIL_KB 6144 /* abort criterion of the procedure: 6 MB */

/* ---- logging, time, guards ---------------------------------------------------------------------------- */
static struct timeval t0;
static volatile const char *volatile g_step = "start";

static long ms_since_start(void)
{
    struct timeval n;
    gettimeofday(&n, NULL);
    return (n.tv_sec - t0.tv_sec) * 1000L + (n.tv_usec - t0.tv_usec) / 1000L;
}

static long us_now(void)
{
    struct timeval n;
    gettimeofday(&n, NULL);
    return (n.tv_sec - t0.tv_sec) * 1000000L + (n.tv_usec - t0.tv_usec);
}

#define LOG(...) do { printf("[%6ldms] ", ms_since_start()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

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

/* CPU time of this process in ms (user, system). The NPU driver time shows up as system time. */
static void cpu_ms(long *u, long *s)
{
    struct rusage ru;
    *u = *s = -1;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        *u = ru.ru_utime.tv_sec * 1000L + ru.ru_utime.tv_usec / 1000L;
        *s = ru.ru_stime.tv_sec * 1000L + ru.ru_stime.tv_usec / 1000L;
    }
}

static void say(const char *a, const char *b)
{
    if (write(1, a, strlen(a)) < 0) { }
    if (b && write(1, b, strlen(b)) < 0) { }
    if (write(1, "\n", 1) < 0) { }
}

static void on_alarm(int sig)
{
    (void)sig;
    say("poc4: TIMEOUT, whole-run guard fired in step: ", (const char *)g_step);
    _exit(124);
}

static void on_crash(int sig)
{
    (void)sig;
    say("poc4: CRASH signal in step: ", (const char *)g_step);
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

/* ---- the run ------------------------------------------------------------------------------------------- */
struct opts {
    int inferences, secs, stage, classify, iou, target, period_ms, md;
    const char *model;
};

struct state {
    int sdk, vi_open, dev_on, chn_on[3], npu, svp2;
};
static const int g_chn_ids[3] = { 0, 1, HD_CHN };

static void cleanup(struct state *s)
{
    int rc, c;
    g_step = "teardown";
    if (s->svp2) {
        rc = ak_svp2_destroy_chn(0);
        LOG("ak_svp2_destroy_chn(0) = %d", rc);
        s->svp2 = 0;
    }
    if (s->npu) {
        rc = ak_npu_deinit();
        LOG("ak_npu_deinit() = %d", rc);
        s->npu = 0;
    }
    for (c = 2; c >= 0; c--)
        if (s->chn_on[c]) {
            rc = ak_vi_disable_chn(g_chn_ids[c]);
            LOG("ak_vi_disable_chn(%d) = %d", g_chn_ids[c], rc);
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

/* Pull one frame of the discarded channels (the stock has an encoder draining each of them). */
static void drain(int chn)
{
    struct vi_frame f;
    memset(&f, 0, sizeof f);
    if (ak_vi_get_frame(chn, &f) == 0)
        ak_vi_release_frame(chn, &f);
}

/* Cheap motion fallback: the stock md thread feeds a 1536-byte VPSS statistics block to its software filter. The
 * layout is NOT known (LOW): print only layout-independent figures (non-zero 32-bit words, their sum and maximum). */
static void md_poll(int idx)
{
    uint32_t buf[MD_STAT_BYTES / 4];
    unsigned i, nz = 0, mx = 0;
    unsigned long sum = 0;
    int rc;
    memset(buf, 0, sizeof buf);
    g_step = "ak_vpss_md_get_stat";
    rc = ak_vpss_md_get_stat(0, buf);
    for (i = 0; i < MD_STAT_BYTES / 4; i++) {
        if (buf[i]) nz++;
        if (buf[i] > mx) mx = buf[i];
        sum += buf[i];
    }
    LOG("md %d: rc=%d nonzero_words=%u/%u max=%u sum=%lu", idx, rc, nz, (unsigned)(MD_STAT_BYTES / 4), mx, sum);
}

static void print_results(int idx, const struct svp2_output *out, long dt_us, int *hits)
{
    unsigned n = out->count, i, shown = 0;
    const struct svp2_result *r = (const struct svp2_result *)out->res;

    if (n > MAX_RESULTS || (n && !r)) {
        LOG("inf %d: dt=%ld.%01ld ms count=%u type=%u w12=%u: implausible output, not parsed", idx, dt_us / 1000,
            (dt_us % 1000) / 100, n, (unsigned)out->type, (unsigned)out->w12);
        return;
    }
    LOG("inf %d: dt=%ld.%01ld ms count=%u type=%u", idx, dt_us / 1000, (dt_us % 1000) / 100, n, (unsigned)out->type);
    for (i = 0; i < n; i++) {
        int px1, py1, px2, py2;
        /* the lib's output is data, not trusted: print only boxes inside the image, ordered corners, score 0..1000 */
        if (r[i].x1 < 0 || r[i].y1 < 0 || r[i].x2 < r[i].x1 || r[i].y2 < r[i].y1 || r[i].x2 >= (int32_t)HD_W ||
            r[i].y2 >= (int32_t)HD_H || r[i].score < 0 || r[i].score > 1000) {
            LOG("    #%u class=%d: box/score out of range, not printed", i, (int)r[i].cls);
            continue;
        }
        px1 = (int)((long)r[i].x1 * 100 / (long)(HD_W - 1));
        py1 = (int)((long)r[i].y1 * 100 / (long)(HD_H - 1));
        px2 = (int)((long)r[i].x2 * 100 / (long)(HD_W - 1));
        py2 = (int)((long)r[i].y2 * 100 / (long)(HD_H - 1));
        LOG("    #%u class=%d score=%d box=(%d,%d)-(%d,%d) pct=(%d,%d)-(%d,%d)", i, (int)r[i].cls, (int)r[i].score,
            (int)r[i].x1, (int)r[i].y1, (int)r[i].x2, (int)r[i].y2, px1, py1, px2, py2);
        shown++;
    }
    if (shown)
        (*hits)++;
}

static int run(const struct opts *o, struct state *s)
{
    struct vi_dev_attr da;
    struct vi_chn_attr ca[3];
    struct sdk_param sp;
    struct svp2_attr attr;
    struct svp2_conf conf;
    int rc, c, i, first_done;
    uint32_t mem_total = 0, dma_reserved = 0, depth;
    long loop_end_us = ((long)o->secs - 4) * 1000000L;
    long tmin = 0, tmax = 0, tsum = 0, tn = 0, cpu_u0, cpu_s0, cpu_u1, cpu_s1, run_t0;
    int hits = 0, fails = 0, done = 0;
    struct stat st;

    mem_report("before init");
    g_step = "ak_sdk_init";
    memset(&sp, 0, sizeof sp);
    rc = ak_sdk_init(&sp);
    LOG("ak_sdk_init = %d", rc);
    if (rc) return 1;
    s->sdk = 1;

    g_step = "ak_mem_get_size";
    rc = ak_mem_get_total_size(&mem_total);
    LOG("ak_mem_get_total_size = %d (%u)", rc, (unsigned)mem_total);
    rc = ak_mem_dma_get_reserved_size(&dma_reserved);
    depth = (dma_reserved > 52428800u) ? 3u : 2u;
    LOG("ak_mem_dma_get_reserved_size = %d (%u bytes) -> chn0/chn1 frame depth %u", rc, (unsigned)dma_reserved,
        (unsigned)depth);

    memset(ca, 0, sizeof ca);
    ca[0].chn = 0;  ca[0].fps = 15; ca[0].width = 2304; ca[0].height = 1296; ca[0].depth = depth;
    ca[1].chn = 1;  ca[1].fps = 15; ca[1].width = 640;  ca[1].height = 360;  ca[1].depth = depth;
    ca[2].chn = HD_CHN; ca[2].fps = 20; ca[2].width = HD_W; ca[2].height = HD_H; ca[2].depth = 2;

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
    LOG("ak_vi_set_dev_attr(0) = %d", rc);
    if (rc) goto fail;

    g_step = "ak_vi_set_chn_attr";
    for (c = 0; c < 3; c++) {
        rc = ak_vi_set_chn_attr(g_chn_ids[c], &ca[c]);
        LOG("ak_vi_set_chn_attr(%d, %ux%u, %u fps, depth %u) = %d", g_chn_ids[c], (unsigned)ca[c].width,
            (unsigned)ca[c].height, (unsigned)ca[c].fps, (unsigned)ca[c].depth, rc);
        if (rc) goto fail;
    }

    g_step = "ak_vi_enable_dev";
    rc = ak_vi_enable_dev(0);
    LOG("ak_vi_enable_dev(0) = %d", rc);
    if (rc) goto fail;
    s->dev_on = 1;

    g_step = "ak_vi_enable_chn";
    for (c = 0; c < 3; c++) {
        rc = ak_vi_enable_chn(g_chn_ids[c]);
        LOG("ak_vi_enable_chn(%d) = %d", g_chn_ids[c], rc);
        if (rc) goto fail;
        s->chn_on[c] = 1;
    }
    mem_report("after VI");

    /* ---- stage 1: look at the third channel's frames, no NPU ---- */
    if (o->stage == 1) {
        for (i = 0; i < 3 && !g_stop; i++) {
            struct vi_frame f;
            memset(&f, 0, sizeof f);
            drain(0);
            drain(1);
            g_step = "ak_vi_get_frame(16)";
            rc = ak_vi_get_frame(HD_CHN, &f);
            LOG("frame %d: ak_vi_get_frame(%d) = %d data=0x%08x len=%u ts=%llu phys_word=0x%08x w16=0x%x w20=0x%x w28=0x%x "
                "w32=0x%x w36=0x%x (expect len %u for NV12 %ux%u)",
                i, HD_CHN, rc, (unsigned)f.data, (unsigned)f.len, (unsigned long long)f.ts_ms, (unsigned)f.phys,
                (unsigned)f.w16, (unsigned)f.w20, (unsigned)f.w28, (unsigned)f.w32, (unsigned)f.w36,
                HD_W * HD_H * 3 / 2, (unsigned)HD_W, (unsigned)HD_H);
            if (rc == 0) {
                uint32_t p = 0;
                int pr = ak_mem_dma_vaddr2paddr((const void *)(uintptr_t)f.data, &p);
                LOG("frame %d: ak_mem_dma_vaddr2paddr = %d phys 0x%08x (%s the frame word)", i, pr, (unsigned)p,
                    p == f.phys ? "equals" : "DIFFERS from");
                ak_vi_release_frame(HD_CHN, &f);
            }
        }
        mem_report("after stage 1");
        cleanup(s);
        mem_report("after teardown");
        return 0;
    }

    /* ---- stage 2: NPU ---- */
    g_step = "ak_npu_init";
    rc = ak_npu_init();
    LOG("ak_npu_init() = %d", rc);
    if (rc) goto fail;
    s->npu = 1;
    mem_report("after npu init");

    if (o->stage == 2) {
        cleanup(s);
        mem_report("after teardown");
        return 0;
    }

    /* ---- stage 3: model load ---- */
    if (stat(o->model, &st) == 0)
        LOG("model file %s: %ld bytes (read by the vendor lib, never copied)", o->model, (long)st.st_size);
    else {
        LOG("model file %s: stat failed: %s", o->model, strerror(errno));
        rc = -1;
        goto fail;
    }
    memset(&attr, 0, sizeof attr);
    attr.model_num = 1;
    attr.m[0].env_mode = 0;
    attr.m[0].model_type = 10;
    attr.m[0].classify_thr = (uint32_t)o->classify;
    attr.m[0].iou_thr = (uint32_t)o->iou;
    attr.m[0].target_type = (uint32_t)o->target;
    memset(&conf, 0, sizeof conf);
    conf.path = o->model;
    conf.mode = 0;
    g_step = "ak_svp2_create_chn";
    rc = ak_svp2_create_chn(0, &attr, &conf);
    LOG("ak_svp2_create_chn(0, model_type 10, classify %d, iou %d, target %d, conf_mode 0) = %d", o->classify, o->iou,
        o->target, rc);
    if (rc) goto fail;
    s->svp2 = 1;
    mem_report("after model load");

    if (o->stage == 3) {
        cleanup(s);
        mem_report("after teardown");
        return 0;
    }

    /* ---- stage 4: inference loop ---- */
    first_done = 0;
    cpu_ms(&cpu_u0, &cpu_s0);
    run_t0 = us_now();
    for (i = 0; i < o->inferences && !g_stop; i++) {
        struct vi_frame f;
        struct svp2_input in;
        struct svp2_output out;
        uint32_t phys = 0;
        long a, dt;

        if (us_now() - run_t0 > loop_end_us) {
            LOG("loop time budget reached after %d inferences", done);
            break;
        }
        drain(0);
        drain(1);
        if (o->md)
            md_poll(i);

        memset(&f, 0, sizeof f);
        g_step = "ak_vi_get_frame(16)";
        rc = ak_vi_get_frame(HD_CHN, &f);
        if (rc) {
            LOG("inf %d: ak_vi_get_frame(%d) = %d", i, HD_CHN, rc);
            if (++fails >= 3) break;
            continue;
        }
        if (!first_done) {
            LOG("first frame: data=0x%08x len=%u (NV12 %ux%u = %u) ts=%llu phys_word=0x%08x", (unsigned)f.data,
                (unsigned)f.len, (unsigned)HD_W, (unsigned)HD_H, HD_W * HD_H * 3 / 2, (unsigned long long)f.ts_ms,
                (unsigned)f.phys);
            first_done = 1;
        }
        g_step = "ak_mem_dma_vaddr2paddr";
        rc = ak_mem_dma_vaddr2paddr((const void *)(uintptr_t)f.data, &phys);
        if (rc || f.len < HD_W * HD_H * 3 / 2) {
            LOG("inf %d: vaddr2paddr rc=%d phys 0x%08x len %u: cannot submit this frame", i, rc, (unsigned)phys,
                (unsigned)f.len);
            ak_vi_release_frame(HD_CHN, &f);
            if (++fails >= 3) break;
            continue;
        }
        memset(&in, 0, sizeof in);
        in.img_type = 0;
        in.width = HD_W; in.height = HD_H;
        in.roi_w = HD_W; in.roi_h = HD_H;
        in.phys = phys;
        in.virt = f.data;
        memset(&out, 0, sizeof out);

        g_step = "ak_svp2_process";
        a = us_now();
        rc = ak_svp2_process(0, &in, &out);
        dt = us_now() - a;
        if (rc) {
            LOG("inf %d: ak_svp2_process = 0x%x (%d) after %ld ms, ak_npu_reset_hw()", i, (unsigned)rc, rc, dt / 1000);
            ak_npu_reset_hw();
            ak_vi_release_frame(HD_CHN, &f);
            if (++fails >= 3) break;
            continue;
        }
        fails = 0;
        print_results(i, &out, dt, &hits);
        g_step = "ak_svp2_release";
        rc = ak_svp2_release(&out);
        if (rc) LOG("inf %d: ak_svp2_release = %d", i, rc);
        ak_vi_release_frame(HD_CHN, &f);

        tn++;
        tsum += dt;
        if (tn == 1 || dt < tmin) tmin = dt;
        if (dt > tmax) tmax = dt;
        done++;
        if (done % 20 == 0) {
            long avail;
            mem_report("loop");
            avail = proc_kb("/proc/meminfo", "MemAvailable");
            if (avail >= 0 && avail < MIN_AVAIL_KB) {
                LOG("MemAvailable %ld kB below %d kB: stopping", avail, MIN_AVAIL_KB);
                break;
            }
        }
        if (o->period_ms > 0)
            usleep((useconds_t)o->period_ms * 1000u);
    }
    cpu_ms(&cpu_u1, &cpu_s1);
    mem_report("after loop");
    if (tn)
        LOG("summary: %d inferences in %ld ms wall, process() min/avg/max = %ld/%ld/%ld us, %d with at least one result, "
            "cpu user %ld ms, sys %ld ms (whole process, includes VI and discarded frames)",
            done, (us_now() - run_t0) / 1000, tmin, tsum / tn, tmax, hits, cpu_u1 - cpu_u0, cpu_s1 - cpu_s0);
    else
        LOG("summary: no inference completed");
    cleanup(s);
    mem_report("after teardown");
    return tn ? 0 : 1;

fail:
    LOG("FAILED in step %s (rc %d), cleaning up", (const char *)g_step, rc);
    cleanup(s);
    return 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: poc4_detect [-n inferences] [-t secs] [-S 1..4] [-m model] [-c classify 0..1000] [-i iou 0..10] "
                    "[-T target 0..2] [-p period_ms] [-M]\n");
    return 3;
}

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
    struct opts o = { 50, 30, 4, 700, 3, 0, 100, 0, DEFAULT_MODEL };
    struct state s;
    int ch, rc;
    extern char *optarg;
    extern int optind;

    while ((ch = getopt(argc, argv, "n:t:S:m:c:i:T:p:M")) != -1) {
        switch (ch) {
        case 'n': if (parse_int(optarg, 1, 5000, &o.inferences)) return usage(); break;
        case 't': if (parse_int(optarg, 8, 120, &o.secs)) return usage(); break;
        case 'S': if (parse_int(optarg, 1, 4, &o.stage)) return usage(); break;
        case 'm': o.model = optarg; break;
        case 'c': if (parse_int(optarg, 0, 1000, &o.classify)) return usage(); break;
        case 'i': if (parse_int(optarg, 0, 10, &o.iou)) return usage(); break;
        case 'T': if (parse_int(optarg, 0, 2, &o.target)) return usage(); break;
        case 'p': if (parse_int(optarg, 0, 5000, &o.period_ms)) return usage(); break;
        case 'M': o.md = 1; break;
        default: return usage();
        }
    }
    if (optind != argc)
        return usage();
    if (strstr(o.model, "..") || strncmp(o.model, MODEL_DIR, sizeof MODEL_DIR - 1) != 0) {
        fprintf(stderr, "poc4: the model must be a file under " MODEL_DIR " (read-only, the camera's own copy)\n");
        return 3;
    }
    memset(&s, 0, sizeof s);
    gettimeofday(&t0, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_signals();
    alarm((unsigned)o.secs);
    rc = run(&o, &s);
    alarm(0);
    LOG("exit %d", rc);
    return rc;
}
