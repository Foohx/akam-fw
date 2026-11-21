/* Small deterministic fuzz of the control API parsers (random bytes and mutations of valid inputs), meant to run under
 * ASan/UBSan: key file, flat JSON, canonical query and path, request verifier headers, registry validators, settings file.
 * Checks crash freedom plus a few invariants (a parsed key file re-serialises to something that parses to the same keys; a
 * canonical query is a fixed point; nothing random verifies).
 * usage: rot2k_control_api_fuzz [iterations (default 200000)] [seed (default 1)] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_keys.h"
#include "api_sig.h"
#include "json_min.h"
#include "settings_reg.h"
#include "sha256.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 11);
}
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* random mutation of src into dst (cap bytes), returns the length */
static size_t mutate(const char *src, size_t n, char *dst, size_t cap, const char *alphabet) {
    size_t i, al = strlen(alphabet), len = n < cap ? n : cap;
    memcpy(dst, src, len);
    for (i = rnd() % 4; i > 0; i--) {
        size_t p = len ? rnd() % len : 0;
        switch (rnd() % 5) {
        case 0: if (len) dst[p] = alphabet[rnd() % al]; break;                       /* replace */
        case 1: if (len) dst[p] = (char)rnd(); break;                                /* random byte */
        case 2: if (len) { memmove(dst + p, dst + p + 1, len - p - 1); len--; } break; /* delete */
        case 3: if (len < cap) { memmove(dst + p + 1, dst + p, len - p); dst[p] = alphabet[rnd() % al]; len++; } break;
        default: len = p; break;                                                     /* truncate */
        }
    }
    return len;
}

static size_t randbuf(char *dst, size_t cap, const char *alphabet) {
    size_t n = rnd() % (cap + 1), i, al = strlen(alphabet);
    for (i = 0; i < n; i++) dst[i] = (rnd() % 8 == 0) ? (char)rnd() : alphabet[rnd() % al];
    return n;
}

static int nop_set(const char *k, const char *v, st_apply_t *e) { (void)k; (void)v; (void)e; return 0; }
static const char *const OPTS[] = {"auto", "on", "off", NULL};
static const char *const GL[] = {"led", NULL};
static const char *const GZ[] = {"zone4", NULL};
#define RW (ST_ACC_READ | ST_ACC_WRITE)
static const st_def_t TBL[] = {
    {"a.bool", ST_BOOL, 0, 0, NULL, "0", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
    {"a.int", ST_INT, -5, 100, NULL, "7", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
    {"a.enum", ST_ENUM, 0, 0, OPTS, "auto", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
    {"a.str", ST_STRING, 0, 12, NULL, "x", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
    {"a.led", ST_PATTERN, 0, 0, GL, "off", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
    {"a.zone", ST_PATTERN, 0, 100, GZ, "0:100:0:100", AP_LIVE, 1, RW, ST_EXPOSE_HTTP, 0, 0, 0, 0, NULL, nop_set, NULL},
};

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000, it;
    char a[2048], b[2048], c[4096];
    streg_t R;
    akeys_t ks;
    akey_t k0;
    api_sig_t sg;
    static jflat_t jf;
    long keys_ok = 0, json_ok = 0, q_ok = 0, file_ok = 0;
    char good_keys[512], good_json[] = "{\"a.int\":\"15\",\"a.bool\":true,\"a.enum\":\"on\",\"a.zone\":\"1:2:3:4\"}";
    const char good_file[] = "a.int=9\na.enum=on\na.led=3:100:200:1000\na.zone=1:2:3:4\n";
    uint8_t r36[36];
    int i;

    if (argc > 2) rs ^= (uint64_t)atol(argv[2]) * 0x9e3779b97f4a7c15ull;
    for (i = 0; i < 36; i++) r36[i] = (uint8_t)(i * 5 + 1);
    memset(&ks, 0, sizeof ks);
    CHECK(akey_make(&k0, AKEY_ADMIN, "admin one", 1700000000, r36) == 0 && akeys_add(&ks, &k0) == 0);
    r36[35] = 9;
    CHECK(akey_make(&k0, AKEY_READ, "read", 5, r36) == 0 && akeys_add(&ks, &k0) == 0);
    CHECK(akeys_serialize(&ks, good_keys, sizeof good_keys) > 0);
    CHECK(streg_init(&R, TBL, sizeof TBL / sizeof TBL[0], 1, 0, NULL) == 0);
    apisig_init(&sg, 60);

    for (it = 0; it < iters; it++) {
        size_t n;
        /* ---- key file ---- */
        {
            akeys_t p, q;
            n = (it & 1) ? mutate(good_keys, strlen(good_keys), a, sizeof a, "0123456789abcdef:\nadminreadwrit ") : randbuf(a, 300, "0123456789abcdef:\n");
            if (akeys_parse(&p, a, n, NULL) == 0) {
                int m;
                keys_ok++;
                m = akeys_serialize(&p, b, sizeof b);
                CHECK(m >= 0 && akeys_parse(&q, b, (size_t)m, NULL) == 0 && q.n == p.n && memcmp(p.k, q.k, sizeof p.k[0] * p.n) == 0);
            } else {
                CHECK(p.n == 0);
            }
        }
        /* ---- flat JSON ---- */
        n = (it & 1) ? mutate(good_json, strlen(good_json), a, sizeof a, "{}[]\":,\\u0123456789truefalsn-.e ") : randbuf(a, 200, "{}\":,tfrue0123456789\\ ");
        if (jflat_parse(&jf, a, n) == 0) {
            size_t j;
            json_ok++;
            CHECK(jf.n <= JFLAT_MAX);
            for (j = 0; j < jf.n; j++) CHECK(strlen(jf.key[j]) <= JFLAT_KEY_MAX && strlen(jf.val[j]) <= JFLAT_VAL_MAX);
            /* feed the registry: must never crash, whatever the keys and values are */
            {
                st_result_t res[JFLAT_MAX];
                int st = streg_patch(&R, ST_SRC_HTTP, (int)(rnd() % 4), jf.n, jf.key, jf.val, res);
                CHECK(st == 200 || st == 207 || st == 422);
                if (st == 422 && jf.n) CHECK(streg_json_patch_reply(&R, jf.n, jf.key, jf.val, res, c, sizeof c) != 0);
            }
        }
        /* ---- canonical query and path ---- */
        n = randbuf(a, 120, "ab=&%%0123456789ABCDEFabcdef+~.-_ z");
        a[n] = 0;
        {
            char o1[3 * APISIG_QUERY_MAX + 1], o2[3 * APISIG_QUERY_MAX + 1];
            int r1 = apisig_canon_query(a, o1, sizeof o1);
            if (r1 >= 0) {
                q_ok++;
                CHECK(apisig_canon_query(o1, o2, sizeof o2) == r1 && strcmp(o1, o2) == 0); /* fixed point, or a pure-% edge */
            }
            (void)apisig_path_ok(a);
        }
        /* ---- verifier: random headers can never pass ---- */
        {
            apisig_req_t q;
            apisig_result_t res;
            char key[16], ts[24], nonce[40], chal[40], sig[72], path[64];
            memset(&q, 0, sizeof q);
            memcpy(key, (it & 3) ? ks.k[0].id : "deadbeef", 9);
            if (rnd() % 8 == 0) key[rnd() % 8] = (char)rnd();
            snprintf(ts, sizeof ts, "%u", rnd());
            akam_hex(r36, 16, nonce); nonce[rnd() % 32] = (char)(rnd() % 3 ? (uint32_t)nonce[0] : rnd());
            akam_hex(r36, 16, chal);
            akam_hex(r36, 32, sig); sig[rnd() % 64] = (char)(rnd() % 4 ? (uint32_t)sig[0] : rnd());
            snprintf(path, sizeof path, "/akam/v1/%s", (it & 2) ? "settings" : "info");
            q.method = (it & 4) ? "GET" : "PATCH"; q.path = path; q.query = (it & 8) ? a : NULL;
            q.body = c; q.body_len = rnd() % 64;
            q.h_key = key; q.h_signature = sig;
            if (it % 5 == 0) q.h_challenge = chal; else { q.h_timestamp = ts; q.h_nonce = nonce; }
            if (it % 7 == 0) q.h_signature = NULL;
            if (it % 11 == 0) q.h_key = NULL;
            memset(c, 'x', 64);
            if (it % 13 == 0) apisig_hello(&sg, (uint64_t)it, r36, chal);
            CHECK(apisig_verify(&sg, &ks, (int)(it & 1), (int64_t)rnd(), (uint64_t)it, &q, AKEY_READ, &res) != 0);
        }
        /* ---- registry validators and the settings file ---- */
        n = (it & 1) ? mutate(good_file, sizeof good_file - 1, a, sizeof a, "a.intboolenumledzone=:\n0123456789-onoffauto ") : randbuf(a, 150, "a.intbolenumzl=:\n0123456789-");
        {
            size_t el, ign, fl;
            if (streg_load_text(&R, a, n, &el, &ign, &fl) == 0) file_ok++;
            a[n] = 0;
            {
                size_t d;
                for (d = 0; d < R.n; d++) (void)streg_validate(&R.defs[d], a);
            }
        }
        if (it % 1000 == 0) {
            CHECK(streg_json_schema(&R, c, sizeof c) > 0);
            CHECK(streg_save_text(&R, c, sizeof c) >= 0);
        }
    }
    printf("fuzz: %ld iterations, accepted keyfiles %ld, json %ld, queries %ld, settings files %ld, %d failures\n", iters,
           keys_ok, json_ok, q_ok, file_ok, fails);
    return fails ? 1 : 0;
}
