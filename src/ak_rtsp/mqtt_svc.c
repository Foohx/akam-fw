/* See mqtt_svc.h. */
#define _GNU_SOURCE
#include "mqtt_svc.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "mqtt.h"

#define RX_CAP 640           /* biggest packet kept: a command is under 200 bytes; larger ones are skipped */
#define TX_CAP 1408          /* biggest packet sent: one discovery document (under 1024) plus its topic */
#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS 5000
#define THREAD_STACK 65536
#define DNS_WAIT_MS 20000    /* longest wait for a name lookup */

const char *mqtt_cfg_path = MQTT_CFG_PATH;
const char *mqtt_pass_path = MQTT_PASS_PATH;
mqtt_svc_tune_t mqtt_svc_tune = { 5000, 60000, 60, 2000, 10000, 10000 };

static const mqtt_ops_t *g_ops;
void mqtt_svc_set_ops(const mqtt_ops_t *ops) { g_ops = ops; }

/* ------------------------------------------------------------------------ config */

static int in_set(const char *s, size_t n, const char *extra, int allow_dot_hyphen)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (allow_dot_hyphen && (c == '.' || c == '-')) || (extra && c && strchr(extra, c));
        if (!ok) return 0;
    }
    return 1;
}

static int parse_num(const char *s, size_t n, long lo, long hi, long *out)
{
    long v = 0;
    if (n < 1 || n > 5) return -1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

enum { K_EN, K_HOST, K_PORT, K_USER, K_ID, K_BASE, K_DISC, K_PREFIX, K_INT, K_COUNT };
static const char *const k_names[K_COUNT] = { "enabled", "host", "port", "user", "client_id", "base_topic",
                                              "discovery", "discovery_prefix", "interval" };

int mqtt_cfg_parse(const char *text, size_t len, mqtt_cfg_t *out)
{
    mqtt_cfg_t c;
    unsigned seen = 0;
    size_t i = 0;
    int lines = 0;
    if (!text || len > MQTT_CFG_MAX) return -1;
    memset(&c, 0, sizeof(c));
    c.port = 1883;
    c.discovery = 1;
    c.interval = 30;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t n = e - i;
        if (n && text[i + n - 1] == '\r') n--;
        const char *l = text + i;
        const char *eq = memchr(l, '=', n);
        if (++lines > K_COUNT || memchr(l, '\0', n) || !eq) return -1;
        size_t kl = (size_t)(eq - l), vl = n - kl - 1;
        const char *v = eq + 1;
        int k;
        for (k = 0; k < K_COUNT; k++)
            if (strlen(k_names[k]) == kl && !memcmp(l, k_names[k], kl)) break;
        if (k == K_COUNT || (seen & (1u << k))) return -1;
        seen |= 1u << k;
        long x;
        switch (k) {
        case K_EN:
            if (vl != 1 || (v[0] != '0' && v[0] != '1')) return -1;
            c.enabled = v[0] - '0';
            break;
        case K_HOST:
            if (vl < 1 || vl > 63 || !in_set(v, vl, NULL, 1) || v[0] == '-' || v[0] == '.') return -1;
            memcpy(c.host, v, vl);
            break;
        case K_PORT:
            if (parse_num(v, vl, 1, 65535, &x) != 0) return -1;
            c.port = (int)x;
            break;
        case K_USER:
            if (vl > 32 || !in_set(v, vl, "._@+-", 0) || (vl == 1 && v[0] == '-')) return -1;   /* "-" means "none" in the helper */
            memcpy(c.user, v, vl);
            break;
        case K_ID:
            if (vl < 1 || vl > 32 || !in_set(v, vl, "_-", 0)) return -1;
            memcpy(c.client_id, v, vl);
            break;
        case K_BASE:
            if (vl > 64) return -1;
            memcpy(c.base, v, vl);
            if (!mqtt_ha_topic_ok(c.base, 64)) return -1;
            break;
        case K_PREFIX:
            if (vl > 32) return -1;
            memcpy(c.prefix, v, vl);
            if (!mqtt_ha_topic_ok(c.prefix, 32)) return -1;
            break;
        case K_DISC:
            if (vl == 2 && !memcmp(v, "on", 2)) c.discovery = 1;
            else if (vl == 3 && !memcmp(v, "off", 3)) c.discovery = 0;
            else return -1;
            break;
        case K_INT:
            if (parse_num(v, vl, 10, 300, &x) != 0) return -1;
            c.interval = (int)x;
            break;
        }
        i = e + 1;
    }
    if (!(seen & (1u << K_EN)) || !(seen & (1u << K_HOST))) return -1;
    *out = c;
    return 0;
}

int mqtt_cfg_load(mqtt_cfg_t *out)
{
    char buf[MQTT_CFG_MAX + 2];
    memset(out, 0, sizeof(*out));
    int fd = open(mqtt_cfg_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        printf("[mqtt] config file unreadable (%s): MQTT stays off\n", strerror(errno));
        return -1;
    }
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || n > MQTT_CFG_MAX || mqtt_cfg_parse(buf, (size_t)n, out) != 0) {
        printf("[mqtt] config file invalid: MQTT stays off (see akam_mqtt.sh status)\n");
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

int mqtt_pass_load(char *out, size_t cap)
{
    char buf[MQTT_PASS_MAX + 4];
    out[0] = '\0';
    int fd = open(mqtt_pass_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        printf("[mqtt] password file unreadable (%s)\n", strerror(errno));
        return -1;
    }
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    if (st.st_mode & 077) {
        close(fd);
        printf("[mqtt] password file is readable by other users (mode 0600 needed): ignored\n");
        return -1;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || n > MQTT_PASS_MAX + 1) { memset(buf, 0, sizeof(buf)); return -1; }
    if (n > 0 && buf[n - 1] == '\n') n--;
    int bad = (size_t)n >= cap;
    for (ssize_t i = 0; i < n && !bad; i++)
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] == 0x7f) bad = 1;
    if (!bad) { memcpy(out, buf, (size_t)n); out[n] = '\0'; }
    memset(buf, 0, sizeof(buf));
    if (bad) printf("[mqtt] password file has an invalid content: ignored\n");
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------------ state shared with the callers */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;     /* serialises reload / stop */
static pthread_mutex_t g_st = PTHREAD_MUTEX_INITIALIZER;       /* state, counters, test result */
static char g_state[MQTT_STATE_MAX] = "off";
static unsigned g_published, g_received, g_applied, g_rejected, g_connects;
static char g_test_res[MQTT_STATE_MAX] = "idle";
static int g_test_running;

static pthread_t g_tid;
static int g_running;
static int g_wake[2] = { -1, -1 };
static volatile int g_stop;
static mqtt_cfg_t g_cfg;
static char g_pass[MQTT_PASS_MAX + 1];
static mqtt_ha_t g_ha;
static uint8_t g_tx[TX_CAP];

static void set_state(const char *s)
{
    pthread_mutex_lock(&g_st);
    int changed = strcmp(g_state, s) != 0;
    snprintf(g_state, sizeof(g_state), "%s", s);
    pthread_mutex_unlock(&g_st);
    if (changed) printf("[mqtt] %s\n", s);
}

static void set_state_err(const char *reason)
{
    char s[MQTT_STATE_MAX];
    snprintf(s, sizeof(s), "error:%s", reason);
    set_state(s);
}

void mqtt_svc_state(char *state, size_t cap)
{
    pthread_mutex_lock(&g_st);
    snprintf(state, cap, "%s", g_state);
    pthread_mutex_unlock(&g_st);
}

void mqtt_svc_stats(char *out, size_t cap)
{
    pthread_mutex_lock(&g_st);
    snprintf(out, cap, "published=%u received=%u applied=%u rejected=%u connects=%u", g_published, g_received,
             g_applied, g_rejected, g_connects);
    pthread_mutex_unlock(&g_st);
}

void mqtt_svc_test_result(char *out, size_t cap)
{
    pthread_mutex_lock(&g_st);
    snprintf(out, cap, "%s", g_test_res);
    pthread_mutex_unlock(&g_st);
}

/* ------------------------------------------------------------------------ connection */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
    int     fd;
    int     wake;                 /* read end of the wake pipe, -1 for the one-shot test */
    uint8_t rx[RX_CAP];
    size_t  rxlen, skip;
    int64_t last_rx;
} conn_t;

/* Wait for events on fd (and the wake pipe): 1 ready, 0 timeout, -1 error, -2 stop requested. */
static int wait_fd(int fd, short ev, int timeout_ms, int wake)
{
    struct pollfd p[2];
    int n = 0;
    p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++;
    if (wake >= 0) { p[n].fd = wake; p[n].events = POLLIN; p[n].revents = 0; n++; }
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        int64_t left = end - now_ms();
        if (left < 0) left = 0;
        int r = poll(p, (nfds_t)n, (int)left);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0;
        if (n == 2 && (p[1].revents & POLLIN)) return -2;
        if (p[0].revents & (ev | POLLERR | POLLHUP | POLLNVAL)) return 1;
    }
}

#ifndef MQTT_NO_DNS
/* TEST-ONLY hook (mqtt_svc.h): replaces the system resolver. */
int (*mqtt_svc_resolver)(const char *host, struct in_addr *out);

static int sys_resolve(const char *host, struct in_addr *out)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    *out = ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* A name lookup can block for 10 to 30 s on a dead resolver and cannot be cancelled, so it runs in a short detached
 * thread that fills a heap slot; the MQTT thread polls the slot together with the wake pipe. A stop or reload therefore
 * never waits for DNS: the helper is abandoned and frees the slot itself when it finally returns. */
typedef struct {
    pthread_mutex_t mu;
    int             done, abandoned, ok;
    struct in_addr  addr;
    char            host[64];
} dns_job_t;

static void dns_job_free(dns_job_t *j)
{
    pthread_mutex_destroy(&j->mu);
    free(j);
}

static void *dns_main(void *arg)
{
    dns_job_t *j = arg;
    struct in_addr a;
    int ok = (mqtt_svc_resolver ? mqtt_svc_resolver : sys_resolve)(j->host, &a) == 0;
    pthread_mutex_lock(&j->mu);
    j->ok = ok;
    if (ok) j->addr = a;
    j->done = 1;
    int drop = j->abandoned;
    pthread_mutex_unlock(&j->mu);
    if (drop) dns_job_free(j);
    return NULL;
}

static int resolve_name(const char *host, int wake, struct in_addr *out, const char **reason)
{
    pthread_t t;
    pthread_attr_t at;
    dns_job_t *j = calloc(1, sizeof(*j));
    if (!j) { *reason = "dns"; return -1; }
    pthread_mutex_init(&j->mu, NULL);
    snprintf(j->host, sizeof(j->host), "%s", host);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, THREAD_STACK);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, dns_main, j);
    pthread_attr_destroy(&at);
    if (rc != 0) { dns_job_free(j); *reason = "dns"; return -1; }
    int64_t end = now_ms() + DNS_WAIT_MS;
    int res = -1, stopped = 0;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        int done = j->done;
        pthread_mutex_unlock(&j->mu);
        if (done || now_ms() >= end) break;
        struct pollfd p = { wake, POLLIN, 0 };
        (void)poll(&p, wake >= 0 ? 1 : 0, 20);
        if (wake >= 0 && (p.revents & POLLIN)) { stopped = 1; break; }
    }
    pthread_mutex_lock(&j->mu);
    if (j->done) {
        if (j->ok) { *out = j->addr; res = 0; }
        pthread_mutex_unlock(&j->mu);
        dns_job_free(j);
    } else {
        j->abandoned = 1;                         /* the helper frees the slot when the lookup returns */
        pthread_mutex_unlock(&j->mu);
    }
    if (res != 0) *reason = stopped ? "stopped" : "dns";
    return res;
}
#endif

/* The broker address: an IPv4 literal, or (unless built with -DMQTT_NO_DNS, which saves about 16 KB of the static
 * binary) a name that the system resolver knows. 0 and *sa, or -1 with *reason. */
static int resolve(const mqtt_cfg_t *cfg, int wake, struct sockaddr_in *sa, const char **reason)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->host, &sa->sin_addr) == 1) return 0;
#ifdef MQTT_NO_DNS
    (void)wake;
    *reason = "dns";
    return -1;
#else
    return resolve_name(cfg->host, wake, &sa->sin_addr, reason);
#endif
}

static int dial(const mqtt_cfg_t *cfg, int wake, const char **reason)
{
    struct sockaddr_in sa;
    if (resolve(cfg, wake, &sa, reason) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { *reason = "connect"; return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) { close(fd); *reason = "connect"; return -1; }
    if (r < 0) {
        int w = wait_fd(fd, POLLOUT, CONNECT_TIMEOUT_MS, wake);
        int err = 0;
        socklen_t el = sizeof(err);
        if (w == -2) { close(fd); *reason = "stopped"; return -1; }
        if (w == 0) { close(fd); *reason = "timeout"; return -1; }
        if (w < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) { close(fd); *reason = "connect"; return -1; }
    }
    return fd;
}

/* Send everything, waiting at most SEND_TIMEOUT_MS for a slow broker. 0, or -1 (error, timeout, stop). */
static int send_all(conn_t *c, const uint8_t *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(c->fd, b + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(c->fd, POLLOUT, SEND_TIMEOUT_MS, c->wake) != 1) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* Read what is there (waiting up to timeout_ms): >= 0 bytes read (0 = timeout), -1 closed or error, -2 stop. Bytes
 * of a packet that is too large for rx are thrown away (c->skip) instead of stored. */
static int pump(conn_t *c, int timeout_ms)
{
    int w = wait_fd(c->fd, POLLIN, timeout_ms, c->wake);
    if (w <= 0) return w;
    uint8_t tmp[RX_CAP];
    ssize_t n;
    do { n = recv(c->fd, tmp, c->skip ? sizeof(tmp) : RX_CAP - c->rxlen, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (n == 0) return -1;
    c->last_rx = now_ms();
    if (c->skip) {
        size_t drop = (size_t)n < c->skip ? (size_t)n : c->skip;
        c->skip -= drop;
        if ((size_t)n > drop) {                       /* the next packet starts inside this read */
            memcpy(c->rx + c->rxlen, tmp + drop, (size_t)n - drop);
            c->rxlen += (size_t)n - drop;
        }
        return (int)n;
    }
    memcpy(c->rx + c->rxlen, tmp, (size_t)n);
    c->rxlen += (size_t)n;
    return (int)n;
}

static void consume(conn_t *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rxlen - n);
    c->rxlen -= n;
}

/* The next whole packet in rx: 1 (p filled, drop it with consume()), 0 none yet, -1 malformed stream. */
static int next_pkt(conn_t *c, mqtt_pkt_t *p)
{
    int r = mqtt_frame(c->rx, c->rxlen, RX_CAP - 5, p);
    if (r > 0) return 1;
    if (r == -2) {                                    /* well formed but too big to keep: skip it */
        if (p->total <= c->rxlen) {                   /* all of it is here: the next packet may start right behind */
            consume(c, p->total);
            return next_pkt(c, p);
        }
        c->skip = p->total - c->rxlen;
        c->rxlen = 0;
        return 0;
    }
    return r;
}

static const char *connack_reason(int rc)
{
    switch (rc) {
    case 1: return "refused_protocol";
    case 2: return "refused_id";
    case 3: return "refused_unavailable";
    case 4: return "refused_credentials";
    case 5: return "refused_not_authorized";
    default: return "refused";
    }
}

/* Send CONNECT and wait for the CONNACK. 0 = accepted, else -1 with *reason. */
static int handshake(conn_t *c, uint8_t *tx, size_t txcap, const mqtt_conn_t *mc, const char **reason)
{
    int n = mqtt_enc_connect(tx, txcap, mc);
    if (n < 0) { *reason = "config"; return -1; }
    if (send_all(c, tx, (size_t)n) != 0) { *reason = "closed"; return -1; }
    int64_t end = now_ms() + mqtt_svc_tune.connack_ms;
    for (;;) {
        mqtt_pkt_t p;
        int r = next_pkt(c, &p);
        if (r < 0) { *reason = "protocol"; return -1; }
        if (r > 0) {
            int sp, rc;
            if (mqtt_dec_connack(&p, &sp, &rc) != 0) { *reason = "protocol"; return -1; }
            if (rc != 0) { *reason = connack_reason(rc); return -1; }
            consume(c, p.total);
            return 0;
        }
        int64_t left = end - now_ms();
        if (left <= 0) { *reason = "timeout"; return -1; }
        r = pump(c, (int)left);
        if (r == -2) { *reason = "stopped"; return -1; }
        if (r < 0) { *reason = "closed"; return -1; }
    }
}

/* ------------------------------------------------------------------------ the session */

static int pub(conn_t *c, const char *topic, const char *payload, int retain)
{
    int n = mqtt_enc_publish(g_tx, sizeof(g_tx), topic, payload, strlen(payload), retain);
    if (n < 0) return 0;                              /* does not fit: skipped, not a connection error */
    if (send_all(c, g_tx, (size_t)n) != 0) return -1;
    pthread_mutex_lock(&g_st);
    g_published++;
    pthread_mutex_unlock(&g_st);
    return 0;
}

static int pub_state(conn_t *c, size_t i, int force)
{
    char t[MQTT_HA_TOPIC_MAX], v[MQTT_HA_VAL_MAX];
    int retain;
    if (!mqtt_ha_state(&g_ha, i, force, t, sizeof(t), v, sizeof(v), &retain)) return 0;
    return pub(c, t, v, retain);
}

static void sync_counters(void)
{
    pthread_mutex_lock(&g_st);
    g_applied = g_ha.applied;
    g_rejected = g_ha.invalid;
    pthread_mutex_unlock(&g_st);
}

/* After CONNACK: availability, discovery, the command subscription, every state. */
static int announce(conn_t *c)
{
    char t[MQTT_HA_TOPIC_MAX], f[MQTT_HA_TOPIC_MAX];
    char doc[MQTT_HA_PAYLOAD_MAX];
    size_t n = mqtt_ha_count();
    if (mqtt_ha_status_topic(&g_ha, t, sizeof(t)) < 0 || mqtt_ha_cmd_filter(&g_ha, f, sizeof(f)) < 0) return -1;
    if (pub(c, t, "online", 1) != 0) return -1;
    if (g_ha.discovery) {
        for (size_t i = 0; i < n; i++) {
            int dl = mqtt_ha_discovery(&g_ha, i, t, sizeof(t), doc, sizeof(doc));
            if (dl > 0 && pub(c, t, doc, 1) != 0) return -1;
        }
    }
    int sn = mqtt_enc_subscribe(g_tx, sizeof(g_tx), 1, f);
    if (sn < 0 || send_all(c, g_tx, (size_t)sn) != 0) return -1;
    for (size_t i = 0; i < n; i++)
        if (pub_state(c, i, 1) != 0) return -1;
    return 0;
}

static int handle_packet(conn_t *c, const mqtt_pkt_t *p)
{
    switch (p->type) {
    case MQTT_PUBLISH: {
        mqtt_pub_t m;
        if (mqtt_dec_publish(p, &m) != 0) return -1;
        pthread_mutex_lock(&g_st);
        g_received++;
        pthread_mutex_unlock(&g_st);
        if (m.qos == 1) {
            uint8_t ack[4];
            if (mqtt_enc_puback(ack, sizeof(ack), m.pid) < 0 || send_all(c, ack, 4) != 0) return -1;
        }
        if (m.qos == 2) return 0;                     /* never requested */
        int idx = mqtt_ha_command(&g_ha, m.topic, m.tlen, m.payload, m.plen, m.retain);
        sync_counters();
        if (idx >= 0 && pub_state(c, (size_t)idx, 1) != 0) return -1;
        if (idx < 0 && g_ha.reject_idx >= 0 && pub_state(c, (size_t)g_ha.reject_idx, 1) != 0) return -1;   /* HA shows the real value */
        return 0;
    }
    case MQTT_PINGRESP: case MQTT_SUBACK: case MQTT_PUBACK: case 5: case 7: case MQTT_UNSUBACK:
        return 0;
    default:
        return -1;                                    /* CONNECT, SUBSCRIBE, PINGREQ... from a broker: not MQTT */
    }
}

static void say_bye(conn_t *c)
{
    char t[MQTT_HA_TOPIC_MAX];
    if (mqtt_ha_status_topic(&g_ha, t, sizeof(t)) >= 0) {
        int n = mqtt_enc_publish(g_tx, sizeof(g_tx), t, "offline", 7, 1);
        if (n > 0) (void)send(c->fd, g_tx, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    int n = mqtt_enc_disconnect(g_tx, sizeof(g_tx));
    if (n > 0) (void)send(c->fd, g_tx, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* One connection, from dial to loss or stop. Returns 1 when the broker accepted the connection (the backoff then
 * starts again from its minimum), else 0; reason says why it ended (empty on a requested stop). */
static int session(const mqtt_cfg_t *cfg, const char *pass, char *reason, size_t rcap)
{
    conn_t *c = calloc(1, sizeof(*c));                /* 650 bytes: not on the stack */
    const char *why = "closed";
    int accepted = 0;
    reason[0] = '\0';
    if (!c) { snprintf(reason, rcap, "connect"); return 0; }
    c->fd = -1;
    c->wake = g_wake[0];

    char status[MQTT_HA_TOPIC_MAX], id[40];
    if (mqtt_ha_init(&g_ha, g_ops, cfg->base, cfg->prefix, cfg->discovery) != 0 ||
        mqtt_ha_status_topic(&g_ha, status, sizeof(status)) < 0) {
        snprintf(reason, rcap, "config");
        free(c);
        return 0;
    }
    snprintf(id, sizeof(id), "%s", cfg->client_id[0] ? cfg->client_id : g_ha.host);

    c->fd = dial(cfg, c->wake, &why);
    if (c->fd < 0) {
        if (strcmp(why, "stopped")) snprintf(reason, rcap, "%s", why);
        free(c);
        return 0;
    }
    mqtt_conn_t mc;
    memset(&mc, 0, sizeof(mc));
    mc.client_id = id;
    mc.user = cfg->user;
    mc.pass = cfg->user[0] ? pass : NULL;           /* a password needs a user name (MQTT 3.1.1) */
    mc.keepalive = (uint16_t)mqtt_svc_tune.keepalive_s;
    mc.clean = 1;
    mc.will_topic = status;
    mc.will_msg = "offline";
    mc.will_retain = 1;
    if (handshake(c, g_tx, sizeof(g_tx), &mc, &why) != 0) {
        if (strcmp(why, "stopped")) snprintf(reason, rcap, "%s", why);
        close(c->fd);
        free(c);
        return 0;
    }
    accepted = 1;
    set_state("connected");
    pthread_mutex_lock(&g_st);
    g_connects++;
    pthread_mutex_unlock(&g_st);

    int64_t ka_ms = (int64_t)mqtt_svc_tune.keepalive_s * 1000;
    int64_t now = now_ms();
    int64_t next_tick = now + mqtt_svc_tune.tick_ms;
    int64_t next_sensor = now + (int64_t)cfg->interval * 1000;
    int64_t ping_sent = 0;
    size_t n = mqtt_ha_count();
    c->last_rx = now;
    why = "closed";
    if (announce(c) != 0) goto out;

    while (!g_stop) {
        int64_t wake_at = next_tick < next_sensor ? next_tick : next_sensor;
        int64_t ping_at = ping_sent ? ping_sent + mqtt_svc_tune.ping_reply_ms : c->last_rx + ka_ms / 2;
        if (ping_at < wake_at) wake_at = ping_at;
        now = now_ms();
        int wait = wake_at > now ? (int)(wake_at - now) : 0;
        int r = pump(c, wait);
        if (r == -2) { why = ""; break; }
        if (r < 0) goto out;
        if (r > 0) ping_sent = 0;                     /* anything from the broker proves it is alive */
        for (;;) {
            mqtt_pkt_t p;
            int q = next_pkt(c, &p);
            if (q < 0) { why = "protocol"; goto out; }
            if (q == 0) break;
            if (handle_packet(c, &p) != 0) { why = "protocol"; goto out; }
            consume(c, p.total);
        }
        now = now_ms();
        if (ping_sent && now - ping_sent >= mqtt_svc_tune.ping_reply_ms) { why = "timeout"; goto out; }
        if (!ping_sent && now - c->last_rx >= ka_ms / 2) {
            int pn = mqtt_enc_pingreq(g_tx, sizeof(g_tx));
            if (pn < 0 || send_all(c, g_tx, (size_t)pn) != 0) goto out;
            ping_sent = now;
        }
        if (now >= next_tick) {
            for (size_t i = 0; i < n; i++)
                if (!mqtt_ha_is_periodic(i) && pub_state(c, i, 0) != 0) goto out;
            next_tick = now + mqtt_svc_tune.tick_ms;
        }
        if (now >= next_sensor) {
            for (size_t i = 0; i < n; i++)
                if (mqtt_ha_is_periodic(i) && pub_state(c, i, 1) != 0) goto out;
            next_sensor = now + (int64_t)cfg->interval * 1000;
        }
    }
    why = "";
    say_bye(c);
out:
    snprintf(reason, rcap, "%s", why);
    close(c->fd);
    free(c);
    return accepted;
}

static void sleep_wake(unsigned ms)
{
    (void)wait_fd(g_wake[0], POLLIN, (int)ms, -1);    /* readable wake pipe = stop */
}

static void *thread_main(void *arg)
{
    unsigned attempt = 0;
    uint32_t rng = (uint32_t)now_ms() ^ ((uint32_t)getpid() << 8);
    char reason[24];
    (void)arg;
    /* below the video threads: a busy single core serves the stream first */
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
    set_state("connecting");
    while (!g_stop) {
        int ok = session(&g_cfg, g_pass, reason, sizeof(reason));
        if (g_stop) break;
        if (ok) attempt = 0;
        set_state_err(reason[0] ? reason : "closed");
        rng = rng * 1664525u + 1013904223u;
        unsigned d = mqtt_backoff_ms(attempt, rng >> 8, mqtt_svc_tune.backoff_min_ms, mqtt_svc_tune.backoff_max_ms);
        if (attempt < 30) attempt++;
        sleep_wake(d);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ start / stop */

static void stop_locked(void)
{
    if (!g_running) return;
    g_stop = 1;
    if (write(g_wake[1], "x", 1) < 0) { /* the pipe is only a wake-up: a full pipe is already awake */ }
    pthread_join(g_tid, NULL);
    close(g_wake[0]);
    close(g_wake[1]);
    g_wake[0] = g_wake[1] = -1;
    memset(g_pass, 0, sizeof(g_pass));
    g_running = 0;
}

static int start_locked(const mqtt_cfg_t *cfg, const char *pass)
{
    pthread_attr_t at;
    if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    g_cfg = *cfg;
    snprintf(g_pass, sizeof(g_pass), "%s", pass);
    g_stop = 0;
    pthread_mutex_lock(&g_st);
    g_published = g_received = g_applied = g_rejected = g_connects = 0;
    pthread_mutex_unlock(&g_st);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, THREAD_STACK);
    int rc = pthread_create(&g_tid, &at, thread_main, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        close(g_wake[0]);
        close(g_wake[1]);
        g_wake[0] = g_wake[1] = -1;
        memset(g_pass, 0, sizeof(g_pass));
        return -1;
    }
    g_running = 1;
    return 0;
}

int mqtt_svc_reload(char *state, size_t cap)
{
    mqtt_cfg_t cfg;
    char pass[MQTT_PASS_MAX + 1];
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    int ld = mqtt_cfg_load(&cfg);
    stop_locked();
    if (ld != 0 || !cfg.enabled || !g_ops) {
        set_state("off");
    } else if (mqtt_pass_load(pass, sizeof(pass)) != 0) {
        set_state("off");
        rc = -1;
    } else if (start_locked(&cfg, pass) != 0) {
        set_state_err("thread");
        rc = -1;
    } else {
        set_state("connecting");
    }
    memset(pass, 0, sizeof(pass));
    if (state && cap) mqtt_svc_state(state, cap);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void mqtt_svc_stop(void)
{
    pthread_mutex_lock(&g_lock);
    stop_locked();
    set_state("off");
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------------ one-shot test */

static mqtt_cfg_t g_tcfg;
static char g_tpass[MQTT_PASS_MAX + 1];

static void *test_main(void *arg)
{
    const char *why = "closed";
    conn_t *c = calloc(1, sizeof(*c));
    uint8_t tx[512];
    char id[40], res[MQTT_STATE_MAX];
    (void)arg;
    snprintf(res, sizeof(res), "error:connect");
    if (c) {
        char hn[33] = "";
        c->wake = -1;
        c->fd = dial(&g_tcfg, -1, &why);
        if (c->fd >= 0) {
            if (g_ops && !g_tcfg.client_id[0]) g_ops->hostname(hn);
            snprintf(id, sizeof(id), "%.27s-test", g_tcfg.client_id[0] ? g_tcfg.client_id : hn[0] ? hn : "akam");
            mqtt_conn_t mc;
            memset(&mc, 0, sizeof(mc));
            mc.client_id = id;
            mc.user = g_tcfg.user;
            mc.pass = g_tcfg.user[0] ? g_tpass : NULL;
            mc.keepalive = 30;
            mc.clean = 1;
            c->last_rx = now_ms();
            if (handshake(c, tx, sizeof(tx), &mc, &why) == 0) {
                int n = mqtt_enc_disconnect(tx, sizeof(tx));
                if (n > 0) (void)send(c->fd, tx, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
                why = NULL;
            }
            close(c->fd);
        }
        free(c);
    }
    if (!why) snprintf(res, sizeof(res), "ok");
    else snprintf(res, sizeof(res), "error:%s", why);
    memset(g_tpass, 0, sizeof(g_tpass));
    pthread_mutex_lock(&g_st);
    snprintf(g_test_res, sizeof(g_test_res), "%s", res);
    g_test_running = 0;
    pthread_mutex_unlock(&g_st);
    return NULL;
}

int mqtt_svc_test_start(void)
{
    pthread_t t;
    pthread_attr_t at;
    mqtt_cfg_t cfg;
    char pass[MQTT_PASS_MAX + 1];
    pthread_mutex_lock(&g_st);
    if (g_test_running) { pthread_mutex_unlock(&g_st); return 0; }
    pthread_mutex_unlock(&g_st);
    if (mqtt_cfg_load(&cfg) != 0 || mqtt_pass_load(pass, sizeof(pass)) != 0) {
        pthread_mutex_lock(&g_st);
        snprintf(g_test_res, sizeof(g_test_res), "error:config");
        pthread_mutex_unlock(&g_st);
        return -1;
    }
    pthread_mutex_lock(&g_st);
    if (g_test_running) { pthread_mutex_unlock(&g_st); return 0; }
    g_test_running = 1;
    snprintf(g_test_res, sizeof(g_test_res), "running");
    g_tcfg = cfg;
    snprintf(g_tpass, sizeof(g_tpass), "%s", pass);
    pthread_mutex_unlock(&g_st);
    memset(pass, 0, sizeof(pass));
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, THREAD_STACK);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, test_main, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        pthread_mutex_lock(&g_st);
        g_test_running = 0;
        snprintf(g_test_res, sizeof(g_test_res), "error:thread");
        memset(g_tpass, 0, sizeof(g_tpass));
        pthread_mutex_unlock(&g_st);
        return -1;
    }
    return 0;
}
