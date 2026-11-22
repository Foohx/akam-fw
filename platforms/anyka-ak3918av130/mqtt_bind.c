/* See mqtt_bind.h. The only app file with connect(). Host-compilable (the unit test replaces the network). */
#define _GNU_SOURCE
#include "api_events.h"
#include "mqtt_bind.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "mqtt.h"
#include "orient.h"
#include "ptz_vel.h"

#define RX_CAP 512            /* a broker sends CONNACK, PINGRESP, PUBACK: anything bigger is not for us */
#define TX_CAP 1408           /* one discovery document (under 1024) plus its topic */
#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS 5000
#define PING_REPLY_MS 10000
#define CONNACK_MS 10000
#define KEEPALIVE_S 60
#define BACKOFF_MIN_MS 5000
#define BACKOFF_MAX_MS 60000
#define STABLE_MS 60000
#define STACK_BYTES 65536
#define STOP_WAIT_MS 3000

/* ------------------------------------------------------------------------ the real network */

static int g_wake[2] = { -1, -1 };
static volatile int g_stop;

static int64_t real_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void drain_wake(void)
{
    char b[16];
    while (g_wake[0] >= 0 && read(g_wake[0], b, sizeof b) > 0) {
    }
}

/* 1 ready, 0 timeout (or a wake-up), -1 error, -2 stop requested. */
static int wait_fd(int fd, short ev, int timeout_ms)
{
    struct pollfd p[2];
    int n = 0, r;
    p[n].fd = fd;
    p[n].events = ev;
    p[n].revents = 0;
    n++;
    if (g_wake[0] >= 0) {
        p[n].fd = g_wake[0];
        p[n].events = POLLIN;
        p[n].revents = 0;
        n++;
    }
    if (g_stop)
        return -2;
    r = poll(p, (nfds_t)n, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (n == 2 && p[1].revents) {
        drain_wake();
        if (g_stop)
            return -2;
    }
    if (r == 0 || !(p[0].revents & (ev | POLLERR | POLLHUP | POLLNVAL)))
        return 0;
    return 1;
}

static int real_dial(const a2k_mqtt_cfg_t *cfg, char *local_ip, size_t lcap, const char **reason)
{
    struct sockaddr_in sa, la;
    socklen_t ll = sizeof la;
    int fd, one = 1, r;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->host, &sa.sin_addr) != 1) {
        /* a name: the LAN resolver only (decloud blackholes the WAN). This call can block for the resolver's timeout:
         * it only ever blocks this thread. */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(cfg->host, NULL, &hints, &res) != 0 || !res) {
            *reason = "dns";
            return -1;
        }
        sa.sin_addr = ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        if (g_stop) {
            *reason = "stopped";
            return -1;
        }
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *reason = "connect";
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        *reason = "connect";
        return -1;
    }
    if (r < 0) {
        int w = wait_fd(fd, POLLOUT, CONNECT_TIMEOUT_MS), err = 0;
        socklen_t el = sizeof err;
        if (w == -2 || w == 0 || w < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            close(fd);
            *reason = w == -2 ? "stopped" : w == 0 ? "timeout" : "connect";
            return -1;
        }
    }
    local_ip[0] = '\0';
    if (getsockname(fd, (struct sockaddr *)&la, &ll) == 0 && la.sin_family == AF_INET)
        inet_ntop(AF_INET, &la.sin_addr, local_ip, (socklen_t)lcap);
    return fd;
}

static int real_send(int fd, const uint8_t *b, size_t n)
{
    size_t off = 0;
    int64_t end = real_now() + SEND_TIMEOUT_MS; /* one deadline for the whole packet: a trickling broker cannot stretch it */
    while (off < n) {
        ssize_t w = send(fd, b + off, n - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int64_t left = end - real_now();
            if (left <= 0 || wait_fd(fd, POLLOUT, (int)left) != 1)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int real_recv(int fd, uint8_t *b, size_t cap, int timeout_ms)
{
    int w = wait_fd(fd, POLLIN, timeout_ms);
    ssize_t n;
    if (w <= 0)
        return w;
    do {
        n = recv(fd, b, cap, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return n == 0 ? -1 : (int)n;
}

static void real_close(int fd) { close(fd); }

static void real_sleep(unsigned ms)
{
    struct pollfd p;
    int64_t end = real_now() + ms;
    p.fd = g_wake[0];
    p.events = POLLIN;
    while (!g_stop) {
        int64_t left = end - real_now();
        if (left <= 0)
            break;
        p.revents = 0;
        if (poll(&p, 1, (int)left) > 0)
            drain_wake();
    }
}

static const char g_none[1] = { 0 }; /* (the repo C scan forbids the empty string literal) */

static const a2k_mqtt_net_t g_real_net = { real_dial, real_send, real_recv, real_close, real_now, real_sleep };
static const a2k_mqtt_net_t *g_net = &g_real_net;

void a2k_mqtt_test_set_net(const a2k_mqtt_net_t *n) { g_net = n ? n : &g_real_net; }

/* ------------------------------------------------------------------------ shared state */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct a2k_mqtt_det g_cur;                 /* latest report */
static struct a2k_mqtt_det g_ring[A2K_MQTT_RING]; /* transitions not published yet, oldest first */
static int g_ring_n;
static int g_prev_present = -1;
static int64_t g_last_on;
static char g_state[40] = "off";
static volatile int g_running, g_done;
static pthread_t g_tid;
static a2k_mqtt_cfg_t g_cfg;
static char g_mac_path[128];
static int g_rtsp_port;
static a2k_mqtt_viewers_fn g_viewers;
static uint8_t g_tx[TX_CAP];
static a2k_zones_t g_zones;                       /* set before start; names only are used */
static int g_snap_enabled, g_night_enabled, g_sys_enabled;
static a2k_mqtt_leds_t g_leds;                    /* get == NULL: no controls */
static a2k_mqtt_ctl_t g_ctl;                      /* get == NULL: no ctl entities */
static int g_resync, g_btn_n;                     /* under g_mu: announce the zones / areas again; button presses to publish */
#define BTN_Q 4
static char g_bq[BTN_Q][A2K_MQTT_GESTURE_MAX + 1]; /* under g_mu: gesture messages to publish on BASE/button */
static int g_bq_n;
static int (*g_btn_list)(char names[][A2K_BTN_NAME]); /* the configured gestures (device triggers), NULL = none */
static a2k_mqtt_vid_t g_vid;                      /* get == NULL: no video settings entities */
static a2k_mqtt_ptz_t g_ptz;                      /* press == NULL: no PTZ entities */
#define SNAP_HDR_ROOM 176                         /* room before the JPEG for the PUBLISH header (1 + 4 + 2 + topic) */
static uint8_t *g_snap;                           /* the newest snapshot JPEG not published yet: own copy, SNAP_HDR_ROOM bytes in */
static size_t g_snap_len;
static unsigned g_snap_sent, g_snap_dropped;
static char g_det[A2K_MQTT_DET_MAX + 1];          /* the newest detections list not published yet ("" = none) */
static unsigned g_det_sent, g_det_dropped;

static void set_state(const char *s)
{
    int changed;
    pthread_mutex_lock(&g_mu);
    changed = strcmp(g_state, s) != 0;
    snprintf(g_state, sizeof g_state, "%s", s);
    pthread_mutex_unlock(&g_mu);
    if (changed)
        a2k_log("mqtt: %s", s);
}

void a2k_mqtt_conn_state(char *out, size_t cap)
{
    pthread_mutex_lock(&g_mu);
    snprintf(out, cap, "%s", g_state);
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_set_zones(const a2k_zones_t *z)
{
    memset(&g_zones, 0, sizeof g_zones);
    if (z)
        g_zones = *z;
}

void a2k_mqtt_set_snapshot(int enabled) { g_snap_enabled = enabled ? 1 : 0; }
void a2k_mqtt_set_night(int enabled) { g_night_enabled = enabled ? 1 : 0; }
void a2k_mqtt_set_leds(const a2k_mqtt_leds_t *ops)
{
    memset(&g_leds, 0, sizeof g_leds);
    if (ops && ops->get && ops->set)
        g_leds = *ops;
}

void a2k_mqtt_set_ctl(const a2k_mqtt_ctl_t *ops)
{
    memset(&g_ctl, 0, sizeof g_ctl);
    if (ops && ops->get && ops->set && ops->areas)
        g_ctl = *ops;
}

void a2k_mqtt_set_vid(const a2k_mqtt_vid_t *ops)
{
    memset(&g_vid, 0, sizeof g_vid);
    if (ops && ops->get && ops->set)
        g_vid = *ops;
}

static void wake_locked(void)
{
    if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
    }
}

void a2k_mqtt_zones_changed(const a2k_zones_t *z)
{
    pthread_mutex_lock(&g_mu);
    memset(&g_zones, 0, sizeof g_zones);
    if (z)
        g_zones = *z;
    g_resync = 1;
    if (g_running)
        wake_locked();
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_resync(void)
{
    pthread_mutex_lock(&g_mu);
    g_resync = 1;
    if (g_running)
        wake_locked();
    pthread_mutex_unlock(&g_mu);
}

static void orient_resync(int flip, int mirror)
{
    (void)flip;
    (void)mirror;
    a2k_mqtt_resync();
}

int a2k_mqtt_follow_orientation(void) { return a2k_orient_listen(orient_resync); }

void a2k_mqtt_set_buttons(int (*list)(char names[][A2K_BTN_NAME])) { g_btn_list = list; }

void a2k_mqtt_button_gesture(const char *gesture, const char *json)
{
    if (!gesture || !json || strlen(json) > A2K_MQTT_GESTURE_MAX)
        return;
    pthread_mutex_lock(&g_mu);
    if (g_running && g_ctl.get) {
        if (g_bq_n < BTN_Q)
            snprintf(g_bq[g_bq_n++], sizeof g_bq[0], "%s", json);
        if (strcmp(gesture, "short") == 0 && g_btn_n < 4)
            g_btn_n++; /* the Home Assistant `event` entity "Button" keeps its press event for the short press */
        wake_locked();
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_button_event(const char *type)
{
    if (!type || strcmp(type, "press") != 0)
        return;
    pthread_mutex_lock(&g_mu);
    if (g_running && g_ctl.get) {
        if (g_btn_n < 4)
            g_btn_n++;
        wake_locked();
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_set_ptz(const a2k_mqtt_ptz_t *ops)
{
    memset(&g_ptz, 0, sizeof g_ptz);
    if (ops && ops->press)
        g_ptz = *ops;
}

static void fill_leds(struct a2k_mqtt_det *d)
{
    d->led_valid = 0;
    for (int k = 0; g_leds.get && k < A2K_LED_N; k++) {
        d->led[k][0] = '\0';
        if (g_leds.get(k, d->led[k], A2K_LED_VAL) != 0)
            d->led[k][0] = '\0';
        else
            d->led_valid = 1;
    }
}

void a2k_mqtt_set_sys(int enabled) { g_sys_enabled = enabled ? 1 : 0; }

void a2k_mqtt_sys(const hal_sys_sample_t *s)
{
    pthread_mutex_lock(&g_mu);
    g_cur.sys = *s;
    g_cur.sys_valid = 1;
    pthread_mutex_unlock(&g_mu); /* published at the next periodic republish: no wake-up, no ring entry */
}

void a2k_mqtt_detections(const char *json)
{
    size_t n = json ? strlen(json) : 0;
    pthread_mutex_lock(&g_mu);
    if (g_running) {
        if (n == 0 || n > A2K_MQTT_DET_MAX)
            g_det_dropped++;
        else {
            if (g_det[0])
                g_det_dropped++; /* the newest wins */
            memcpy(g_det, json, n + 1);
            wake_locked();
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_det_stats(unsigned *sent, unsigned *dropped)
{
    pthread_mutex_lock(&g_mu);
    *sent = g_det_sent;
    *dropped = g_det_dropped;
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_snap_stats(unsigned *sent, unsigned *dropped)
{
    pthread_mutex_lock(&g_mu);
    *sent = g_snap_sent;
    *dropped = g_snap_dropped;
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_snapshot(const uint8_t *jpeg, size_t len)
{
    uint8_t *copy = NULL;
    if (jpeg && len > 0 && len <= A2K_MQTT_SNAP_MAX)
        copy = malloc(SNAP_HDR_ROOM + len);
    if (copy)
        memcpy(copy + SNAP_HDR_ROOM, jpeg, len);
    pthread_mutex_lock(&g_mu);
    if (!copy || !g_running || strcmp(g_state, "connected") != 0) {
        g_snap_dropped++;
        pthread_mutex_unlock(&g_mu);
        free(copy);
        return;
    }
    free(g_snap); /* an older snapshot nobody sent yet: the newest wins */
    g_snap = copy;
    g_snap_len = len;
    if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_detection(const a2k_det_t *d, const char *status, int64_t epoch)
{
    struct a2k_mqtt_det s;
    int changed;
    memset(&s, 0, sizeof s);
    a2k_api_event_person(d->present ? 1 : 0, d->present ? d->last_box.score : 0);
    s.valid = 1;
    s.present = d->present ? 1 : 0;
    if (s.present) {
        s.score = d->last_box.score;
        s.cls = d->last_box.cls;
        s.x1 = d->last_box.x1;
        s.y1 = d->last_box.y1;
        s.x2 = d->last_box.x2;
        s.y2 = d->last_box.y2;
    }
    s.zmask = d->zone_mask;
    s.heads = d->heads;
    s.head_score = d->head_score;
    snprintf(s.status, sizeof s.status, "%s", status ? status : "run");
    pthread_mutex_lock(&g_mu);
    if (s.present && g_prev_present != 1)
        g_last_on = epoch;
    g_prev_present = s.present;
    s.last_on_epoch = g_last_on;
    s.dn = g_cur.dn; /* the day/night state is not the detection's to change */
    s.sys_valid = g_cur.sys_valid; /* nor the system metrics */
    s.sys = g_cur.sys;
    changed = !g_cur.valid || g_cur.present != s.present || g_cur.zmask != s.zmask || strcmp(g_cur.status, s.status) != 0;
    g_cur = s;
    if (changed && g_running) {
        if (g_ring_n == A2K_MQTT_RING) { /* bounded: the oldest transition goes, the newest state always stays */
            memmove(&g_ring[0], &g_ring[1], sizeof g_ring[0] * (A2K_MQTT_RING - 1));
            g_ring_n--;
        }
        g_ring[g_ring_n++] = s;
        /* under the lock: a2k_mqtt_stop clears g_running under it before it closes the pipe, so this never writes to a
         * closed (or reused) descriptor. The pipe is non-blocking: a full pipe is already a pending wake-up. */
        if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_night(const struct a2k_mqtt_night *n)
{
    pthread_mutex_lock(&g_mu);
    g_cur.dn = *n;
    if (g_running && g_night_enabled) {
        if (g_ring_n == A2K_MQTT_RING) {
            memmove(&g_ring[0], &g_ring[1], sizeof g_ring[0] * (A2K_MQTT_RING - 1));
            g_ring_n--;
        }
        g_ring[g_ring_n++] = g_cur;
        if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void a2k_mqtt_ptz(const char *state, int known, int pan, int tilt)
{
    pthread_mutex_lock(&g_mu);
    g_cur.ptz.valid = 1;
    snprintf(g_cur.ptz.state, sizeof g_cur.ptz.state, "%s", state);
    g_cur.ptz.known = known;
    g_cur.ptz.pan = pan;
    g_cur.ptz.tilt = tilt;
    if (g_running && g_ptz.press) {
        if (g_ring_n == A2K_MQTT_RING) {
            memmove(&g_ring[0], &g_ring[1], sizeof g_ring[0] * (A2K_MQTT_RING - 1));
            g_ring_n--;
        }
        g_ring[g_ring_n++] = g_cur;
        if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
        }
    }
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------------ session */

typedef struct {
    int     h;
    char    ip[48];
    uint8_t rx[RX_CAP];
    size_t  rxlen;
    int64_t last_rx, last_tx;
} conn_t;

static int pub(conn_t *c, const char *topic, const char *payload, int retain)
{
    int n = mqtt_enc_publish(g_tx, sizeof g_tx, topic, payload, strlen(payload), retain);
    if (n < 0)
        return 0; /* does not fit: skipped, not a connection error */
    if (g_net->send_all(c->h, g_tx, (size_t)n) != 0)
        return -1;
    c->last_tx = g_net->now_ms();
    return 0;
}

/* State and attributes of entity e. */
static int pub_ent(conn_t *c, const a2k_mqtt_ids_t *ids, int e, const struct a2k_mqtt_det *d, int viewers)
{
    char t[160], v[64], doc[400];
    if (a2k_mqtt_state(ids, e, d, viewers, t, sizeof t, v, sizeof v) && pub(c, t, v, 0) != 0)
        return -1;
    if (a2k_mqtt_attrs(ids, e, d, c->ip, g_rtsp_port, t, sizeof t, doc, sizeof doc) && pub(c, t, doc, 0) != 0)
        return -1;
    return 0;
}

/* State of ctl entity k from the app (the real value, whatever was commanded). */
static int pub_ctl_one(conn_t *c, const a2k_mqtt_ids_t *ids, int k)
{
    char v[A2K_CTL_VAL], t[160];
    if (!ids->ctl || !g_ctl.get || g_ctl.get(k, v, sizeof v) != 0 || !a2k_mqtt_ctl_state(ids, k, v, t, sizeof t))
        return 0;
    return pub(c, t, v, 0);
}

static int pub_ctl(conn_t *c, const a2k_mqtt_ids_t *ids)
{
    for (int k = 0; ids->ctl && k < A2K_CTL_N; k++)
        if (pub_ctl_one(c, ids, k) != 0)
            return -1;
    return 0;
}

static int pub_vid_one(conn_t *c, const a2k_mqtt_ids_t *ids, int k)
{
    char v[16], t[160];
    if (!ids->vid || !g_vid.get || g_vid.get(k, v, sizeof v) != 0 || !a2k_mqtt_vid_state(ids, k, v, t, sizeof t))
        return 0;
    return pub(c, t, v, 0);
}

static int pub_vid(conn_t *c, const a2k_mqtt_ids_t *ids)
{
    for (int k = 0; ids->vid && k < A2K_VID_N; k++)
        if (pub_vid_one(c, ids, k) != 0)
            return -1;
    return 0;
}

/* The ids' zone and tracking-area names from the live data (g_zones under the lock, the areas from the app). */
static void ids_sync(a2k_mqtt_ids_t *ids)
{
    char names[A2K_CTL_SLOTS][A2K_ZONE_NAME_MAX + 1];
    int n = 0;
    pthread_mutex_lock(&g_mu);
    ids->nzones = g_zones.n < A2K_ZONE_MAX ? g_zones.n : A2K_ZONE_MAX;
    for (int z = 0; z < ids->nzones; z++)
        snprintf(ids->zone[z], sizeof ids->zone[z], "%s", g_zones.z[z].name);
    pthread_mutex_unlock(&g_mu);
    if (ids->ctl && g_ctl.areas)
        n = g_ctl.areas(names);
    ids->ntarea = n < 0 ? 0 : n > A2K_CTL_SLOTS ? A2K_CTL_SLOTS : n;
    for (int s = 0; s < ids->ntarea; s++)
        snprintf(ids->tarea[s], sizeof ids->tarea[s], "%s", names[s]);
    ids->nbtn = 0;
    if (ids->ctl && g_btn_list) {
        n = g_btn_list(ids->btn);
        ids->nbtn = n < 0 ? 0 : n > A2K_BTN_TRIG_N ? A2K_BTN_TRIG_N : n;
    }
}

/* The device triggers of the configured gestures; the slots past them are cleared (an empty retained config). */
static int announce_btn(conn_t *c, const a2k_mqtt_ids_t *ids)
{
    char t[160], doc[1024];
    for (int k = 0; ids->ctl && k < A2K_BTN_TRIG_N; k++) {
        int n = a2k_mqtt_discovery(ids, A2K_E_BTN0 + k, t, sizeof t, doc, sizeof doc);
        if (n > 0) {
            if (pub(c, t, doc, 1) != 0)
                return -1;
        } else if (a2k_mqtt_btn_config_topic(ids, k, t, sizeof t) == 0 && pub(c, t, g_none, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

/* The zones or the tracking areas changed: clear the entities that went away, announce the current ones, publish the values. */
static int resync(conn_t *c, a2k_mqtt_ids_t *ids, const struct a2k_mqtt_det *d)
{
    char old[A2K_ZONE_MAX][A2K_ZONE_NAME_MAX + 1], t[160], doc[1024];
    int on = ids->nzones;
    for (int z = 0; z < on; z++)
        snprintf(old[z], sizeof old[z], "%s", ids->zone[z]);
    ids_sync(ids);
    if (g_cfg.discovery) {
        for (int z = 0; z < on; z++) {
            int keep = 0;
            for (int y = 0; y < ids->nzones; y++)
                keep |= !strcmp(old[z], ids->zone[y]);
            if (!keep && a2k_mqtt_zone_config_topic(ids, old[z], t, sizeof t) == 0 && pub(c, t, g_none, 1) != 0)
                return -1;
        }
        for (int z = 0; z < ids->nzones; z++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_ZONE0 + z, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->ctl && k < A2K_CTL_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_CTL0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0) {
                if (pub(c, t, doc, 1) != 0)
                    return -1;
            } else if (k >= A2K_CTL_SLOT0 && a2k_mqtt_ctl_config_topic(ids, k, t, sizeof t) == 0 && pub(c, t, g_none, 1) != 0) {
                return -1; /* a slot without an area: an empty retained config removes the entity an earlier announce made */
            }
        }
        if (announce_btn(c, ids) != 0)
            return -1;
    }
    for (int z = 0; z < ids->nzones; z++)
        if (pub_ent(c, ids, A2K_E_ZONE0 + z, d, -1) != 0)
            return -1;
    if (pub_vid(c, ids) != 0)
        return -1;
    return pub_ctl(c, ids);
}

static int pub_ptz(conn_t *c, const a2k_mqtt_ids_t *ids, const struct a2k_mqtt_det *d)
{
    for (int e = A2K_E_PTZ_STATE; ids->ptz && e < A2K_E_PTZ0 + A2K_PTZ_N; e++)
        if (pub_ent(c, ids, e, d, -1) != 0)
            return -1;
    return 0;
}

static int pub_sys(conn_t *c, const a2k_mqtt_ids_t *ids, const struct a2k_mqtt_det *d)
{
    for (int k = 0; ids->sys && k < A2K_SYS_N; k++)
        if (pub_ent(c, ids, A2K_E_SYS0 + k, d, -1) != 0)
            return -1;
    for (int k = 0; ids->leds && k < A2K_LED_N; k++) /* the controls ride with the periodic republish */
        if (pub_ent(c, ids, A2K_E_LED0 + k, d, -1) != 0)
            return -1;
    if (pub_ctl(c, ids) != 0 || pub_vid(c, ids) != 0)
        return -1;
    return pub_ptz(c, ids, d);
}

static int pub_all(conn_t *c, const a2k_mqtt_ids_t *ids, const struct a2k_mqtt_det *d)
{
    int viewers = g_viewers ? g_viewers() : -1;
    for (int e = 0; e < A2K_E_COUNT; e++)
        if (pub_ent(c, ids, e, d, viewers) != 0)
            return -1;
    for (int z = 0; z < ids->nzones; z++)
        if (pub_ent(c, ids, A2K_E_ZONE0 + z, d, -1) != 0)
            return -1;
    if (ids->night && pub_ent(c, ids, A2K_E_NIGHT, d, -1) != 0)
        return -1;
    return pub_sys(c, ids, d);
}

/* The newest snapshot, raw, not retained on BASE/snapshot. The packet is built in place (the header, from the codec's own
 * remaining-length encoder, is written right before the JPEG in the buffer a2k_mqtt_snapshot reserved room in): one
 * copy of the JPEG, one send. */
static int pub_snapshot(conn_t *c, const a2k_mqtt_ids_t *ids)
{
    uint8_t *jpeg;
    size_t len, tl;
    char t[160];
    uint8_t hdr[SNAP_HDR_ROOM];
    int vl, n, hn;
    pthread_mutex_lock(&g_mu);
    jpeg = g_snap;
    len = g_snap_len;
    g_snap = NULL;
    g_snap_len = 0;
    pthread_mutex_unlock(&g_mu);
    if (!jpeg)
        return 0;
    n = a2k_mqtt_snapshot_topic(ids, t, sizeof t);
    tl = n > 0 ? (size_t)n : 0;
    if (n < 0 || !mqtt_topic_name_ok(t, tl) || len > A2K_MQTT_SNAP_MAX) {
        free(jpeg);
        return 0; /* not publishable: skipped, not a connection error */
    }
    hdr[0] = 0x30; /* PUBLISH, QoS 0, not retained */
    vl = mqtt_varlen_enc(hdr + 1, 4, (uint32_t)(2 + tl + len));
    if (vl < 0) {
        free(jpeg);
        return 0;
    }
    hn = 1 + vl;
    hdr[hn++] = (uint8_t)(tl >> 8);
    hdr[hn++] = (uint8_t)(tl & 0xff);
    memcpy(hdr + hn, t, tl);
    hn += (int)tl;
    if (hn > SNAP_HDR_ROOM) {
        free(jpeg);
        return 0;
    }
    memcpy(jpeg + SNAP_HDR_ROOM - hn, hdr, (size_t)hn);
    if (g_net->send_all(c->h, jpeg + SNAP_HDR_ROOM - hn, (size_t)hn + len) != 0) {
        free(jpeg);
        return -1;
    }
    free(jpeg);
    c->last_tx = g_net->now_ms();
    pthread_mutex_lock(&g_mu);
    g_snap_sent++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int announce(conn_t *c, const a2k_mqtt_ids_t *ids, const struct a2k_mqtt_det *d)
{
    char t[160], doc[1024];
    if (a2k_mqtt_status_topic(ids, t, sizeof t) < 0 || pub(c, t, "online", 1) != 0)
        return -1;
    if (g_cfg.discovery) {
        for (int e = 0; e < A2K_E_COUNT; e++) {
            int n = a2k_mqtt_discovery(ids, e, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int e = A2K_E_IMAGE; e < A2K_E_ZONE0 + ids->nzones; e++) {
            int n = a2k_mqtt_discovery(ids, e, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        if (ids->night) {
            int n = a2k_mqtt_discovery(ids, A2K_E_NIGHT, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->ptz && k < A2K_PTZ_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_PTZ0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->sys && k < A2K_SYS_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_SYS0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->leds && k < A2K_LED_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_LED0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->ctl && k < A2K_CTL_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_CTL0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0) {
                if (pub(c, t, doc, 1) != 0)
                    return -1;
            } else if (k >= A2K_CTL_SLOT0 && a2k_mqtt_ctl_config_topic(ids, k, t, sizeof t) == 0 && pub(c, t, g_none, 1) != 0) {
                return -1; /* no area in this slot: clears what an earlier run announced */
            }
        }
        if (ids->ctl) {
            int n = a2k_mqtt_discovery(ids, A2K_E_BUTTON, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        for (int k = 0; ids->vid && k < A2K_VID_N; k++) {
            int n = a2k_mqtt_discovery(ids, A2K_E_VID0 + k, t, sizeof t, doc, sizeof doc);
            if (n > 0 && pub(c, t, doc, 1) != 0)
                return -1;
        }
        if (announce_btn(c, ids) != 0)
            return -1;
        if (!ids->snapshot) {
            /* snapshots off: an empty retained config removes a snapshot entity an earlier run announced */
            snprintf(t, sizeof t, "%s/image/%s/snapshot/config", ids->prefix, ids->node);
            if (pub(c, t, g_none, 1) != 0)
                return -1;
        }
    }
    return pub_all(c, ids, d);
}

/* Reads from the broker until one whole packet is in c->rx: 1 (p filled, drop it with consume), 0 nothing yet,
 * -1 malformed, -2 stop. */
static int next_pkt(conn_t *c, mqtt_pkt_t *p)
{
    int r = mqtt_frame(c->rx, c->rxlen, RX_CAP - 5, p);
    return r > 0 ? 1 : r == -2 ? -1 : r;
}

static void consume(conn_t *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rxlen - n);
    c->rxlen -= n;
}

static int fill(conn_t *c, int timeout_ms)
{
    int r;
    if (c->rxlen >= RX_CAP)
        return -1;
    r = g_net->recv_some(c->h, c->rx + c->rxlen, RX_CAP - c->rxlen, timeout_ms);
    if (r > 0) {
        c->rxlen += (size_t)r;
        c->last_rx = g_net->now_ms();
    }
    return r;
}

static int handshake(conn_t *c, const mqtt_conn_t *mc, const char **reason)
{
    int n = mqtt_enc_connect(g_tx, sizeof g_tx, mc);
    int64_t end;
    if (n < 0) {
        *reason = "config";
        return -1;
    }
    if (g_net->send_all(c->h, g_tx, (size_t)n) != 0) {
        *reason = "closed";
        return -1;
    }
    end = g_net->now_ms() + CONNACK_MS;
    for (;;) {
        mqtt_pkt_t p;
        int r = next_pkt(c, &p), sp, rc;
        int64_t left;
        if (r < 0) {
            *reason = "protocol";
            return -1;
        }
        if (r > 0) {
            if (mqtt_dec_connack(&p, &sp, &rc) != 0) {
                *reason = "protocol";
                return -1;
            }
            if (rc != 0) {
                *reason = mqtt_connack_reason(rc);
                return -1;
            }
            consume(c, p.total);
            return 0;
        }
        left = end - g_net->now_ms();
        if (left <= 0) {
            *reason = "timeout";
            return -1;
        }
        r = fill(c, (int)left);
        if (r == -2) {
            *reason = "stopped";
            return -1;
        }
        if (r < 0) {
            *reason = "closed";
            return -1;
        }
    }
}

/* Anything a broker may legitimately send to a client that subscribed to nothing. */
static int cmd_allowed(void)
{
    static int64_t t0;
    static int n;
    int64_t now = g_net->now_ms();
    if (now - t0 >= A2K_MQTT_CMD_WINDOW_MS || now < t0) {
        t0 = now;
        n = 0;
    }
    return n++ < A2K_MQTT_CMD_MAX;
}

/* A command of one of the LED controls: apply, then publish the real value back (what stayed when the text was refused). */
static int obey_led(conn_t *c, const a2k_mqtt_ids_t *ids, const mqtt_pub_t *m)
{
    struct a2k_mqtt_det d;
    char val[A2K_LED_VAL];
    int e;
    if (!ids->leds || !g_leds.get || !a2k_mqtt_led_command(ids, m->topic, m->tlen, m->payload, m->plen, &e, val, sizeof val))
        return 0;
    if (!cmd_allowed())
        return 0;
    (void)g_leds.set(e, val);
    memset(&d, 0, sizeof d);
    if (g_leds.get(e, d.led[e], A2K_LED_VAL) != 0)
        return 0;
    d.led_valid = 1;
    return pub_ent(c, ids, A2K_E_LED0 + e, &d, -1);
}

/* A press of a PTZ button: queued on the PTZ thread (never run here). The real state follows through a2k_mqtt_ptz. */
static int obey_ptz(const a2k_mqtt_ids_t *ids, const mqtt_pub_t *m)
{
    int cmd;
    if (!ids->ptz || !g_ptz.press || !a2k_mqtt_ptz_command(ids, m->topic, m->tlen, m->payload, m->plen, &cmd))
        return 0;
    if (cmd_allowed())
        (void)g_ptz.press(cmd);
    return 0;
}

/* The own budget of ptz_vel: A2K_MQTT_VEL_MAX per second (a window, like cmd_allowed). */
static int vel_allowed(void)
{
    static int64_t t0;
    static int n;
    int64_t now = g_net->now_ms();
    if (now - t0 >= 1000 || now < t0) {
        t0 = now;
        n = 0;
    }
    return n++ < A2K_MQTT_VEL_MAX;
}

/* BASE/ptz_vel/set: "PAN TILT [LEASE_MS]" or "STOP" (see mqtt_bind.h). Not obeyed without the ops, with a retained flag (checked by the caller) or over the budget. */
static int obey_ptz_vel(const a2k_mqtt_ids_t *ids, const mqtt_pub_t *m)
{
    char want[200], buf[48], *tok[3] = { NULL, NULL, NULL }, *p;
    int n, nt = 0, lease = 0;
    const char *why;
    ptzv_cmd_t c;
    if (!ids->ptz || !g_ptz.vel || m->plen < 1 || m->plen >= sizeof buf)
        return 0;
    n = snprintf(want, sizeof want, "%s/ptz_vel/set", ids->base);
    if (n <= 0 || n >= (int)sizeof want || (size_t)n != m->tlen || memcmp(m->topic, want, m->tlen) != 0)
        return 0;
    if (!vel_allowed())
        return 0;
    memcpy(buf, m->payload, m->plen);
    buf[m->plen] = '\0';
    if (!strcmp(buf, "STOP") || !strcmp(buf, "stop")) {
        if (g_ptz.vel_stop)
            (void)g_ptz.vel_stop();
        return 0;
    }
    for (p = buf; *p && nt < 3;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        tok[nt++] = p;
        while (*p && *p != ' ' && *p != ',')
            p++;
        if (*p)
            *p++ = '\0';
    }
    while (*p == ' ' || *p == ',')
        p++;
    if (nt < 2 || *p) /* exactly two or three numbers */
        return 0;
    if (ptzv_parse_args(tok[0], tok[1], tok[2], &c, &lease, &why) != 0)
        return 0;
    if (!c.sx && !c.sy) {
        if (g_ptz.vel_stop)
            (void)g_ptz.vel_stop();
    } else {
        (void)g_ptz.vel(c.sx, c.sy, c.dx, c.dy, lease);
    }
    return 0;
}

/* A command of a ctl entity: apply, then publish the real values back (the area sensor and the texts may have changed too). */
static int obey_ctl(conn_t *c, a2k_mqtt_ids_t *ids, const mqtt_pub_t *m)
{
    char val[A2K_CTL_VAL];
    int k;
    if (!ids->ctl || !g_ctl.get || !a2k_mqtt_ctl_command(ids, m->topic, m->tlen, m->payload, m->plen, &k, val, sizeof val))
        return 0;
    if (!cmd_allowed())
        return 0;
    (void)g_ctl.set(k, val);
    for (int e = 0; e < A2K_CTL_N; e++) /* the values, not the entities: a changed list of areas is announced by the resync the app asked for */
        if ((e == k || e == A2K_CTL_AREAS || e == A2K_CTL_ZONES || e == A2K_CTL_IGNORE || (k >= A2K_CTL_FLIP && k <= A2K_CTL_CEILING && e >= A2K_CTL_FLIP && e <= A2K_CTL_CEILING)) && pub_ctl_one(c, ids, e) != 0)
            return -1;
    return 0;
}

/* A command of a video setting: apply, then publish the real values of that stream back (a refused value, or one the encoder will refuse, comes back as it was). */
static int obey_vid(conn_t *c, const a2k_mqtt_ids_t *ids, const mqtt_pub_t *m)
{
    char val[16];
    int k;
    if (!ids->vid || !g_vid.get || !a2k_mqtt_vid_command(ids, m->topic, m->tlen, m->payload, m->plen, &k, val, sizeof val))
        return 0;
    if (!cmd_allowed())
        return 0;
    (void)g_vid.set(k, val);
    for (int e = k - k % A2K_VP_N; e < k - k % A2K_VP_N + A2K_VP_N; e++)
        if (pub_vid_one(c, ids, e) != 0)
            return -1;
    return 0;
}

static int handle_packet(conn_t *c, a2k_mqtt_ids_t *ids, const mqtt_pkt_t *p)
{
    switch (p->type) {
    case MQTT_PINGRESP: case MQTT_PUBACK: case 5: case 7: case MQTT_SUBACK: case MQTT_UNSUBACK:
        return 0;
    case MQTT_PUBLISH: {
        mqtt_pub_t m;
        uint8_t ack[4];
        if (mqtt_dec_publish(p, &m) != 0)
            return -1;
        if (m.qos == 1 && mqtt_enc_puback(ack, sizeof ack, m.pid) == 4 && g_net->send_all(c->h, ack, 4) != 0)
            return -1;
        if (m.retain)
            return 0; /* a retained command would replay at every reconnect: never obeyed */
        if (obey_led(c, ids, &m) != 0) /* the only commands: the LED controls and the PTZ buttons; any other topic is ignored */
            return -1;
        if (obey_ptz(ids, &m) != 0 || obey_ptz_vel(ids, &m) != 0 || obey_vid(c, ids, &m) != 0)
            return -1;
        return obey_ctl(c, ids, &m);
    }
    default:
        return -1;
    }
}

static void say_bye(conn_t *c, const a2k_mqtt_ids_t *ids)
{
    char t[160];
    int n;
    /* DISCONNECT makes the broker drop the will, so it only follows an "offline" that was fully sent */
    if (a2k_mqtt_status_topic(ids, t, sizeof t) < 0)
        return;
    n = mqtt_enc_publish(g_tx, sizeof g_tx, t, "offline", 7, 1);
    if (n < 0 || g_net->send_all(c->h, g_tx, (size_t)n) != 0)
        return;
    n = mqtt_enc_disconnect(g_tx, sizeof g_tx);
    if (n > 0)
        (void)g_net->send_all(c->h, g_tx, (size_t)n);
}

/* One connection, from dial to loss or stop. 1 when the broker accepted it, else 0; reason says why it ended (empty on
 * a requested stop). */
static int session(char *reason, size_t rcap)
{
    static conn_t c; /* only this thread uses it; 300 bytes off its small stack */
    a2k_mqtt_ids_t ids;
    mqtt_conn_t mc;
    struct a2k_mqtt_det cur;
    char mac[18], host[40] = { 0 }, status[160], id[40];
    const char *why = "closed";
    int accepted = 0, ir, r_hs;
    int64_t now, next_periodic, ping_sent = 0, ka_ms = (int64_t)KEEPALIVE_S * 1000;

    memset(&c, 0, sizeof c);
    c.h = -1;
    reason[0] = '\0';
    if (gethostname(host, sizeof host - 1) != 0 || !host[0])
        snprintf(host, sizeof host, "akam-rot2k");
    if (a2k_mqtt_mac_load(g_mac_path, mac) != 0)
        mac[0] = '\0';
    ir = a2k_mqtt_ids_init(&ids, &g_cfg, host, mac, A2K_APP_VERSION);
    ids.ctl = g_ctl.get != NULL;
    ids_sync(&ids);
    ids.snapshot = g_snap_enabled;
    ids.night = g_night_enabled;
    ids.sys = g_sys_enabled;
    ids.leds = g_leds.get != NULL;
    ids.ptz = g_ptz.press != NULL;
    ids.vid = g_vid.get != NULL;
    if (ir != 0 || a2k_mqtt_status_topic(&ids, status, sizeof status) < 0) {
        snprintf(reason, rcap, "%s", ir == -2 ? "mac" : "config");
        return 0;
    }
    /* the host name of the stock firmware is "YYOS": useless as a MQTT identity, the default is ours */
    snprintf(id, sizeof id, "%s", g_cfg.client_id[0] ? g_cfg.client_id : "akam-rot2k");

    c.h = g_net->dial(&g_cfg, c.ip, sizeof c.ip, &why);
    if (c.h < 0) {
        if (strcmp(why, "stopped"))
            snprintf(reason, rcap, "%s", why);
        return 0;
    }
    memset(&mc, 0, sizeof mc);
    mc.client_id = id;
    mc.user = g_cfg.user;
    mc.pass = g_cfg.user[0] ? g_cfg.pass : NULL;
    mc.keepalive = KEEPALIVE_S;
    mc.clean = 1;
    mc.will_topic = status;
    mc.will_msg = "offline";
    mc.will_retain = 1;
    c.last_rx = c.last_tx = g_net->now_ms();
    r_hs = handshake(&c, &mc, &why);
    memset(g_tx, 0, sizeof g_tx); /* the CONNECT packet held the password */
    if (r_hs != 0) {
        if (strcmp(why, "stopped"))
            snprintf(reason, rcap, "%s", why);
        g_net->close_h(c.h);
        return 0;
    }
    accepted = 1;
    set_state("connected");
    pthread_mutex_lock(&g_mu);
    g_ring_n = 0; /* the current state is published below: older transitions are history */
    g_det[0] = '\0'; /* and a detections list */
    free(g_snap); /* likewise a snapshot taken while the broker was away */
    g_snap = NULL;
    g_snap_len = 0;
    cur = g_cur;
    pthread_mutex_unlock(&g_mu);
    fill_leds(&cur);
    now = g_net->now_ms();
    next_periodic = now + (int64_t)g_cfg.interval * 1000;
    why = "closed";
    if (announce(&c, &ids, &cur) != 0)
        goto out;
    if (ids.leds || ids.ptz || ids.ctl || ids.vid) {
        char filter[100];
        int sn = a2k_mqtt_led_filter(&ids, filter, sizeof filter) == 0 ? mqtt_enc_subscribe(g_tx, sizeof g_tx, 1, filter) : -1;
        if (sn < 0 || g_net->send_all(c.h, g_tx, (size_t)sn) != 0)
            goto out;
        c.last_tx = g_net->now_ms();
    }

    while (!g_stop) {
        int64_t wake_at = now + 1000, ping_at, idle_since;
        int r, viewers;
        /* published transitions first: the state of the person is the point of this module */
        for (;;) {
            struct a2k_mqtt_det ev;
            int have = 0;
            pthread_mutex_lock(&g_mu);
            if (g_ring_n > 0) {
                ev = g_ring[0];
                memmove(&g_ring[0], &g_ring[1], sizeof g_ring[0] * (size_t)(A2K_MQTT_RING - 1));
                g_ring_n--;
                have = 1;
            }
            pthread_mutex_unlock(&g_mu);
            if (!have)
                break;
            for (int e = A2K_E_PERSON; e <= A2K_E_LAST; e++)
                if (pub_ent(&c, &ids, e, &ev, -1) != 0)
                    goto out;
            for (int z = 0; z < ids.nzones; z++)
                if (pub_ent(&c, &ids, A2K_E_ZONE0 + z, &ev, -1) != 0)
                    goto out;
            if (ids.night && pub_ent(&c, &ids, A2K_E_NIGHT, &ev, -1) != 0)
                goto out;
            if (pub_ptz(&c, &ids, &ev) != 0)
                goto out;
        }
        {
            int rs, nb;
            struct a2k_mqtt_det cd;
            pthread_mutex_lock(&g_mu);
            rs = g_resync;
            g_resync = 0;
            nb = g_btn_n;
            g_btn_n = 0;
            cd = g_cur;
            pthread_mutex_unlock(&g_mu);
            if (rs && resync(&c, &ids, &cd) != 0)
                goto out;
            for (; nb > 0 && ids.ctl; nb--) {
                char bt[160];
                if (a2k_mqtt_button_topic(&ids, bt, sizeof bt) == 0 && pub(&c, bt, a2k_mqtt_button_payload, 0) != 0)
                    goto out;
            }
            for (;;) { /* every gesture, in order: BASE/button {"gesture","actions","result"} */
                char bj[A2K_MQTT_GESTURE_MAX + 1], bt[160];
                int have = 0;
                pthread_mutex_lock(&g_mu);
                if (g_bq_n > 0) {
                    memcpy(bj, g_bq[0], sizeof bj);
                    memmove(g_bq[0], g_bq[1], sizeof g_bq[0] * (BTN_Q - 1));
                    g_bq_n--;
                    have = 1;
                }
                pthread_mutex_unlock(&g_mu);
                if (!have)
                    break;
                if (ids.ctl && a2k_mqtt_gesture_topic(&ids, bt, sizeof bt) == 0 && pub(&c, bt, bj, 0) != 0)
                    goto out;
            }
        }
        {
            char dj[A2K_MQTT_DET_MAX + 1], dt[160];
            pthread_mutex_lock(&g_mu);
            memcpy(dj, g_det, sizeof dj);
            g_det[0] = '\0';
            pthread_mutex_unlock(&g_mu);
            if (dj[0] && a2k_mqtt_detections_topic(&ids, dt, sizeof dt) > 0) {
                if (pub(&c, dt, dj, 0) != 0)
                    goto out;
                pthread_mutex_lock(&g_mu);
                g_det_sent++;
                pthread_mutex_unlock(&g_mu);
            }
        }
        if (pub_snapshot(&c, &ids) != 0)
            goto out;
        idle_since = c.last_rx < c.last_tx ? c.last_rx : c.last_tx;
        ping_at = ping_sent ? ping_sent + PING_REPLY_MS : idle_since + ka_ms / 2;
        if (next_periodic < wake_at)
            wake_at = next_periodic;
        if (ping_at < wake_at)
            wake_at = ping_at;
        now = g_net->now_ms();
        r = g_net->recv_some(c.h, c.rx + c.rxlen, RX_CAP - c.rxlen > 0 ? RX_CAP - c.rxlen : 0,
                             wake_at > now ? (int)(wake_at - now) : 0);
        if (r == -2) {
            why = g_none;
            break;
        }
        if (r < 0)
            goto out;
        if (r > 0) {
            c.rxlen += (size_t)r;
            c.last_rx = g_net->now_ms();
            ping_sent = 0;
        }
        for (;;) {
            mqtt_pkt_t p;
            int q = next_pkt(&c, &p);
            if (q < 0 || (q > 0 && handle_packet(&c, &ids, &p) != 0)) {
                why = "protocol";
                goto out;
            }
            if (q == 0)
                break;
            consume(&c, p.total);
        }
        if (c.rxlen >= RX_CAP) {
            why = "protocol";
            goto out;
        }
        now = g_net->now_ms();
        if (ping_sent && now - ping_sent >= PING_REPLY_MS) {
            why = "timeout";
            goto out;
        }
        if (!ping_sent && (now - c.last_rx >= ka_ms / 2 || now - c.last_tx >= ka_ms / 2)) {
            int pn = mqtt_enc_pingreq(g_tx, sizeof g_tx);
            if (pn < 0 || g_net->send_all(c.h, g_tx, (size_t)pn) != 0)
                goto out;
            c.last_tx = now;
            ping_sent = now;
        }
        if (now >= next_periodic) {
            pthread_mutex_lock(&g_mu);
            cur = g_cur;
            pthread_mutex_unlock(&g_mu);
            fill_leds(&cur);
            viewers = g_viewers ? g_viewers() : -1;
            for (int e = 0; e < A2K_E_COUNT; e++)
                if (pub_ent(&c, &ids, e, &cur, viewers) != 0)
                    goto out;
            for (int z = 0; z < ids.nzones; z++)
                if (pub_ent(&c, &ids, A2K_E_ZONE0 + z, &cur, -1) != 0)
                    goto out;
            if (ids.night && pub_ent(&c, &ids, A2K_E_NIGHT, &cur, -1) != 0)
                goto out;
            if (pub_sys(&c, &ids, &cur) != 0)
                goto out;
            next_periodic = now + (int64_t)g_cfg.interval * 1000;
        }
    }
    why = g_none;
    say_bye(&c, &ids);
out:
    snprintf(reason, rcap, "%s", why);
    g_net->close_h(c.h);
    return accepted;
}

static void *thread_main(void *arg)
{
    unsigned attempt = 0;
    uint32_t rng = (uint32_t)g_net->now_ms() ^ ((uint32_t)getpid() << 8);
    char reason[24], s[40];
    (void)arg;
    /* below the video threads: a busy single core serves the stream first */
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);
    while (!g_stop) {
        int64_t t0 = g_net->now_ms();
        int ok = session(reason, sizeof reason);
        unsigned d;
        if (g_stop)
            break;
        /* the backoff starts again from its minimum only after a connection that held: a broker that accepts and drops
         * at once (bad ACL, duplicate client id) must not be hammered every minimum */
        if (ok && g_net->now_ms() - t0 >= STABLE_MS)
            attempt = 0;
        snprintf(s, sizeof s, "error:%s", reason[0] ? reason : "closed");
        set_state(s);
        rng = rng * 1664525u + 1013904223u;
        d = mqtt_backoff_ms(attempt, rng >> 8, BACKOFF_MIN_MS, BACKOFF_MAX_MS);
        if (attempt < 30)
            attempt++;
        g_net->sleep_ms(d);
    }
    g_done = 1;
    return NULL;
}

/* ------------------------------------------------------------------------ start / stop */

int a2k_mqtt_start(const char *conf_path, const char *mac_path, int rtsp_port, a2k_mqtt_viewers_fn viewers)
{
    a2k_mqtt_cfg_t cfg;
    pthread_attr_t at;
    char desc[200], pass_path[300];
    int ld;
    if (g_running)
        return 0;
    ld = a2k_mqtt_cfg_load(conf_path, &cfg);
    if (ld == 1) {
        a2k_log("mqtt: no %s, MQTT off", conf_path);
        return 1;
    }
    if (ld != 0) {
        a2k_log("mqtt: %s is invalid, MQTT stays off (nothing from the file is printed)", conf_path);
        return -1;
    }
    if (!cfg.enabled) {
        a2k_log("mqtt: enabled=0, MQTT off");
        a2k_mqtt_cfg_wipe(&cfg);
        return 1;
    }
    /* the password lives in its own card file next to the config (same layout as the 1080p); an unusable one keeps MQTT off */
    if (a2k_mqtt_pass_path(conf_path, pass_path, sizeof pass_path) != 0 ||
        a2k_mqtt_pass_load(pass_path, cfg.pass, sizeof cfg.pass) != 0) {
        a2k_log("mqtt: the %s file is unusable (unreadable, too long or control characters), MQTT stays off",
                A2K_MQTT_PASS_NAME);
        a2k_mqtt_cfg_wipe(&cfg);
        return -1;
    }
    a2k_mqtt_cfg_describe(&cfg, desc, sizeof desc);
    a2k_log("mqtt: enabled, %s", desc);
    g_cfg = cfg;
    a2k_mqtt_cfg_wipe(&cfg);
    snprintf(g_mac_path, sizeof g_mac_path, "%s", mac_path);
    g_rtsp_port = rtsp_port;
    g_viewers = viewers;
    g_stop = 0;
    g_done = 0;
    pthread_mutex_lock(&g_mu);
    g_snap_sent = g_snap_dropped = 0;
    g_det_sent = g_det_dropped = 0;
    g_det[0] = '\0';
    g_ring_n = 0;
    snprintf(g_state, sizeof g_state, "connecting");
    pthread_mutex_unlock(&g_mu);
    if (pipe(g_wake) != 0) {
        a2k_log("mqtt: pipe failed, MQTT off");
        a2k_mqtt_cfg_wipe(&g_cfg);
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(g_wake[i], F_SETFL, fcntl(g_wake[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);
    }
    g_running = 1;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, STACK_BYTES);
    if (pthread_create(&g_tid, &at, thread_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        g_running = 0;
        close(g_wake[0]);
        close(g_wake[1]);
        g_wake[0] = g_wake[1] = -1;
        a2k_mqtt_cfg_wipe(&g_cfg);
        a2k_log("mqtt: cannot start the thread, MQTT off");
        return -1;
    }
    pthread_attr_destroy(&at);
    return 0;
}

void a2k_mqtt_stop(void)
{
    int waited = 0;
    if (!g_running)
        return;
    pthread_mutex_lock(&g_mu);
    g_running = 0; /* the detection thread stops queueing and waking before the pipe goes away */
    pthread_mutex_unlock(&g_mu);
    g_stop = 1;
    if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) {
        /* already awake */
    }
    while (!g_done && waited < STOP_WAIT_MS) {
        usleep(10000);
        waited += 10;
    }
    if (g_done) {
        pthread_join(g_tid, NULL);
        a2k_mqtt_cfg_wipe(&g_cfg);
        close(g_wake[0]);
        close(g_wake[1]);
        g_wake[0] = g_wake[1] = -1;
    } else {
        /* a resolver that never returns: the process is exiting, the OS reclaims the thread */
        pthread_detach(g_tid);
        a2k_log("mqtt: thread still busy at stop, left to the OS");
    }
    set_state("off");
    pthread_mutex_lock(&g_mu);
    if (g_done) { /* the thread is gone: nobody reads the pending snapshot any more */
        free(g_snap);
        g_snap = NULL;
        g_snap_len = 0;
    }
    pthread_mutex_unlock(&g_mu);
}
