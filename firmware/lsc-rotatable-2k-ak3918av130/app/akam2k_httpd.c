/* See akam2k_httpd.h. */
#include "akam2k_httpd.h"

#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "akam2k_apievt.h"
#include "akam2k_log.h"
#include "http_req.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HDR_RESERVE 256
#define MAX_WAITERS 2

enum { C_FREE = 0, C_READ, C_WAIT };

typedef struct {
    int fd, state;
    uint32_t ip;
    char ipstr[16];
    uint64_t t_accept, t_last, deadline;
    size_t len, head_len;
    long need;
    int parsed;
    uint32_t since;
    hreq_t rq;
    char *buf; /* HREQ_HEAD_MAX + HREQ_BODY_MAX + 1, malloc'd at accept and freed at close: idle slots hold no buffer */
} conn_t;

static conn_t g_conn[A2K_HTTPD_MAX_CONNS];
static char g_resp[HDR_RESERVE + A2K_API_RESP_MAX + 1]; /* header is built right before the body: one send() */
static pthread_t g_tid;
static volatile int g_stop, g_started;
static int g_lfd = -1, g_wake[2] = { -1, -1 }, g_port_cfg, g_loop, g_bound;
static a2k_api_env_t g_env;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int a2k_httpd_port(void) { return g_bound; }

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 207: return "Multi-Status";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return s >= 500 ? "Internal Server Error" : "Error";
    }
}

/* Writes all of p within A2K_HTTPD_WRITE_MS; the sockets are non blocking. */
static void send_all(int fd, const char *p, size_t n)
{
    uint64_t end = now_ms() + A2K_HTTPD_WRITE_MS;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf;
            uint64_t t = now_ms();
            if (t >= end)
                return;
            pf.fd = fd;
            pf.events = POLLOUT;
            pf.revents = 0;
            poll(&pf, 1, (int)(end - t));
            continue;
        }
        return;
    }
}

/* body is g_resp + HDR_RESERVE (len bytes) or NULL (an error text is built here). */
static void respond(conn_t *c, int status, unsigned retry_s, size_t len)
{
    char hdr[HDR_RESERVE];
    int h;
    if (status < 100)
        status = 500;
    h = snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %lu\r\nCache-Control: no-store\r\n"
                 "Connection: close\r\n",
                 status, reason(status), (unsigned long)len);
    if (h < 0 || (size_t)h >= sizeof hdr - 40)
        return;
    if (retry_s)
        h += snprintf(hdr + h, sizeof hdr - (size_t)h, "Retry-After: %u\r\n", retry_s);
    memcpy(hdr + h, "\r\n", 2);
    h += 2;
    memcpy(g_resp + HDR_RESERVE - (size_t)h, hdr, (size_t)h);
    send_all(c->fd, g_resp + HDR_RESERVE - (size_t)h, (size_t)h + len);
}

static void respond_err(conn_t *c, int status, const char *code, const char *detail)
{
    int n = snprintf(g_resp + HDR_RESERVE, A2K_API_RESP_MAX, "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    respond(c, status, 0, n > 0 ? (size_t)n : 0);
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0) {
        /* half close and drain what the peer already sent: closing with unread data makes the kernel send a RST that can
         * destroy the reply (the 4xx of an oversize request) before the client reads it */
        char tmp[512];
        int k;
        shutdown(c->fd, SHUT_WR);
        for (k = 0; k < 32 && recv(c->fd, tmp, sizeof tmp, 0) > 0; k++) {
        }
        close(c->fd);
    }
    memset(&c->rq, 0, sizeof c->rq);
    free(c->buf);
    c->buf = NULL;
    c->fd = -1;
    c->state = C_FREE;
    c->len = 0;
    c->parsed = 0;
}

static int waiters(void)
{
    int i, n = 0;
    for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++)
        n += g_conn[i].state == C_WAIT;
    return n;
}

static void dispatch(conn_t *c, uint64_t now)
{
    a2k_api_in_t in;
    a2k_api_out_t out;
    memset(&in, 0, sizeof in);
    in.rq = &c->rq;
    in.body = c->need > 0 ? c->buf + c->head_len : NULL;
    in.body_len = (size_t)c->need;
    in.peer_ip = c->ip;
    in.peer_str = c->ipstr;
    a2k_api_handle(&in, g_resp + HDR_RESERVE, A2K_API_RESP_MAX, &out);
    if (out.wait && waiters() < MAX_WAITERS) {
        c->state = C_WAIT;
        c->since = out.since;
        c->deadline = now + (uint64_t)out.wait_s * 1000u;
        return;
    }
    if (out.wait) { /* too many held connections: answer now with what there is */
        out.len = (size_t)a2k_api_events_reply(out.since, g_resp + HDR_RESERVE, A2K_API_RESP_MAX);
        out.status = 200;
    }
    respond(c, out.status, out.retry_s, out.len);
    if (out.status == 201) /* a new key's secret was in the reply: do not leave it in the shared buffer */
        memset(g_resp, 0, sizeof g_resp);
    conn_close(c);
}

static void finish_wait(conn_t *c)
{
    size_t n = (size_t)a2k_api_events_reply(c->since, g_resp + HDR_RESERVE, A2K_API_RESP_MAX);
    respond(c, n ? 200 : 500, 0, n);
    conn_close(c);
}

static void conn_read(conn_t *c, uint64_t now)
{
    size_t room;
    ssize_t n;
    long he;
    int rc;
    if (c->state == C_WAIT) { /* the client closed (or spoke again): drop the held request */
        char tmp[16];
        n = recv(c->fd, tmp, sizeof tmp, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || n > 0)
            conn_close(c);
        return;
    }
    room = c->parsed ? c->head_len + (size_t)c->need - c->len : (size_t)HREQ_HEAD_MAX - c->len;
    if (room == 0)
        return;
    n = recv(c->fd, c->buf + c->len, room, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_close(c);
        return;
    }
    if (n < 0)
        return;
    c->len += (size_t)n;
    c->t_last = now;
    if (!c->parsed) {
        he = hreq_head_end(c->buf, c->len);
        if (he == 0)
            return;
        if (he < 0) {
            if (he == -2)
                respond_err(c, 400, "bad_request", "lines end in CRLF");
            else
                respond_err(c, 431, "head_too_large", "the request head is over 4096 bytes");
            conn_close(c);
            return;
        }
        rc = hreq_parse(c->buf, (size_t)he, &c->rq);
        if (rc != 0) {
            respond_err(c, rc, rc == 413 ? "body_too_large" : "bad_request", rc == 413 ? "the body is over 8192 bytes" : "malformed request");
            conn_close(c);
            return;
        }
        c->parsed = 1;
        c->head_len = (size_t)he;
        c->need = c->rq.content_length > 0 ? c->rq.content_length : 0;
        if (c->rq.chunked || c->rq.expect)
            c->need = 0; /* answered by the API layer (411 / 417) without reading a body */
    }
    if (c->len >= c->head_len + (size_t)c->need)
        dispatch(c, now);
}

static uint32_t peer_count(uint32_t ip)
{
    int i;
    uint32_t n = 0;
    for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++)
        n += g_conn[i].state != C_FREE && g_conn[i].ip == ip;
    return n;
}

static void do_accept(uint64_t now)
{
    int k;
    for (k = 0; k < 4; k++) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int fd = accept(g_lfd, (struct sockaddr *)&sa, &sl), i;
        uint32_t ip;
        conn_t *c = NULL;
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED)
                usleep(100000); /* EMFILE and friends: the listener stays readable, do not spin */
            return;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        ip = ntohl(sa.sin_addr.s_addr);
        for (i = 0; i < A2K_HTTPD_MAX_CONNS && !c; i++)
            if (g_conn[i].state == C_FREE)
                c = &g_conn[i];
        if (c && peer_count(ip) < A2K_HTTPD_MAX_PER_PEER) {
            c->buf = malloc(HREQ_HEAD_MAX + HREQ_BODY_MAX + 1);
            if (!c->buf)
                c = NULL;
        }
        if (!c || peer_count(ip) >= A2K_HTTPD_MAX_PER_PEER) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
            if (send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL) < 0) {
                /* best effort: the peer is dropped either way */
            }
            close(fd);
            continue;
        }
        {
            char *b = c->buf;
            memset(c, 0, sizeof *c);
            c->buf = b;
        }
        c->fd = fd;
        c->state = C_READ;
        c->ip = ip;
        snprintf(c->ipstr, sizeof c->ipstr, "%u.%u.%u.%u", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
        c->t_accept = c->t_last = now;
    }
}

static void listener_close(void)
{
    int i;
    if (g_lfd >= 0) {
        close(g_lfd);
        g_lfd = -1;
        g_bound = 0;
        a2k_log("api: no API key installed, port closed");
    }
    for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++)
        if (g_conn[i].state != C_FREE)
            conn_close(&g_conn[i]);
}

static void listener_open(void)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port_cfg);
    sa.sin_addr.s_addr = htonl(g_loop ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, A2K_HTTPD_MAX_CONNS) != 0) {
        a2k_log("api: cannot listen on port %d (%s); retrying", g_port_cfg, strerror(errno));
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        g_bound = ntohs(sa.sin_port);
    g_lfd = fd;
    a2k_log("api: listening on port %d (%d key(s))", g_bound, a2k_api_keys_count());
}

static void *thread_main(void *arg)
{
    uint64_t last_tick = now_ms();
    (void)arg;
    while (!g_stop) {
        struct pollfd pf[2 + A2K_HTTPD_MAX_CONNS];
        int map[A2K_HTTPD_MAX_CONNS], np = 0, i, timeout = 1000, nm = 0;
        uint64_t now = now_ms();
        if (a2k_api_keys_count() > 0 && g_lfd < 0)
            listener_open();
        else if (a2k_api_keys_count() == 0 && (g_lfd >= 0))
            listener_close();
        pf[np].fd = g_wake[0];
        pf[np].events = POLLIN;
        np++;
        if (g_lfd >= 0) {
            pf[np].fd = g_lfd;
            pf[np].events = POLLIN;
            np++;
        }
        for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++) {
            conn_t *c = &g_conn[i];
            uint64_t due;
            if (c->state == C_FREE)
                continue;
            pf[np].fd = c->fd;
            pf[np].events = POLLIN;
            map[nm++] = i;
            np++;
            due = c->state == C_WAIT ? c->deadline : c->t_last + A2K_HTTPD_READ_IDLE_MS;
            if (due <= now)
                timeout = 0;
            else if (due - now < (uint64_t)timeout)
                timeout = (int)(due - now);
        }
        if (poll(pf, (nfds_t)np, timeout) < 0 && errno != EINTR)
            usleep(100000);
        now = now_ms();
        {
            int base = 1 + (g_lfd >= 0);
            if (pf[0].revents & POLLIN) {
                char tmp[64];
                while (read(g_wake[0], tmp, sizeof tmp) > 0) {
                }
                for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++)
                    if (g_conn[i].state == C_WAIT && a2k_apievt_seq() > g_conn[i].since)
                        finish_wait(&g_conn[i]);
            }
            if (g_lfd >= 0 && (pf[1].revents & POLLIN))
                do_accept(now);
            for (i = 0; i < nm; i++) {
                conn_t *c = &g_conn[map[i]];
                short re = pf[base + i].revents;
                if (c->state == C_FREE)
                    continue;
                if (re & (POLLIN | POLLHUP | POLLERR))
                    conn_read(c, now);
            }
        }
        for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++) {
            conn_t *c = &g_conn[i];
            if (c->state == C_WAIT && now >= c->deadline)
                finish_wait(c);
            else if (c->state == C_READ && (now - c->t_last >= A2K_HTTPD_READ_IDLE_MS || now - c->t_accept >= A2K_HTTPD_REQUEST_MS)) {
                respond_err(c, 408, "timeout", "the request was not received in time");
                conn_close(c);
            }
        }
        if (now - last_tick >= 1000) {
            last_tick = now;
            a2k_api_tick();
        }
    }
    listener_close();
    return NULL;
}

int a2k_httpd_start(int port, int loopback_only, const a2k_api_env_t *env)
{
    pthread_attr_t at;
    int i;
    if (g_started)
        return 0;
    if (env)
        g_env = *env;
    else
        memset(&g_env, 0, sizeof g_env);
    g_port_cfg = port;
    g_loop = loopback_only;
    g_stop = 0;
    for (i = 0; i < A2K_HTTPD_MAX_CONNS; i++) {
        g_conn[i].fd = -1;
        g_conn[i].state = C_FREE;
    }
    if (pipe(g_wake) != 0) {
        a2k_log("api: cannot create the wake pipe");
        return -1;
    }
    for (i = 0; i < 2; i++)
        fcntl(g_wake[i], F_SETFL, fcntl(g_wake[i], F_GETFL, 0) | O_NONBLOCK);
    a2k_api_init(&g_env);
    a2k_apievt_set_wake(g_wake[1]);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&g_tid, &at, thread_main, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_apievt_set_wake(-1);
        close(g_wake[0]);
        close(g_wake[1]);
        a2k_log("api: cannot start the HTTP thread");
        return -1;
    }
    pthread_attr_destroy(&at);
    g_started = 1;
    if (a2k_api_keys_count() == 0)
        a2k_log("api: no API key installed: the port stays closed (akam_api_key new --scope admin --name NAME creates the first one)");
    return 0;
}

void a2k_httpd_stop(void)
{
    if (!g_started)
        return;
    g_stop = 1;
    a2k_apievt_set_wake(-1);
    if (write(g_wake[1], "s", 1) < 0) {
    }
    pthread_join(g_tid, NULL);
    close(g_wake[0]);
    close(g_wake[1]);
    g_wake[0] = g_wake[1] = -1;
    g_started = 0;
}
