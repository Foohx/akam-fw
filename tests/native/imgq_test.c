/* Host test of src/ak_rtsp/imgq.c: start-up options (file, environment, precedence), the img.* keys and
 * their calls into the ISP layer (stubs here), with no camera. */
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ae.h"
#include "imgq.h"
#include "isp.h"
#include "isp_nr.h"
#include "night.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

/* ---- stubs of the device layer ---- */
static int g_night;
static int g_wb_conf_calls, g_wb_conf_night, g_wb_manual_calls, g_init_day, g_init_night, g_resets, g_ae_sets;
static int g_fail_wb_conf, g_fail_init;
static int g_tunnel_cmds[64], g_tunnel_n;

static int g_lock_depth, g_lock_total, g_ready = 1;
void isp_cfg_lock(void)   { CHECK(g_lock_depth == 0); g_lock_depth++; g_lock_total++; }
void isp_cfg_unlock(void) { CHECK(g_lock_depth == 1); g_lock_depth--; }
int  isp_pipeline_ready(void) { return g_ready; }
static int g_sharp_all_v, g_sharp_all_calls;
void isp_set_sharp_all(int a) { g_sharp_all_v = a; g_sharp_all_calls++; }
int  isp_get_sharp_all(void) { return g_sharp_all_v; }
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

/* NR knobs: record only, the composers are tested by isp_nr_test.c */
static int g_nr_vals[8] = { 0, 0, 0, 0, 100, 100, 100, 100 }, g_nr_calls, g_nr_rc = 1, g_nr_dumps;
int  isp_nr_knob_set(int k, int v) { g_nr_calls++; if (g_nr_rc < 0) return g_nr_rc; g_nr_vals[k] = v; return g_nr_rc; }
int  isp_nr_knob_get(int k) { return g_nr_vals[k]; }
int  isp_nr_status(char *out, size_t cap) { snprintf(out, cap, "img.nr=stub"); return 0; }
int  isp_nr_dump_live(void) { g_nr_dumps++; return 5; }

static int g_qp_calls, g_qp_a, g_qp_b, g_qp_fail;
int  venc_set_qp_live(int a, int b) { g_qp_calls++; g_qp_a = a; g_qp_b = b; if (g_qp_fail) { errno = EIO; return -1; } return 0; }

/* ---- helpers ---- */
static void clear_env(void)
{
    unsetenv("AKAM_VENC_RC");
    unsetenv("AKAM_IMG_WB");
    unsetenv("AKAM_IMG_POLL");
    unsetenv("AKAM_VENC_IPDELTA");
}

static void write_conf(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static int read_conf(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
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
    imgq_load(NULL, NULL);
    CHECK(!imgq_venc_rc_stock() && imgq_wb_conf() && !imgq_poll_stats());   /* wb=conf is the default */
    imgq_load("/nonexistent/image", "/nonexistent/ram");
    CHECK(!imgq_venc_rc_stock() && imgq_wb_conf() && !imgq_poll_stats());

    /* file: comments, blanks, spaces, unknown keys and bad values ignored */
    write_conf(path,
               "# comment\n\n  venc_rc = stock\nwb=conf\npoll=on\nunknown=1\nvenc_rc_x=stock\n   # indented comment\n");
    imgq_load(path, NULL);
    CHECK(imgq_venc_rc_stock() && imgq_wb_conf() && imgq_poll_stats());
    write_conf(path, "venc_rc=bogus\nwb=\npoll=maybe\n");
    imgq_load(path, NULL);
    CHECK(!imgq_venc_rc_stock() && imgq_wb_conf() && !imgq_poll_stats());

    /* the RAM file is read after the saved file and wins over it */
    char ram[] = "/tmp/imgq_test_ram_XXXXXX";
    int rfd = mkstemp(ram);
    assert(rfd >= 0);
    close(rfd);
    write_conf(path, "venc_rc=stock\nwb=conf\npoll=1\n");
    write_conf(ram, "wb=manual\nvenc_rc=legacy\n");
    imgq_load(path, ram);
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && imgq_poll_stats());
    unlink(ram);

    /* environment wins over the file */
    write_conf(path, "venc_rc=stock\nwb=conf\npoll=1\n");
    setenv("AKAM_VENC_RC", "legacy", 1);
    setenv("AKAM_IMG_WB", "manual", 1);
    setenv("AKAM_IMG_POLL", "0", 1);
    imgq_load(path, NULL);
    CHECK(!imgq_venc_rc_stock() && !imgq_wb_conf() && !imgq_poll_stats());
    setenv("AKAM_VENC_RC", "stock", 1);
    clear_env();
    setenv("AKAM_VENC_RC", "stock", 1);
    imgq_load(NULL, NULL);
    CHECK(imgq_venc_rc_stock());
    clear_env();
    imgq_load(NULL, NULL);

    /* keys */
    CHECK(imgq_ctl_get("ae.speed", out, sizeof(out)) == -1);
    CHECK(imgq_ctl_set("ae.speed", "1", msg, sizeof(msg)) == -1);
    CHECK(imgq_ctl_get("img.venc_rc", out, sizeof(out)) == 0 && strstr(out, "img.venc_rc=legacy"));
    CHECK(imgq_ctl_get("img.wb", out, sizeof(out)) == 0 && !strcmp(out, "img.wb=conf"));
    CHECK(imgq_ctl_get("img.poll", out, sizeof(out)) == 0 && !strcmp(out, "img.poll=0"));
    CHECK(imgq_ctl_get("img.dump", out, sizeof(out)) == 0 && strstr(out, "write only"));
    g_lines = 0;
    imgq_ctl_list(collect, NULL);
    CHECK(g_lines == 16);

    CHECK(imgq_ctl_set("img.venc_rc", "stock", msg, sizeof(msg)) == 1 && strstr(msg, "AKAM_VENC_RC=stock"));
    CHECK(!imgq_venc_rc_stock());

    /* ipdelta: start-up option only, -12..12, default 0 */
    CHECK(imgq_venc_ipdelta() == 0);
    write_conf(path, "ipdelta=3\n");
    imgq_load(path, NULL);
    CHECK(imgq_venc_ipdelta() == 3);
    write_conf(path, "ipdelta=13\n");
    imgq_load(path, NULL);
    CHECK(imgq_venc_ipdelta() == 0);
    write_conf(path, "ipdelta=-12\n");
    imgq_load(path, NULL);
    CHECK(imgq_venc_ipdelta() == -12);
    write_conf(path, "ipdelta=2x\n");
    imgq_load(path, NULL);
    CHECK(imgq_venc_ipdelta() == 0);
    setenv("AKAM_VENC_IPDELTA", "-4", 1);
    imgq_load(NULL, NULL);
    CHECK(imgq_venc_ipdelta() == -4);
    clear_env();
    imgq_load(NULL, NULL);
    CHECK(imgq_venc_ipdelta() == 0);

    /* img.qp: live QP bounds, validated before the ioctl, never saved */
    CHECK(imgq_ctl_get("img.qp", out, sizeof(out)) == 0 && strstr(out, "img.qp=unchanged"));
    CHECK(imgq_ctl_set("img.qp", "22 40", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.qp=22 40 (live, not saved)"));
    CHECK(g_qp_calls == 1 && g_qp_a == 22 && g_qp_b == 40);
    CHECK(imgq_ctl_get("img.qp", out, sizeof(out)) == 0 && strstr(out, "img.qp=22 40"));
    CHECK(imgq_ctl_set("img.qp", "28,43", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.qp=28 43 (live, not saved)"));   /* MIN,MAX */
    CHECK(imgq_ctl_set("img.qp", "28 43", msg, sizeof(msg)) == 0 && g_qp_a == 28 && g_qp_b == 43);
    CHECK(imgq_ctl_set("img.qp", "22,40", msg, sizeof(msg)) == 0 && g_qp_a == 22 && g_qp_b == 40);                       /* back */
    {
        static const char *const bad[] = { "", "22", "22 40 5", "9 40", "22 52", "40 40", "41 40", "a b", "22,", "22 40x", "-1 40", " " };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            CHECK(imgq_ctl_set("img.qp", bad[i], msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.qp"));
        CHECK(g_qp_calls == 4); /* no refused value reached the ioctl */
        g_qp_fail = 1;
        CHECK(imgq_ctl_set("img.qp", "20 45", msg, sizeof(msg)) == 1 && strstr(msg, "refused by the encoder"));
        CHECK(imgq_ctl_get("img.qp", out, sizeof(out)) == 0 && strstr(out, "img.qp=22 40")); /* unchanged on refusal */
        g_qp_fail = 0;
    }

    /* img.sharp_all: needs the pipeline, 0|1 */
    CHECK(imgq_ctl_get("img.sharp_all", out, sizeof(out)) == 0 && !strcmp(out, "img.sharp_all=0"));
    CHECK(imgq_ctl_set("img.sharp_all", "2", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.sharp_all"));
    CHECK(imgq_ctl_set("img.sharp_all", "1", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.sharp_all=1 (RAM only)"));
    CHECK(g_sharp_all_v == 1 && g_sharp_all_calls == 1);

    /* img.poll */
    CHECK(imgq_ctl_set("img.poll", "1", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.poll=1"));
    CHECK(imgq_poll_stats());
    CHECK(imgq_ctl_set("img.poll", "2", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.poll"));
    CHECK(imgq_poll_stats());
    CHECK(imgq_ctl_set("img.poll", "off", msg, sizeof(msg)) == 0 && !imgq_poll_stats());

    /* ISP not initialised yet (the 8091 server starts first): the ISP keys refuse, nothing is sent */
    g_ready = 0;
    g_lock_total = 0; /* the img.sharp_all check above took the lock once */
    CHECK(imgq_ctl_set("img.sharp_all", "0", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised") && g_sharp_all_calls == 1);
    CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised"));
    CHECK(imgq_ctl_set("img.reload", "1", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised"));
    CHECK(imgq_ctl_set("img.dump", "all", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised"));
    CHECK(g_wb_conf_calls == 0 && g_init_day == 0 && g_tunnel_n == 0 && g_lock_total == 0);
    g_ready = 1;

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

    /* after a day / night switch (night.c): wb=manual puts the manual block back after the day reload, wb=conf
     * and the night reload leave the file's block alone */
    int manual_before = g_wb_manual_calls;
    CHECK(!imgq_wb_conf());                                  /* the img.wb sequence above ended on manual */
    imgq_after_mode_switch(1);
    CHECK(g_wb_manual_calls == manual_before);
    imgq_after_mode_switch(0);
    CHECK(g_wb_manual_calls == manual_before + 1);
    g_night = 0;
    CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0 && imgq_wb_conf());
    imgq_after_mode_switch(0);
    imgq_after_mode_switch(1);
    CHECK(g_wb_manual_calls == manual_before + 1);

    /* img.wb is saved in the conf file (other lines kept), only when the value changes */
    {
        char sp[] = "/tmp/imgq_test_save_XXXXXX";
        int sfd = mkstemp(sp);
        assert(sfd >= 0);
        close(sfd);
        char buf[512];
        struct stat st;

        clear_env();
        unlink(sp);                                                      /* absent file = defaults */
        imgq_load(sp, NULL);
        CHECK(imgq_wb_conf() && stat(sp, &st) != 0);
        CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0 && stat(sp, &st) != 0);  /* no change: no file */
        CHECK(imgq_ctl_set("img.wb", "auto", msg, sizeof(msg)) == 1 && stat(sp, &st) != 0);  /* refused: no file */
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.wb=manual"));
        CHECK(stat(sp, &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "wb=manual\n"));
        imgq_load(sp, NULL);                                             /* survives a restart */
        CHECK(!imgq_wb_conf());

        write_conf(sp, "# mine\nvenc_rc=legacy\n  wb = manual\npoll=1\n");   /* other lines kept, wb replaced in place */
        imgq_load(sp, NULL);
        CHECK(!imgq_wb_conf());
        CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0 && imgq_wb_conf());
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "# mine\nvenc_rc=legacy\nwb=conf\npoll=1\n"));
        imgq_load(sp, NULL);
        CHECK(imgq_wb_conf() && imgq_poll_stats());

        write_conf(sp, "poll=1\n");                                      /* appended; conf alone adds nothing */
        imgq_load(sp, NULL);
        CHECK(imgq_ctl_set("img.wb", "conf", msg, sizeof(msg)) == 0);
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "poll=1\n"));
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0);
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "poll=1\nwb=manual\n"));

        /* no trailing newline, CRLF and a duplicate wb line (the last one wins at load, one line is written) */
        write_conf(sp, "poll=1");
        imgq_load(sp, NULL);
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0);
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "poll=1\nwb=manual\n"));
        write_conf(sp, "poll=1\r\nwb=manual\r\nwb=conf\r\n");
        imgq_load(sp, NULL);
        CHECK(imgq_wb_conf());
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0);
        CHECK(read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "poll=1\r\nwb=manual\n"));
        imgq_load(sp, NULL);
        CHECK(!imgq_wb_conf() && imgq_poll_stats());

        /* a network SET (persist off) applies the value but never writes the file */
        write_conf(sp, "poll=1\n");
        imgq_load(sp, NULL);
        imgq_set_persist(0);
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK img.wb=manual"));
        imgq_set_persist(1);
        CHECK(!imgq_wb_conf() && read_conf(sp, buf, sizeof(buf)) && !strcmp(buf, "poll=1\n"));

        /* an invalid saved value is ignored: default conf */
        write_conf(sp, "wb=purple\n");
        imgq_load(sp, NULL);
        CHECK(imgq_wb_conf());

        /* a failed save is reported in the answer, the value still applies */
        imgq_load("/nonexistent_dir_imgq/sub/image", NULL);
        CHECK(imgq_ctl_set("img.wb", "manual", msg, sizeof(msg)) == 0 && strstr(msg, "OK img.wb=manual (not saved:") &&
              !imgq_wb_conf());
        imgq_load(NULL, NULL);
        unlink(sp);
    }

    /* NR knobs: ranges, answers, not ready, refusal, failure, GET img.nr, SET img.dump nr */
    {
        char o[400];
        g_ready = 1;
        CHECK(imgq_ctl_get("img.nr2d_bias", o, sizeof(o)) == 0 && !strcmp(o, "img.nr2d_bias=0 (RAM only)"));
        CHECK(imgq_ctl_get("img.nr2d", o, sizeof(o)) == 0 && !strcmp(o, "img.nr2d=100 (RAM only)"));
        CHECK(imgq_ctl_set("img.nr2d_bias", "5", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid img.nr2d_bias value 5 (want 0..4)"));
        CHECK(imgq_ctl_set("img.nr2d_bias", "-1", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid"));
        CHECK(imgq_ctl_set("img.nr2d", "49", msg, sizeof(msg)) == 1 && strstr(msg, "50..400"));
        CHECK(imgq_ctl_set("img.nr2d", "401", msg, sizeof(msg)) == 1);
        CHECK(imgq_ctl_set("img.uvnr", "abc", msg, sizeof(msg)) == 1 && strstr(msg, "ERR invalid"));
        CHECK(imgq_ctl_set("img.sharp_coring", "99", msg, sizeof(msg)) == 1 && strstr(msg, "100..300"));
        CHECK(imgq_ctl_set("img.sharp_coring", "301", msg, sizeof(msg)) == 1);
        CHECK(imgq_ctl_set("img.nr3d_motion", "201", msg, sizeof(msg)) == 1 && strstr(msg, "50..200"));
        CHECK(g_nr_calls == 0);                                  /* invalid values never reach the ISP layer */
        CHECK(imgq_ctl_set("img.nr2d_bias", "2", msg, sizeof(msg)) == 0 &&
              !strcmp(msg, "OK img.nr2d_bias=2 (RAM only, NR1+NR2 re-sent)") && g_nr_vals[0] == 2);
        CHECK(imgq_ctl_get("img.nr2d_bias", o, sizeof(o)) == 0 && !strcmp(o, "img.nr2d_bias=2 (RAM only)"));
        CHECK(imgq_ctl_set("img.nr3d_motion", "150", msg, sizeof(msg)) == 0 && strstr(msg, "3D_NR re-sent") && g_nr_vals[7] == 150);
        g_nr_rc = 0;
        CHECK(imgq_ctl_set("img.nr2d_bias", "0", msg, sizeof(msg)) == 0 && strstr(msg, "default: nothing sent"));
        g_nr_rc = ISP_NR_REFUSED;
        CHECK(imgq_ctl_set("img.uvnr", "200", msg, sizeof(msg)) == 1 && strstr(msg, "ERR img.uvnr: live table not as decoded") && g_nr_vals[5] == 100);
        g_nr_rc = -1;
        errno = 0;
        CHECK(imgq_ctl_set("img.uvnr_bias", "1", msg, sizeof(msg)) == 1 && strstr(msg, "ERR img.uvnr_bias: the ISP refused"));
        g_nr_rc = 1;
        g_ready = 0;
        int calls = g_nr_calls;
        CHECK(imgq_ctl_set("img.nr2d", "200", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised") && g_nr_calls == calls);
        CHECK(imgq_ctl_get("img.nr", o, sizeof(o)) == 0 && strstr(o, "not initialised"));
        CHECK(imgq_ctl_set("img.dump", "nr", msg, sizeof(msg)) == 1 && strstr(msg, "not initialised"));
        g_ready = 1;
        CHECK(imgq_ctl_get("img.nr", o, sizeof(o)) == 0 && !strcmp(o, "img.nr=stub"));
        CHECK(imgq_ctl_set("img.nr", "1", msg, sizeof(msg)) == 1 && strstr(msg, "read-only"));
        CHECK(imgq_ctl_set("img.dump", "nr", msg, sizeof(msg)) == 0 && strstr(msg, "OK img.dump=nr read=5") && g_nr_dumps == 1);
    }

    CHECK(g_lock_depth == 0 && g_lock_total > 0);            /* every ISP key took and released the lock */
    unlink(path);
    if (g_failed) { printf("imgq_test: %d failure(s)\n", g_failed); return 1; }
    printf("imgq_test: OK\n");
    return 0;
}
