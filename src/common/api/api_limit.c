/* See api_limit.h. */
#include "api_limit.h"

#include <string.h>

void alim_init(alim_t *l, uint32_t rate_milli, uint32_t burst, uint64_t now_ms)
{
    memset(l, 0, sizeof *l);
    l->rate_milli = rate_milli;
    l->burst = burst;
    l->tokens_milli = (uint64_t)burst * 1000u; /* full at start: a restart must not delay the owner */
    l->tok_ms = now_ms;
    l->fail_win_ms = l->hello_win_ms = now_ms;
}

/* The slot of ip; a new peer takes a free slot, else evicts the least recently used one that is neither locked nor a recent good
 * peer (new addresses cannot push out a locked attacker or the owner), else a good one, else the oldest locked one. */
static alim_peer_t *peer_get(alim_t *l, uint32_t ip, uint64_t now_ms, int create)
{
    alim_peer_t *best = NULL;
    int i;
    for (i = 0; i < APILIM_PEERS; i++)
        if (l->p[i].ip == ip)
            return &l->p[i];
    if (!create)
        return NULL;
    for (i = 0; i < APILIM_PEERS; i++) {
        int lk, blk;
        if (l->p[i].ip == 0) {
            best = &l->p[i];
            break;
        }
        if (!best) {
            best = &l->p[i];
            continue;
        }
        /* keep locked attackers and recently good peers: rank 0 = free to evict, 1 = good, 2 = locked */
        lk = l->p[i].lock_until_ms > now_ms ? 2 : l->p[i].good_until_ms > now_ms ? 1 : 0;
        blk = best->lock_until_ms > now_ms ? 2 : best->good_until_ms > now_ms ? 1 : 0;
        if (lk < blk || (lk == blk && l->p[i].last_use_ms < best->last_use_ms))
            best = &l->p[i];
    }
    memset(best, 0, sizeof *best);
    best->ip = ip;
    best->last_use_ms = now_ms;
    return best;
}

static unsigned secs_up(uint64_t ms)
{
    uint64_t s = (ms + 999u) / 1000u;
    return s < 1 ? 1u : s > 86400u ? 86400u : (unsigned)s;
}

static void roll_fail_window(alim_t *l, uint64_t now_ms)
{
    if (now_ms - l->fail_win_ms >= 60000u) {
        l->fail_win_ms = now_ms;
        l->fail_cnt = 0;
    }
}

unsigned alim_peer_check(alim_t *l, uint32_t ip, uint64_t now_ms)
{
    alim_peer_t *p = peer_get(l, ip, now_ms, 0);
    roll_fail_window(l, now_ms);
    if (p) {
        if (p->fails && now_ms - p->last_fail_ms >= APILIM_QUIET_MS)
            p->fails = 0, p->lock_until_ms = 0;
        if (p->lock_until_ms > now_ms)
            return secs_up(p->lock_until_ms - now_ms);
    }
    if (l->fail_cnt >= APILIM_FAIL_PER_MIN && !(p && p->good_until_ms > now_ms))
        return secs_up(l->fail_win_ms + 60000u - now_ms);
    return 0;
}

void alim_peer_fail(alim_t *l, uint32_t ip, uint64_t now_ms)
{
    alim_peer_t *p = peer_get(l, ip, now_ms, 1);
    roll_fail_window(l, now_ms);
    l->fail_cnt++;
    p->last_use_ms = p->last_fail_ms = now_ms;
    if (p->fails < 255)
        p->fails++;
    if (p->fails >= APILIM_FREE_FAILS) {
        uint64_t lock = APILIM_LOCK_BASE_MS;
        unsigned n = (unsigned)p->fails - APILIM_FREE_FAILS;
        while (n-- && lock < APILIM_LOCK_MAX_MS)
            lock <<= 1;
        if (lock > APILIM_LOCK_MAX_MS)
            lock = APILIM_LOCK_MAX_MS;
        p->lock_until_ms = now_ms + lock;
    }
}

void alim_peer_ok(alim_t *l, uint32_t ip, uint64_t now_ms)
{
    alim_peer_t *p = peer_get(l, ip, now_ms, 1);
    p->fails = 0;
    p->lock_until_ms = 0;
    p->last_use_ms = now_ms;
    p->good_until_ms = now_ms + APILIM_QUIET_MS;
}

static void refill(alim_t *l, uint64_t now_ms)
{
    uint64_t cap = (uint64_t)l->burst * 1000u;
    if (now_ms > l->tok_ms) {
        uint64_t add = (now_ms - l->tok_ms) * l->rate_milli / 1000u; /* ms x (milli-req/s) / 1000 = milli-tokens */
        if (add) {
            l->tokens_milli += add;
            l->tok_ms = now_ms;
        }
    } else {
        l->tok_ms = now_ms; /* the clock never goes backwards (monotonic); be safe anyway */
    }
    if (l->tokens_milli > cap)
        l->tokens_milli = cap;
}

int alim_accept_peek(alim_t *l, uint64_t now_ms)
{
    refill(l, now_ms);
    return l->tokens_milli >= 1000u;
}

int alim_accept_take(alim_t *l, uint64_t now_ms)
{
    refill(l, now_ms);
    if (l->tokens_milli < 1000u)
        return 0;
    l->tokens_milli -= 1000u;
    return 1;
}

unsigned alim_accept_retry_s(const alim_t *l)
{
    uint64_t need = l->tokens_milli >= 1000u ? 0 : 1000u - l->tokens_milli;
    return l->rate_milli ? secs_up(need * 1000u / l->rate_milli) : 60u;
}

unsigned alim_hello(alim_t *l, uint32_t ip, uint64_t now_ms)
{
    alim_peer_t *p = peer_get(l, ip, now_ms, 1);
    uint64_t sec = now_ms / 1000u;
    if (now_ms - l->hello_win_ms >= 60000u) {
        l->hello_win_ms = now_ms;
        l->hello_cnt = 0;
    }
    if (l->hello_cnt >= APILIM_HELLO_PER_MIN && !(p->good_until_ms > now_ms)) /* a recently authenticated peer is not shut out by a hello flood */
        return secs_up(l->hello_win_ms + 60000u - now_ms);
    if (p->hello_sec != sec) {
        p->hello_sec = sec;
        p->hello_n = 0;
    }
    if (p->hello_n >= APILIM_HELLO_PEER_PER_S)
        return 1;
    p->hello_n++;
    p->last_use_ms = now_ms;
    l->hello_cnt++;
    return 0;
}
