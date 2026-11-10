/* Host test of src/ak_rtsp/imgq.c: start-up options (file, environment, precedence), the img.* keys and
 * their calls into the ISP layer (stubs here), with no camera. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ae.h"
#include "imgq.h"
#include "isp.h"
#include "night.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

/* ---- stubs of the device layer ---- */
static int g_night;
static int g_wb_conf_calls, g_wb_conf_night, g_wb_manual_calls, g_init_day, g_init_night, g_resets, g_ae_sets;
static int g_fail_wb_conf, g_fail_init;
static int g_tunnel_cmds[64], g_tunnel_n;

int  night_is_night(void) { return g_night; }
int  isp_reapply_wb_conf(int night) { g_wb_conf_calls++; g_wb_conf_night = night; return g_fail_wb_conf ? -1 : 0; }
int  isp_set_wb_manual(uint16_t r, uint16_t g, uint16_t b)
{
    CHECK(r == 460 && g == 256 && b == 504);
    g_wb_manual_calls++;
    return 0;
}
int  isp_pipeline_init(void) { g_init_day++; return g_fail_init ? -1 : 0; }
int  isp_pipeline_init_night(void) { g_init_night++; return g_fail_init ? -1 : 0; }
void isp_reset_picture_baselines(void) { g_resets++; }
void ae_get_tuning(ae_tuning_t *out) { memset(out, 0, sizeof(*out)); out->speed = 7; }
int  ae_set_tuning(const ae_tuning_t *in) { CHECK(in->speed == 7); g_ae_sets++; return 0; }
int  isp_tunnel(uint32_t cmd, void *payload)
{
    (void)payload;
    if (g_tunnel_n < 64) g_tunnel_cmds[g_tunnel_n++] = (int)cmd;
    return -1; /* every block unreadable: the dump reports it */
}

/* ---- helpers ---- */
static void clear_env(void)
{
    unsetenv("AKAM_VENC_RC");
    unsetenv("AKAM_IMG_WB");
    unsetenv("AKAM_IMG_POLL");
}

static void write_conf(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static int g_lines;
static char g_line[16][160];
static void collect(void *arg, const char *line)
{
    (void)arg;
    if (g_lines < 16) snprintf(g_line[g_lines++], sizeof(g_line[0]), "%s", line);
}

int main(void)
{
    char path[] = "/tmp/imgq_test_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    char msg[256], out[160];

    /* defaults */
    clear_env();
    imgq_load(NULL);
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && !imgq_poll_stats());
    imgq_load("/nonexistent/image");
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && !imgq_poll_stats());

    /* file: comments, blanks, spaces, unknown keys and bad values ignored */
    write_conf(path,
               "# comment\n\n  venc_rc = stock\nwb=conf\npoll=on\nunknown=1\nvenc_rc_x=stock\n   # indented comment\n");
    imgq_load(path);
    CHECK(imgq_venc_rc_stock() && imgq_wb_conf() && imgq_poll_stats());
    write_conf(path, "venc_rc=bogus\nwb=\npoll=maybe\n");
    imgq_load(path);
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && !imgq_poll_stats());

    /* environment wins over the file */
    write_conf(path, "venc_rc=stock\nwb=conf\npoll=1\n");
    setenv("AKAM_VENC_RC", "legacy", 1);
    setenv("AKAM_IMG_WB", "manual", 1);
    setenv("AKAM_IMG_POLL", "0", 1);
    imgq_load(path);
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && !imgq_poll_stats());
    setenv("AKAM_VENC_RC", "stock", 1);
    clear_env();
    setenv("AKAM_VENC_RC", "stock", 1);
    imgq_load(NULL);
    CHECK(imgq_venc_rc_stock());
    clear_env();
    imgq_load(NULL);

    /* keys */
    CHECK(imgq_ctl_get("ae.speed", out, sizeof(out)) == -1);
    CHECK(imgq_ctl_set("ae.speed", "1", msg, sizeof(msg)) == -1);
    CHECK(imgq_ctl_get("img.venc_rc", out, sizeof(out)) == 0 && strstr(out, "img.venc_rc=legacy"));
    CHECK(imgq_ctl_get("img.wb", out, sizeof(out)) == 0 && !strcmp(out, "img.wb=manual"));
    CHECK(imgq_ctl_get("img.poll", out, sizeof(out)) == 0 && !strcmp(out, "img.poll=0"));
    CHECK(imgq_ctl_get("img.dump", out, sizeof(out)) == 0 && strstr(out, "write only"));
    g_lines = 0;
    imgq_ctl_list(collect, NULL);
    CHECK(g_lines == 5);

    CHECK(imgq_ctl_set("img.venc_rc", "stock", msg, sizeof(msg)) == 1 && strstr(msg, "AKAM_VENC_RC=stock"));
    CHECK(!imgq_venc_rc_stock());

    /* img.poll */
    CHECK(imgq_ctl_set("img.poll", "1", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.poll=1"));
    CHECK(imgq_poll_stats());
    CHECK(imgq_ctl_set("img.poll", "2", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.poll"));
    CHECK(imgq_poll_stats());
    CHECK(imgq_ctl_set("img.poll", "off", msg, sizeof(msg)) == 0 && !imgq_poll_stats());

    /* img.wb */
    g_night = 0;
    CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.wb=conf"));
    CHECK(g_wb_conf_calls == 1 && g_wb_conf_night == 0 && imgq_wb_conf());
    g_night = 1;
    CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0 && g_wb_conf_night == 1);
    CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 1 && strstr(msg, "day setting"));
    CHECK(g_wb_manual_calls == 0 && imgq_wb_conf());
    g_night = 0;
    CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.wb=manual"));
    CHECK(g_wb_manual_calls == 1 && !imgq_wb_conf());
    CHECK(imgq_ctl_set("img.wb", "auto", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.wb"));
    g_fail_wb_conf = 1;
    CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 1 && !imgq_wb_conf());
    g_fail_wb_conf = 0;

    /* img.reload */
    CHECK(imgq_ctl_set("img.reload", "0", msg, sizeof(msg)) == 1);
    CHECK(g_init_day == 0 && g_init_night == 0);
    g_night = 0;
    CHECK(imgq_ctl_set("img.reload", "1", msg, sizeof(msg)) == 0 && strstr(msg, "day calibration"));
    CHECK(g_init_day == 1 && g_init_night == 0 && g_resets == 1 && g_ae_sets == 1);
    CHECK(strstr(msg, "manual WB re-applied") && g_wb_manual_calls == 2);   /* wb is manual: kept */
    g_night = 1;
    CHECK(imgq_ctl_set("img.reload", "1", msg, sizeof(msg)) == 0 && strstr(msg, "night calibration"));
    CHECK(g_init_night == 1 && g_ae_sets == 2 && g_wb_manual_calls == 2);
    g_fail_init = 1;
    CHECK(imgq_ctl_set("img.reload", "1", msg, sizeof(msg)) == 1 && strstr(msg, "could not be re-sent"));
    CHECK(g_ae_sets == 2);
    g_fail_init = 0;

    /* img.dump: bad value, then a driver that reads nothing */
    CHECK(imgq_ctl_set("img.dump", "everything", msg, sizeof(msg)) == 1 && strstr(msg, "confirmed|all"));
    g_tunnel_n = 0;
    CHECK(imgq_ctl_set("img.dump", "confirmed", msg, sizeof(msg)) == 1 && strstr(msg, "no block could be read"));
    CHECK(g_tunnel_n == 5);                                 /* only the confirmed getters were tried */
    g_tunnel_n = 0;
    CHECK(imgq_ctl_set("img.dump", "all", msg, sizeof(msg)) == 1);
    CHECK(g_tunnel_n == 28);                                /* the guessed ones too */

    unlink(path);
    if (g_failed) { printf("imgq_test: %d failure(s)\n", g_failed); return 1; }
    printf("imgq_test: OK\n");
    return 0;
}
