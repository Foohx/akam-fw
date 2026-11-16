/* Request handling of the local control API (specs/control-api.md section 4.2): routing, authentication (api_sig), rate limits
 * (api_limit), the endpoints, the key store and the audit ring. No sockets and no threads of its own: the HTTP server
 * (akam2k_httpd.c) hands over one parsed request and gets the reply body; settings and actions run on the settings worker
 * (akam2k_settings.h). Everything here runs on the ONE HTTP thread, so the module has no locks.
 *
 * Endpoints (all under /akam/v1, JSON): GET hello (unsigned), info, schema, settings[?prefix=], settings/<key>, state,
 * events[?since=N&timeout=S] (long poll), audit (admin), keys (admin); PATCH settings; POST actions/<name>; POST keys (admin);
 * DELETE keys/<id> (admin). Errors are {"error":"code","detail":"text"}. */
#ifndef AKAM2K_API_H
#define AKAM2K_API_H
#include <stddef.h>
#include <stdint.h>

#include "http_req.h"

#define A2K_API_PORT 8092
#define A2K_API_PREFIX "/akam/v1"
#define A2K_API_KEYS "/config/akam/api_keys"
#define A2K_API_AUDIT "/config/akam/audit"
#define A2K_API_RESP_MAX 12288        /* the largest reply body (the schema is about 6 KB) */
#define A2K_API_EVENT_WAIT_MAX 25     /* seconds a GET /events may wait */
#define A2K_API_CLOCK_FLOOR 1767225600 /* 2026-01-01: a wall clock below it is "not set" */
#define A2K_API_AUDIT_RING 64
#define A2K_API_AUDIT_PERSIST 20

/* Injected so the host tests are deterministic; NULL members use the real clocks, the app's random source and the paths above. */
typedef struct {
    int64_t (*wall_s)(void);
    uint64_t (*mono_ms)(void);
    int (*rng)(void *buf, size_t n); /* 0 or -1 */
    const char *keys_path;
    const char *audit_path;
    uint32_t rate_milli, burst; /* accepted-request bucket (api_limit.h); 0 = the spec defaults. A test seam: the camera never sets them */
} a2k_api_env_t;

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

/* On-camera key tool (akam2k_rtsp --api-key new|list|revoke ...), run from SSH by overlay/akam/akam_api_key.sh before any
 * hardware init. new --scope read|write|admin --name LABEL prints the copy block (the secret) ONCE on stdout; list never prints a
 * secret; revoke <id|name> [--force]. Returns the process exit status (0, 1 failure, 2 usage, 4 the pool is not seeded). */
int a2k_api_key_cli(const a2k_api_env_t *env, int argc, char **argv);

#endif
