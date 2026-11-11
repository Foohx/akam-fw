/* Host integration test of src/ak_rtsp/mqtt_svc.c (+ mqtt.c, mqtt_ha.c) against a fake MQTT broker written in this
 * file with its own, independent packet parser (it does not use mqtt.c): the config file parser, the dormant
 * guarantee (no thread, no socket while off), CONNECT with credentials and will, availability, retained discovery,
 * state publishes, the command allow-list over the wire, oversized and hostile packets, keepalive, a broker that
 * stops answering, a broker that drops, refusals with backoff, a stalled and a slow CONNACK, a closed port, clean
 * stop (offline + DISCONNECT, thread and fd gone) and the one-shot connection test. ASan/UBSan; no camera, loopback
 * only. The camera is a table of keys (the ops of mqtt_ha.h). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mqtt_svc.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want))) { \
    fprintf(stderr, "FAIL line %d: got [%s] want [%s]\n", __LINE__, (got), (want)); g_failed++; } } while (0)
#define SECRET "S3NTINEL-p4ss"

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ fake camera */
static pthread_mutex_t g_cam_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { const char *key; char val[40]; } kv_t;
static kv_t g_kv[] = {
    { "led.status", "auto" }, { "night.ir_led", "auto" }, { "night.mode", "auto" }, { "audio.out_volume", "60" },
    { "isp.brightness", "0" }, { "isp.contrast", "0" }, { "isp.saturation", "0" }, { "isp.sharpness", "0" },
    { "audio.talk_enable", "1" }, { "audio.out_test", "0" }, { "night.state", "day" }, { "sys.time_set", "1" },
    { "rtsp.clients", "0" }, { "net.wifi_rssi", "-60" }, { "sys.uptime", "100" }, { "sys.cpu", "5" },
    { "sys.mem_free", "20000" }, { "sys.temp", "unknown" }, { "video.fps_actual", "150" }, { "video.bitrate", "1500" },
    { "net.mac", "aa:bb:cc:dd:ee:0f" },
};
static int g_set_calls;
static char g_set_key[64], g_set_val[64];
static int f_get(const char *key, char *out, size_t cap)
{
    int rc = -1;
    pthread_mutex_lock(&g_cam_mu);
    for (size_t i = 0; i < sizeof(g_kv) / sizeof(g_kv[0]); i++)
        if (!strcmp(g_kv[i].key, key)) { snprintf(out, cap, "%s", g_kv[i].val); rc = 0; break; }
    pthread_mutex_unlock(&g_cam_mu);
    return rc;
}
static void kv_set(const char *key, const char *v)
{
    pthread_mutex_lock(&g_cam_mu);
    for (size_t i = 0; i < sizeof(g_kv) / sizeof(g_kv[0]); i++)
        if (!strcmp(g_kv[i].key, key)) snprintf(g_kv[i].val, sizeof(g_kv[i].val), "%s", v);
    pthread_mutex_unlock(&g_cam_mu);
}
static int f_set(const char *key, const char *value, int persist, char *msg, size_t cap)
{
    (void)persist;
    pthread_mutex_lock(&g_cam_mu);
    g_set_calls++;
    snprintf(g_set_key, sizeof(g_set_key), "%s", key);
    snprintf(g_set_val, sizeof(g_set_val), "%s", value);
    pthread_mutex_unlock(&g_cam_mu);
    kv_set(key, value);
    snprintf(msg, cap, "OK");
    return 0;
}
static void f_host(char *out) { strcpy(out, "akam-0f0f"); }
static const char *f_ver(void) { return "b1-test"; }
static const mqtt_ops_t g_ops = { f_get, f_set, f_host, f_ver };
static int set_calls(void) { pthread_mutex_lock(&g_cam_mu); int n = g_set_calls; pthread_mutex_unlock(&g_cam_mu); return n; }

/* ------------------------------------------------------------------ fake broker */
#define MAXREC 600
typedef struct { char topic[220]; char payload[1300]; size_t plen; int retain; } rec_t;
static struct {
    pthread_mutex_t mu, wmu;
    int lfd, port, conn_fd, quit;
    pthread_t tid;
    /* behaviour */
    int connack_rc, stall_connack, slow_connack, garbage_connack, ignore_ping, close_on_accept;
    /* records */
    int connects, protocol_errors, pings, disconnects, pubacks, subs;
    int64_t conn_ms[64];
    char client_id[80], user[80], pass[300], will_topic[220], will_msg[40], sub_filter[220];
    int keepalive, clean, has_will, will_retain, has_user, has_pass, proto_level;
    rec_t pubs[MAXREC];
    int npub;
} B;

static void bw(const uint8_t *b, size_t n)
{
    pthread_mutex_lock(&B.wmu);
    int fd = B.conn_fd;
    size_t off = 0;
    while (fd >= 0 && off < n) {
        ssize_t w = send(fd, b + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) break;
        off += (size_t)w;
    }
    pthread_mutex_unlock(&B.wmu);
}

/* A PUBLISH from the broker to the connected client (QoS 0 or 1). */
static void broker_publish(const char *topic, const void *payload, size_t plen, int retain, int qos)
{
    size_t tl = strlen(topic), body = 2 + tl + (qos ? 2 : 0) + plen;
    uint8_t *b = malloc(body + 5);
    size_t o = 0;
    b[o++] = (uint8_t)(0x30 | (qos << 1) | (retain ? 1 : 0));
    size_t v = body;
    do { uint8_t c = v & 0x7f; v >>= 7; if (v) c |= 0x80; b[o++] = c; } while (v);
    b[o++] = (uint8_t)(tl >> 8); b[o++] = (uint8_t)tl;
    memcpy(b + o, topic, tl); o += tl;
    if (qos) { b[o++] = 0x12; b[o++] = 0x34; }
    memcpy(b + o, payload, plen); o += plen;
    bw(b, o);
    free(b);
}

static void broker_drop(void)
{
    pthread_mutex_lock(&B.wmu);
    if (B.conn_fd >= 0) shutdown(B.conn_fd, SHUT_RDWR);
    pthread_mutex_unlock(&B.wmu);
}

static int rd_u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

/* Parse one packet body; independent of src/ak_rtsp/mqtt.c. */
static void broker_packet(unsigned type, unsigned flags, const uint8_t *b, size_t n)
{
    pthread_mutex_lock(&B.mu);
    if (type == 1) {                                  /* CONNECT */
        size_t o = 0;
        int ok = flags == 0 && n >= 10 && rd_u16(b) == 4 && !memcmp(b + 2, "MQTT", 4);
        if (ok) {
            B.proto_level = b[6];
            unsigned cf = b[7];
            B.keepalive = rd_u16(b + 8);
            B.clean = (cf >> 1) & 1;
            B.has_will = (cf >> 2) & 1;
            B.will_retain = (cf >> 5) & 1;
            B.has_user = (cf >> 7) & 1;
            B.has_pass = (cf >> 6) & 1;
            if (((cf >> 3) & 3) != 0 || (cf & 1) || (B.has_pass && !B.has_user)) ok = 0;     /* will qos must be 0 */
            o = 10;
            B.will_topic[0] = B.will_msg[0] = B.user[0] = B.pass[0] = '\0';
            for (int field = 0; ok && field < 5; field++) {
                int want = field == 0 || (field == 1 && B.has_will) || (field == 2 && B.has_will) ||
                           (field == 3 && B.has_user) || (field == 4 && B.has_pass);
                if (!want) continue;
                if (o + 2 > n) { ok = 0; break; }
                size_t l = (size_t)rd_u16(b + o);
                o += 2;
                if (o + l > n) { ok = 0; break; }
                char *dst = field == 0 ? B.client_id : field == 1 ? B.will_topic : field == 2 ? B.will_msg : field == 3 ? B.user : B.pass;
                size_t cap = field == 0 ? sizeof(B.client_id) : field == 1 ? sizeof(B.will_topic) : field == 2 ? sizeof(B.will_msg) : field == 3 ? sizeof(B.user) : sizeof(B.pass);
                if (l >= cap) { ok = 0; break; }
                memcpy(dst, b + o, l);
                dst[l] = '\0';
                o += l;
            }
            if (ok && o != n) ok = 0;
        }
        if (!ok) B.protocol_errors++;
        B.connects++;
        if (B.connects <= 64) B.conn_ms[B.connects - 1] = now_ms();
        int rc = B.connack_rc, stall = B.stall_connack, slow = B.slow_connack, garbage = B.garbage_connack;
        pthread_mutex_unlock(&B.mu);
        uint8_t ack[4] = { 0x20, 0x02, 0x00, (uint8_t)rc };
        if (garbage) { static const uint8_t g[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; bw(g, sizeof(g)); }
        else if (stall) { /* say nothing */ }
        else if (slow) { for (int i = 0; i < 4; i++) { bw(ack + i, 1); usleep(15000); } }
        else bw(ack, 4);
        return;
    }
    if (type == 3) {                                  /* PUBLISH from the camera: QoS 0 only */
        int qos = (flags >> 1) & 3;
        if (qos != 0 || n < 2 || (size_t)rd_u16(b) + 2 > n) B.protocol_errors++;
        else {
            size_t tl = (size_t)rd_u16(b);
            if (B.npub < MAXREC && tl < sizeof(B.pubs[0].topic) && n - 2 - tl < sizeof(B.pubs[0].payload)) {
                rec_t *r = &B.pubs[B.npub++];
                memcpy(r->topic, b + 2, tl);
                r->topic[tl] = '\0';
                r->plen = n - 2 - tl;
                memcpy(r->payload, b + 2 + tl, r->plen);
                r->payload[r->plen] = '\0';
                r->retain = flags & 1;
            } else B.protocol_errors++;
        }
    } else if (type == 8) {                           /* SUBSCRIBE */
        size_t l = n >= 4 ? (size_t)rd_u16(b + 2) : 0;
        if (flags != 2 || n < 5 || 4 + l + 1 != n || l >= sizeof(B.sub_filter)) B.protocol_errors++;
        else {
            memcpy(B.sub_filter, b + 4, l);
            B.sub_filter[l] = '\0';
            B.subs++;
            uint8_t ack[5] = { 0x90, 0x03, b[0], b[1], 0x00 };
            pthread_mutex_unlock(&B.mu);
            bw(ack, 5);
            return;
        }
    } else if (type == 12) {                          /* PINGREQ */
        if (flags != 0 || n != 0) B.protocol_errors++;
        B.pings++;
        int ign = B.ignore_ping;
        pthread_mutex_unlock(&B.mu);
        static const uint8_t pr[2] = { 0xd0, 0x00 };
        if (!ign) bw(pr, 2);
        return;
    } else if (type == 14) {                          /* DISCONNECT */
        B.disconnects++;
    } else if (type == 4) {                           /* PUBACK of our QoS 1 delivery */
        if (n == 2 && rd_u16(b) == 0x1234) B.pubacks++; else B.protocol_errors++;
    } else {
        B.protocol_errors++;
    }
    pthread_mutex_unlock(&B.mu);
}

static void *broker_main(void *arg)
{
    (void)arg;
    uint8_t buf[16384];
    while (!B.quit) {
        struct pollfd p = { B.lfd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        int fd = accept(B.lfd, NULL, NULL);
        if (fd < 0) continue;
        pthread_mutex_lock(&B.mu);
        int close_now = B.close_on_accept;
        pthread_mutex_unlock(&B.mu);
        if (close_now) { close(fd); continue; }
        pthread_mutex_lock(&B.wmu);
        B.conn_fd = fd;
        pthread_mutex_unlock(&B.wmu);
        size_t have = 0;
        for (;;) {
            struct pollfd q = { fd, POLLIN, 0 };
            if (B.quit) break;
            int pr = poll(&q, 1, 50);
            if (pr == 0) continue;
            if (pr < 0) break;
            ssize_t n = recv(fd, buf + have, sizeof(buf) - have, 0);
            if (n <= 0) break;
            have += (size_t)n;
            for (;;) {
                if (have < 2) break;
                size_t o = 1, rl = 0, mult = 1;
                int more = 1;
                while (o < have && more) { rl += (size_t)(buf[o] & 0x7f) * mult; mult *= 128; more = buf[o] & 0x80; o++; if (o > 5) break; }
                if (more && o >= have) break;
                if (more) { pthread_mutex_lock(&B.mu); B.protocol_errors++; pthread_mutex_unlock(&B.mu); have = 0; break; }
                if (have < o + rl) break;
                broker_packet(buf[0] >> 4, buf[0] & 15, buf + o, rl);
                memmove(buf, buf + o + rl, have - o - rl);
                have -= o + rl;
            }
        }
        pthread_mutex_lock(&B.wmu);
        B.conn_fd = -1;
        pthread_mutex_unlock(&B.wmu);
        close(fd);
    }
    return NULL;
}

static void broker_start(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    memset(&B, 0, sizeof(B));
    pthread_mutex_init(&B.mu, NULL);
    pthread_mutex_init(&B.wmu, NULL);
    B.conn_fd = -1;
    B.lfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(B.lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(B.lfd, 8) != 0) { perror("broker"); exit(2); }
    getsockname(B.lfd, (struct sockaddr *)&a, &al);
    B.port = ntohs(a.sin_port);
    pthread_create(&B.tid, NULL, broker_main, NULL);
}

static void broker_stop(void)
{
    B.quit = 1;
    pthread_join(B.tid, NULL);
    close(B.lfd);
}

#define BGET(field) ({ pthread_mutex_lock(&B.mu); __typeof__(B.field) v_ = B.field; pthread_mutex_unlock(&B.mu); v_; })
#define BSET(field, val) do { pthread_mutex_lock(&B.mu); B.field = (val); pthread_mutex_unlock(&B.mu); } while (0)

/* Index of the first publish at or after `from` on exactly this topic, or -1. payload copied to out. */
static int pub_find(int from, const char *topic, char *out, size_t cap, int *retain)
{
    int r = -1;
    pthread_mutex_lock(&B.mu);
    for (int i = from; i < B.npub; i++)
        if (!strcmp(B.pubs[i].topic, topic)) {
            if (out) snprintf(out, cap, "%s", B.pubs[i].payload);
            if (retain) *retain = B.pubs[i].retain;
            r = i;
            break;
        }
    pthread_mutex_unlock(&B.mu);
    return r;
}

static int pub_count_prefix(int from, const char *prefix)
{
    int n = 0;
    pthread_mutex_lock(&B.mu);
    for (int i = from; i < B.npub; i++)
        if (!strncmp(B.pubs[i].topic, prefix, strlen(prefix))) n++;
    pthread_mutex_unlock(&B.mu);
    return n;
}

/* ------------------------------------------------------------------ helpers */
static int wait_state(const char *want, int ms)
{
    int64_t end = now_ms() + ms;
    char s[MQTT_STATE_MAX];
    do {
        mqtt_svc_state(s, sizeof(s));
        if (!strcmp(s, want)) return 1;
        usleep(5000);
    } while (now_ms() < end);
    mqtt_svc_state(s, sizeof(s));
    fprintf(stderr, "  (wanted state [%s], is [%s])\n", want, s);
    return 0;
}

#define WAIT_FOR(cond, ms) ({ int64_t e_ = now_ms() + (ms); int ok_ = 0; \
    do { if (cond) { ok_ = 1; break; } usleep(5000); } while (now_ms() < e_); if (!ok_ && (cond)) ok_ = 1; ok_; })

static int count_dir(const char *path)
{
    int n = 0;
    DIR *d = opendir(path);
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}
static int tasks(void) { return count_dir("/proc/self/task"); }
static int fds(void) { return count_dir("/proc/self/fd"); }

static char g_dir[256], g_cfg[300], g_pass[300];
static void write_file(const char *path, const char *text, mode_t mode)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { perror(path); exit(2); }
    if (write(fd, text, strlen(text)) < 0) exit(2);
    close(fd);
    chmod(path, mode);
}
static void write_cfg(const char *extra)
{
    char t[600];
    snprintf(t, sizeof(t), "enabled=1\nhost=127.0.0.1\nport=%d\n%s", B.port, extra);
    write_file(g_cfg, t, 0600);
}

/* ------------------------------------------------------------------ tests */
static void test_parser(void)
{
    mqtt_cfg_t c;
    static const char full[] = "enabled=1\nhost=broker.lan\nport=1884\nuser=a.b_c@d+e-f\nclient_id=cam_1-x\n"
                               "base_topic=home/cams/one\ndiscovery=off\ndiscovery_prefix=ha\ninterval=60\n";
    CHECK(mqtt_cfg_parse(full, sizeof(full) - 1, &c) == 0);
    CHECK(c.enabled == 1 && !strcmp(c.host, "broker.lan") && c.port == 1884 && !strcmp(c.user, "a.b_c@d+e-f") &&
          !strcmp(c.client_id, "cam_1-x") && !strcmp(c.base, "home/cams/one") && c.discovery == 0 &&
          !strcmp(c.prefix, "ha") && c.interval == 60);
    CHECK(mqtt_cfg_parse("host=192.0.2.5\nenabled=0", strlen("host=192.0.2.5\nenabled=0"), &c) == 0 && c.enabled == 0 && c.port == 1883 && c.discovery == 1 &&
          c.interval == 30 && c.user[0] == 0 && c.base[0] == 0 && c.client_id[0] == 0 && c.prefix[0] == 0);   /* no final LF, defaults */
    CHECK(mqtt_cfg_parse("enabled=1\r\nhost=h\r\n", strlen("enabled=1\r\nhost=h\r\n"), &c) == 0 && !strcmp(c.host, "h"));                        /* CRLF tolerated */
    CHECK(mqtt_cfg_parse("enabled=1\nhost=h\nuser=\n", strlen("enabled=1\nhost=h\nuser=\n"), &c) == 0 && c.user[0] == 0);                           /* empty user = none */
    static const struct { const char *t; const char *why; } bad[] = {
        { "", "empty" }, { "enabled=1\n", "no host" }, { "host=h\n", "no enabled" }, { "enabled=2\nhost=h\n", "enabled 2" },
        { "enabled=1\nenabled=1\nhost=h\n", "dup" }, { "enabled=1\nhost=h\nbogus=1\n", "unknown key" },
        { "enabled=1\nhost=h\nport=0\n", "port 0" }, { "enabled=1\nhost=h\nport=65536\n", "port 65536" },
        { "enabled=1\nhost=h\nport=18x\n", "port junk" }, { "enabled=1\nhost=h\nport=\n", "port empty" },
        { "enabled=1\nhost=h\nport=123456\n", "port 6 digits" }, { "enabled=1\nhost=\n", "empty host" },
        { "enabled=1\nhost=-h\n", "host dash" }, { "enabled=1\nhost=a b\n", "host space" }, { "enabled=1\nhost=a/b\n", "host slash" },
        { "enabled=1\nhost=h\nuser=a b\n", "user space" }, { "enabled=1\nhost=h\nuser=a:b\n", "user colon" },
        { "enabled=1\nhost=h\nclient_id=\n", "empty id" }, { "enabled=1\nhost=h\nclient_id=a.b\n", "id dot" },
        { "enabled=1\nhost=h\nbase_topic=/a\n", "base lead slash" }, { "enabled=1\nhost=h\nbase_topic=a/+\n", "base wildcard" },
        { "enabled=1\nhost=h\nbase_topic=a//b\n", "base double slash" }, { "enabled=1\nhost=h\nbase_topic=\n", "base empty" },
        { "enabled=1\nhost=h\ndiscovery=yes\n", "discovery yes" }, { "enabled=1\nhost=h\ndiscovery=1\n", "discovery 1" },
        { "enabled=1\nhost=h\ninterval=9\n", "interval 9" }, { "enabled=1\nhost=h\ninterval=301\n", "interval 301" },
        { "enabled=1\nhost=h\ninterval=1e2\n", "interval junk" }, { "enabled=1\nhost=h\n\n", "blank line" },
        { "enabled=1\nhost=h\njunk\n", "no equals" }, { "enabled = 1\nhost=h\n", "spaces" }, { " enabled=1\nhost=h\n", "leading space" },
        { "enabled=1\nhost=h\n#c\n", "comment" }, { "enabled=1\nhost=h\npass=secret\n", "pass is not a config key" },
        { "enabled=1\nhost=h\nuser=a\xff\n", "user high byte" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (mqtt_cfg_parse(bad[i].t, strlen(bad[i].t), &c) == 0) fprintf(stderr, "  accepted: %s\n", bad[i].why);
        CHECK(mqtt_cfg_parse(bad[i].t, strlen(bad[i].t), &c) == -1);
    }
    CHECK(mqtt_cfg_parse("enabled=1\0host=h\n", 17, &c) == -1);                                                   /* NUL */
    char big[MQTT_CFG_MAX + 40];
    memset(big, 'a', sizeof(big));
    CHECK(mqtt_cfg_parse(big, sizeof(big), &c) == -1);
    char lh[100], hh[70];
    memset(hh, 'a', 64); hh[64] = 0;
    snprintf(lh, sizeof(lh), "enabled=1\nhost=%s\n", hh);
    CHECK(mqtt_cfg_parse(lh, strlen(lh), &c) == -1);                                                              /* host 64 */
    hh[63] = 0;
    snprintf(lh, sizeof(lh), "enabled=1\nhost=%s\n", hh);
    CHECK(mqtt_cfg_parse(lh, strlen(lh), &c) == 0);                                                               /* host 63 */
    CHECK(mqtt_cfg_parse(NULL, 0, &c) == -1);
}

static void test_dormant(void)
{
    char st[MQTT_STATE_MAX];
    int t0 = tasks(), f0 = fds();
    /* the module has never been started: nothing exists */
    mqtt_svc_state(st, sizeof(st));
    CHECK_STR(st, "off");
    /* no file */
    unlink(g_cfg);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && !strcmp(st, "off"));
    CHECK(tasks() == t0 && fds() == f0 && BGET(connects) == 0);
    /* enabled=0 */
    write_file(g_cfg, "enabled=0\nhost=127.0.0.1\nport=1883\n", 0600);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && !strcmp(st, "off"));
    CHECK(tasks() == t0 && fds() == f0);
    /* invalid files */
    static const char *const bad[] = { "garbage\n", "enabled=1\nhost=127.0.0.1\nport=0\n", "enabled=1\n", "enabled=1\nhost=a b\n" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(g_cfg, bad[i], 0600);
        CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && !strcmp(st, "off"));
        CHECK(tasks() == t0 && fds() == f0 && BGET(connects) == 0);
    }
    /* a directory instead of a file, a FIFO would hang a naive open: both stay off */
    unlink(g_cfg);
    mkdir(g_cfg, 0700);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && !strcmp(st, "off") && tasks() == t0);
    rmdir(g_cfg);
    CHECK(mkfifo(g_cfg, 0600) == 0);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && !strcmp(st, "off") && tasks() == t0 && fds() == f0);
    unlink(g_cfg);
    /* enabled, but the password file is readable by others: refused, stays off, nothing connects */
    write_cfg("user=bob\n");
    write_file(g_pass, SECRET "\n", 0644);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == -1 && !strcmp(st, "off"));
    CHECK(tasks() == t0 && fds() == f0 && BGET(connects) == 0);
    write_file(g_pass, SECRET "\tx\n", 0600);                  /* control character */
    CHECK(mqtt_svc_reload(st, sizeof(st)) == -1 && !strcmp(st, "off") && tasks() == t0);
    char longpw[200];
    memset(longpw, 'p', 129); longpw[129] = 0;
    write_file(g_pass, longpw, 0600);                          /* 129 characters */
    CHECK(mqtt_svc_reload(st, sizeof(st)) == -1 && tasks() == t0);
    unlink(g_pass);
    /* a test with no valid file never starts anything */
    unlink(g_cfg);
    CHECK(mqtt_svc_test_start() == -1);
    mqtt_svc_test_result(st, sizeof(st));
    CHECK_STR(st, "error:config");
    CHECK(tasks() == t0 && fds() == f0 && BGET(connects) == 0);
    /* and the camera was never touched */
    CHECK(set_calls() == 0);
}

static void test_session(void)
{
    char st[MQTT_STATE_MAX], v[1300];
    int retain = -1;
    int t0 = tasks(), f0 = fds();
    write_cfg("user=bob\ninterval=10\n");
    write_file(g_pass, SECRET "\n", 0600);
    CHECK(mqtt_svc_reload(st, sizeof(st)) == 0 && (!strcmp(st, "connecting") || !strcmp(st, "connected")));
    CHECK(wait_state("connected", 3000));
    CHECK(tasks() == t0 + 1);                                  /* exactly one extra thread */
    CHECK(WAIT_FOR(pub_find(0, "akam/akam-0f0f/volume/state", NULL, 0, NULL) >= 0, 3000));

    /* CONNECT: credentials, keepalive, clean session, will */
    pthread_mutex_lock(&B.mu);
    CHECK_STR(B.client_id, "akam-0f0f");
    CHECK(B.has_user && B.has_pass && !strcmp(B.user, "bob") && !strcmp(B.pass, SECRET));
    CHECK(B.keepalive == 2 && B.clean == 1 && B.proto_level == 4);
    CHECK(B.has_will && B.will_retain && !strcmp(B.will_topic, "akam/akam-0f0f/status") && !strcmp(B.will_msg, "offline"));
    CHECK_STR(B.sub_filter, "akam/akam-0f0f/+/set");
    pthread_mutex_unlock(&B.mu);
    CHECK(BGET(subs) == 1 && BGET(connects) == 1);

    /* availability first, retained; discovery retained; states */
    CHECK(pub_find(0, "akam/akam-0f0f/status", v, sizeof(v), &retain) == 0 && !strcmp(v, "online") && retain == 1);
    int disc = pub_count_prefix(0, "homeassistant/");
    CHECK(disc == 19);                                          /* 20 entities, temperature absent on this camera */
    CHECK(pub_find(0, "homeassistant/select/akam_aabbccddee0f/status_led/config", v, sizeof(v), &retain) > 0 && retain == 1);
    CHECK(strstr(v, "\"unique_id\":\"akam_aabbccddee0f_status_led\"") != NULL);
    CHECK(pub_find(0, "homeassistant/sensor/akam_aabbccddee0f/temperature/config", NULL, 0, NULL) == -1);
    pthread_mutex_lock(&B.mu);
    int ordered = 1;
    for (int i = 0; i < B.npub && i <= disc; i++)               /* nothing but status + discovery before the states */
        if (i > 0 && strncmp(B.pubs[i].topic, "homeassistant/", 14) != 0) ordered = 0;
    pthread_mutex_unlock(&B.mu);
    CHECK(ordered);
    CHECK(pub_find(0, "akam/akam-0f0f/status_led/state", v, sizeof(v), &retain) > 0 && !strcmp(v, "auto") && retain == 1);
    CHECK(pub_find(0, "akam/akam-0f0f/fps/state", v, sizeof(v), &retain) > 0 && !strcmp(v, "15.0") && retain == 0);
    CHECK(pub_find(0, "akam/akam-0f0f/wifi_signal/state", v, sizeof(v), &retain) > 0 && !strcmp(v, "-60"));
    CHECK(pub_find(0, "akam/akam-0f0f/talk/state", v, sizeof(v), NULL) > 0 && !strcmp(v, "ON"));
    CHECK(pub_find(0, "akam/akam-0f0f/beep/state", NULL, 0, NULL) == -1);       /* a button has no state */
    CHECK(pub_find(0, "akam/akam-0f0f/temperature/state", NULL, 0, NULL) == -1);

    /* a valid command: applied through the setter, state republished at once */
    int base = BGET(npub);
    int calls = set_calls();
    broker_publish("akam/akam-0f0f/volume/set", "42", 2, 0, 0);
    CHECK(WAIT_FOR(pub_find(base, "akam/akam-0f0f/volume/state", v, sizeof(v), &retain) >= 0, 3000));
    CHECK(!strcmp(v, "42") && retain == 1);
    CHECK(set_calls() == calls + 1 && !strcmp(g_set_key, "audio.out_volume") && !strcmp(g_set_val, "42"));

    /* invalid, hostile and out-of-allow-list commands: nothing happens, the connection stays up */
    base = BGET(npub);
    calls = set_calls();
    broker_publish("akam/akam-0f0f/volume/set", "101", 3, 0, 0);
    broker_publish("akam/akam-0f0f/volume/set", "5;reboot", 8, 0, 0);
    broker_publish("akam/akam-0f0f/reboot/set", "PRESS", 5, 0, 0);
    broker_publish("akam/akam-0f0f/uptime/set", "0", 1, 0, 0);
    broker_publish("akam/akam-0f0f/status_led/set", "blink", 5, 0, 0);
    broker_publish("akam/akam-0f0f/beep/set", "PRESS", 5, 1, 0);          /* retained: refused */
    broker_publish("akam/akam-0f0f/status_led/state", "off", 3, 0, 0);    /* not a command topic */
    broker_publish("other/topic", "x", 1, 0, 0);
    usleep(200000);
    CHECK(set_calls() == calls);
    CHECK(pub_find(base, "akam/akam-0f0f/volume/state", NULL, 0, NULL) < 0);
    mqtt_svc_state(st, sizeof(st));
    CHECK_STR(st, "connected");
    char stats[120];
    mqtt_svc_stats(stats, sizeof(stats));
    CHECK(strstr(stats, "rejected=6") != NULL);
    CHECK(strstr(stats, "applied=1") != NULL);

    /* a packet far too big for the receive buffer is skipped, the stream stays in sync */
    {
        size_t n = 70000;
        char *big = malloc(n);
        memset(big, '9', n);
        base = BGET(npub);
        broker_publish("akam/akam-0f0f/volume/set", big, n, 0, 0);
        free(big);
        broker_publish("akam/akam-0f0f/volume/set", "17", 2, 0, 0);
        CHECK(WAIT_FOR(({ char x_[40] = ""; pub_find(base, "akam/akam-0f0f/volume/state", x_, sizeof(x_), NULL) >= 0 && set_calls() == calls + 1; }), 3000));
        CHECK(!strcmp(g_set_val, "17"));
        mqtt_svc_state(st, sizeof(st));
        CHECK_STR(st, "connected");
    }

    /* QoS 1 delivery: PUBACK comes back */
    broker_publish("akam/akam-0f0f/brightness/set", "-5", 2, 0, 1);
    CHECK(WAIT_FOR(BGET(pubacks) == 1, 3000));
    CHECK(WAIT_FOR(!strcmp(g_set_key, "isp.brightness"), 1000));

    /* a change made elsewhere (SSH, ONVIF, the shell) is published within a few ticks, retained */
    base = BGET(npub);
    kv_set("led.status", "off");
    CHECK(WAIT_FOR(pub_find(base, "akam/akam-0f0f/status_led/state", v, sizeof(v), &retain) >= 0, 3000));
    CHECK(!strcmp(v, "off") && retain == 1);
    kv_set("night.state", "night");
    CHECK(WAIT_FOR(pub_find(base, "akam/akam-0f0f/night_state/state", v, sizeof(v), &retain) >= 0, 3000));
    CHECK(!strcmp(v, "night") && retain == 0);
    /* an unchanged control is not republished by the scan */
    int again = pub_count_prefix(base, "akam/akam-0f0f/status_led/state");
    usleep(400000);
    CHECK(pub_count_prefix(base, "akam/akam-0f0f/status_led/state") == again);

    /* keepalive: the broker is silent, the camera pings and the broker answers */
    CHECK(WAIT_FOR(BGET(pings) >= 2, 4000));
    mqtt_svc_state(st, sizeof(st));
    CHECK_STR(st, "connected");

    /* a broker that stops answering pings is left after ping_reply_ms and the camera comes back with a new
     * connection (the error state itself only lasts one backoff period, too short to poll for reliably) */
    int c0 = BGET(connects), p0 = BGET(pings);
    BSET(ignore_ping, 1);
    CHECK(WAIT_FOR(BGET(connects) == c0 + 1, 6000));
    CHECK(BGET(pings) > p0);
    BSET(ignore_ping, 0);
    CHECK(wait_state("connected", 6000));
    CHECK(BGET(connects) == c0 + 1);

    /* the broker drops the connection: reconnect, availability and discovery are announced again */
    CHECK(WAIT_FOR(BGET(subs) == BGET(connects), 3000));        /* the announcement of this session is complete */
    base = BGET(npub);
    c0 = BGET(connects);
    broker_drop();
    CHECK(WAIT_FOR(BGET(connects) == c0 + 1, 4000));
    CHECK(wait_state("connected", 3000));
    CHECK(WAIT_FOR(BGET(subs) == BGET(connects), 3000));
    CHECK(pub_count_prefix(base, "homeassistant/") == 19);
    CHECK(pub_find(base, "akam/akam-0f0f/status", v, sizeof(v), NULL) >= 0 && !strcmp(v, "online"));
    mqtt_svc_stats(stats, sizeof(stats));
    CHECK(strstr(stats, "connects=") != NULL);

    /* sensors come back on the interval (10 s is the minimum, so only check the scan did not republish them) */
    CHECK(BGET(protocol_errors) == 0);

    /* clean stop: offline (retained) and DISCONNECT, then thread and socket are gone */
    base = BGET(npub);
    int d0 = BGET(disconnects);
    mqtt_svc_stop();
    mqtt_svc_state(st, sizeof(st));
    CHECK_STR(st, "off");
    CHECK(WAIT_FOR(pub_find(base, "akam/akam-0f0f/status", v, sizeof(v), &retain) >= 0, 2000));
    CHECK(!strcmp(v, "offline") && retain == 1);
    CHECK(WAIT_FOR(BGET(disconnects) == d0 + 1, 2000));
    CHECK(tasks() == t0 && fds() == f0);
    int calls_after = set_calls();
    usleep(200000);
    CHECK(BGET(npub) == base + 1 || BGET(npub) == base);        /* nothing is published once off */
    CHECK(set_calls() == calls_after);
    mqtt_svc_stop();                                            /* safe when off */
    CHECK(tasks() == t0 && fds() == f0);
}

static void test_failures(void)
{
    char st[MQTT_STATE_MAX];
    int t0 = tasks(), f0 = fds();
    write_cfg("user=bob\n");
    write_file(g_pass, SECRET "\n", 0600);

    /* refusals: the reason is reported, attempts repeat with a growing delay inside the backoff bounds */
    static const struct { int rc; const char *state; } refusals[] = {
        { 1, "error:refused_protocol" }, { 2, "error:refused_id" }, { 3, "error:refused_unavailable" },
        { 4, "error:refused_credentials" }, { 5, "error:refused_not_authorized" }, { 9, "error:refused" },
    };
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        BSET(connack_rc, refusals[i].rc);
        int c0 = BGET(connects);
        mqtt_svc_reload(st, sizeof(st));
        CHECK(wait_state(refusals[i].state, 3000));
        if (i == 4) {                                           /* watch the backoff on one of them */
            CHECK(WAIT_FOR(BGET(connects) >= c0 + 5, 8000));
            pthread_mutex_lock(&B.mu);
            for (int k = c0 + 1; k < c0 + 5; k++) {
                int64_t gap = B.conn_ms[k] - B.conn_ms[k - 1];
                unsigned exp = mqtt_svc_tune.backoff_min_ms << (k - c0 - 1);
                if (exp > mqtt_svc_tune.backoff_max_ms) exp = mqtt_svc_tune.backoff_max_ms;
                if (!(gap >= (int64_t)exp * 75 / 100 && gap <= (int64_t)mqtt_svc_tune.backoff_max_ms + 300)) fprintf(stderr, "  gap %d = %lld ms, expected about %u\n", k, (long long)gap, exp);
                CHECK(gap >= (int64_t)exp * 75 / 100 && gap <= (int64_t)mqtt_svc_tune.backoff_max_ms + 300);
            }
            pthread_mutex_unlock(&B.mu);
        }
        mqtt_svc_stop();
        CHECK(tasks() == t0 && fds() == f0);
    }
    BSET(connack_rc, 0);

    /* a CONNACK that never comes */
    BSET(stall_connack, 1);
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("error:timeout", 3000));
    mqtt_svc_stop();
    BSET(stall_connack, 0);
    CHECK(tasks() == t0 && fds() == f0);

    /* not MQTT at all */
    BSET(garbage_connack, 1);
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("error:protocol", 3000));
    mqtt_svc_stop();
    BSET(garbage_connack, 0);

    /* a CONNACK delivered one byte at a time */
    BSET(slow_connack, 1);
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("connected", 3000));
    mqtt_svc_stop();
    BSET(slow_connack, 0);

    /* the broker accepts and hangs up */
    BSET(close_on_accept, 1);
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("error:closed", 3000));
    mqtt_svc_stop();
    BSET(close_on_accept, 0);
    CHECK(tasks() == t0 && fds() == f0);

    /* nothing listens on the port */
    {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, (struct sockaddr *)&a, sizeof(a));
        getsockname(s, (struct sockaddr *)&a, &al);
        close(s);
        char t[200];
        snprintf(t, sizeof(t), "enabled=1\nhost=127.0.0.1\nport=%d\n", ntohs(a.sin_port));
        write_file(g_cfg, t, 0600);
        mqtt_svc_reload(st, sizeof(st));
        CHECK(wait_state("error:connect", 3000));
        mqtt_svc_stop();
        if (getenv("MQTT_TEST_DNS")) {      /* needs a working resolver; off by default (a dead one can take 30 s) */
            write_file(g_cfg, "enabled=1\nhost=no-such-host.invalid\n", 0600);
            mqtt_svc_reload(st, sizeof(st));
            CHECK(wait_state("error:dns", 15000));
            mqtt_svc_stop();
        }
        CHECK(tasks() == t0 && fds() == f0);
    }

    /* a reload while running restarts with the new file; a reload to "off" stops it */
    write_cfg("user=bob\ndiscovery=off\n");
    int base = BGET(npub);
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("connected", 3000));
    CHECK(WAIT_FOR(pub_find(base, "akam/akam-0f0f/volume/state", NULL, 0, NULL) >= 0, 3000));
    CHECK(pub_count_prefix(base, "homeassistant/") == 0);       /* discovery=off: states only */
    write_cfg("user=bob\nbase_topic=home/cam1\ndiscovery_prefix=ha\nclient_id=cam_one\n");
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("connected", 3000));
    CHECK(WAIT_FOR(pub_find(base, "ha/select/akam_aabbccddee0f/status_led/config", NULL, 0, NULL) >= 0, 3000));
    CHECK(pub_find(base, "home/cam1/status", NULL, 0, NULL) >= 0);
    pthread_mutex_lock(&B.mu);
    CHECK_STR(B.client_id, "cam_one");
    CHECK_STR(B.sub_filter, "home/cam1/+/set");
    CHECK_STR(B.will_topic, "home/cam1/status");
    pthread_mutex_unlock(&B.mu);
    write_file(g_cfg, "enabled=0\nhost=127.0.0.1\n", 0600);
    mqtt_svc_reload(st, sizeof(st));
    CHECK_STR(st, "off");
    CHECK(tasks() == t0 && fds() == f0);

    /* no user name: no password is sent even if a password file exists (MQTT 3.1.1 forbids it) */
    write_cfg("");
    mqtt_svc_reload(st, sizeof(st));
    CHECK(wait_state("connected", 3000));
    pthread_mutex_lock(&B.mu);
    CHECK(!B.has_user && !B.has_pass);
    pthread_mutex_unlock(&B.mu);
    mqtt_svc_stop();
    CHECK(BGET(protocol_errors) == 0);
}

static void test_oneshot(void)
{
    char st[MQTT_STATE_MAX];
    int t0 = tasks(), f0 = fds();
    write_cfg("user=bob\n");
    write_file(g_pass, SECRET "\n", 0600);
    mqtt_svc_test_result(st, sizeof(st));
    int d0 = BGET(disconnects), c0 = BGET(connects);
    /* the service is off; the test works anyway and never starts it */
    CHECK(mqtt_svc_test_start() == 0);
    CHECK(WAIT_FOR(({ mqtt_svc_test_result(st, sizeof(st)); strcmp(st, "running") != 0; }), 3000));
    CHECK_STR(st, "ok");
    CHECK(BGET(connects) == c0 + 1 && WAIT_FOR(BGET(disconnects) == d0 + 1, 2000));
    pthread_mutex_lock(&B.mu);
    CHECK_STR(B.client_id, "akam-0f0f-test");                   /* never the id of the real session */
    CHECK(!strcmp(B.user, "bob") && !strcmp(B.pass, SECRET) && !B.has_will);
    pthread_mutex_unlock(&B.mu);
    mqtt_svc_state(st, sizeof(st));
    CHECK_STR(st, "off");
    CHECK(WAIT_FOR(tasks() == t0, 2000) && fds() == f0);

    BSET(connack_rc, 4);
    CHECK(mqtt_svc_test_start() == 0);
    CHECK(WAIT_FOR(({ mqtt_svc_test_result(st, sizeof(st)); strcmp(st, "running") != 0; }), 3000));
    CHECK_STR(st, "error:refused_credentials");
    BSET(connack_rc, 0);
    BSET(stall_connack, 1);
    CHECK(mqtt_svc_test_start() == 0);
    CHECK(WAIT_FOR(({ mqtt_svc_test_result(st, sizeof(st)); strcmp(st, "running") != 0; }), 3000));
    CHECK_STR(st, "error:timeout");
    BSET(stall_connack, 0);
    CHECK(WAIT_FOR(tasks() == t0, 2000) && fds() == f0);
    CHECK(BGET(protocol_errors) == 0);
}

int main(int argc, char **argv)
{
    char logfile[300];
    if (argc < 2) { fprintf(stderr, "usage: mqtt_svc_test WORKDIR\n"); return 2; }
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    snprintf(g_cfg, sizeof(g_cfg), "%s/mqtt", g_dir);
    snprintf(g_pass, sizeof(g_pass), "%s/mqtt_pass", g_dir);
    snprintf(logfile, sizeof(logfile), "%s/log.txt", g_dir);
    umask(022);
    mqtt_cfg_path = g_cfg;
    mqtt_pass_path = g_pass;
    mqtt_svc_tune = (mqtt_svc_tune_t){ 60, 480, 2, 100, 600, 400 };
    mqtt_svc_set_ops(&g_ops);

    /* everything the module prints goes to a file that is scanned for the password at the end */
    fflush(stdout);
    int saved = dup(1);
    int lf = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(lf, 1);
    close(lf);

    test_parser();
    broker_start();
    test_dormant();
    test_session();
    test_failures();
    test_oneshot();
    broker_stop();

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    /* nothing secret in the log, in any topic or payload the broker saw, or in any discovery document */
    {
        FILE *f = fopen(logfile, "r");
        char line[512];
        int leaked = 0, lines = 0;
        while (f && fgets(line, sizeof(line), f)) {
            lines++;
            if (strstr(line, SECRET) || strstr(line, "aa:bb:cc") || strstr(line, "aabbcc")) leaked = 1;
        }
        if (f) fclose(f);
        CHECK(lines > 5 && !leaked);
        for (int i = 0; i < B.npub; i++)
            CHECK(!strstr(B.pubs[i].payload, SECRET) && !strstr(B.pubs[i].topic, SECRET));
    }
    if (g_failed) {
        fprintf(stderr, "mqtt_svc_test: %d FAILED; module log:\n", g_failed);
        FILE *f = fopen(logfile, "r");
        char line[512];
        while (f && fgets(line, sizeof(line), f)) fputs(line, stderr);
        return 1;
    }
    printf("mqtt_svc_test: OK\n");
    return 0;
}
