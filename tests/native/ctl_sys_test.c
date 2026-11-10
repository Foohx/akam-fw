/* Host test of src/ak_rtsp/ctl_sys.c on a fake /proc and /sys tree and a fake clock. No camera, no network
 * interface needed (the test points the module at an interface that does not exist). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ctl_sys.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static char g_root[256];
static int64_t g_now;
static int64_t fake_clock(void) { return g_now; }

static void put(const char *rel, const char *text)
{
    char path[512], dir[512];
    snprintf(path, sizeof(path), "%s%s", g_root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + strlen(g_root) + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void del(const char *rel)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s", g_root, rel);
    unlink(path);
}

static const char *get(const char *key)
{
    static char v[8][64];
    static int i;
    char *o = v[i++ & 7];
    if (ctl_sys_get(key, o, 64) != 0) return "<notkey>";
    return o;
}
#define IS(key, want) (strcmp(get(key), (want)) == 0)

static void stat_file(unsigned long user, unsigned long idle, unsigned long iowait)
{
    char b[256];
    snprintf(b, sizeof(b), "cpu  %lu 0 0 %lu %lu 0 0 0 0 0\ncpu0 1 2 3 4 5 6 7 8 9 10\nintr 1\n", user, idle, iowait);
    put("/proc/stat", b);
}

static int g_emitted;
static int g_emit_unknown;
static void emit(const char *k, const char *v, void *u)
{
    (void)k; (void)u;
    g_emitted++;
    if (!strcmp(v, "unknown")) g_emit_unknown++;
}

int main(void)
{
    char tmpl[] = "/tmp/ctl_sys_test.XXXXXX";
    if (!mkdtemp(tmpl)) return 2;
    snprintf(g_root, sizeof(g_root), "%s", tmpl);

    ctl_sys_set_root(g_root);
    ctl_sys_set_ifname("nosuch0");
    ctl_sys_set_clock(fake_clock);
    ctl_sys_reset();
    g_now = 10000;

    /* --- nothing exists: every key is unknown (counters aside) --- */
    CHECK(IS("sys.uptime", "unknown"));
    CHECK(IS("sys.load", "unknown"));
    CHECK(IS("sys.cpu", "unknown"));
    CHECK(IS("sys.mem_total", "unknown"));
    CHECK(IS("sys.mem_free", "unknown"));
    CHECK(IS("sys.temp", "unknown"));
    CHECK(IS("net.ssid", "unknown"));
    CHECK(IS("net.wifi_rssi", "unknown"));
    CHECK(IS("net.wifi_rate", "unknown"));
    CHECK(IS("net.ip", "unknown"));
    CHECK(IS("net.mac", "unknown"));
    CHECK(IS("video.fps_actual", "unknown"));
    CHECK(IS("video.bitrate", "0"));
    CHECK(IS("rtsp.clients", "0"));
    CHECK(IS("rtsp.auth", "unknown"));

    /* --- unknown keys --- */
    char b[64];
    CHECK(ctl_sys_get("ae.speed", b, sizeof(b)) == -1);
    CHECK(ctl_sys_get("sys.hostname", b, sizeof(b)) == -1);
    CHECK(ctl_sys_get("sys.", b, sizeof(b)) == -1);
    CHECK(ctl_sys_get("", b, sizeof(b)) == -1);
    CHECK(ctl_sys_get(NULL, b, sizeof(b)) == -1);

    /* --- fake files, first sample --- */
    put("/proc/uptime", "86400.55 12345.00\n");
    put("/proc/loadavg", "0.12 0.10 0.05 1/60 1234\n");
    put("/proc/meminfo", "MemTotal:       57344 kB\nMemFree:        10000 kB\nMemAvailable:   21000 kB\nBuffers: 500 kB\nCached: 4000 kB\n");
    stat_file(1000, 9000, 0);
    put("/sys/class/thermal/thermal_zone0/temp", "45300\n");
    put("/proc/net/wireless",
        "Inter-| sta-|   Quality        |   Discarded packets               | Missing\n"
        " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon\n"
        "nosuch0: 0000   70.  -58.  -256        0      0      0      0      0        0\n");
    ctl_sys_set_root(g_root);  /* a sensor that appears later is only probed every 30 s: restart the probe */
    g_now = 20000;  /* the cache of the empty run is older than 1 s */
    CHECK(IS("sys.uptime", "86400"));
    CHECK(IS("sys.load", "0.12 0.10 0.05"));
    CHECK(IS("sys.mem_total", "57344"));
    CHECK(IS("sys.mem_free", "21000"));          /* MemAvailable wins */
    CHECK(IS("sys.cpu", "unknown"));             /* first sample: no interval yet */
    CHECK(IS("sys.temp", "45.3"));
    CHECK(IS("net.wifi_rssi", "-58"));

    /* --- cache: files change, within 1 s nothing moves --- */
    put("/proc/uptime", "86401.55 1.00\n");
    stat_file(1500, 9500, 0);
    g_now = 20999;
    CHECK(IS("sys.uptime", "86400"));
    CHECK(IS("sys.cpu", "unknown"));
    g_now = 21000;                                /* exactly 1 s: refresh */
    CHECK(IS("sys.uptime", "86401"));
    /* delta: busy 500, total 1000 -> 50 % */
    CHECK(IS("sys.cpu", "50"));

    /* --- cpu: iowait counts as idle; delta total 0 keeps unknown rather than inventing --- */
    stat_file(1500 + 100, 9500 + 100, 200);       /* busy +100 of total +400 -> 25 % */
    g_now = 22000;
    CHECK(IS("sys.cpu", "25"));
    g_now = 23000;                                /* identical counters: dt = 0 */
    CHECK(IS("sys.cpu", "unknown"));
    stat_file(1000, 9000, 0);                     /* counters went backwards (reboot): no value, rebaseline */
    g_now = 24000;
    CHECK(IS("sys.cpu", "unknown"));
    stat_file(1100, 9100, 0);
    g_now = 25000;
    CHECK(IS("sys.cpu", "50"));

    /* --- memory fallback when MemAvailable is missing --- */
    put("/proc/meminfo", "MemTotal:       57344 kB\nMemFree:        10000 kB\nBuffers: 500 kB\nCached: 4000 kB\n");
    g_now = 26000;
    CHECK(IS("sys.mem_free", "14500"));
    put("/proc/meminfo", "MemTotal:       57344 kB\nMemFree:        10000 kB\n");   /* cannot compute it */
    g_now = 27000;
    CHECK(IS("sys.mem_free", "unknown"));
    CHECK(IS("sys.mem_total", "57344"));

    /* --- temperature: thermal zone absent, hwmon used, absent again, out of range --- */
    del("/sys/class/thermal/thermal_zone0/temp");
    put("/sys/class/hwmon/hwmon1/temp1_input", "51250\n");
    g_now = 28000;
    CHECK(IS("sys.temp", "51.3"));
    del("/sys/class/hwmon/hwmon1/temp1_input");
    g_now = 29000;
    CHECK(IS("sys.temp", "unknown"));
    /* no sensor: the probe is not repeated every second */
    put("/sys/class/thermal/thermal_zone0/temp", "999999\n");
    g_now = 30000;
    CHECK(IS("sys.temp", "unknown"));
    put("/sys/class/thermal/thermal_zone0/temp", "-5000\n");
    g_now = 31000;
    CHECK(IS("sys.temp", "unknown"));
    g_now = 59500;
    CHECK(IS("sys.temp", "-5.0"));
    /* remembered sensor keeps being read, and a vanished one is probed again */
    put("/sys/class/thermal/thermal_zone0/temp", "40000\n");
    g_now = 60600;
    CHECK(IS("sys.temp", "40.0"));
    del("/sys/class/thermal/thermal_zone0/temp");
    g_now = 61700;
    CHECK(IS("sys.temp", "unknown"));
    put("/sys/class/thermal/thermal_zone0/temp", "-5000\n");
    g_now = 92000;
    CHECK(IS("sys.temp", "-5.0"));

    /* --- wireless: level that is not certainly dBm is not taken --- */
    put("/proc/net/wireless",
        "Inter-| sta-|   Quality        |   Discarded packets               | Missing\n"
        " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon\n"
        "nosuch0: 0000   70   200  0        0      0      0      0      0        0\n");
    g_now = 32000;
    CHECK(IS("net.wifi_rssi", "unknown"));
    put("/proc/net/wireless", "nosuch0: 0000  70.  -71  -256 0 0 0 0 0 0\n");
    g_now = 33000;
    CHECK(IS("net.wifi_rssi", "-71"));
    put("/proc/net/wireless", "wlan1: 0000  70.  -40.  -256 0 0 0 0 0 0\n");   /* another interface */
    g_now = 34000;
    CHECK(IS("net.wifi_rssi", "unknown"));

    /* --- garbage in files --- */
    put("/proc/uptime", "abc\n");
    put("/proc/loadavg", "x y z\n");
    g_now = 35000;
    CHECK(IS("sys.uptime", "unknown"));
    CHECK(IS("sys.load", "unknown"));

    /* --- rtsp counters, auth, stream measurement --- */
    ctl_sys_note_rtsp_client(1);
    ctl_sys_note_rtsp_auth(1);
    g_now = 36000;
    CHECK(IS("rtsp.clients", "1"));
    CHECK(IS("rtsp.auth", "on"));
    ctl_sys_note_rtsp_auth(2);
    g_now = 37000;
    CHECK(IS("rtsp.auth", "broken"));
    ctl_sys_note_rtsp_auth(0);
    g_now = 38000;
    CHECK(IS("rtsp.auth", "off"));
    ctl_sys_note_rtsp_client(-1);
    g_now = 39000;
    CHECK(IS("rtsp.clients", "0"));

    ctl_sys_note_video_frame(1000);               /* before play: ignored by the window (edge resets it) */
    ctl_sys_note_rtsp_play(1);
    g_now = 40000;
    CHECK(IS("video.fps_actual", "unknown"));     /* baseline only */
    CHECK(IS("video.bitrate", "unknown"));
    for (int i = 0; i < 20; i++) ctl_sys_note_video_frame(12500);   /* 20 frames, 250000 B */
    g_now = 41000;                                /* 20 frames in 1 s, 2000 kbit/s */
    CHECK(IS("video.fps_actual", "200"));
    CHECK(IS("video.bitrate", "2000"));
    for (int i = 0; i < 15; i++) ctl_sys_note_video_frame(10000);
    g_now = 42000;                                /* window now spans 2 s: 35 frames, 400000 B */
    CHECK(IS("video.fps_actual", "175"));
    CHECK(IS("video.bitrate", "1600"));
    g_now = 52000;                                /* idle for 10 s: the 5 s window keeps only newer points */
    CHECK(IS("video.fps_actual", "0"));           /* nothing newer than 5 s: the newest older sample, 0 frames in 10 s */
    ctl_sys_note_rtsp_play(0);
    g_now = 53000;
    CHECK(IS("video.fps_actual", "unknown"));
    CHECK(IS("video.bitrate", "0"));

    /* --- list: every key once, same count as ctl_sys_count --- */
    g_now = 60000;
    ctl_sys_list(emit, NULL);
    CHECK(g_emitted == (int)ctl_sys_count());
    CHECK(ctl_sys_count() == 15);
    CHECK(ctl_sys_key(0) && !strcmp(ctl_sys_key(0), "sys.uptime"));
    CHECK(ctl_sys_key(15) == NULL);
    CHECK(g_emit_unknown >= 1);

    /* --- small output buffer truncates, does not overflow --- */
    char tiny[4];
    CHECK(ctl_sys_get("sys.mem_total", tiny, sizeof(tiny)) == 0);
    CHECK(strlen(tiny) == 3);

    /* real clock and real /proc must not crash (values not checked) */
    ctl_sys_set_clock(NULL);
    ctl_sys_set_root("");
    ctl_sys_reset();
    CHECK(ctl_sys_get("sys.uptime", b, sizeof(b)) == 0);

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
    if (system(cmd) != 0) { /* best effort */ }

    if (g_failed) { printf("ctl_sys_test: %d FAILED\n", g_failed); return 1; }
    printf("ctl_sys_test: OK\n");
    return 0;
}
