/* Host test of the detection thread's ending (akam2k_detect.c with stubbed vendor calls): when detection ends on its
 * own, chn 16 is disabled; when the vendor refuses, it is drained (every frame got is released) until the app stops;
 * when the app itself is stopping, nothing is disabled. Usage: rot2k_detect_thread_test ok|refused|stopping */
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_abi.h"
#include "akam2k_cap.h"
#include "akam2k_detect.h"
#include "akam2k_ptz.h"
#include "akam2k_track_run.h"
#include "akam2k_zlive.h"
#include "akam2k_mqtt.h"

volatile sig_atomic_t a2k_stop;
static int g_disable_rc, g_disable_n, g_get_n, g_rel_n, g_deinit_n;

void a2k_log(const char *fmt, ...) { (void)fmt; }
int a2k_snap_request(int64_t now_ms) { (void)now_ms; return 0; }
void a2k_mqtt_detection(const a2k_det_t *d, const char *status, int64_t epoch) { (void)d; (void)status; (void)epoch; }
void a2k_leds_person(int present) { (void)present; }
void a2k_mqtt_detections(const char *json) { (void)json; }
int a2k_ptz_privacy_active(void) { return 0; }
a2k_track_t *a2k_track_run_get(void) { return NULL; }
int a2k_track_enabled(a2k_track_t *t) { (void)t; return 0; }
void a2k_ptz_info(a2k_ptz_info_t *o) { memset(o, 0, sizeof *o); }
int a2k_zlive_applies(int st) { (void)st; return 1; }
void a2k_zlive_get(a2k_zones_t *o, unsigned *g) { memset(o, 0, sizeof *o); if (g) *g = 1; }
void a2k_track_feed(int64_t now_ms, int64_t cap_ms, const struct tl_box *b, int n, int fw, int fh) { (void)now_ms; (void)cap_ms; (void)b; (void)n; (void)fw; (void)fh; }
void a2k_set_step(const char *s) { (void)s; }
void a2k_mem_report(const char *t) { (void)t; }
long a2k_mem_avail_kb(void) { return 100000; }
int ak_npu_init(void) { return 1; } /* fails: detection ends at once */
int ak_npu_deinit(void) { g_deinit_n++; return 0; }
void ak_npu_reset_hw(void) {}
int ak_svp2_create_chn(int h, const struct svp2_attr *a, const struct svp2_conf *c) { (void)h; (void)a; (void)c; return 1; }
int ak_svp2_destroy_chn(int h) { (void)h; return 0; }
int ak_svp2_process(int h, const struct svp2_input *i, struct svp2_output *o) { (void)h; (void)i; (void)o; return 1; }
int ak_svp2_release(struct svp2_output *o) { (void)o; return 0; }
int ak_mem_dma_vaddr2paddr(const void *v, uint32_t *p) { (void)v; (void)p; return 1; }
int ak_vi_disable_chn(int chn) { g_disable_n += chn == A2K_DET_CHN; return g_disable_rc; }
int ak_vi_get_frame(int chn, struct vi_frame *f) { (void)f; g_get_n += chn == A2K_DET_CHN; return 0; }
int ak_vi_release_frame(int chn, struct vi_frame *f) { (void)f; g_rel_n += chn == A2K_DET_CHN; return 0; }

static int fails;
static void chk(int ok, const char *what)
{
    printf("%s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

int main(int argc, char **argv)
{
    struct a2k_detect_cfg c;
    const char *mode = argc > 1 ? argv[1] : "";

    a2k_detect_defaults(&c);
    g_disable_rc = strcmp(mode, "refused") == 0 ? -1 : 0;
    if (strcmp(mode, "stopping") == 0)
        a2k_stop = 1;
    chk(a2k_detect_start(&c) == 0, "thread started");
    if (strcmp(mode, "refused") == 0) {
        usleep(1200 * 1000); /* ~5 drain rounds at 5 Hz */
        chk(g_get_n >= 3, "refused: chn 16 is drained (frames got)");
        chk(g_get_n == g_rel_n, "refused: every frame got is released");
        a2k_stop = 1;
    } else if (strcmp(mode, "ok") == 0) {
        usleep(300 * 1000);
    } else {
        usleep(100 * 1000);
    }
    a2k_detect_stop(); /* joins: must return promptly even while draining */
    if (strcmp(mode, "ok") == 0) {
        chk(g_disable_n == 1, "ok: chn 16 disabled once after detection ended");
        chk(a2k_detect_chn_disabled() == 1, "ok: the teardown is told chn 16 is already off");
        chk(g_get_n == 0, "ok: no drain after a successful disable");
    } else if (strcmp(mode, "refused") == 0) {
        chk(g_disable_n == 1, "refused: disable tried once");
        chk(a2k_detect_chn_disabled() == 0, "refused: the teardown still disables chn 16");
        chk(g_get_n == g_rel_n, "refused: still balanced after the join");
    } else {
        chk(g_disable_n == 0 && g_get_n == 0, "stopping: chn 16 untouched when the app is stopping");
    }
    printf("rot2k detect thread (%s): %d failed\n", mode, fails);
    return fails != 0;
}
