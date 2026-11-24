/* Request handling of the local control API (specs/control-api.md section 4.2), shared by the Rotatable 2K and the 1080p (it was
 * akam2k_api.c; the a2k_ names are that history and were not renamed): routing, authentication (api_sig), rate limits (api_limit),
 * the endpoints, the key store and the audit ring. No sockets and no threads of its own: the HTTP server (api_httpd.c) hands over one
 * parsed request and gets the reply body. The model binds it through a2k_api_env_t: its name and version, a log function, the file
 * store, the settings backend (registry, executor, actions) and extra routes (api_backend.h). Everything here runs on the ONE HTTP
 * thread, so the module has no locks.
 *
 * Endpoints (all under /akam/v1, JSON): GET hello (unsigned), info, schema, settings[?prefix=], settings/<key>, state,
 * events[?since=N&timeout=S] (long poll), audit (admin), keys (admin); PATCH settings; POST actions/<name>; POST keys (admin);
 * DELETE keys/<id> (admin). Errors are {"error":"code","detail":"text"}. */
#ifndef AKAM2K_API_H
#define AKAM2K_API_H
#include <stddef.h>
#include <stdint.h>

#include "api_backend.h"
#include "http_req.h"

#define A2K_API_PORT 8092
#define A2K_API_PREFIX "/akam/v1"
#define A2K_API_STORE_MAX 4096        /* the largest key file or audit file the core reads or writes */
#ifndef A2K_API_RESP_MAX
#define A2K_API_RESP_MAX 12288        /* the largest reply body (the schema is about 6 KB); the 2K app builds with -DA2K_API_RESP_MAX=18432 (its settings schema is larger) */
#endif
#define A2K_API_EVENT_WAIT_MAX 25     /* seconds a GET /events may wait */
#define A2K_API_CLOCK_FLOOR 1767225600 /* 2026-01-01: a wall clock below it is "not set" */
#define A2K_API_AUDIT_RING 64
#define A2K_API_AUDIT_PERSIST 20

/* The binding. The clocks and the random source are injected so the host tests are deterministic (NULL = the real ones; the random
 * source is rtsp_auth_rng). Required: model, version, store_read, store_write, backend and the key path (a NULL key path means no key
 * file: the API stays closed). */
typedef struct {
    int64_t (*wall_s)(void);
    uint64_t (*mono_ms)(void);
    int (*rng)(void *buf, size_t n); /* 0 or -1 */
    const char *keys_path;           /* the key file, mode 0600 (the 2K: /config/akam/api_keys, the 1080p: /etc/config/akam/api_keys) */
    const char *audit_path;          /* the persisted write audit (NULL = not persisted) */
    uint32_t rate_milli, burst; /* accepted-request bucket (api_limit.h); 0 = the spec defaults. A test seam: the camera never sets them */
    const char *model;               /* "lsc-rotatable-2k", "lsc-smart-indoor-1080p": GET /info */
    const char *version;             /* the app version string: GET /info */
    void (*log)(const char *line);   /* one formatted line without a newline, NULL = silent */
    int (*store_read)(const char *path, char *buf, size_t cap); /* length (0 = empty), -1 absent, -2 too big; buf is NUL terminated */
    int (*store_write)(const char *path, const char *text, size_t len, unsigned mode); /* atomic replace, skipped when unchanged; 0 or -1 */
    const a2k_api_backend_t *backend;
    const a2k_api_ext_t *ext;
    size_t n_ext;
} a2k_api_env_t;

/* The log line of the binding (a no-op when env.log is NULL): for api_httpd.c, which shares the env of a2k_api_init. */
void a2k_api_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Loads the key file and the persisted audit lines. Returns the number of keys (0 = the API stays closed). */
int a2k_api_init(const a2k_api_env_t *env);
/* Re-reads the key file when it changed (the on-camera tool and the other tools write it). Returns the key count. A file that is
 * invalid or readable by others closes the API (fail closed) and sets a2k_api_keys_error(). */
int a2k_api_keys_refresh(void);
int a2k_api_keys_count(void);
const char *a2k_api_keys_error(void); /* NULL, "keys_invalid" or "keys_perm" */
/* Called about once a second by the HTTP thread: key refresh, audit persistence (burst 2 then one per 30 s). */
void a2k_api_tick(void);

typedef struct {
    const hreq_t *rq;
    const void *body; /* rq->content_length bytes, NULL when none */
    size_t body_len;
    uint32_t peer_ip; /* host order; 0 unknown */
    const char *peer_str; /* dotted quad, for the audit */
} a2k_api_in_t;

typedef struct {
    int status;        /* HTTP status; 0 with wait = 1 means "hold the connection" */
    size_t len;        /* reply body length (in the buffer given to a2k_api_handle) */
    unsigned retry_s;  /* Retry-After for 429/503, 0 = none */
    int wait;          /* GET /events with nothing to report yet: wait up to wait_s seconds, then a2k_api_events_reply */
    uint32_t since;
    unsigned wait_s;
} a2k_api_out_t;

void a2k_api_handle(const a2k_api_in_t *in, char *out, size_t cap, a2k_api_out_t *res);
/* The /events body for a held request (woken by an event or by its deadline). Returns the length. */
int a2k_api_events_reply(uint32_t since, char *out, size_t cap);

/* On-camera key tool (akamd --api-key new|list|revoke ...), run from SSH by overlay/akam/akam_api_key.sh before any
 * hardware init. new --scope read|write|admin --name LABEL prints the copy block (the secret) ONCE on stdout; list never prints a
 * secret; revoke <id|name> [--force]. Returns the process exit status (0, 1 failure, 2 usage, 4 the pool is not seeded). */
int a2k_api_key_cli(const a2k_api_env_t *env, int argc, char **argv);

#endif
