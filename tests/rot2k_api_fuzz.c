/* Deterministic fuzz of the HTTP request-head parser (http_req.c) under ASan/UBSan, specs/control-api.md section 12: random
 * bytes, mutations of valid requests, oversize and truncated heads, partial reads (the way the server feeds it: growing prefixes of the
 * same input through hreq_head_end), and a check of the invariants of every accepted request (pointers inside the buffer, limits held).
 * usage: rot2k_api_fuzz [iterations] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_req.h"

static uint64_t g_s = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    g_s ^= g_s << 13;
    g_s ^= g_s >> 7;
    g_s ^= g_s << 17;
    return (uint32_t)(g_s >> 16);
}

static const char *const SEEDS[] = {
    "GET /akam/v1/info HTTP/1.1\r\nHost: cam\r\nX-Akam-Key: 3fa91c07\r\nX-Akam-Timestamp: 1700000000\r\n"
    "X-Akam-Nonce: 00112233445566778899aabbccddeeff\r\nX-Akam-Signature: 00\r\n\r\n",
    "PATCH /akam/v1/settings?prefix=led.&x=%2B+y HTTP/1.1\r\nContent-Length: 20\r\nContent-Type: application/json\r\n\r\n",
    "POST /akam/v1/actions/ptz.move HTTP/1.0\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
    "DELETE /akam/v1/keys/3fa91c07 HTTP/1.1\r\nConnection: close\r\n\r\n",
};

static void check_accepted(const char *buf, size_t cap, const hreq_t *r)
{
    const char *lo = buf, *hi = buf + cap;
    const char *p[] = { r->method, r->path, r->query, r->key, r->timestamp, r->nonce, r->challenge, r->signature };
    size_t i;
    for (i = 0; i < sizeof p / sizeof p[0]; i++)
        if (p[i] && !(p[i] >= lo && p[i] < hi)) {
            printf("pointer outside the buffer\n");
            exit(1);
        }
    if (!r->method || !r->path || r->path[0] != '/' || strlen(r->path) > HREQ_PATH_MAX || (r->query && strlen(r->query) > HREQ_QUERY_MAX) ||
        r->content_length < -1 || r->content_length > HREQ_BODY_MAX || r->head_len > HREQ_HEAD_MAX || strlen(r->method) > 7) {
        printf("invariant broken\n");
        exit(1);
    }
    for (i = 0; r->path[i]; i++)
        if ((unsigned char)r->path[i] <= 0x20 || (unsigned char)r->path[i] >= 0x7f || r->path[i] == '#' || r->path[i] == '?') {
            printf("bad path byte\n");
            exit(1);
        }
}

static void run_one(const char *in, size_t n)
{
    static char buf[HREQ_HEAD_MAX + 256];
    hreq_t r;
    long he;
    size_t k;
    int accepted_whole = 0;
    if (n > sizeof buf - 1)
        n = sizeof buf - 1;
    /* how the server reads: growing prefixes; the head end must never be reported twice differently */
    long first = 0;
    for (k = 1; k <= n; k += 1 + (k > 64 ? 17 : 0)) {
        he = hreq_head_end(in, k);
        if (he != 0 && first == 0)
            first = he;
        if (first > 0 && he != first && he != 0) {
            printf("head end moved: %ld then %ld\n", first, he);
            exit(1);
        }
    }
    memcpy(buf, in, n);
    buf[n] = '\0';
    he = hreq_head_end(buf, n);
    if (he > 0) {
        if (hreq_parse(buf, (size_t)he, &r) == 0) {
            check_accepted(buf, (size_t)he, &r);
            accepted_whole = 1;
        }
    }
    /* parse with arbitrary head_len (a caller bug must still be safe) */
    memcpy(buf, in, n);
    if (n >= 4) {
        size_t hl = rnd() % (n + 1);
        if (hreq_parse(buf, hl, &r) == 0)
            check_accepted(buf, hl, &r);
    }
    (void)accepted_whole;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200000, i;
    static char in[HREQ_HEAD_MAX + 256];
    size_t nseed = sizeof SEEDS / sizeof SEEDS[0];
    long accepted = 0;
    for (i = 0; i < iters; i++) {
        size_t n;
        switch (rnd() % 4) {
        case 0: /* random bytes */
            n = rnd() % 300;
            for (size_t j = 0; j < n; j++)
                in[j] = (char)rnd();
            break;
        case 1: /* a seed with a few random byte changes */
        {
            const char *sd = SEEDS[rnd() % nseed];
            n = strlen(sd);
            memcpy(in, sd, n);
            for (int m = 0; m < 1 + (int)(rnd() % 4); m++)
                in[rnd() % n] = (char)rnd();
            break;
        }
        case 2: /* a seed with a chunk cut out or doubled */
        {
            const char *s = SEEDS[rnd() % nseed];
            size_t l = strlen(s), a = rnd() % l, b = a + rnd() % (l - a + 1);
            n = 0;
            memcpy(in, s, b);
            n = b;
            if (rnd() & 1) {
                memcpy(in + n, s + a, l - a);
                n += l - a;
            } else {
                memcpy(in + n, s + b, l - b);
                n += l - b;
            }
            break;
        }
        default: /* oversize: a long run in the request line or in one header */
        {
            const char *s = SEEDS[rnd() % nseed];
            size_t l = strlen(s), cut = rnd() % l, run = rnd() % 5000;
            memcpy(in, s, cut);
            n = cut;
            for (size_t j = 0; j < run && n < sizeof in - 1; j++)
                in[n++] = (char)('a' + rnd() % 26);
            for (size_t j = cut; j < l && n < sizeof in - 1; j++)
                in[n++] = s[j];
            break;
        }
        }
        in[n] = '\0';
        /* strlen-based seeds only: random-byte cases may hold NULs, which is part of the test (n is the real length) */
        run_one(in, n);
    }
    for (size_t s = 0; s < nseed; s++) { /* the seeds themselves are accepted */
        static char b[512];
        hreq_t r;
        long he;
        snprintf(b, sizeof b, "%s", SEEDS[s]);
        he = hreq_head_end(b, strlen(b));
        if (he > 0 && hreq_parse(b, (size_t)he, &r) == 0)
            accepted++;
    }
    if (accepted != (long)nseed) {
        printf("the seed requests must parse (%ld of %zu)\n", accepted, nseed);
        return 1;
    }
    printf("%ld iterations, no crash, invariants held\n", iters);
    return 0;
}
