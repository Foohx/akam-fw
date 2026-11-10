#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ak_rtsp.h"
#include "ae.h"
#include "isp.h"
#include "isp_blocks.h"
#include "night.h"
#include "imgq.h"
#include "cfg_file.h"

/* Day white-balance gains of the hand-made manual block (R, G, B; 256 = 1.0x): the values the sensor_isp.conf
 * day block itself carries. */
#define WB_MANUAL_R 460
#define WB_MANUAL_G 256
#define WB_MANUAL_B 504

static volatile int g_venc_rc_stock;
/* Default: the sensor_isp.conf WB block, as the stock firmware sends it (user's choice after a side by side
 * look on 2026-11-10: the colours are better than with the hand-made manual block). */
static volatile int g_wb_conf = 1;
static volatile int g_poll;
static volatile int g_loaded;
static char g_conf_path[256] = IMGQ_CONF_FILE;  /* where img.wb is saved (the file imgq_load() read) */
/* ensure_loaded() runs from main(), the AE thread and the 8091 thread: the first one in loads, the others wait. */
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

static int parse_flag(const char *v)
{
    if (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true")) return 1;
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) return 0;
    return -1;
}

static void apply_option(const char *key, const char *val)
{
    if (!strcmp(key, "venc_rc")) {
        if (!strcmp(val, "stock")) g_venc_rc_stock = 1;
        else if (!strcmp(val, "legacy")) g_venc_rc_stock = 0;
    } else if (!strcmp(key, "wb")) {
        if (!strcmp(val, "conf")) g_wb_conf = 1;
        else if (!strcmp(val, "manual")) g_wb_conf = 0;
        else printf("[imgq] invalid wb \"%s\" ignored (want conf|manual)\n", val);
    } else if (!strcmp(key, "poll")) {
        int f = parse_flag(val);
        if (f >= 0) g_poll = f;
    }
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

static void load_file(const char *path)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        trim(p);
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        apply_option(p, v);
    }
    fclose(f);
}

void imgq_load(const char *conf_file, const char *ram_file)
{
    g_venc_rc_stock = 0;
    g_wb_conf = 1;
    g_poll = 0;
    snprintf(g_conf_path, sizeof(g_conf_path), "%s", conf_file ? conf_file : "");  /* "" = never saved (tests) */
    load_file(conf_file);
    if (g_venc_rc_stock)
        printf("[imgq] WARNING: venc_rc=stock is saved in %s: if the encoder oopses, the camera reboots into the "
               "same crash. Use %s (RAM, gone at reboot) for the first tests.\n", conf_file, IMGQ_RAM_FILE);
    load_file(ram_file);
    const char *e;
    if ((e = getenv("AKAM_VENC_RC")) != NULL) apply_option("venc_rc", e);
    if ((e = getenv("AKAM_IMG_WB")) != NULL) apply_option("wb", e);
    if ((e = getenv("AKAM_IMG_POLL")) != NULL) apply_option("poll", e);
    printf("[imgq] options: venc_rc=%s wb=%s poll=%d\n", g_venc_rc_stock ? "stock" : "legacy",
           g_wb_conf ? "conf" : "manual", g_poll);
    g_loaded = 1;
}

static void ensure_loaded(void)
{
    if (g_loaded) return;
    pthread_mutex_lock(&g_load_lock);
    if (!g_loaded) imgq_load(IMGQ_CONF_FILE, IMGQ_RAM_FILE);
    pthread_mutex_unlock(&g_load_lock);
}

int imgq_venc_rc_stock(void) { ensure_loaded(); return g_venc_rc_stock; }
int imgq_wb_conf(void)       { ensure_loaded(); return g_wb_conf; }
int imgq_poll_stats(void)    { ensure_loaded(); return g_poll; }

static int is_ours(const char *key)
{
    return !strcmp(key, "img.venc_rc") || !strcmp(key, "img.wb") || !strcmp(key, "img.poll") ||
           !strcmp(key, "img.reload") || !strcmp(key, "img.dump");
}

int imgq_ctl_get(const char *key, char *out, size_t cap)
{
    if (!is_ours(key)) return -1;
    ensure_loaded();
    if (!strcmp(key, "img.venc_rc"))
        snprintf(out, cap, "img.venc_rc=%s (read-only, decided at encoder creation)", g_venc_rc_stock ? "stock" : "legacy");
    else if (!strcmp(key, "img.wb"))
        snprintf(out, cap, "img.wb=%s", g_wb_conf ? "conf" : "manual");
    else if (!strcmp(key, "img.poll"))
        snprintf(out, cap, "img.poll=%d", g_poll);
    else
        snprintf(out, cap, "%s=(write only)", key);
    return 0;
}

void imgq_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *keys[] = { "img.venc_rc", "img.wb", "img.poll", "img.reload", "img.dump" };
    char line[128];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (imgq_ctl_get(keys[i], line, sizeof(line)) == 0) emit(arg, line);
    }
}

static int dump_get(void *ctx, uint32_t cmd, void *buf)
{
    (void)ctx;
    return isp_tunnel(cmd, buf);
}

static void dump_emit(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

static uint32_t now_ms(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static int do_dump(const char *val, char *msg, size_t cap)
{
    int all;
    if (!strcmp(val, "all")) all = 1;
    else if (!strcmp(val, "confirmed")) all = 0;
    else {
        snprintf(msg, cap, "ERR invalid img.dump value %s (want confirmed|all)", val);
        return 1;
    }
    char path[96];
    mkdir(IMGQ_DUMP_DIR, 0755);
    snprintf(path, sizeof(path), "%s/isp_blocks.bin", IMGQ_DUMP_DIR);
    FILE *bin = fopen(path, "wb");
    isp_dump_opts_t o = { SENSOR_ISP_CONF, all, bin, now_ms() };
    int n = isp_blocks_dump(dump_get, NULL, &o, dump_emit, NULL);
    if (bin) fclose(bin);
    if (n < 0) {
        snprintf(msg, cap, "ERR img.dump: no block could be read (see the log)");
        return 1;
    }
    snprintf(msg, cap, "OK img.dump=%s read=%d blocks, details in the log%s", val, n,
             bin ? ", binary in " IMGQ_DUMP_DIR "/isp_blocks.bin" : " (binary file not written)");
    return 0;
}

static int do_reload(char *msg, size_t cap)
{
    int night = night_is_night();
    int rc = night ? isp_pipeline_init_night() : isp_pipeline_init();
    if (rc != 0) {
        snprintf(msg, cap, "ERR img.reload: the %s calibration could not be re-sent", night ? "night" : "day");
        return 1;
    }
    isp_reset_picture_baselines();
    /* The file's exposure block carries its own dead-band and speed: re-apply the live AE tuning. */
    ae_tuning_t t;
    ae_get_tuning(&t);
    ae_set_tuning(&t);
    if (!night && !g_wb_conf) isp_set_wb_manual(WB_MANUAL_R, WB_MANUAL_G, WB_MANUAL_B);
    snprintf(msg, cap, "OK img.reload=1 (%s calibration re-sent, AE tuning re-applied%s)", night ? "night" : "day",
             (!night && !g_wb_conf) ? ", manual WB re-applied" : "");
    return 0;
}

/* Called by night.c at the end of every day / night switch, with the ISP configuration lock held. The reload of
 * the day subfile has just sent the file's WB block, which is what wb=conf wants (and the night subfile carries its
 * own). With wb=manual the hand-made block has to follow the reload, as img.reload does, or it would be lost at
 * the first night -> day switch. */
void imgq_after_mode_switch(int night)
{
    ensure_loaded();
    if (!night && !g_wb_conf && isp_set_wb_manual(WB_MANUAL_R, WB_MANUAL_G, WB_MANUAL_B) == 0)
        printf("[imgq] manual WB re-applied after the switch to day (img.wb=manual)\n");
}

static int not_ready(char *msg, size_t cap)
{
    snprintf(msg, cap, "ERR ISP not initialised yet (start-up in progress), retry in a few seconds");
    return 1;
}

/* Saves wb=conf|manual in the conf file, keeping its other lines (venc_rc, poll, comments) as they are: the line is
 * replaced in place, or appended. Nothing is written when the file has no wb line and the value is the default
 * (conf). Returns 0, or -1 with errno set. */
static int save_wb(int conf)
{
    char old[2048], out[2200];
    size_t n = 0, w = 0;
    if (!g_conf_path[0]) return 0;
    int found = 0;
    FILE *f = fopen(g_conf_path, "r");
    if (f) {
        n = fread(old, 1, sizeof(old) - 1, f);
        fclose(f);
    }
    old[n] = '\0';
    if (n == sizeof(old) - 1) { errno = EFBIG; return -1; }  /* never truncate a hand-edited file */
    for (char *p = old; *p;) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *q = p;
        while (q < p + len && (*q == ' ' || *q == '\t')) q++;
        int is_wb = !strncmp(q, "wb", 2) && (q[2] == '=' || q[2] == ' ' || q[2] == '\t');
        if (is_wb) {
            if (!found) w += (size_t)snprintf(out + w, sizeof(out) - w, "wb=%s\n", conf ? "conf" : "manual");
            found = 1;
        } else if (len || nl) {
            memcpy(out + w, p, len);
            w += len;
            out[w++] = '\n';
        }
        p += len + (nl ? 1 : 0);
    }
    if (!found) {
        if (conf) return 0;
        w += (size_t)snprintf(out + w, sizeof(out) - w, "wb=manual\n");
    }
    return cfg_write_atomic(g_conf_path, out, w);
}

/* img.wb: called with the ISP configuration lock held (the night thread cannot switch mode meanwhile). */
static int set_wb(const char *val, char *msg, size_t cap)
{
    int was_conf = g_wb_conf;
    if (!strcmp(val, "conf")) {
        if (isp_reapply_wb_conf(night_is_night()) != 0) {
            snprintf(msg, cap, "ERR img.wb: the WB block could not be re-sent");
            return 1;
        }
        g_wb_conf = 1;
    } else if (!strcmp(val, "manual")) {
        if (night_is_night()) {
            snprintf(msg, cap, "ERR img.wb=manual is a day setting: the night calibration keeps its own WB");
            return 1;
        }
        if (isp_set_wb_manual(WB_MANUAL_R, WB_MANUAL_G, WB_MANUAL_B) != 0) {
            snprintf(msg, cap, "ERR img.wb: the manual WB block could not be sent");
            return 1;
        }
        g_wb_conf = 0;
    } else {
        snprintf(msg, cap, "ERR invalid img.wb value %s (want manual|conf)", val);
        return 1;
    }
    snprintf(msg, cap, "OK img.wb=%s", g_wb_conf ? "conf" : "manual");
    /* Saved only when the value changed (flash wear); the answer says so if it could not be. */
    if (g_wb_conf != was_conf && save_wb(g_wb_conf) < 0)
        snprintf(msg, cap, "OK img.wb=%s (not saved: %s)", g_wb_conf ? "conf" : "manual", strerror(errno));
    return 0;
}

int imgq_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    if (!is_ours(key)) return -1;
    ensure_loaded();

    if (!strcmp(key, "img.venc_rc")) {
        snprintf(msg, cap, "ERR img.venc_rc is decided when the encoder is created: start ak_rtsp with "
                           "AKAM_VENC_RC=stock, or put venc_rc=stock in " IMGQ_RAM_FILE " (RAM) or " IMGQ_CONF_FILE);
        return 1;
    }
    if (!strcmp(key, "img.wb")) {
        if (!isp_pipeline_ready()) return not_ready(msg, cap);
        isp_cfg_lock();
        int rc = set_wb(val, msg, cap);
        isp_cfg_unlock();
        return rc;
    }
    if (!strcmp(key, "img.poll")) {
        int f = parse_flag(val);
        if (f < 0) {
            snprintf(msg, cap, "ERR invalid img.poll value %s (want 0|1)", val);
            return 1;
        }
        g_poll = f;
        snprintf(msg, cap, "OK img.poll=%d", f);
        return 0;
    }
    if (!strcmp(key, "img.reload")) {
        if (strcmp(val, "1") != 0) {
            snprintf(msg, cap, "ERR invalid img.reload value %s (want 1)", val);
            return 1;
        }
        if (!isp_pipeline_ready()) return not_ready(msg, cap);
        isp_cfg_lock();
        int rc = do_reload(msg, cap);
        isp_cfg_unlock();
        return rc;
    }
    if (!isp_pipeline_ready()) return not_ready(msg, cap);
    isp_cfg_lock();
    int rc = do_dump(val, msg, cap);
    isp_cfg_unlock();
    return rc;
}
