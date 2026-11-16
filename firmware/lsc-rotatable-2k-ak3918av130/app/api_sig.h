/* Request signing for the local control API (specs/control-api.md section 5): canonical string builder and request
 * verifier. Pure: time, randomness and the key table are INJECTED, there is no I/O, no allocation, no global state. The
 * caller serialises access to one api_sig_t (the API thread). Per-peer lockout and the accepted-rate token bucket
 * (spec 5.5, 7.1) belong to the HTTP layer (phase 2); this file gives the nonce cache and challenge table they sit on.
 *
 * Canonical query rule fixed here (the spec leaves two points open, the vectors pin them): a parameter without '=' is
 * written "name=" ; sorting is bytewise on the RE-ENCODED name, then on the re-encoded value. An empty segment ("a&&b",
 * "&a", "a&") or a "%00" is malformed. */
#ifndef AKAM_API_SIG_H
#define AKAM_API_SIG_H
#include <stddef.h>
#include <stdint.h>

#include "api_keys.h"
#include "sha256.h"

#define APISIG_VERSION_LINE "AKAM1-HMAC-SHA256"
#define APISIG_PATH_MAX 128
#define APISIG_QUERY_MAX 512       /* raw query bytes accepted */
#define APISIG_QUERY_PARAMS 24
#define APISIG_CANON_MAX 2304      /* canonical string buffer */
#define APISIG_NONCE_CAP 64
#define APISIG_CHAL_CAP 8
#define APISIG_CHAL_TTL_MS 30000u
#define APISIG_RATE_MILLI_DEFAULT 400 /* accepted requests per second x 1000 (spec 5.5) */
#define APISIG_BURST_DEFAULT 14       /* 0.4 x 125 + 14 = 64 = APISIG_NONCE_CAP */
#define APISIG_WINDOW_DEFAULT 60
#define APISIG_WINDOW_MIN 10
#define APISIG_WINDOW_MAX 300

/* Verifier outcomes: the HTTP status to send; the machine code is in apisig_result_t.error. */
enum { APISIG_OK = 0 };

typedef struct {
    int http;          /* 0 = ok, else 400, 401, 403, 429 */
    const char *error; /* "bad_request","bad_header","missing_auth","bad_signature","clock_skew","clock_unset",
                          "replay","bad_challenge","scope","busy"; NULL when ok */
    int key_idx;       /* index in the key table when ok (or when the failure is scope), else -1 */
    uint8_t scope;     /* the key's scope when known */
    int64_t skew_s;    /* clock_skew: now - timestamp, so the reply can show the camera time */
} apisig_result_t;

typedef struct {
    uint8_t used;
    uint32_t kid; /* key id as a number (8 hex digits) */
    uint8_t nonce[16];
    uint64_t expires_ms; /* monotonic */
} apisig_nonce_t;

typedef struct {
    uint8_t used;
    uint8_t chal[16];
    uint64_t expires_ms; /* monotonic */
} apisig_chal_t;

typedef struct {
    apisig_nonce_t nonce[APISIG_NONCE_CAP];
    apisig_chal_t chal[APISIG_CHAL_CAP];
    uint32_t window_s;
    unsigned long hmac_calls; /* HMACs run (test hook: unknown key and wrong signature must each run exactly one) */
} api_sig_t;

/* window_s is clamped to 10..300. */
void apisig_init(api_sig_t *s, uint32_t window_s);
/* Nonce retention = 2 x window + 5 s. */
uint64_t apisig_retention_ms(const api_sig_t *s);
/* Spec 5.5 invariant: capacity >= accepted_rate x retention + burst. rate in milli-requests per second. 1 when it holds. */
int apisig_cache_invariant_ok(size_t capacity, uint32_t rate_milli_per_s, uint32_t retention_s, uint32_t burst);

/* ---- building blocks ---- */
/* Path rule (5.2): "/" then segments of [A-Za-z0-9._~-], no empty segment, no "." segment, no ".." anywhere, no "%", no trailing
 * slash, at most APISIG_PATH_MAX. 1 when accepted. */
int apisig_path_ok(const char *path);
/* Method must be exactly one of GET POST PATCH DELETE (upper case). */
int apisig_method_ok(const char *method);
/* Canonical query of the raw query (no leading '?') into out. Returns its length, or -1 malformed, -2 too long/many. */
int apisig_canon_query(const char *raw, char *out, size_t cap);
/* The canonical string. line6/line7 are the timestamp and nonce, or "challenge:<hex>" twice. The method is upper-cased
 * by the builder (the HTTP layer separately rejects non-canonical case). query = raw query or NULL/"". Returns length, or
 * -1 on a bad path/query/field, -2 buffer too small. */
int apisig_canon(char *out, size_t cap, const char *method, const char *path, const char *query, const void *body,
                 size_t body_len, const char *line6, const char *line7, const char *key_id);
/* Signature of a canonical string. */
void apisig_sign(const uint8_t secret[32], const char *canon, size_t canon_len, uint8_t out[SHA256_LEN]);

/* ---- /hello challenge ---- */
/* Issues a challenge from 16 injected random bytes: 32 hex in out (33 bytes). 0, or -1 when the table is full (fail
 * closed; the HTTP layer answers 429). Expired entries are purged first. */
int apisig_hello(api_sig_t *s, uint64_t now_mono_ms, const uint8_t rnd[16], char hex_out[33]);
/* {"api":1,"clock_set":true|false,"challenge":"<32 hex>"}; returns length or -1. */
int apisig_hello_json(char *out, size_t cap, int clock_set, const char *chal_hex);

/* ---- verification ---- */
typedef struct {
    const char *method, *path, *query; /* query: raw, no '?', may be NULL */
    const void *body;
    size_t body_len;
    /* header values, NULL when absent */
    const char *h_key, *h_timestamp, *h_nonce, *h_challenge, *h_signature;
} apisig_req_t;

/* Verifies in the spec 5.3 order. clock_set/now_wall_s: the camera wall clock (ignored in challenge mode);
 * now_mono_ms: monotonic. need_scope: the minimum level of the endpoint (AKEY_*). Returns 0 and fills res on success (the
 * nonce or challenge is then consumed), else the HTTP status. A request that fails before the signature compare consumes
 * nothing; a request that passes the signature but lacks the scope consumes its nonce (403). */
int apisig_verify(api_sig_t *s, const akeys_t *keys, int clock_set, int64_t now_wall_s, uint64_t now_mono_ms,
                  const apisig_req_t *rq, int need_scope, apisig_result_t *res);
/* Revoked key: drop its cached nonces. */
void apisig_drop_key(api_sig_t *s, const char *key_id);

#endif
