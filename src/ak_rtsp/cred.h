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
 * Thread safety: state is process-global, one mutex guards every cred_* entry point (lock order: cred, then
 * rtsp_auth). cred_init is called once from main() before the threads start (lazy init under the lock also works).
 * Nothing here sleeps: a failure returns the limiter delay and the CALLER waits or defers the answer.
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
#ifndef CRED_NONCE_CACHE                 /* tests shrink it to exercise eviction */
#define CRED_NONCE_CACHE 1024            /* 12 bytes each */
#endif
#define CRED_NONCE_HASH 8                /* bytes of SHA-1(nonce) kept */
#define CRED_STEP_GRACE_S 600            /* the time base before a clock step stays valid this long */
#define CRED_PEERS 8                     /* addresses tracked by the lockout */
#define CRED_PEER_FREE_FAILS 3           /* failures a peer may make before it is locked out */
#define CRED_PEER_FORGET_US (300LL * 1000000)
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

/* Copy the user name into out (cap >= 1); false (out empty) when there are no valid plain credentials. */
bool cred_get_user(char *out, size_t cap);
bool cred_have_plain(void);

/* Parse a file's content (exposed for tests): 0 = valid. user and pass receive
 * NUL-terminated copies (sizes CRED_USER_MAX + 1, CRED_PASS_MAX + 1). */
int cred_parse(const char *text, size_t len, char *user, char *pass);

/* WS-Security UsernameToken, PasswordDigest = Base64(SHA1(nonce + created +
 * password)) with nonce the decoded Nonce. All four arguments are the element
 * texts as received (NUL-terminated, nothing trimmed). Constant-time compare;
 * nonce reuse refused: each verified nonce is remembered (8 bytes of its hash + the second at which its token goes
 * stale) in CRED_NONCE_CACHE slots. Created must be within +/- 5 minutes of the token clock: the wall clock when it
 * is >= 2024, else a clock anchored on the first authenticated token (created and monotonic time), so that a captured
 * token is not replayable with an unset clock either. After a step of the clock (SetSystemDateAndTime, NTP) the
 * previous time base stays valid for CRED_STEP_GRACE_S, for clients that cached the device time. When every slot
 * holds a live nonce the one with the oldest Created is forgotten and tokens not newer than it are refused from then
 * on (never a refusal for a valid newer token). Every failure (unknown user, no plain credentials, malformed
 * input, bad digest, replay, stale Created) counts in the shared limiter and sets *delay_ms to the time to wait
 * before answering (the caller sleeps or defers; may be NULL); a success resets the counter. A valid token older than
 * everything the cache remembers is refused without counting. Returns 1 = authenticated, 0 = refused (answer
 * ter:NotAuthorized, nothing more specific). */
int cred_check_wsse(const char *username, const char *nonce_b64, const char *created,
                    const char *digest_b64, unsigned *delay_ms);
/* Same, and *counted (may be NULL) is 0 when the refusal was not blamed on the peer (authentic token older than the
 * replay cache remembers): the caller must not count it in a per-peer lockout either. */
int cred_check_wsse_ex(const char *username, const char *nonce_b64, const char *created,
                       const char *digest_b64, unsigned *delay_ms, int *counted);

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

/* Count one failed attempt in the shared limiter (for callers that reject a request after the credentials passed,
 * e.g. a replayed Digest nonce count or a wrong URI); *delay_ms as in cred_check_wsse (may be NULL). */
void cred_note_failure(unsigned *delay_ms);

/* False only when the RTSP password is off (no htdigest file): ONVIF reads are
 * then open and changes refused (spec 3.3). True for a valid file AND for an
 * invalid one (fail closed: authentication is required and nobody passes). */
bool cred_auth_enabled(void);
/* The mode behind it (the htdigest file, re-read): OFF, ON or BROKEN. */
rtsp_auth_mode_t cred_auth_mode(void);

/* Per-peer lockout for ONVIF over HTTP (ip = sin_addr.s_addr). After CRED_PEER_FREE_FAILS failures from one address
 * (counted with cred_peer_failed by the caller, which also covers failures that cred_check_* cannot see) the address
 * is locked for 1, 2, 4, 8, 16 s (growing with each further failure, forgotten after 5 quiet minutes). While
 * cred_peer_locked is true the caller must answer as for a failed login WITHOUT evaluating the credentials, so a
 * correct password gets no earlier answer than a wrong one and a reconnect does not shorten the wait. Other
 * addresses are unaffected. 8 addresses are tracked (the least recently failing one is replaced); cred_peer_ok
 * forgets the address after a success. retry_ms may be NULL. */
bool cred_peer_locked(uint32_t ip, unsigned *retry_ms);
void cred_peer_failed(uint32_t ip);
void cred_peer_ok(uint32_t ip);
