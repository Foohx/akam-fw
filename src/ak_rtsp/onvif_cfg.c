/* See onvif_cfg.h. */
#define _GNU_SOURCE
#include "onvif_cfg.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cred.h"
#include "http_srv.h"
#include "onvif.h"
#include "onvif_device.h"

const char *onvif_cfg_path = ONVIF_CFG_PATH;
const wsd_test_t *onvif_svc_wsd_test;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_state[ONVIF_STATE_MAX] = "off";
static int g_running;   /* HTTP server and WS-Discovery are up, on g_port */
static int g_port;

/* ------------------------------------------------------------------------ parser */

int onvif_cfg_port_ok(long port)
{
    if (port < 1 || port > 65535) return 0;
    if ((port >= 21 && port <= 24) || port == 554 || port == 8091) return 0;
    return 1;
}

static int parse_port(const char *s, size_t n, int *out)
{
    if (n < 1 || n > 5) return -1;
    long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (!onvif_cfg_port_ok(v)) return -1;
    *out = (int)v;
    return 0;
}

int onvif_cfg_parse(const char *text, size_t len, onvif_cfg_t *out)
{
    int en = -1, port = -1, lines = 0;
    size_t i = 0;
    if (!text || len > ONVIF_CFG_MAX) return -1;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        size_t n = e - i;
        if (n && text[i + n - 1] == '\r') n--;      /* the helper strips one CR too */
        const char *l = text + i;
        if (++lines > 2) return -1;
        if (memchr(l, '\0', n)) return -1;
        if (n == 9 && !memcmp(l, "enabled=", 8) && (l[8] == '0' || l[8] == '1')) {
            if (en >= 0) return -1;
            en = l[8] - '0';
        } else if (n > 5 && !memcmp(l, "port=", 5)) {
            if (port >= 0 || parse_port(l + 5, n - 5, &port) != 0) return -1;
        } else {
            return -1;
        }
        i = e + 1;
    }
    if (en < 0 || port < 0) return -1;
    out->enabled = en;
    out->port = port;
    return 0;
}

int onvif_cfg_load(onvif_cfg_t *out)
{
    char buf[ONVIF_CFG_MAX + 2];
    out->enabled = 0;
    out->port = 0;
    int fd = open(onvif_cfg_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return 1;
        printf("[onvif] flag file unreadable (%s): ONVIF stays off\n", strerror(errno));
        return -1;
    }
    struct stat st;
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0 || n > ONVIF_CFG_MAX || onvif_cfg_parse(buf, (size_t)n, out) != 0) {
        printf("[onvif] flag file invalid (need enabled=0|1 and port=N): ONVIF stays off\n");
        out->enabled = 0;
        out->port = 0;
        return -1;
    }
    return 0;
}

size_t onvif_cfg_urlenc(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!cap) return 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~';
        if (o + (plain ? 1 : 3) >= cap) break;
        if (plain) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------------ services */

static int xaddr_cb(const struct in_addr *local, char *out, size_t cap, void *ctx)
{
    (void)ctx;
    int port = http_srv_port();
    char ip[INET_ADDRSTRLEN];
    if (port == 0 || !inet_ntop(AF_INET, local, ip, sizeof(ip))) return -1;
    int n = port == 80 ? snprintf(out, cap, "http://%s/onvif/device_service", ip)
                       : snprintf(out, cap, "http://%s:%d/onvif/device_service", ip, port);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static void stop_locked(void)
{
    if (!g_running) return;
    wsd_stop();
    onvif_stop();
    g_running = 0;
    g_port = 0;
}

static int start_wsd(void)
{
    char host[33], enc[100], scopes[400], uuid[46];
    onvif_hostname_get(host);
    onvif_cfg_urlenc(host, enc, sizeof(enc));
    snprintf(scopes, sizeof(scopes),
             "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/Profile/S "
             "onvif://www.onvif.org/name/%s onvif://www.onvif.org/hardware/" ONVIF_HARDWARE_ID
             " onvif://www.onvif.org/location/", enc);
    onvif_uuid(uuid);
    wsd_info_t info;
    memset(&info, 0, sizeof(info));
    info.uuid = uuid;
    info.scopes = scopes;
    info.metadata_version = 1;
    info.xaddr = xaddr_cb;
    info.test = onvif_svc_wsd_test;
    return wsd_start(&info);
}

/* Called with g_lock held and ONVIF wanted on `port`. Returns 0 (state on:<port>) or -1 (state error:...). */
static int bring_up(int port)
{
    if (g_running && g_port != port) stop_locked();
    if (g_running) {
        wsd_stop();                 /* same port: only rebuild the discovery scopes */
    } else if (onvif_start(port) != 0) {
        int e = errno;
        snprintf(g_state, sizeof(g_state), "error:%s",
                 e == EADDRINUSE ? "port_in_use" : e == EACCES ? "permission_denied" : "http_failed");
        printf("[onvif] HTTP server on port %d failed: %s\n", port, strerror(e));
        return -1;
    }
    if (start_wsd() != 0) {
        int e = errno;
        onvif_stop();
        g_running = 0;
        g_port = 0;
        snprintf(g_state, sizeof(g_state), "error:discovery_failed");
        printf("[onvif] WS-Discovery failed: %s\n", strerror(e));
        return -1;
    }
    g_running = 1;
    g_port = port;
    snprintf(g_state, sizeof(g_state), "on:%d", port);
    printf("[onvif] on, HTTP port %d\n", port);
    return 0;
}

int onvif_svc_reload(char *state, size_t cap)
{
    onvif_cfg_t cfg;
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    int ld = onvif_cfg_load(&cfg);
    if (ld != 0 || !cfg.enabled) {
        stop_locked();
        snprintf(g_state, sizeof(g_state), "off");
    } else {
        cred_refresh(true);
        if (cred_auth_mode() != RTSP_AUTH_OFF && !cred_have_plain()) {
            stop_locked();
            snprintf(g_state, sizeof(g_state), "no_password");
            printf("[onvif] enabled but no valid clear-text credentials (run ./akam rtsp-auth set): not started\n");
            rc = -1;
        } else {
            rc = bring_up(cfg.port);
        }
    }
    if (state && cap) snprintf(state, cap, "%s", g_state);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void onvif_svc_state(char *state, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    snprintf(state, cap, "%s", g_state);
    pthread_mutex_unlock(&g_lock);
}

void onvif_svc_stop(void)
{
    pthread_mutex_lock(&g_lock);
    stop_locked();
    snprintf(g_state, sizeof(g_state), "off");
    pthread_mutex_unlock(&g_lock);
}
