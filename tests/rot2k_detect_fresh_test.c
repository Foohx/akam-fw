/* Host test of the detection thread's frame choice (akam2k_detect.c with stubbed vendor calls). chn 16 holds a queue of
 * timestamped frames (the stub get_frame pops the OLDEST, like the VI queue); the thread must run the NPU on the NEWEST
 * one, release every older frame it skipped and the one it used, and keep get/release balanced. A box that overshoots the
 * image is clamped, not dropped. Prints "rot2k detect fresh: N failed". */
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
#define NQ 2 /* the chn 16 queue depth */
static uint32_t g_q_data[NQ] = { 0x1000, 0x2000 };
static uint64_t g_q_ts[NQ] = { 1000, 1050 };
static int g_slow_first_ms; /* the first get of a run waits this long, like a get on an empty queue */
static volatile int g_q_head;
static int g_get_n, g_rel_n, g_proc_n;
static uint32_t g_proc_virt, g_rel_first;
static struct svp2_result g_res = { 0, 10, 0, 330, 200, 800 }; /* overshoots a 320x192 image on two sides */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char g_logbuf[4096];

int a2k_snap_request(int64_t now_ms) { (void)now_ms; return 0; }
void a2k_mqtt_detection(const a2k_det_t *d, const char *status, int64_t epoch) { (void)d; (void)status; (void)epoch; }
void a2k_leds_person(int present) { (void)present; }
void a2k_ptz_info(a2k_ptz_info_t *o) { memset(o, 0, sizeof *o); }
int a2k_zlive_applies(int st) { (void)st; return 1; }
void a2k_zlive_get(a2k_zones_t *o, unsigned *g) { memset(o, 0, sizeof *o); if (g) *g = 1; }
void a2k_track_feed(int64_t now_ms, const struct tl_box *b, int n, int fw, int fh) { (void)now_ms; (void)b; (void)n; (void)fw; (void)fh; }
void a2k_log(const char *fmt, ...)
{
    va_list ap;
    char line[512];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    if (strlen(g_logbuf) + strlen(line) + 2 < sizeof g_logbuf) {
        strcat(g_logbuf, line);
        strcat(g_logbuf, "\n");
    }
    pthread_mutex_unlock(&mu);
}
void a2k_set_step(const char *s) { (void)s; }
void a2k_mem_report(const char *t) { (void)t; }
long a2k_mem_avail_kb(void) { return 100000; }
int ak_npu_init(void) { return 0; }
int ak_npu_deinit(void) { return 0; }
void ak_npu_reset_hw(void) {}
int ak_svp2_create_chn(int h, const struct svp2_attr *a, const struct svp2_conf *c) { (void)h; (void)a; (void)c; return 0; }
int ak_svp2_destroy_chn(int h) { (void)h; return 0; }
int ak_svp2_process(int h, const struct svp2_input *i, struct svp2_output *o)
{
    (void)h;
    if (!g_proc_n++)
        g_proc_virt = i->virt;
    o->count = 1;
    o->res = &g_res;
    return 0;
}
int ak_svp2_release(struct svp2_output *o) { (void)o; return 0; }
int ak_mem_dma_vaddr2paddr(const void *v, uint32_t *p) { *p = (uint32_t)(uintptr_t)v; return 0; }
int ak_vi_disable_chn(int chn) { (void)chn; return 0; }
int ak_vi_get_frame(int chn, struct vi_frame *f)
{
    if (chn != A2K_DET_CHN || g_q_head >= NQ)
        return -1; /* queue empty */
    if (g_slow_first_ms && !g_q_head) {
        usleep(g_slow_first_ms * 1000);
        g_slow_first_ms = 0;
    }
    f->data = g_q_data[g_q_head];
    f->len = A2K_DET_W * A2K_DET_H * 3 / 2;
    f->ts_ms = g_q_ts[g_q_head++];
    g_get_n++;
    return 0;
}
int ak_vi_release_frame(int chn, struct vi_frame *f)
{
    if (chn == A2K_DET_CHN) {
        if (!g_rel_n)
            g_rel_first = f->data;
        g_rel_n++;
    }
    return 0;
}

static int fails;
static void chk(int ok, const char *what)
{
    printf("%s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

int main(void)
{
    struct a2k_detect_cfg c;

    a2k_detect_defaults(&c);
    c.debug = 1;
    chk(a2k_detect_start(&c) == 0, "thread started");
    usleep(400 * 1000);
    a2k_stop = 1;
    a2k_detect_stop();
    chk(g_get_n == NQ, "both queued frames (depth 2) were pulled in the first tick");
    chk(g_proc_n >= 1 && g_proc_virt == g_q_data[NQ - 1], "the NPU ran on the NEWEST frame");
    chk(g_rel_first == g_q_data[0], "the oldest frame was released first (skipped)");
    chk(g_rel_n == g_get_n, "every frame got is released exactly once");
    pthread_mutex_lock(&mu);
    chk(strstr(g_logbuf, "detect-debug:") && strstr(g_logbuf, "drained=1") && strstr(g_logbuf, "count=1"),
        "debug line reports the drained count and result count");
    chk(strstr(g_logbuf, "box=10,0,319,191") != NULL, "an overshooting box is clamped into the image, not dropped");
    pthread_mutex_unlock(&mu);

    /* Second run: the first get is slow (it waited for a new frame): the thread must not pull again in that tick, so it
     * never waits on an empty queue more than once. 120 ms covers the first tick only (period 200 ms). */
    g_q_head = g_get_n = g_rel_n = g_proc_n = 0;
    g_proc_virt = 0;
    g_slow_first_ms = 30;
    a2k_stop = 0;
    chk(a2k_detect_start(&c) == 0, "thread restarted");
    usleep(120 * 1000);
    a2k_stop = 1;
    a2k_detect_stop();
    chk(g_get_n == 1 && g_proc_n == 1 && g_proc_virt == g_q_data[0], "after a slow get the thread stops pulling and uses that frame");
    chk(g_rel_n == g_get_n, "get/release still balanced after a slow get");
    printf("rot2k detect fresh: %d failed\n", fails);
    return fails != 0;
}
