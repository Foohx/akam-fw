/* Minimal HTTP/1.1 server for the ONVIF services; see http_srv.h for the design and the limits. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#include "http_srv.h"

#define HTTP_WRITE_TIMEOUT_MS 5000
#define HTTP_LINGER_MS        1000
#define HTTP_STACK_BYTES      (64 * 1024)
#define HTTP_NICE             10      /* the video path comes first on this single core */
#define HTTP_RBUF_INIT        1024
#define HTTP_ACCEPT_PAUSE_MS  200

enum { S_READ, S_WRITE, S_LINGER, S_DEFER };

typedef struct {
    int    fd;
    int    state;
    char  *rbuf;
    size_t rcap, rlen, scan;
    size_t head_len, body_len;      /* head_len == 0: request head not complete yet */
    int    off_path, off_query, off_auth, off_ctype;
    int    is_post, is_get, http10, want_close, expect, sent100;
    long long defer_until;          /* ms: S_DEFER ends here */
    long long t_start, t_act;       /* ms: first byte of the request in progress (0 = none) / last progress */
    http_resp_t resp;
    char   obuf[HTTP_OBUF];
    size_t olen, ooff, body_left;
    int    close_after, linger;
    int    served;                  /* a request was dispatched on this connection (leaves the short first-request deadline) */
    struct sockaddr_in peer, local;
} conn_t;

typedef struct { char *path; unsigned methods; http_handler_t fn; void *user; } route_t;

static struct {
    pthread_mutex_t lock;
    pthread_t       tid;
    int             running;
    volatile int    stop;
    int             listen_fd, wake_r, wake_w;
    volatile int    port; /* plain int: read lock-free by http_srv_port(), written under the lock (a 4-byte aligned word access is atomic on ARMv5) */
    conn_t         *conns;
    route_t         routes[HTTP_MAX_ROUTES];
    int             nroutes;
    int             req_ms, idle_ms;
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1, .wake_r = -1, .wake_w = -1 };

/* ------------------------------------------------------------------ handler helpers */

void http_resp_set(http_resp_t *r, int status, const char *content_type)
{
    r->status = status;
    r->content_type = content_type;
}

int http_resp_header(http_resp_t *r, const char *name, const char *value)
{
    size_t need = strlen(name) + 2 + strlen(value) + 2;
    if (strpbrk(name, "\r\n") || strpbrk(value, "\r\n")) return -1;
    if (r->extra_len + need >= sizeof(r->extra)) return -1;
    r->extra_len += (size_t)snprintf(r->extra + r->extra_len, sizeof(r->extra) - r->extra_len, "%s: %s\r\n", name, value);
    return 0;
}

static size_t mem_produce(void *ctx, char *buf, size_t cap)
{
    http_resp_t *r = ctx;
    size_t left = r->body_len - r->mem_off;
    if (cap > left) cap = left;
    memcpy(buf, r->mem + r->mem_off, cap);
    r->mem_off += cap;
    return cap;
}

void http_resp_body_mem(http_resp_t *r, const void *data, size_t len)
{
    r->mem = data;
    r->mem_off = 0;
    r->body_len = len;
    r->produce = len ? mem_produce : NULL;
    r->ctx = r;
}

void http_resp_body_stream(http_resp_t *r, size_t len, http_produce_fn fn, void *ctx)
{
    r->body_len = len;
    r->produce = len ? fn : NULL;
    r->ctx = ctx;
}

void http_resp_defer(http_resp_t *r, unsigned ms)
{
    r->defer_ms = ms > HTTP_DEFER_MAX_MS ? HTTP_DEFER_MAX_MS : ms;
}

void *http_resp_alloc(http_resp_t *r, size_t size)
{
    if (r->owned) return NULL;
    r->owned = calloc(1, size);
    return r->owned;
}

/* ------------------------------------------------------------------ routes */

int http_srv_route(const char *path, unsigned methods, http_handler_t fn, void *user)
{
    int rc = -1;
    pthread_mutex_lock(&g.lock);
    if (!g.running && g.nroutes < HTTP_MAX_ROUTES && path && path[0] == '/' && fn && methods) {
        char *p = strdup(path);
        if (p) {
            g.routes[g.nroutes++] = (route_t){ p, methods, fn, user };
            rc = 0;
        }
    }
    pthread_mutex_unlock(&g.lock);
    return rc;
}

/* ------------------------------------------------------------------ helpers */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return status >= 500 ? "Server Error" : status >= 400 ? "Client Error" : "OK";
    }
}

static void conn_close(conn_t *c)
{
    free(c->rbuf);
    free(c->resp.owned);
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int has_token(const char *v, const char *tok)
{
    size_t n = strlen(tok);
    for (const char *p = v; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - s) == n && strncasecmp(s, tok, n) == 0) return 1;
    }
    return 0;
}

/* Parse the request head in rbuf[0 .. hl) (hl = offset of the final CRLFCRLF), in place. Returns 0 or the HTTP
 * status to answer. Only offsets are stored (rbuf may be reallocated afterwards). */
static int parse_head(conn_t *c, size_t hl)
{
    char *b = c->rbuf;
    long long cl = -1;
    int te = 0;
    c->off_path = c->off_query = c->off_auth = c->off_ctype = -1;
    if (memchr(b, 0, hl)) return 400;       /* a NUL would hide the rest of its line from the C-string parsing below */
    b[hl] = 0;
    size_t pos = 0;
    int first = 1;
    while (pos <= hl) {
        char *line = b + pos;
        char *e = NULL;
        for (size_t i = pos; i < hl; i++)
            if (b[i] == '\r' && b[i + 1] == '\n') { e = b + i; break; }
        if (!e) e = b + hl;
        *e = 0;
        pos = (size_t)(e - b) + 2;
        if (memchr(line, '\n', (size_t)(e - line)) || memchr(line, '\r', (size_t)(e - line))) return 400;
        if (first) {
            first = 0;
            char *sp1 = strchr(line, ' ');
            if (!sp1) return 400;
            *sp1++ = 0;
            char *sp2 = strchr(sp1, ' ');
            if (!sp2) return 400;
            *sp2++ = 0;
            if (strchr(sp2, ' ')) return 400;
            if (strcmp(line, "GET") == 0) c->is_get = 1;
            else if (strcmp(line, "POST") == 0) c->is_post = 1;
            if (!*line || sp1[0] != '/') return 400;
            char *q = strchr(sp1, '?');
            if (q) { *q++ = 0; c->off_query = (int)(q - b); }
            c->off_path = (int)(sp1 - b);
            if (strcmp(sp2, "HTTP/1.1") == 0) c->http10 = 0;
            else if (strcmp(sp2, "HTTP/1.0") == 0) c->http10 = 1;
            else if (strncmp(sp2, "HTTP/", 5) == 0 && sp2[5] >= '0' && sp2[5] <= '9') return 505;
            else return 400;
            continue;
        }
        if (*line == ' ' || *line == '\t') return 400;      /* obsolete line folding */
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return 400;
        *colon = 0;
        if (strpbrk(line, " \t")) return 400;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if (strcasecmp(line, "content-length") == 0) {
            if (!*v || strlen(v) > 9 || strspn(v, "0123456789") != strlen(v)) return 400;
            long long n = atoll(v);
            if (cl >= 0 && cl != n) return 400;
            cl = n;
        } else if (strcasecmp(line, "transfer-encoding") == 0) {
            te = 1;
        } else if (strcasecmp(line, "connection") == 0) {
            if (has_token(v, "close")) c->want_close = 1;
        } else if (strcasecmp(line, "expect") == 0) {
            if (strcasecmp(v, "100-continue") == 0) c->expect = 1;
        } else if (strcasecmp(line, "authorization") == 0) {
            c->off_auth = (int)(v - b);
        } else if (strcasecmp(line, "content-type") == 0) {
            c->off_ctype = (int)(v - b);
        }
    }
    if (te) return 411;                                      /* chunked (or any) request body: not supported */
    if (cl > HTTP_BODY_MAX) return 413;
    if (cl < 0 && c->is_post) return 411;
    c->body_len = cl > 0 ? (size_t)cl : 0;
    return 0;
}

/* Build the status line and headers into obuf and put the first body bytes after them. Returns 0 or -1. */
static int fill_body(conn_t *c)
{
    size_t space = HTTP_OBUF - c->olen;
    size_t want = c->body_left < space ? c->body_left : space;
    if (!want) return 0;
    size_t n = c->resp.produce(c->resp.ctx, c->obuf + c->olen, want);
    if (n == 0 || n > want) return -1;
    c->olen += n;
    c->body_left -= n;
    return 0;
}

static int begin_resp(conn_t *c)
{
    http_resp_t *r = &c->resp;
    if (!r->status) { r->status = 500; r->body_len = 0; }
    if (r->body_len && !r->produce) { r->status = 500; r->body_len = 0; }
    if (r->content_type && strlen(r->content_type) > 100) r->content_type = NULL;
    c->close_after = r->close || c->want_close || c->http10;
    c->body_left = r->body_len;
    r->extra[sizeof(r->extra) - 1] = 0;
    int h = snprintf(c->obuf, HTTP_OBUF, "HTTP/1.1 %d %s\r\n%s%s%sContent-Length: %zu\r\nConnection: %s\r\n%s\r\n",
                     r->status, reason(r->status),
                     (r->content_type && r->body_len) ? "Content-Type: " : "",
                     (r->content_type && r->body_len) ? r->content_type : "",
                     (r->content_type && r->body_len) ? "\r\n" : "",
                     r->body_len, c->close_after ? "close" : "keep-alive", r->extra);
    if (h < 0 || h >= HTTP_OBUF) return -1;
    c->olen = (size_t)h;
    c->ooff = 0;
    c->state = S_WRITE;
    c->t_act = now_ms();
    return fill_body(c);
}

/* Answer a request the server itself rejects; the connection is closed after the reply (lingering, so that an
 * unread request body does not turn the close into a reset that destroys the reply). */
static int send_error(conn_t *c, int status)
{
    free(c->resp.owned);
    memset(&c->resp, 0, sizeof(c->resp));
    c->resp.status = status;
    c->resp.close = 1;
    c->linger = 1;
    return begin_resp(c);
}

static void dispatch(conn_t *c)
{
    http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = c->rbuf;
    req.path = c->rbuf + c->off_path;
    req.query = c->off_query >= 0 ? c->rbuf + c->off_query : NULL;
    req.authorization = c->off_auth >= 0 ? c->rbuf + c->off_auth : NULL;
    req.content_type = c->off_ctype >= 0 ? c->rbuf + c->off_ctype : NULL;
    req.body = c->rbuf + c->head_len;
    req.body_len = c->body_len;
    req.peer = c->peer;
    req.local = c->local;
    c->served = 1;

    free(c->resp.owned);
    memset(&c->resp, 0, sizeof(c->resp));
    const route_t *rt = NULL;
    for (int i = 0; i < g.nroutes; i++)
        if (strcmp(g.routes[i].path, req.path) == 0) { rt = &g.routes[i]; break; }
    unsigned m = c->is_post ? HTTP_M_POST : c->is_get ? HTTP_M_GET : 0;
    if (!rt) {
        c->resp.status = 404;
    } else if (!(rt->methods & m)) {
        c->resp.status = 405;
        http_resp_header(&c->resp, "Allow", rt->methods == HTTP_M_POST ? "POST" : rt->methods == HTTP_M_GET ? "GET" : "GET, POST");
    } else {
        rt->fn(&req, &c->resp, rt->user);
    }
}

/* ------------------------------------------------------------------ per-connection state machine */

static int try_parse(conn_t *c)
{
    if (c->rlen == 0) return 0;
    if (c->head_len == 0) {
        size_t i = c->scan, found = (size_t)-1;
        for (; i + 4 <= c->rlen; i++)
            if (c->rbuf[i] == '\r' && memcmp(c->rbuf + i, "\r\n\r\n", 4) == 0) { found = i; break; }
        if (found == (size_t)-1) {
            c->scan = c->rlen >= 3 ? c->rlen - 3 : 0;
            if (c->rlen >= HTTP_HEAD_MAX) return send_error(c, 431);
            return 0;
        }
        if (found + 4 > HTTP_HEAD_MAX) return send_error(c, 431);
        int st = parse_head(c, found);
        if (st) return send_error(c, st);
        c->head_len = found + 4;
        size_t need = c->head_len + c->body_len;
        if (c->rcap < need) {
            char *nb = realloc(c->rbuf, need);
            if (!nb) return send_error(c, 503);
            c->rbuf = nb;
            c->rcap = need;
        }
    }
    size_t need = c->head_len + c->body_len;
    if (c->rlen < need) {
        if (c->expect && !c->sent100) {
            c->sent100 = 1;
            (void)send(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
        return 0;
    }
    dispatch(c);
    if (c->resp.defer_ms) {
        c->defer_until = now_ms() + c->resp.defer_ms;
        c->state = S_DEFER;
        return 0;
    }
    return begin_resp(c);
}

/* The response is fully written: keep-alive, linger or close. Returns -1 to close the connection. */
static int resp_done(conn_t *c)
{
    free(c->resp.owned);
    c->resp.owned = NULL;
    if (c->close_after) {
        if (!c->linger) return -1;
        shutdown(c->fd, SHUT_WR);
        c->state = S_LINGER;
        c->t_act = now_ms();
        return 0;
    }
    size_t need = c->head_len + c->body_len;
    memmove(c->rbuf, c->rbuf + need, c->rlen - need);
    c->rlen -= need;
    c->head_len = c->body_len = c->scan = 0;
    c->is_get = c->is_post = c->http10 = c->want_close = c->expect = c->sent100 = 0;
    c->state = S_READ;
    c->t_start = c->rlen ? now_ms() : 0;
    c->t_act = now_ms();
    if (c->rlen == 0) { free(c->rbuf); c->rbuf = NULL; c->rcap = 0; }
    return 0;
}

static int write_step(conn_t *c)
{
    for (int it = 0; it < 8; it++) {
        while (c->ooff < c->olen) {
            ssize_t n = send(c->fd, c->obuf + c->ooff, c->olen - c->ooff, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            c->ooff += (size_t)n;
            c->t_act = now_ms();
        }
        if (c->body_left == 0) return resp_done(c);
        c->ooff = c->olen = 0;
        if (fill_body(c) < 0) return -1;
    }
    return 0;           /* yield to the other connections; POLLOUT fires again */
}

/* Run the machine until it needs I/O. Returns -1 when the connection must be closed. */
static int pump(conn_t *c)
{
    for (;;) {
        if (c->state == S_WRITE) {
            if (write_step(c) < 0) return -1;
            if (c->state == S_WRITE) return 0;
        } else if (c->state == S_READ) {
            if (try_parse(c) < 0) return -1;
            if (c->state != S_WRITE) return 0;
        } else {
            return 0;
        }
    }
}

static int on_readable(conn_t *c)
{
    if (c->state == S_WRITE || c->state == S_DEFER) return -1;      /* only reached by POLLHUP while a reply is pending: the peer is gone */
    if (c->state == S_LINGER) {
        char tmp[256];
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) return -1;
        return 0;
    }
    if (!c->rbuf) {
        c->rbuf = malloc(HTTP_RBUF_INIT);
        if (!c->rbuf) return -1;
        c->rcap = HTTP_RBUF_INIT;
    }
    if (c->rlen == c->rcap) {
        if (c->head_len == 0 && c->rcap < HTTP_HEAD_MAX) {
            size_t nc = c->rcap * 2 > HTTP_HEAD_MAX ? HTTP_HEAD_MAX : c->rcap * 2;
            char *nb = realloc(c->rbuf, nc);
            if (!nb) return -1;
            c->rbuf = nb;
            c->rcap = nc;
        } else {
            return 0;
        }
    }
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) ? 0 : -1;
    c->rlen += (size_t)n;
    c->t_act = now_ms();
    if (!c->t_start) c->t_start = c->t_act;
    return pump(c);
}

static long long deadline(const conn_t *c)
{
    switch (c->state) {
    case S_WRITE:  return c->t_act + HTTP_WRITE_TIMEOUT_MS;
    case S_LINGER: return c->t_act + HTTP_LINGER_MS;
    case S_DEFER:  return c->defer_until;
    /* a connection that never completed a request is held to the request deadline, not the keep-alive one */
    default:       return c->t_start ? c->t_start + g.req_ms : c->t_act + (c->served ? g.idle_ms : g.req_ms);
    }
}

static long long g_accept_pause;     /* ms: do not poll the listen socket before this time */

static void accept_all(void)
{
    for (;;) {
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        int fd = accept4(g.listen_fd, (struct sockaddr *)&pa, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            /* out of descriptors or memory: the listen socket stays readable, so polling it would spin the CPU */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
                g_accept_pause = now_ms() + HTTP_ACCEPT_PAUSE_MS;
            return;                   /* EAGAIN, or a transient error: poll again */
        }
        conn_t *c = NULL;
        for (int i = 0; i < HTTP_MAX_CONNS; i++)
            if (g.conns[i].fd < 0) { c = &g.conns[i]; break; }
        if (!c) {
            /* Full: the longest-idle connection with nothing buffered (never-sent or idle keep-alive) makes room, so
             * idle sockets cannot keep every slot. Connections with a request in progress or a reply pending stay. */
            for (int i = 0; i < HTTP_MAX_CONNS; i++) {
                conn_t *e = &g.conns[i];
                if (e->state == S_READ && e->rlen == 0 && !e->t_start && (!c || e->t_act < c->t_act)) c = e;
            }
            if (c) conn_close(c);
        }
        if (!c) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->state = S_READ;
        c->peer = pa;
        socklen_t ll = sizeof(c->local);
        if (getsockname(fd, (struct sockaddr *)&c->local, &ll) < 0) memset(&c->local, 0, sizeof(c->local));
        c->t_act = now_ms();
    }
}

static void *server_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), HTTP_NICE);
    struct pollfd pfd[2 + HTTP_MAX_CONNS];
    while (!g.stop) {
        long long now = now_ms(), next = -1;
        pfd[0] = (struct pollfd){ g.wake_r, POLLIN, 0 };
        int paused = now < g_accept_pause;
        pfd[1] = (struct pollfd){ g.listen_fd, paused ? 0 : POLLIN, 0 };
        if (paused) next = g_accept_pause;
        for (int i = 0; i < HTTP_MAX_CONNS; i++) {
            conn_t *c = &g.conns[i];
            pfd[2 + i] = (struct pollfd){ c->fd, 0, 0 };
            if (c->fd < 0) continue;
            pfd[2 + i].events = c->state == S_WRITE ? POLLOUT : c->state == S_DEFER ? 0 : POLLIN;
            long long d = deadline(c);
            if (next < 0 || d < next) next = d;
        }
        int to = next < 0 ? -1 : (next - now < 0 ? 0 : (int)(next - now) + 1);
        int r = poll(pfd, 2 + HTTP_MAX_CONNS, to);
        if (r < 0 && errno != EINTR) break;
        if (g.stop || (r > 0 && pfd[0].revents)) break;
        if (r > 0 && (pfd[1].revents & POLLIN)) accept_all();
        for (int i = 0; i < HTTP_MAX_CONNS; i++) {
            conn_t *c = &g.conns[i];
            if (c->fd < 0) continue;
            int bad = 0;
            short ev = r > 0 ? pfd[2 + i].revents : 0;
            if (ev & (POLLERR | POLLNVAL)) bad = 1;
            else if (ev & POLLOUT) bad = write_step(c) < 0 || pump(c) < 0;
            else if (ev & (POLLIN | POLLHUP)) bad = on_readable(c) < 0;
            if (!bad && now_ms() >= deadline(c)) {
                if (c->state == S_DEFER) {
                    c->resp.defer_ms = 0;
                    bad = begin_resp(c) < 0 || pump(c) < 0;
                } else if (c->state == S_READ && c->t_start) bad = send_error(c, 408) < 0 || pump(c) < 0;
                else bad = 1;
            }
            if (bad) conn_close(c);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ start / stop */

static void teardown(void)
{
    if (g.conns) {
        for (int i = 0; i < HTTP_MAX_CONNS; i++)
            if (g.conns[i].fd >= 0) conn_close(&g.conns[i]);
        free(g.conns);
        g.conns = NULL;
    }
    if (g.listen_fd >= 0) close(g.listen_fd);
    if (g.wake_r >= 0) close(g.wake_r);
    if (g.wake_w >= 0) close(g.wake_w);
    g.listen_fd = g.wake_r = g.wake_w = -1;
    g.port = 0;
}

int http_srv_start(int port)
{
    int rc = -1;
    pthread_mutex_lock(&g.lock);
    if (g.running) {
        rc = (port == g.port || port == 0) ? 0 : -1;
        if (rc) errno = EADDRINUSE;
        goto out;
    }
    if (port < 0 || port > 65535) { errno = EINVAL; goto out; }
    if (!g.req_ms) g.req_ms = HTTP_REQ_TIMEOUT_MS;
    if (!g.idle_ms) g.idle_ms = HTTP_IDLE_TIMEOUT_MS;
    g.conns = calloc(HTTP_MAX_CONNS, sizeof(conn_t));
    if (!g.conns) goto out;
    for (int i = 0; i < HTTP_MAX_CONNS; i++) g.conns[i].fd = -1;
    int wp[2];
    if (pipe2(wp, O_CLOEXEC | O_NONBLOCK) < 0) { teardown(); goto out; }
    g.wake_r = wp[0];
    g.wake_w = wp[1];
    g.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g.listen_fd < 0) { int e = errno; teardown(); errno = e; goto out; }
    int one = 1;
    setsockopt(g.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    socklen_t al = sizeof(a);
    if (bind(g.listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g.listen_fd, HTTP_MAX_CONNS) < 0 ||
        getsockname(g.listen_fd, (struct sockaddr *)&a, &al) < 0) {
        int e = errno;
        teardown();
        errno = e;
        goto out;
    }
    g.port = ntohs(a.sin_port);
    g.stop = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HTTP_STACK_BYTES);
    int pr = pthread_create(&g.tid, &attr, server_thread, NULL);
    pthread_attr_destroy(&attr);
    if (pr != 0) { teardown(); errno = pr; goto out; }
    g.running = 1;
    printf("[http] listening on port %d\n", g.port);
    rc = 0;
out:
    pthread_mutex_unlock(&g.lock);
    return rc;
}

void http_srv_stop(void)
{
    pthread_mutex_lock(&g.lock);
    if (g.running) {
        g.stop = 1;
        (void)!write(g.wake_w, "x", 1);
        pthread_join(g.tid, NULL);
        g.running = 0;
        teardown();
        printf("[http] stopped\n");
    }
    pthread_mutex_unlock(&g.lock);
}

int http_srv_port(void)
{
    /* No lock: a handler (server thread) may call this while http_srv_stop holds the lock and joins that thread. */
    return g.port;
}

void http_srv_set_timeouts(int req_ms, int idle_ms)
{
    pthread_mutex_lock(&g.lock);
    if (!g.running) {
        g.req_ms = req_ms > 0 ? req_ms : HTTP_REQ_TIMEOUT_MS;
        g.idle_ms = idle_ms > 0 ? idle_ms : HTTP_IDLE_TIMEOUT_MS;
    }
    pthread_mutex_unlock(&g.lock);
}
