/* Optional RTSP Digest authentication (RFC 2617, "MD5", no qop in the
 * challenge; see specs/03-spec-rtsp-auth.md). Pure logic: no sockets, no
 * SmolRTSP, time and randomness passed in or hooked, so it is tested on the
 * host (tests/native/rtsp_auth_test.c) and wired to the RTSP server in rtsp.c.
 *
 * Credentials live in a file on the camera's persistent CONFIG partition:
 *   /etc/config/akam/rtsp_auth       one line:  user:akam:<HA1 in lowercase hex>
 * with HA1 = MD5("user:akam:password"). The password itself is never stored.
 * The file is read again on every protected request:
 *   absent                -> RTSP_AUTH_OFF     (no authentication, the default)
 *   present and valid     -> RTSP_AUTH_ON      (Digest required)
 *   present, not valid    -> RTSP_AUTH_BROKEN  (fail closed: everything refused)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTSP_AUTH_PATH "/etc/config/akam/rtsp_auth"
#define RTSP_AUTH_REALM "akam"
#define RTSP_AUTH_USER_MAX 32
#define RTSP_AUTH_NONCE_HEX 32
/* A nonce can be used to authenticate for this long after it was issued. */
#define RTSP_AUTH_NONCE_LIFETIME_US (120LL * 1000000)
/* Failed attempts on one connection before it is closed. */
#define RTSP_AUTH_CONN_MAX_FAILS 5
/* The process-wide failure counter is forgotten after this much quiet time. */
#define RTSP_AUTH_FAIL_WINDOW_US (60LL * 1000000)

typedef enum { RTSP_AUTH_OFF, RTSP_AUTH_ON, RTSP_AUTH_BROKEN } rtsp_auth_mode_t;

/* Credentials, shared by all connections (one per process). */
typedef struct {
    const char *path;
    rtsp_auth_mode_t mode;
    char user[RTSP_AUTH_USER_MAX + 1];
    char ha1[33];
    unsigned generation;     /* bumped whenever mode, user or HA1 changes */
    bool loaded;
    unsigned global_fails;   /* failed attempts in the current window */
    int64_t last_fail_us;
} rtsp_auth_db_t;

/* State of one RTSP connection. */
typedef struct {
    bool has_nonce;
    char nonce[RTSP_AUTH_NONCE_HEX + 1];
    int64_t nonce_us;
    bool authed;             /* proved the password against generation gen */
    unsigned gen;
    uint32_t last_nc;        /* highest nc seen (qop=auth responses only) */
    unsigned fails;          /* failed attempts on this connection */
} rtsp_auth_conn_t;

typedef enum {
    RTSP_AUTH_ALLOW,         /* serve the request */
    RTSP_AUTH_DENY,          /* answer 401 with .challenge, after .delay_ms */
    RTSP_AUTH_DROP,          /* answer 401 (if .challenge[0]) then close */
} rtsp_auth_verdict_t;

typedef struct {
    rtsp_auth_verdict_t verdict;
    char challenge[192];     /* WWW-Authenticate value; "" = none */
    unsigned delay_ms;       /* sleep before answering (failed attempts) */
    char log[112];           /* one journal line, "" = nothing to log; never a secret */
} rtsp_auth_result_t;

/* Randomness source, replaceable by tests: fills buf with n bytes, 0 = ok.
 * The default reads getrandom() and falls back to /dev/urandom. */
extern int (*rtsp_auth_rng)(void *buf, size_t n);

void rtsp_auth_db_init(rtsp_auth_db_t *db, const char *path);
/* Re-read the credentials file; returns the current mode. */
rtsp_auth_mode_t rtsp_auth_db_refresh(rtsp_auth_db_t *db);
/* Parse the content of a credentials file (exposed for tests): 0 = valid. */
int rtsp_auth_parse_file(const char *text, size_t len, char *user, char *ha1);

void rtsp_auth_conn_init(rtsp_auth_conn_t *c);

/* Everything but OPTIONS needs credentials. */
bool rtsp_auth_method_protected(const char *method);

/* Decide on one request. hdr is the value of the Authorization header (not
 * NUL terminated; NULL when the request has none). method is the request's
 * method token. now_us is a monotonic clock. */
void rtsp_auth_check(rtsp_auth_db_t *db, rtsp_auth_conn_t *c, const char *method,
                     const char *hdr, size_t hdr_len, int64_t now_us,
                     rtsp_auth_result_t *out);

/* For a connection that is already streaming: false once its authentication
 * no longer matches the configuration (auth enabled, or other credentials,
 * since it connected), so that the stream is cut and the client must log in
 * again. Re-reads the file. */
bool rtsp_auth_session_ok(rtsp_auth_db_t *db, const rtsp_auth_conn_t *c);

/* Copy uri into out with any "user[:password]@" part replaced by "***@", so
 * that a client that puts its credentials in the URL cannot leak them into
 * the journal. */
void rtsp_auth_redact_uri(const char *uri, char *out, size_t cap);

/* Digest response, exposed for tests. qop_nc may be NULL (RFC 2069 form). */
void rtsp_auth_response(char out[33], const char *ha1, const char *nonce,
                        const char *nc, const char *cnonce, const char *method,
                        const char *uri);
