/* Person detection thread, Rotatable 2K own app. The call sequence is the hardware-validated one of PoC-4
 * (poc4_detect.c): ak_npu_init, ak_svp2_create_chn (stock model, type 10), then per inference ak_vi_get_frame(16),
 * ak_mem_dma_vaddr2paddr, ak_svp2_process, results, ak_svp2_release, ak_vi_release_frame (the frame is released
 * immediately after the call, never held across the pause). Results are the lib's output and are validated like PoC-4
 * (count cap, box inside the image, ordered corners, score 0..1000). This file opens no file and no device node: the
 * state file is written by akam2k_detstate.c, the vendor libraries open theirs. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "akam2k_abi.h"
#include "akam2k_cap.h"
#include "akam2k_detect.h"
#include "akam2k_detstate.h"
#include "akam2k_leds.h"
#include "akam2k_log.h"
#include "akam2k_mqtt.h"
#include "akam2k_ptz.h"
#include "akam2k_snap.h"
#include "akam2k_track_run.h"
#include "akam2k_zlive.h"
#include "rtp_clock.h"

#define MAX_RESULTS 64u
#define MIN_AVAIL_KB 6144       /* as the PoC-4 procedure: below 6 MB free the detection stops */
#define NO_FRAME_GIVEUP_MS 5000 /* chn 16 yields no frame for this long: detection gives up */
#define PROC_FAIL_MAX 3         /* consecutive ak_svp2_process / submit failures before giving up */
#define HEARTBEAT_MS 5000       /* the state file is rewritten at least this often (a reader sees a live writer) */
#define STATS_MS 60000
#define MAX_DRAIN 2             /* frames pulled per tick at most = the chn 16 queue depth: get_frame BLOCKS on an empty queue */
#define SLOW_GET_MS 10          /* a get slower than this waited for a new frame: that frame is fresh, stop pulling */
#define DEBUG_LOG_MS 1000       /* --detect-debug: at most one per-inference line per second (plus every positive change) */

static struct a2k_detect_cfg g_cfg;
static pthread_t g_th;
static int g_started;
static a2k_zones_t g_eff; /* the zones in force (detection thread only): all at home, only the @screen ones elsewhere (a2k_zones_effective) */
static volatile int g_chn_off; /* 1: the thread disabled VI chn 16 itself (the teardown must not do it again) */

void a2k_detect_defaults(struct a2k_detect_cfg *c)
{
    c->enabled = 1;
    c->hz = 5;
    c->threshold = 700;
    c->on_count = 2;
    c->off_ms = 3000;
    c->debug = 0;
    memset(&c->zones, 0, sizeof c->zones);
}

static int64_t now_ms(void) { return rtp_mono_us() / 1000; }

static void write_state(a2k_det_t *d, const a2k_zdet_t *zd, const char *status, int64_t *seq)
{
    struct a2k_det_snapshot s;
    static int warned;
    int i;
    memset(&s, 0, sizeof s);
    d->zone_mask = a2k_zdet_mask(zd);
    s.zn = zd->n;
    s.zall = g_eff.mode == A2K_ZONE_MODE_ALL;
    s.zmask = d->zone_mask;
    for (i = 0; i < zd->n; i++)
        s.zname[i] = g_eff.z[i].name;
    s.status = status;
    s.enabled = g_cfg.enabled;
    s.hz = g_cfg.hz;
    s.threshold = g_cfg.threshold;
    s.seq = ++*seq;
    s.now_epoch = (int64_t)time(NULL);
    s.uptime_ms = now_ms();
    s.det = d;
    /* the MQTT thread (a no-op when MQTT is off) gets the same state straight from here, without reading the file */
    a2k_mqtt_detection(d, status, s.now_epoch);
    a2k_leds_person(d->present);
    if (a2k_det_state_write(A2K_DET_STATE_FILE, &s) != 0 && !warned) {
        warned = 1;
        a2k_log("detect: cannot write %s (logged once)", A2K_DET_STATE_FILE);
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Validates the results of one inference into an observation. Returns 0, or -1 for an implausible output. A box that
 * overshoots the image by a few pixels (a person cut by the frame edge, the model may report x2 = 320 or y2 = 192) is
 * CLAMPED into the image instead of dropped: dropping it made a person standing at the edge invisible. A box whose
 * corners are unordered, or a score outside 0..1000, is skipped and counted in *rejected. */
static int collect(const struct svp2_output *out, struct a2k_det_obs *obs, unsigned *rejected, const a2k_zones_t *zones,
                   struct tl_box *tb, int *ntb)
{
    const struct svp2_result *r = (const struct svp2_result *)out->res;
    unsigned i;

    a2k_det_obs_reset(obs);
    *ntb = 0;
    if (out->count > MAX_RESULTS || (out->count && !r))
        return -1;
    for (i = 0; i < out->count; i++) {
        struct a2k_det_box b;
        int x1 = clampi(r[i].x1, 0, (int)A2K_DET_W - 1), y1 = clampi(r[i].y1, 0, (int)A2K_DET_H - 1);
        int x2 = clampi(r[i].x2, 0, (int)A2K_DET_W - 1), y2 = clampi(r[i].y2, 0, (int)A2K_DET_H - 1);
        if (r[i].x2 < r[i].x1 || r[i].y2 < r[i].y1 || r[i].score < 0 || r[i].score > 1000) {
            (*rejected)++; /* one bad box is skipped, the others still count */
            continue;
        }
        b.cls = r[i].cls;
        b.score = r[i].score;
        b.x1 = x1;
        b.y1 = y1;
        b.x2 = x2;
        b.y2 = y2;
        if (b.cls == A2K_DET_CLASS_PERSON && a2k_zones_ignored(zones, &b))
            continue; /* feet inside an ignore rectangle: not a person for anything (presence, zones, tracking) */
        if (zones->n > 0 && b.cls == A2K_DET_CLASS_PERSON) {
            unsigned hit = a2k_zones_hit(zones, &b);
            if (hit)
                a2k_det_obs_add_z(obs, &b, hit); /* a person whose feet are outside every zone does not count */
            else
                continue;
        } else {
            a2k_det_obs_add(obs, &b);
        }
        if (b.cls == A2K_DET_CLASS_PERSON && *ntb < TL_MAX_BOXES) {
            tb[*ntb].x1 = b.x1;
            tb[*ntb].y1 = b.y1;
            tb[*ntb].x2 = b.x2 + 1; /* pixel indexes -> edges, so a one pixel box is not empty */
            tb[*ntb].y2 = b.y2 + 1;
            (*ntb)++;
        }
    }
    if (zones->n > 0)
        a2k_det_obs_finish_mask(obs, a2k_zones_active_mask(zones), zones->mode == A2K_ZONE_MODE_ALL);
    return 0;
}

/* Pulls the NEWEST frame of chn 16. The stock keeps 20 fps in a depth-2 queue and the thread runs at 5 Hz, so the
 * queue holds stale frames at each tick. ak_vi_get_frame BLOCKS (up to one frame period, 50 ms) on an empty queue, so a
 * "get until empty" loop never ends early (hardware: 3 extra gets per tick, ~280 ms per cycle instead of 200). Bounded
 * instead: at most MAX_DRAIN (= queue depth) gets, and none after a get that was slow (it waited for a new frame, so the
 * frame is already the freshest). Older frames are released at once, at most 2 are ever held. Returns 0 with the frame in
 * *f and *drained = older frames skipped, or the first get_frame error when there was no frame at all. */
static void (*volatile g_frame_cb)(const uint8_t *y, unsigned w, unsigned h, unsigned stride);
void a2k_detect_set_frame_cb(void (*cb)(const uint8_t *y, unsigned w, unsigned h, unsigned stride)) { g_frame_cb = cb; }

static int fetch_newest(struct vi_frame *f, int *drained)
{
    struct vi_frame n;
    int rc, k;
    int64_t t;

    *drained = 0;
    memset(f, 0, sizeof *f);
    t = now_ms();
    rc = ak_vi_get_frame(A2K_DET_CHN, f);
    if (rc)
        return rc;
    for (k = 1; k < MAX_DRAIN && now_ms() - t <= SLOW_GET_MS; k++) {
        memset(&n, 0, sizeof n);
        t = now_ms();
        if (ak_vi_get_frame(A2K_DET_CHN, &n) != 0)
            break;
        ak_vi_release_frame(A2K_DET_CHN, f);
        *f = n;
        (*drained)++;
    }
    return 0;
}

/* The suffix of a zone line in the log: suspended, @screen, or (position anchored) at home. */
static const char *zone_tag(const struct a2k_zone *z)
{
    return z->off ? " (suspended)" : z->anchor == A2K_ZA_SCREEN ? " @screen" : " (at home)";
}

static void log_zones(const a2k_zones_t *z)
{
    int zi;
    if (z->n > 0) {
        a2k_log("detect: %d zone(s), zone_mode %s (a person counts by its feet point)", z->n, z->mode == A2K_ZONE_MODE_ALL ? "all" : "any");
        for (zi = 0; zi < z->n; zi++)
            a2k_log("detect: zone %s = %d,%d,%d,%d %%%s", z->z[zi].name, z->z[zi].x1, z->z[zi].y1, z->z[zi].x2, z->z[zi].y2,
                    zone_tag(&z->z[zi]));
    } else {
        a2k_log("detect: no zone, the whole image counts");
    }
    for (zi = 0; zi < z->ni; zi++)
        a2k_log("detect: ignore %s = %d,%d,%d,%d %%%s", z->ig[zi].name, z->ig[zi].x1, z->ig[zi].y1, z->ig[zi].x2, z->ig[zi].y2,
                zone_tag(&z->ig[zi]));
}

static void log_zone_event(const char *name, const struct a2k_det_event *ev)
{
    if (ev->type == A2K_DET_EV_ON)
        a2k_log("detect: zone %s person ON score=%d box=%d,%d,%d,%d", name, ev->box.score, ev->box.x1, ev->box.y1, ev->box.x2,
                ev->box.y2);
    else
        a2k_log("detect: zone %s person OFF after %lld.%01llds", name, (long long)(ev->present_ms / 1000),
                (long long)((ev->present_ms % 1000) / 100));
}

static void log_event(const struct a2k_det_event *ev)
{
    if (ev->type == A2K_DET_EV_ON)
        a2k_log("detect: person ON score=%d box=%d,%d,%d,%d", ev->box.score, ev->box.x1, ev->box.y1, ev->box.x2,
                ev->box.y2);
    else
        a2k_log("detect: person OFF after %lld.%01llds (no person for %lld.%01llds) last score=%d box=%d,%d,%d,%d",
                (long long)(ev->present_ms / 1000), (long long)((ev->present_ms % 1000) / 100),
                (long long)(ev->absent_ms / 1000), (long long)((ev->absent_ms % 1000) / 100), ev->box.score,
                ev->box.x1, ev->box.y1, ev->box.x2, ev->box.y2);
}

/* Sleeps until the deadline in slices, so a stop request is honoured within 50 ms. */
static void wait_until(int64_t deadline)
{
    int64_t t;
    while (!a2k_stop && (t = now_ms()) < deadline)
        usleep((useconds_t)((deadline - t > 50 ? 50 : deadline - t) * 1000));
}

/* Detection ended on its own (not by the app stopping) while VI chn 16 is still enabled: nobody reads its queue any
 * more, and it is not known whether a full queue could ever hold up chn 0/1. Disable the channel; when the vendor
 * refuses, keep draining it (get, release at once, ~5 Hz) until the app stops. Streams are never touched. */
static void quiesce_chn(void)
{
    struct vi_frame f;
    int rc;

    a2k_set_step("detect disable chn");
    rc = ak_vi_disable_chn(A2K_DET_CHN);
    a2k_log("detect: ak_vi_disable_chn(%d) = %d after detection ended", A2K_DET_CHN, rc);
    if (rc == 0) {
        g_chn_off = 1;
        return;
    }
    a2k_log("detect: chn %d stays enabled, draining it until stop", A2K_DET_CHN);
    a2k_set_step("detect drain");
    while (!a2k_stop) {
        memset(&f, 0, sizeof f);
        if (ak_vi_get_frame(A2K_DET_CHN, &f) == 0)
            ak_vi_release_frame(A2K_DET_CHN, &f);
        wait_until(now_ms() + 200);
    }
}

int a2k_detect_chn_disabled(void) { return g_chn_off; }

static void *detect_thread(void *arg)
{
    struct svp2_attr attr;
    struct svp2_conf conf;
    a2k_det_t det;
    a2k_zdet_t zd;
    unsigned zgen = 0;
    int zhome = 1;
    struct a2k_det_event zev[A2K_ZONE_MAX];
    struct a2k_det_cfg dc;
    int npu = 0, svp2 = 0, rc, fails = 0;
    int64_t seq = 0, period = 1000 / g_cfg.hz, next, last_hb, last_stats = 0, last_frame_ok;
    unsigned long done = 0, stat_n = 0, implausible = 0, pfail = 0, stat_pos = 0, stat_drained = 0, dbg_n = 0;
    unsigned rejected = 0, stat_rej = 0;
    long long stat_sum = 0, stat_max = 0, stat_age = 0, stat_age_max = 0, min_raw_age = 0, last_ts = 0, last_dbg = 0;
    int have_age = 0;
    const char *final = "stopped";
    (void)arg;

    dc.on_count = g_cfg.on_count;
    dc.off_ms = g_cfg.off_ms;
    a2k_det_init(&det, &dc);
    {
        a2k_ptz_info_t pi;
        a2k_zones_t cz;
        a2k_ptz_info(&pi);
        zhome = a2k_zlive_applies(pi.state);
        a2k_zlive_get(&cz, &zgen);
        a2k_zones_effective(&cz, zhome, &g_eff);
    }
    a2k_zdet_init(&zd, g_eff.n, &dc);

    /* the model load needs RAM the streams must keep: refuse to start below the guard (checked again every minute) */
    if ((rc = (int)a2k_mem_avail_kb()) >= 0 && rc < MIN_AVAIL_KB) {
        a2k_log("detect: MemAvailable %d kB below %d kB, detection not started", rc, MIN_AVAIL_KB);
        final = "failed";
        goto out;
    }
    a2k_set_step("ak_npu_init");
    rc = ak_npu_init();
    a2k_log("detect: ak_npu_init() = %d", rc);
    if (rc) {
        final = "failed";
        a2k_log("detect: NPU init failed, detection off (RTSP keeps running)");
        goto out;
    }
    npu = 1;
    a2k_mem_report("after npu init");

    memset(&attr, 0, sizeof attr);
    attr.model_num = 1;
    attr.m[0].model_type = 10;
    attr.m[0].classify_thr = (uint32_t)g_cfg.threshold;
    attr.m[0].iou_thr = 3;
    attr.m[0].target_type = 0;
    memset(&conf, 0, sizeof conf);
    conf.path = A2K_DET_MODEL;
    a2k_set_step("ak_svp2_create_chn");
    rc = ak_svp2_create_chn(0, &attr, &conf);
    a2k_log("detect: ak_svp2_create_chn(0, model_type 10, classify %d, iou 3, target 0) = %d", g_cfg.threshold, rc);
    if (rc) {
        final = "failed";
        a2k_log("detect: model load failed, detection off (RTSP keeps running)");
        goto out;
    }
    svp2 = 1;
    a2k_mem_report("after model load");
    a2k_log("detect: running at %d Hz, classify %d, ON after %d inferences, OFF after %d ms", g_cfg.hz, g_cfg.threshold,
            g_cfg.on_count, g_cfg.off_ms);
    log_zones(&g_eff);

    last_frame_ok = next = now_ms();
    write_state(&det, &zd, "run", &seq);
    last_hb = now_ms();
    while (!a2k_stop) {
        struct vi_frame f;
        struct svp2_input in;
        struct svp2_output out;
        struct a2k_det_obs obs;
        struct tl_box tbox[TL_MAX_BOXES];
        int ntbox = 0;
        struct a2k_det_event ev;
        uint32_t phys = 0;
        int64_t t0, t1, age = 0;
        int drained = 0;

        wait_until(next);
        if (a2k_stop)
            break;
        t0 = now_ms();
        next += period;
        if (next < t0) /* fell behind (a stall): do not burst */
            next = t0 + period;

        memset(&f, 0, sizeof f);
        a2k_set_step("detect ak_vi_get_frame");
        if (fetch_newest(&f, &drained) != 0) {
            if (t0 - last_frame_ok > NO_FRAME_GIVEUP_MS) {
                a2k_log("detect: no frame from chn %d for %d s, detection off", A2K_DET_CHN, NO_FRAME_GIVEUP_MS / 1000);
                final = "failed";
                break;
            }
            next = t0 + 20; /* retry soon, the encoders are not involved */
            continue;
        }
        last_frame_ok = t0;
        /* The frame clock is not known to be ours: the age is relative to the freshest frame ever seen (raw age minus
         * its minimum), which is the real queueing delay whatever the origin of the timestamps. */
        {
            long long raw = (long long)t0 - (long long)f.ts_ms;
            if (!have_age || raw < min_raw_age) {
                min_raw_age = raw;
                have_age = 1;
            }
            age = raw - min_raw_age;
        }
        a2k_set_step("detect vaddr2paddr");
        rc = ak_mem_dma_vaddr2paddr((const void *)(uintptr_t)f.data, &phys);
        if (rc || f.len < A2K_DET_W * A2K_DET_H * 3 / 2) {
            ak_vi_release_frame(A2K_DET_CHN, &f);
            if (++fails >= PROC_FAIL_MAX) {
                a2k_log("detect: cannot submit frames (vaddr2paddr rc=%d len=%u), detection off", rc, (unsigned)f.len);
                final = "failed";
                break;
            }
            continue;
        }
        if (g_frame_cb)
            g_frame_cb((const uint8_t *)(uintptr_t)f.data, A2K_DET_W, A2K_DET_H, A2K_DET_W); /* the Y plane, before the frame is released */
        memset(&in, 0, sizeof in);
        in.width = A2K_DET_W;
        in.height = A2K_DET_H;
        in.roi_w = A2K_DET_W;
        in.roi_h = A2K_DET_H;
        in.phys = phys;
        in.virt = f.data;
        memset(&out, 0, sizeof out);
        a2k_set_step("detect ak_svp2_process");
        rc = ak_svp2_process(0, &in, &out);
        t1 = now_ms();
        if (rc) {
            pfail++;
            ak_npu_reset_hw();
            ak_vi_release_frame(A2K_DET_CHN, &f);
            if (pfail % 20 == 1)
                a2k_log("detect: ak_svp2_process = 0x%x after %lld ms (%lu failures)", (unsigned)rc,
                        (long long)(t1 - t0), pfail);
            if (++fails >= PROC_FAIL_MAX) {
                a2k_log("detect: %d consecutive inference failures, detection off", fails);
                final = "failed";
                break;
            }
            continue;
        }
        fails = 0;
        {
            /* zones edited at run time, or the camera left / came back to home: the zones in force change */
            a2k_ptz_info_t pi;
            a2k_zones_t cz;
            unsigned gen;
            int home;
            a2k_ptz_info(&pi);
            home = a2k_zlive_applies(pi.state);
            a2k_zlive_get(&cz, &gen);
            if (gen != zgen || home != zhome) {
                zgen = gen;
                zhome = home;
                a2k_zones_effective(&cz, home, &g_eff);
                a2k_zdet_init(&zd, g_eff.n, &dc);
                a2k_log("detect: zones %s", home ? "in force" : "suspended (the camera is not at home; @screen zones stay)");
                if (g_eff.n > 0 || g_eff.ni > 0)
                    log_zones(&g_eff);
                write_state(&det, &zd, "run", &seq);
                last_hb = t1;
            }
        }
        rc = collect(&out, &obs, &rejected, &g_eff, tbox, &ntbox);
        if (g_cfg.debug && t1 - last_dbg >= DEBUG_LOG_MS) {
            a2k_log("detect-debug: #%lu ts=%llu dts=%lld age=%lld ms drained=%d count=%u rejected=%u person=%d score=%d "
                    "box=%d,%d,%d,%d heads=%d",
                    ++dbg_n, (unsigned long long)f.ts_ms, last_ts ? (long long)f.ts_ms - last_ts : 0, (long long)age,
                    drained, (unsigned)out.count, rejected, obs.person, obs.person ? obs.best.score : 0, obs.best.x1,
                    obs.best.y1, obs.best.x2, obs.best.y2, obs.heads);
            last_dbg = t1;
        }
        last_ts = (long long)f.ts_ms;
        ak_svp2_release(&out);
        ak_vi_release_frame(A2K_DET_CHN, &f); /* released right away, before the state work */
        if (rc) {
            if (++implausible % 100 == 1)
                a2k_log("detect: implausible output, ignored (%lu so far)", implausible);
            continue;
        }
        stat_n++;
        stat_pos += obs.person ? 1 : 0;
        stat_drained += (unsigned long)drained;
        stat_age += age;
        if (age > stat_age_max)
            stat_age_max = age;
        stat_sum += t1 - t0;
        if (t1 - t0 > stat_max)
            stat_max = t1 - t0;
        done++;
        a2k_track_feed(t1, tbox, ntbox, (int)A2K_DET_W, (int)A2K_DET_H); /* tracking: one call per inference, n = 0 when nobody */
        {
            int gch = a2k_det_update(&det, t1, &obs, &ev);
            unsigned zch = g_eff.n > 0 ? a2k_zdet_update(&zd, t1, &obs, zev) : 0;
            int zi;
            if (gch) {
                det.last_event_epoch = (int64_t)time(NULL);
                log_event(&ev);
                if (ev.type == A2K_DET_EV_ON)
                    (void)a2k_snap_request(t1); /* one JPEG of the sub stream, taken by the capture thread (never here) */
            }
            for (zi = 0; zi < zd.n; zi++)
                if (zch & (1u << zi))
                    log_zone_event(g_eff.z[zi].name, &zev[zi]);
            if (gch || zch) {
                write_state(&det, &zd, "run", &seq);
                last_hb = t1;
            } else if (t1 - last_hb >= HEARTBEAT_MS) {
                write_state(&det, &zd, "run", &seq);
                last_hb = t1;
            }
        }
        if (t1 - last_stats >= STATS_MS) {
            long avail;
            if (last_stats && stat_n)
                a2k_log("detect: stats %lu inferences, %lu positive, %lld ms avg / %lld ms max per frame+process, "
                        "frame age %lld ms avg / %lld ms max, %lu stale frames skipped, %u boxes rejected, person=%d",
                        stat_n, stat_pos, stat_sum / (long long)stat_n, stat_max, stat_age / (long long)stat_n,
                        stat_age_max, stat_drained, rejected - stat_rej, det.present);
            stat_rej = rejected;
            stat_n = stat_pos = stat_drained = 0;
            stat_sum = stat_max = stat_age = stat_age_max = 0;
            last_stats = t1;
            avail = a2k_mem_avail_kb();
            if (avail >= 0 && avail < MIN_AVAIL_KB) {
                a2k_log("detect: MemAvailable %ld kB below %d kB, detection off", avail, MIN_AVAIL_KB);
                final = "failed";
                break;
            }
        }
    }

out:
    /* teardown order: svp2 destroy, then npu deinit (the encoders and VI stay up until a2k_detect_stop returns) */
    a2k_set_step("detect teardown");
    if (svp2) {
        rc = ak_svp2_destroy_chn(0);
        a2k_log("detect: ak_svp2_destroy_chn(0) = %d (%lu inferences done)", rc, done);
    }
    if (npu) {
        rc = ak_npu_deinit();
        a2k_log("detect: ak_npu_deinit() = %d", rc);
    }
    det.present = 0; /* a reader must not take the last person=1 of a dead detector for a live one (status says why) */
    for (rc = 0; rc < zd.n; rc++)
        zd.z[rc].present = 0;
    write_state(&det, &zd, final, &seq);
    if (!a2k_stop && strcmp(final, "stopped") != 0)
        quiesce_chn();
    return NULL;
}

int a2k_detect_start(const struct a2k_detect_cfg *c)
{
    pthread_attr_t at;
    int rc;

    if (g_started || !c->enabled)
        return 0;
    g_cfg = *c;
    if (g_cfg.hz < 1 || g_cfg.hz > 10 || g_cfg.threshold < 500 || g_cfg.threshold > 950 || g_cfg.on_count < 1 ||
        !a2k_model_path_ok(A2K_DET_MODEL)) {
        a2k_log("detect: invalid configuration, detection off");
        return -1;
    }
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 192 * 1024);
    rc = pthread_create(&g_th, &at, detect_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc) {
        a2k_log("detect: pthread_create = %d, detection off", rc);
        return -1;
    }
    g_started = 1;
    return 0;
}

void a2k_detect_stop(void)
{
    if (!g_started)
        return;
    a2k_set_step("detect join");
    pthread_join(g_th, NULL);
    g_started = 0;
}
