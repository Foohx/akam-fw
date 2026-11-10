/* Read-only metrics of the control port, see ctl_sys.h. */
#include "ctl_sys.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Cold code (runs once a second at most): optimise for size, the firmware has a size budget. */
#if defined(__clang__)
#define AK_SMALL __attribute__((minsize))
#else
#define AK_SMALL
#endif

/* Wireless extensions, inlined: linux/wireless.h is not guaranteed in the sysroot. */
#define AK_SIOCGIWSTATS 0x8B0F
#define AK_SIOCGIWESSID 0x8B1B
#define AK_SIOCGIWRATE  0x8B21
#define AK_IW_QUAL_LEVEL_INVALID 0x20
#define AK_IW_QUAL_DBM           0x08
struct ak_iwreq {
    char iw_name[IFNAMSIZ];
    union {
        struct { void *pointer; uint16_t length; uint16_t flags; } data;
        struct { int32_t value; uint8_t fixed, disabled, flags; } param;
        char pad[16];
    } u;
};

enum {
    K_UPTIME, K_LOAD, K_CPU, K_MEM_TOTAL, K_MEM_FREE, K_TEMP, K_SSID, K_RSSI, K_RATE, K_IP, K_MAC,
    K_FPS, K_BITRATE, K_CLIENTS, K_AUTH, K_COUNT
};
static const char *const g_keys[K_COUNT] = {
    "sys.uptime", "sys.load", "sys.cpu", "sys.mem_total", "sys.mem_free", "sys.temp",
    "net.ssid", "net.wifi_rssi", "net.wifi_rate", "net.ip", "net.mac",
    "video.fps_actual", "video.bitrate", "rtsp.clients", "rtsp.auth"
};

#define VAL_MAX 48
#define CACHE_MS 1000
#define WINDOW_MS 5000
#define SNAPS 8
#define TEMP_RESCAN_MS 30000

typedef struct { int64_t t_ms; uint32_t frames, bytes; } snap_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char     g_root[128] = "";
static char     g_ifname[IFNAMSIZ] = "";
static int64_t (*g_clock)(void);
static char     g_val[K_COUNT][VAL_MAX];
static int      g_have;                 /* cache holds a collection */
static int64_t  g_last_ms;
static char     g_temp_path[96];        /* sensor found by the last probe */
static int64_t  g_temp_rescan_ms;       /* no sensor: next probe at this time */
static int      g_cpu_have;             /* previous /proc/stat sample exists */
static uint64_t g_cpu_total, g_cpu_busy;
static snap_t   g_snap[SNAPS];
static int      g_nsnap, g_snap_head;

/* Written by the RTSP thread, read under g_mu. */
static volatile uint32_t g_frames, g_bytes;
static volatile int32_t  g_clients;
static volatile int      g_playing, g_play_edge, g_auth = -1;

static int64_t now_ms(void)
{
    if (g_clock) return g_clock();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Whole small file into buf (NUL-terminated), length or -1. */
static int read_file(const char *rel, char *buf, size_t cap)
{
    char path[192];
    snprintf(path, sizeof(path), "%s%s", g_root, rel);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static void unk(int k) { strcpy(g_val[k], "unknown"); }

/* Value of "Key:" at the start of a line, in kB, or -1. */
static long long meminfo_kb(const char *txt, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = txt; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            const char *v = p + kl + 1;
            while (*v == ' ' || *v == '\t') v++;
            if (!isdigit((unsigned char)*v)) return -1;
            return strtoll(v, NULL, 10);
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

/* Tenths of a degree from a millidegree file, 1 when valid. */
static int temp_read(const char *rel)
{
    char t[32], *e;
    if (read_file(rel, t, sizeof(t)) <= 0) return 0;
    long mc = strtol(t, &e, 10);
    if (e == t || mc < -50000 || mc > 150000) return 0;
    long d = (mc >= 0 ? mc + 50 : mc - 50) / 100;
    snprintf(g_val[K_TEMP], VAL_MAX, "%s%ld.%ld", (d < 0 && d > -10) ? "-" : "", d / 10, labs(d % 10));
    return 1;
}

/* The sensor path is remembered; with no sensor the probe (up to 40 opens)
 * is retried only every TEMP_RESCAN_MS, not once a second. */
AK_SMALL static void collect_temp(int64_t t)
{
    unk(K_TEMP);
    if (g_temp_path[0]) {
        if (temp_read(g_temp_path)) return;
        g_temp_path[0] = 0;
    } else if (g_temp_rescan_ms && t >= g_temp_rescan_ms - TEMP_RESCAN_MS && t < g_temp_rescan_ms) {
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++) {
            for (int j = (pass ? 1 : 0); j < (pass ? 5 : 1); j++) {
                char rel[96];
                if (pass == 0) snprintf(rel, sizeof(rel), "/sys/class/thermal/thermal_zone%d/temp", i);
                else snprintf(rel, sizeof(rel), "/sys/class/hwmon/hwmon%d/temp%d_input", i, j);
                if (!temp_read(rel)) continue;
                snprintf(g_temp_path, sizeof(g_temp_path), "%s", rel);
                return;
            }
        }
    }
    g_temp_rescan_ms = t + TEMP_RESCAN_MS;
}

AK_SMALL static void collect_proc(int64_t t)
{
    char b[1024];

    if (read_file("/proc/uptime", b, sizeof(b)) > 0 && isdigit((unsigned char)b[0]))
        snprintf(g_val[K_UPTIME], VAL_MAX, "%lu", strtoul(b, NULL, 10));
    else unk(K_UPTIME);

    unk(K_LOAD);
    if (read_file("/proc/loadavg", b, sizeof(b)) > 0) {
        char l1[16], l2[16], l3[16];
        if (sscanf(b, "%15[0-9.] %15[0-9.] %15[0-9.]", l1, l2, l3) == 3)
            snprintf(g_val[K_LOAD], VAL_MAX, "%s %s %s", l1, l2, l3);
    }

    long long tot = -1, mfree = -1, avail = -1, buf = -1, cached = -1;
    if (read_file("/proc/meminfo", b, sizeof(b)) > 0) {
        tot = meminfo_kb(b, "MemTotal");
        mfree = meminfo_kb(b, "MemFree");
        avail = meminfo_kb(b, "MemAvailable");
        buf = meminfo_kb(b, "Buffers");
        cached = meminfo_kb(b, "Cached");
    }
    if (tot >= 0) snprintf(g_val[K_MEM_TOTAL], VAL_MAX, "%lld", tot); else unk(K_MEM_TOTAL);
    if (avail >= 0) snprintf(g_val[K_MEM_FREE], VAL_MAX, "%lld", avail);
    else if (mfree >= 0 && buf >= 0 && cached >= 0)
        snprintf(g_val[K_MEM_FREE], VAL_MAX, "%lld", mfree + buf + cached);
    else unk(K_MEM_FREE);

    /* cpu: busy share since the previous collection */
    unk(K_CPU);
    if (read_file("/proc/stat", b, sizeof(b)) > 0 && !strncmp(b, "cpu ", 4)) {
        unsigned long long f[8] = {0};
        char *p = b + 4, *e;
        int got = 0;
        while (got < 8) {
            while (*p == ' ') p++;
            if (!isdigit((unsigned char)*p)) break;
            f[got++] = strtoull(p, &e, 10);
            p = e;
        }
        if (got >= 4) {
            uint64_t total = 0;
            for (int i = 0; i < got; i++) total += f[i];
            uint64_t busy = total - f[3] - (got > 4 ? f[4] : 0);
            if (g_cpu_have && total > g_cpu_total && busy >= g_cpu_busy) {
                uint64_t dt = total - g_cpu_total, db = busy - g_cpu_busy;
                unsigned pct = (unsigned)((db * 100 + dt / 2) / dt);
                snprintf(g_val[K_CPU], VAL_MAX, "%u", pct > 100 ? 100 : pct);
            }
            g_cpu_have = 1;
            g_cpu_total = total; g_cpu_busy = busy;
        }
    }

    collect_temp(t);
}

static const char *wifi_if(void) { return g_ifname[0] ? g_ifname : "wlan0"; }

static int iw_call(int s, unsigned long req, struct ak_iwreq *r, const char *ifn)
{
    snprintf(r->iw_name, IFNAMSIZ, "%s", ifn);
    return ioctl(s, req, r);
}

/* dBm from the line of wif in /proc/net/wireless, 1 when found. Only a level
 * that is certainly dBm is taken (negative, with or without the trailing '.'). */
AK_SMALL static int rssi_from_proc(const char *wif)
{
    char b[1024];
    if (read_file("/proc/net/wireless", b, sizeof(b)) <= 0) return 0;
    size_t wl = strlen(wif);
    for (char *p = b; p && *p; ) {
        while (*p == ' ') p++;
        if (!strncmp(p, wif, wl) && p[wl] == ':') {
            /* fields: status, link quality, level (no floating point: soft-float is heavy) */
            const char *q = p + wl + 1;
            for (int field = 0; field < 2; field++) {
                while (*q == ' ') q++;
                while (isxdigit((unsigned char)*q) || *q == '.') q++;
            }
            while (*q == ' ') q++;
            if (*q != '-' || !isdigit((unsigned char)q[1])) return 0;
            long lv = strtol(q + 1, NULL, 10);
            if (lv < 1 || lv > 127) return 0;
            snprintf(g_val[K_RSSI], VAL_MAX, "%d", (int)-lv);
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

AK_SMALL static void collect_net(void)
{
    unk(K_SSID); unk(K_RSSI); unk(K_RATE); unk(K_IP); unk(K_MAC);
    const char *wif = wifi_if();
    int have_rssi = rssi_from_proc(wif);

    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct ak_iwreq r;

    memset(&r, 0, sizeof(r));
    char essid[40];
    memset(essid, 0, sizeof(essid));
    r.u.data.pointer = essid; r.u.data.length = 33;
    if (iw_call(s, AK_SIOCGIWESSID, &r, wif) == 0 && r.u.data.flags && r.u.data.length > 0 && r.u.data.length <= 32) {
        size_t o = 0;
        for (size_t i = 0; i < r.u.data.length; i++)
            g_val[K_SSID][o++] = ((unsigned char)essid[i] < 0x20 || essid[i] == 0x7f) ? '?' : essid[i];
        g_val[K_SSID][o] = 0;
    }

    if (!have_rssi) {
        unsigned char st[64];
        memset(st, 0, sizeof(st)); memset(&r, 0, sizeof(r));
        r.u.data.pointer = st; r.u.data.length = sizeof(st);
        if (iw_call(s, AK_SIOCGIWSTATS, &r, wif) == 0) {
            unsigned upd = st[5];   /* iw_statistics: status(2) qual level noise updated */
            if ((upd & AK_IW_QUAL_DBM) && !(upd & AK_IW_QUAL_LEVEL_INVALID))
                snprintf(g_val[K_RSSI], VAL_MAX, "%d", (int)(signed char)st[3]);
        }
    }

    memset(&r, 0, sizeof(r));
    if (iw_call(s, AK_SIOCGIWRATE, &r, wif) == 0 && r.u.param.value > 0)
        snprintf(g_val[K_RATE], VAL_MAX, "%d", (int)((r.u.param.value + 500000) / 1000000));

    const char *ifs[2] = { wif, g_ifname[0] ? NULL : "eth0" };
    for (int i = 0; i < 2 && ifs[i]; i++) {
        struct ifreq ifr;
        if (!strcmp(g_val[K_IP], "unknown")) {
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifs[i]);
            if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in sin;
                memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
                if (sin.sin_family == AF_INET && sin.sin_addr.s_addr)
                    inet_ntop(AF_INET, &sin.sin_addr, g_val[K_IP], VAL_MAX);
            }
        }
        if (!strcmp(g_val[K_MAC], "unknown")) {
            memset(&ifr, 0, sizeof(ifr));
            snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifs[i]);
            if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
                const unsigned char *m = (const unsigned char *)ifr.ifr_hwaddr.sa_data;
                if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])
                    snprintf(g_val[K_MAC], VAL_MAX, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
    }
    close(s);
}

AK_SMALL static void collect_stream(int64_t t)
{
    int playing = g_playing;
    if (g_play_edge) { g_play_edge = 0; g_nsnap = 0; }
    snprintf(g_val[K_CLIENTS], VAL_MAX, "%d", g_clients < 0 ? 0 : (int)g_clients);
    static const char *const am[] = { "off", "on", "broken" };
    if (g_auth >= 0 && g_auth <= 2) snprintf(g_val[K_AUTH], VAL_MAX, "%s", am[g_auth]); else unk(K_AUTH);

    unk(K_FPS);
    if (!playing) {
        g_nsnap = 0;
        strcpy(g_val[K_BITRATE], "0");
        return;
    }
    unk(K_BITRATE);
    /* oldest snapshot within the window, else the newest older one */
    const snap_t *ref = NULL;
    for (int i = 0; i < g_nsnap; i++) {              /* i = 0 is the oldest */
        const snap_t *s = &g_snap[(g_snap_head - g_nsnap + i + SNAPS) % SNAPS];
        int64_t age = t - s->t_ms;
        if (age <= 0) continue;
        ref = s;
        if (age <= WINDOW_MS) break;
    }
    uint32_t fr = g_frames, by = g_bytes;
    if (ref) {
        uint64_t dt = (uint64_t)(t - ref->t_ms);
        uint64_t dfr = (uint32_t)(fr - ref->frames), dby = (uint32_t)(by - ref->bytes);
        snprintf(g_val[K_FPS], VAL_MAX, "%u", (unsigned)((dfr * 10000 + dt / 2) / dt));
        snprintf(g_val[K_BITRATE], VAL_MAX, "%u", (unsigned)((dby * 8 + dt / 2) / dt));
    }
    g_snap[g_snap_head] = (snap_t){ t, fr, by };
    g_snap_head = (g_snap_head + 1) % SNAPS;
    if (g_nsnap < SNAPS) g_nsnap++;
}

/* Caller holds g_mu. */
AK_SMALL static void refresh(void)
{
    int64_t t = now_ms();
    if (g_have && t >= g_last_ms && t - g_last_ms < CACHE_MS) return;
    collect_proc(t);
    collect_net();
    collect_stream(t);
    g_have = 1;
    g_last_ms = t;
}

int ctl_sys_get(const char *key, char *out, size_t cap)
{
    if (!key || !out || !cap) return -1;
    for (int k = 0; k < K_COUNT; k++) {
        if (strcmp(key, g_keys[k])) continue;
        pthread_mutex_lock(&g_mu);
        refresh();
        snprintf(out, cap, "%s", g_val[k]);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    return -1;
}

void ctl_sys_list(void (*emit)(const char *, const char *, void *), void *u)
{
    pthread_mutex_lock(&g_mu);
    refresh();
    for (int k = 0; k < K_COUNT; k++) emit(g_keys[k], g_val[k], u);
    pthread_mutex_unlock(&g_mu);
}

size_t ctl_sys_count(void) { return K_COUNT; }
const char *ctl_sys_key(size_t i) { return i < K_COUNT ? g_keys[i] : NULL; }

void ctl_sys_note_video_frame(size_t bytes) { g_frames = g_frames + 1; g_bytes = g_bytes + (uint32_t)bytes; }
void ctl_sys_note_rtsp_client(int delta) { g_clients = g_clients + delta; }
void ctl_sys_note_rtsp_play(int playing)
{
    playing = playing ? 1 : 0;
    if (playing && !g_playing) g_play_edge = 1;
    g_playing = playing;
}
void ctl_sys_note_rtsp_auth(int mode) { g_auth = mode; }

void ctl_sys_set_root(const char *root)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_root, sizeof(g_root), "%s", root ? root : "");
    g_have = 0; g_temp_path[0] = 0; g_temp_rescan_ms = 0;
    pthread_mutex_unlock(&g_mu);
}
void ctl_sys_set_ifname(const char *ifname)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_ifname, sizeof(g_ifname), "%s", ifname ? ifname : "");
    g_have = 0;
    pthread_mutex_unlock(&g_mu);
}
void ctl_sys_set_clock(int64_t (*fn)(void))
{
    pthread_mutex_lock(&g_mu);
    g_clock = fn;
    g_have = 0;
    pthread_mutex_unlock(&g_mu);
}
void ctl_sys_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_have = 0; g_temp_path[0] = 0; g_temp_rescan_ms = 0; g_cpu_have = 0; g_nsnap = 0; g_snap_head = 0;
    g_frames = 0; g_bytes = 0; g_clients = 0; g_playing = 0; g_play_edge = 0; g_auth = -1;
    pthread_mutex_unlock(&g_mu);
}
