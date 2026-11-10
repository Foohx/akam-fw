/* Minimal HTTP/1.1 server for the ONVIF services (spec 07 section 3.2 and 3.3).
 *
 * One thread, one poll() loop, non-blocking sockets, no thread per connection. DORMANT until http_srv_start():
 * no socket, no thread, no allocation. Limits: request line + headers 4 KB, body 16 KB (Content-Length required
 * for POST, chunked request bodies refused), at most 4 connections (the 5th gets 503 and is closed), 5 s to
 * receive a request once its first byte arrived (an absolute deadline: a slow trickle does not extend it),
 * idle keep-alive connections closed after 30 s, 5 s without write progress. Only GET and POST, exact-path routes
 * registered by the caller; any other path is 404, a route used with a method it does not accept is 405.
 *
 * Responses always carry Content-Length. They are written from a 1 KB per-connection output buffer: the handler
 * states the body length up front and gives a producer callback that is called repeatedly for the next piece
 * (see http_resp_body_stream); a template (soap.h) computes its length first without producing the body. The
 * request buffer (headers + body) stays valid until the response is fully written, so a producer may point into
 * the request. Request buffers are allocated per request and freed while a connection is idle.
 *
 * http_resp_defer postpones a response by up to HTTP_DEFER_MAX_MS without a sleep: other connections keep being
 * served, and the 5 s request/write timeouts do not run while it waits.
 *
 * Handlers run in the server thread: they must be quick and must not block. */
#pragma once
#include <netinet/in.h>
#include <stddef.h>

#define HTTP_DEFAULT_PORT 80
#define HTTP_MAX_CONNS    4
#define HTTP_HEAD_MAX     4096
#define HTTP_BODY_MAX     16384
#define HTTP_OBUF         1024
#define HTTP_REQ_TIMEOUT_MS   5000
#define HTTP_IDLE_TIMEOUT_MS  30000
#define HTTP_MAX_ROUTES   8
#define HTTP_EXTRA_MAX    384
#define HTTP_DEFER_MAX_MS 5000

#define HTTP_M_GET  1
#define HTTP_M_POST 2

typedef struct http_req {
    const char *method;         /* "GET" or "POST" */
    const char *path;           /* query string removed */
    const char *query;          /* NULL when none */
    const char *authorization;  /* Authorization header value or NULL */
    const char *content_type;   /* Content-Type header value or NULL */
    const char *body;           /* not NUL-terminated */
    size_t      body_len;
    struct sockaddr_in peer;    /* the client */
    struct sockaddr_in local;   /* the address this connection reached (getsockname): use it for XAddr */
} http_req_t;

/* Fills `len` bytes (at most `cap`) of the next piece of the body into buf; returns the count. The server calls
 * it until the declared body length is produced; a short count of 0 before that aborts the connection. */
typedef size_t (*http_produce_fn)(void *ctx, char *buf, size_t cap);

typedef struct http_resp {
    int         status;         /* required (a handler that leaves 0 gets 500) */
    const char *content_type;   /* NULL for none */
    size_t      body_len;
    http_produce_fn produce;
    void       *ctx;
    int         close;          /* force "Connection: close" */
    char        extra[HTTP_EXTRA_MAX]; /* extra header lines, "Name: value\r\n" each */
    size_t      extra_len;
    unsigned    defer_ms;       /* set with http_resp_defer */
    /* internal */
    const char *mem;
    size_t      mem_off;
    void       *owned;
} http_resp_t;

typedef void (*http_handler_t)(const http_req_t *req, http_resp_t *resp, void *user);

/* ---- handler helpers ---- */
void http_resp_set(http_resp_t *r, int status, const char *content_type);
/* Add "name: value" (e.g. WWW-Authenticate). 0, or -1 when the 384-byte header budget is exhausted. */
int  http_resp_header(http_resp_t *r, const char *name, const char *value);
/* Hold the finished response back for `ms` (capped to HTTP_DEFER_MAX_MS) without blocking the server loop: the
 * connection waits in its own state while the others are served (used for the failed-login delay of the shared
 * limiter, never a sleep). Call after the status, headers and body are set. The request buffer and any
 * http_resp_alloc memory stay valid meanwhile. A peer that hangs up during the wait drops the connection. */
void http_resp_defer(http_resp_t *r, unsigned ms);
/* Body from memory that stays valid until the response is sent (static data or the request body). */
void http_resp_body_mem(http_resp_t *r, const void *data, size_t len);
/* Body of exactly `len` bytes produced on demand. */
void http_resp_body_stream(http_resp_t *r, size_t len, http_produce_fn fn, void *ctx);
/* Zeroed memory that lives until the response is fully sent (freed by the server). One allocation per response;
 * NULL on failure or a second call. Use it for the producer context (a soap_tpl_t). */
void *http_resp_alloc(http_resp_t *r, size_t size);

/* ---- server ---- */
/* Register an exact path before http_srv_start (not allowed while running, -1). `methods` is HTTP_M_* ORed.
 * Routes survive stop/start. */
int  http_srv_route(const char *path, unsigned methods, http_handler_t fn, void *user);
/* Listen on INADDR_ANY:port (0 = pick a free port, see http_srv_port) and start the thread. 0 on success (also if
 * already running on that port), -1 with errno otherwise. */
int  http_srv_start(int port);
/* Stop the thread, close every socket, free every buffer. Safe when not running. */
void http_srv_stop(void);
int  http_srv_port(void);       /* bound port, 0 when dormant */
/* Test hook: shorten the request / idle timeouts (ms; <= 0 restores the defaults). Only while stopped. */
void http_srv_set_timeouts(int req_ms, int idle_ms);
