/* Rate limits of the local control API (specs/control-api.md 5.5 and 7.1). Pure: the monotonic clock is passed in, there is
 * no I/O, no allocation, no global state; one alim_t belongs to the (single) API thread.
 *   - per peer IPv4 (16 entries, LRU): lockout after APILIM_FREE_FAILS failures, 1 s doubling to 5 min, reset by one
 *     success or 10 quiet minutes. A locked peer costs no crypto (the HTTP layer answers 429 with Retry-After);
 *   - global cap of APILIM_FAIL_PER_MIN failed authentications per minute. Deviation from the spec (which blocks everybody):
 *     a peer that proved itself with a valid signature in the last 10 minutes is exempt from the GLOBAL cap (not from its own
 *     lockout), so an anonymous flood cannot lock the owner out. A fresh peer is refused during the flood (fail closed);
 *   - accepted requests: a token bucket (APISIG_RATE_MILLI_DEFAULT / APISIG_BURST_DEFAULT) so the nonce cache can never
 *     fill: alim_accept_peek before the crypto, alim_accept_take after a signature passed;
 *   - /hello: 10 per minute globally, 2 per second per peer. */
#ifndef AKAM_API_LIMIT_H
#define AKAM_API_LIMIT_H
#include <stdint.h>

#define APILIM_PEERS 16
#define APILIM_FREE_FAILS 5
#define APILIM_LOCK_BASE_MS 1000u
#define APILIM_LOCK_MAX_MS 300000u
#define APILIM_QUIET_MS 600000u
#define APILIM_FAIL_PER_MIN 20
#define APILIM_HELLO_PER_MIN 10
#define APILIM_HELLO_PEER_PER_S 2

typedef struct {
    uint32_t ip; /* 0 = free slot */
    uint8_t fails;
    uint64_t lock_until_ms, last_fail_ms, last_use_ms, good_until_ms;
    uint64_t hello_sec; /* second of the current per-peer /hello count */
    uint8_t hello_n;
} alim_peer_t;

typedef struct {
    alim_peer_t p[APILIM_PEERS];
    uint32_t rate_milli, burst;
    uint64_t tokens_milli, tok_ms; /* bucket level and the time it was last refilled */
    uint64_t fail_win_ms;
    uint32_t fail_cnt;
    uint64_t hello_win_ms;
    uint32_t hello_cnt;
} alim_t;

void alim_init(alim_t *l, uint32_t rate_milli, uint32_t burst, uint64_t now_ms);

/* Before any crypto. 0 = go on; else the Retry-After in seconds (>= 1): the peer is locked, or the global failure cap is hit
 * and the peer is not a recent good one. */
unsigned alim_peer_check(alim_t *l, uint32_t ip, uint64_t now_ms);
/* A request of this peer failed authentication (bad signature, unknown key, skew, replay, bad challenge). */
void alim_peer_fail(alim_t *l, uint32_t ip, uint64_t now_ms);
/* A request of this peer passed the signature: failures and lock cleared, the peer is "good" for 10 minutes. */
void alim_peer_ok(alim_t *l, uint32_t ip, uint64_t now_ms);

/* Token bucket of accepted requests. peek: 1 when a token is available; take: consumes one (1) or refuses (0). */
int alim_accept_peek(alim_t *l, uint64_t now_ms);
int alim_accept_take(alim_t *l, uint64_t now_ms);
/* Seconds until one token is back (>= 1). */
unsigned alim_accept_retry_s(const alim_t *l);

/* /hello: 0 = allowed (and counted), else the Retry-After in seconds. */
unsigned alim_hello(alim_t *l, uint32_t ip, uint64_t now_ms);

#endif
