/* Credential store for the protocols that need the clear-text password
 * (ONVIF WS-Security PasswordDigest; later MQTT). specs/07 section 3.3, user
 * decision U2. The RTSP Digest login keeps using its htdigest file
 * (rtsp_auth.h); `./akam rtsp-auth set` writes both.
 *
 * File /etc/config/akam/credentials (CONFIG partition, mode 0600, directory
 * 0700, owned by root or the running user), text, one `key=value` per line,
 * LF line ends, at most CRED_FILE_MAX bytes:
 *     user=NAME        1..32 of A-Z a-z 0-9 . _ @ + -   (same rule as rtsp_auth)
 *     pass=PASSWORD    1..128 bytes, every byte 0x20..0x7e or >= 0x80, taken
 *                      literally up to the end of the line (spaces, '=' and
 *                      '#' belong to it)
 * Blank lines and lines starting with '#' are ignored. Both keys are required,
 * each once, no other key: anything else makes the file invalid and the plain
 * credentials unavailable (fail closed: no ONVIF login, the RTSP login is not
 * affected). The file is looked at again when its inode, size or mtime change,
 * checked at most every CRED_RECHECK_US. The password is never logged: this
 * module prints nothing at all.
 *
 * Single-threaded use (the ONVIF thread); state is process-global.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rtsp_auth.h"

#ifndef CRED_PATH /* tests point it at a temporary file */
#define CRED_PATH "/etc/config/akam/credentials"
#endif
#define CRED_USER_MAX RTSP_AUTH_USER_MAX
#define CRED_PASS_MAX 128
#define CRED_FILE_MAX 512
#define CRED_RECHECK_US (3LL * 1000000)
/* WS-Security */
#define CRED_NONCE_CACHE 32
#define CRED_NONCE_RAW_MAX 64            /* decoded nonce, bytes */
#define CRED_CREATED_MAX 40              /* xsd:dateTime text, characters */
#define CRED_CLOCK_WINDOW_S 300          /* Created must be within +/- 5 min ... */
#define CRED_CLOCK_MIN_YEAR_S 1704067200 /* ... when the wall clock is >= 2024-01-01 */

typedef enum {
    CRED_ABSENT,      /* no file: no plain credentials */
    CRED_OK,          /* valid */
    CRED_INVALID,     /* present but unusable (format, permissions, size, owner) */
} cred_state_t;

/* Clocks, replaceable by tests. wall: Unix seconds. mono: microseconds, monotonic. */
extern int64_t (*cred_wall_s)(void);
extern int64_t (*cred_mono_us)(void);

/* Point the store at a file (NULL = CRED_PATH) and forget everything (including
 * the nonce cache). Optionally bind the failure limiter: the one of the RTSP
 * server (rtsp_auth_shared_db() in rtsp.c) so that RTSP and ONVIF share one
 * counter and one delay; NULL keeps a private one. */
void cred_init(const char *path, rtsp_auth_db_t *limiter);

/* Look at the file again if it is time (or when force). Returns the state. The
 * accessors below call it themselves, so a caller only needs this for status. */
cred_state_t cred_refresh(bool force);
/* Why CRED_INVALID, a fixed text without any secret ("" otherwise). */
const char *cred_problem(void);

/* The user name, or NULL when there are no valid plain credentials. Valid
 * until the next refresh. */
const char *cred_get_user(void);
bool cred_have_plain(void);

/* Parse a file's content (exposed for tests): 0 = valid. user and pass receive
 * NUL-terminated copies (sizes CRED_USER_MAX + 1, CRED_PASS_MAX + 1). */
int cred_parse(const char *text, size_t len, char *user, char *pass);

/* WS-Security UsernameToken, PasswordDigest = Base64(SHA1(nonce + created +
 * password)) with nonce the decoded Nonce. All four arguments are the element
 * texts as received (NUL-terminated, nothing trimmed). Constant-time compare;
 * nonce reuse refused (last CRED_NONCE_CACHE verified nonces); Created checked
 * within +/- 5 minutes only when the wall clock is >= 2024. Every failure
 * (unknown user, no plain credentials, malformed input, bad digest, replay, old
 * Created) counts in the shared limiter and sets *delay_ms to the time to wait
 * before answering (the caller sleeps or defers; may be NULL); a success resets
 * the counter. Returns 1 = authenticated, 0 = refused (answer ter:NotAuthorized,
 * nothing more specific). */
int cred_check_wsse(const char *username, const char *nonce_b64, const char *created,
                    const char *digest_b64, unsigned *delay_ms);

/* HTTP Digest (RFC 2617, MD5, realm akam) for ONVIF over HTTP: the htdigest
 * credentials of rtsp_auth.c, no plain password needed. hdr is the value of
 * the Authorization header (not NUL terminated), method the HTTP method.
 * Verifies user, realm, algorithm and response; on success fills *d so that
 * the caller can check its own nonce (issue them with rtsp_auth_new_nonce),
 * nc and uri. Same limiter and delay semantics as cred_check_wsse. Returns 1
 * = credentials good, 0 = refused (also when the RTSP password is off: then
 * the caller does not ask for authentication at all, see cred_auth_enabled). */
int cred_check_http_digest(const char *method, const char *hdr, size_t hdr_len,
                           rtsp_auth_digest_t *d, unsigned *delay_ms);

/* False only when the RTSP password is off (no htdigest file): ONVIF reads are
 * then open and changes refused (spec 3.3). True for a valid file AND for an
 * invalid one (fail closed: authentication is required and nobody passes). */
bool cred_auth_enabled(void);
