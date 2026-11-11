/* Host test of src/ak_rtsp/video_cfg.c and video_ctl.c (saved video settings): defaults, strict parsing, ranges,
 * atomic save, the 8091 key contract, "restart to apply" and the network refusal. VIDEO_CFG_PATH is set by the
 * build line. Run: video_cfg_test TEMPDIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "video_cfg.h"
#include "video_ctl.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void parse(const char *t, video_cfg_t *c, int *bad)
{
    *bad = video_cfg_parse(t, strlen(t), c);
}

static int exists(void) { struct stat st; return stat(VIDEO_CFG_PATH, &st) == 0; }

static void emit(void *arg, const char *line)
{
    char *buf = arg;
    strcat(buf, line);
    strcat(buf, "|");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    (void)argv;
    video_cfg_t c;
    int bad;

    printf("defaults\n");
    video_cfg_defaults(&c);
    CHECK(c.bitrate == 1536 && c.fps == 15 && c.gop_s == 2, "defaults are 1536 kbps, 15 fps, 2 s");
    CHECK(video_fps() == 15 && video_kbps() == 1536 && video_gop_frames() == 30 && video_day_vts() == 2284,
          "active settings before any load are the old constants (30 frame GOP, VTS 2284)");
    CHECK(video_cfg_read(&c) == 1 && c.bitrate == 1536, "absent file: 1 and defaults");
    video_cfg_load();
    CHECK(video_gop_frames() == 30 && video_fps() == 15, "load without a file keeps the defaults");

    printf("parse\n");
    parse("", &c, &bad);
    CHECK(bad == 0 && c.fps == 15, "empty file: defaults, no error");
    parse("bitrate=800\nfps=10\ngop=3\n", &c, &bad);
    CHECK(bad == 0 && c.bitrate == 800 && c.fps == 10 && c.gop_s == 3, "all three keys");
    parse("fps=10\r\nbitrate=500\r\n", &c, &bad);
    CHECK(bad == 0 && c.fps == 10 && c.bitrate == 500, "CRLF tolerated");
    parse("bitrate=499\n", &c, &bad);
    CHECK(bad == 1 && c.bitrate == 1536, "bitrate below the range: skipped, default kept");
    parse("bitrate=3001\n", &c, &bad);
    CHECK(bad == 1 && c.bitrate == 1536, "bitrate above the range: skipped");
    parse("bitrate=3000\nbitrate=500\n", &c, &bad);
    CHECK(bad == 0 && c.bitrate == 500, "range limits are inclusive; the last line wins");
    parse("fps=12\n", &c, &bad);
    CHECK(bad == 1 && c.fps == 15, "fps 12 refused (no sensor timing pair for it)");
    parse("fps=5\nfps=30\nfps=0\n", &c, &bad);
    CHECK(bad == 3 && c.fps == 15, "fps 5, 30 and 0 refused");
    parse("gop=0\n", &c, &bad);
    CHECK(bad == 1 && c.gop_s == 2, "gop 0 refused");
    parse("gop=5\n", &c, &bad);
    CHECK(bad == 1 && c.gop_s == 2, "gop 5 refused");
    parse("bitrate=12x\nbitrate=-5\nbitrate=+600\nbitrate= 600\nbitrate=\nbitrate=0x400\n", &c, &bad);
    CHECK(bad == 6 && c.bitrate == 1536, "non-numeric, signed, spaced, empty and hex values refused");
    parse("flip=h\nresolution=1280x720\nwhat\n", &c, &bad);
    CHECK(bad == 3 && c.fps == 15 && c.bitrate == 1536, "flip, resolution (not offered) and a line without = are skipped");
    parse("bitrate=900\nfps=bad\ngop=4\n", &c, &bad);
    CHECK(bad == 1 && c.bitrate == 900 && c.gop_s == 4 && c.fps == 15, "one bad line does not affect the others");
    {
        char big[VIDEO_CFG_MAX + 40];
        memset(big, '#', sizeof(big));
        memcpy(big, "bitrate=900\n", 12);
        bad = video_cfg_parse(big, sizeof(big), &c);
        CHECK(bad == 1 && c.bitrate == 1536, "an oversized file gives the defaults");
    }

    printf("format\n");
    char out[VIDEO_CFG_MAX];
    video_cfg_defaults(&c);
    CHECK(video_cfg_format(&c, out, sizeof(out)) == 0 && out[0] == '\0', "defaults serialise to nothing");
    c.bitrate = 900;
    c.gop_s = 4;
    CHECK(video_cfg_format(&c, out, sizeof(out)) > 0 && !strcmp(out, "bitrate=900\ngop=4\n"), "only non-default keys are written");
    CHECK(video_cfg_format(&c, out, 5) == -1, "a buffer too small is reported");

    printf("8091 keys\n");
    char msg[160], line[80];
    video_ctl_set_persist(1);
    CHECK(video_ctl_set("ae.speed", "1", msg, sizeof(msg)) == -1, "a key of another module is not claimed");
    CHECK(video_ctl_get("video.bitrate", line, sizeof(line)) == -1, "the measured video.bitrate is not ours");
    CHECK(video_ctl_get("video.fps_actual", line, sizeof(line)) == -1, "the measured video.fps_actual is not ours");
    CHECK(video_ctl_get("video.bitrate_cfg", line, sizeof(line)) == 0 && !strcmp(line, "video.bitrate_cfg=1536"), "GET default bitrate");
    CHECK(video_ctl_set("video.bitrate_cfg", "800", msg, sizeof(msg)) == 0 && !strcmp(msg, "OK video.bitrate_cfg=800 (restart to apply)"),
          "SET bitrate: OK, says restart to apply");
    CHECK(exists(), "file written");
    {
        struct stat st;
        stat(VIDEO_CFG_PATH, &st);
        CHECK((st.st_mode & 0777) == 0600, "file mode 0600");
    }
    CHECK(video_ctl_get("video.bitrate_cfg", line, sizeof(line)) == 0 && !strcmp(line, "video.bitrate_cfg=800"), "GET returns the saved value");
    CHECK(video_ctl_get("video.cfg_pending", line, sizeof(line)) == 0 && !strcmp(line, "video.cfg_pending=1"), "pending while the running encoder differs");
    CHECK(video_ctl_set("video.fps_cfg", "10", msg, sizeof(msg)) == 0, "SET fps 10");
    CHECK(video_ctl_set("video.gop_cfg", "3", msg, sizeof(msg)) == 0, "SET gop 3");
    video_cfg_read(&c);
    CHECK(c.bitrate == 800 && c.fps == 10 && c.gop_s == 3, "the three keys are in the file together");
    {
        /* the same value again must not rewrite the file (atomic write = new inode) */
        struct stat s1, s2;
        stat(VIDEO_CFG_PATH, &s1);
        CHECK(video_ctl_set("video.gop_cfg", "3", msg, sizeof(msg)) == 0 && !strncmp(msg, "OK ", 3), "SET to the same value: OK");
        stat(VIDEO_CFG_PATH, &s2);
        CHECK(s1.st_ino == s2.st_ino && s1.st_mtime == s2.st_mtime, "an unchanged SET does not write the flash");
        CHECK(video_ctl_set("video.gop_cfg", "2", msg, sizeof(msg)) == 0, "a changed SET writes");
        stat(VIDEO_CFG_PATH, &s2);
        CHECK(s1.st_ino != s2.st_ino, "a changed value is a new file");
        CHECK(video_ctl_set("video.gop_cfg", "3", msg, sizeof(msg)) == 0, "back to 3");
    }
    CHECK(video_ctl_set("video.fps_cfg", "12", msg, sizeof(msg)) == 1 && !strncmp(msg, "ERR ", 4), "SET fps 12 refused");
    CHECK(video_ctl_set("video.bitrate_cfg", "100", msg, sizeof(msg)) == 1 && !strncmp(msg, "ERR ", 4), "SET bitrate 100 refused");
    CHECK(video_ctl_set("video.bitrate_cfg", "9e2", msg, sizeof(msg)) == 1, "SET bitrate 9e2 refused");
    CHECK(video_ctl_set("video.gop_cfg", "9", msg, sizeof(msg)) == 1, "SET gop 9 refused");
    video_cfg_read(&c);
    CHECK(c.bitrate == 800 && c.fps == 10 && c.gop_s == 3, "refused values leave the file alone");
    CHECK(video_ctl_set("video.cfg_pending", "0", msg, sizeof(msg)) == 1, "cfg_pending is read-only");
    video_ctl_set_persist(0);
    CHECK(video_ctl_set("video.bitrate_cfg", "1000", msg, sizeof(msg)) == 1 && strstr(msg, "from the camera itself"), "a network SET is refused");
    video_cfg_read(&c);
    CHECK(c.bitrate == 800, "and nothing was written");
    CHECK(video_ctl_get("video.fps_cfg", line, sizeof(line)) == 0 && !strcmp(line, "video.fps_cfg=10"), "a network GET works");
    video_ctl_set_persist(1);
    {
        char all[300] = "";
        video_ctl_list(emit, all);
        CHECK(!strcmp(all, "video.bitrate_cfg=800|video.fps_cfg=10|video.gop_cfg=3|video.cfg_pending=1|"), "LIST has the four keys");
    }

    printf("apply at start\n");
    video_cfg_reset_for_test();
    video_cfg_load();
    CHECK(video_kbps() == 800 && video_fps() == 10 && video_gop_frames() == 30 && video_day_vts() == 3426,
          "load: 800 kbps, 10 fps, GOP 3 s = 30 frames, VTS 3426");
    CHECK(video_ctl_get("video.cfg_pending", line, sizeof(line)) == 0 && !strcmp(line, "video.cfg_pending=0"), "nothing pending once loaded");
    CHECK(video_ctl_set("video.fps_cfg", "15", msg, sizeof(msg)) == 0 && strstr(msg, "restart to apply"), "a later SET is pending again");
    CHECK(video_fps() == 10, "the running values never change after the load");

    printf("back to defaults removes the file\n");
    video_ctl_set("video.bitrate_cfg", "1536", msg, sizeof(msg));
    video_ctl_set("video.gop_cfg", "2", msg, sizeof(msg));
    CHECK(video_ctl_set("video.fps_cfg", "15", msg, sizeof(msg)) == 0, "SET the last value back");
    CHECK(!exists(), "all defaults: no file");

    {
        FILE *f = fopen(VIDEO_CFG_PATH, "w");
        if (!f) return 2;
        fputs("fps=7\nbitrate=700\n", f);
        fclose(f);
        video_cfg_reset_for_test();
        video_cfg_load();
        CHECK(video_fps() == 15 && video_kbps() == 700, "a bad line at start: that key keeps the default, the others apply");
        unlink(VIDEO_CFG_PATH);
        video_cfg_reset_for_test();
    }

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
