/* Host unit tests of the control API building blocks (specs/control-api.md phase 1): sha256/hmac, request signing and
 * verification, key store, settings registry, minimal JSON. No camera, no network, no files except the shared vectors.
 * Build and run: see the "Phase 1 building blocks" section of specs/control-api.md.
 * usage: rot2k_control_api_unit <path to tests/fixtures/control_api_vectors.txt> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_keys.h"
#include "api_sig.h"
#include "json_min.h"
#include "settings_reg.h"
#include "sha256.h"

static int fails, checks;
#define CHECK(c)                                                                                  \
    do {                                                                                          \
        checks++;                                                                                 \
        if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); }                \
    } while (0)
#define CHECK_STR(a, b)                                                                           \
    do {                                                                                          \
        checks++;                                                                                 \
        if (strcmp((a), (b))) { fails++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } \
    } while (0)

static void hex_of(const void *d, size_t n, char *out) { akam_hex(d, n, out); }

/* ================= sha256 / hmac ================= */
static void sha_case(const char *msg, size_t n, const char *want) {
    uint8_t d[32];
    char h[65];
    sha256(msg, n, d);
    hex_of(d, 32, h);
    CHECK_STR(h, want);
}

static void hmac_case(const void *key, size_t kl, const void *msg, size_t ml, const char *want, size_t hexlen) {
    uint8_t d[32];
    char h[65];
    hmac_sha256(key, kl, msg, ml, d);
    hex_of(d, 32, h);
    CHECK(strncmp(h, want, hexlen) == 0);
}

static void test_sha(void) {
    uint8_t d[32], buf[400], big[1000];
    char h[65];
    size_t i, n;
    sha256_ctx c;
    /* FIPS 180-4 / NIST examples */
    sha_case("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha_case("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha_case("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    sha_case("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
             112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    /* one million 'a', in odd chunks */
    sha256_init(&c);
    memset(big, 'a', sizeof big);
    for (i = 0; i < 1000; i++) sha256_update(&c, big, 1000);
    sha256_final(&c, d);
    hex_of(d, 32, h);
    CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* padding boundaries, values from openssl */
    {
        static const struct { size_t n; const char *h; } b[] = {
            {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
            {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
            {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
            {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
            {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
            {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
            {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"}};
        for (i = 0; i < sizeof b / sizeof b[0]; i++) sha_case((const char *)big, b[i].n, b[i].h);
    }
    /* incremental in 1..7 byte pieces equals one shot, every length up to 300 */
    for (n = 0; n <= 300; n++) {
        uint8_t a[32], z[32];
        size_t off = 0, step = 1;
        for (i = 0; i < n; i++) buf[i] = (uint8_t)(i * 7 + 3);
        sha256(buf, n, a);
        sha256_init(&c);
        while (off < n) {
            size_t k = step < n - off ? step : n - off;
            sha256_update(&c, buf + off, k);
            off += k;
            step = step % 7 + 1;
        }
        sha256_final(&c, z);
        CHECK(memcmp(a, z, 32) == 0);
    }
    /* RFC 4231 */
    {
        uint8_t k[131], m[50];
        memset(k, 0x0b, 20);
        hmac_case(k, 20, "Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 64);
        hmac_case("Jefe", 4, "what do ya want for nothing?", 28,
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 64);
        memset(k, 0xaa, 20); memset(m, 0xdd, 50);
        hmac_case(k, 20, m, 50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 64);
        for (i = 0; i < 25; i++) k[i] = (uint8_t)(i + 1);
        memset(m, 0xcd, 50);
        hmac_case(k, 25, m, 50, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 64);
        memset(k, 0x0c, 20);
        hmac_case(k, 20, "Test With Truncation", 20, "a3b6167473100ee06e0c796c2955552b", 32);
        memset(k, 0xaa, 131);
        hmac_case(k, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 64);
        hmac_case(k, 131,
                  "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be "
                  "hashed before being used by the HMAC algorithm.",
                  152, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", 64);
    }
    /* helpers */
    {
        uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4}, o[4];
        CHECK(ct_equal(a, b, 4));
        b[3] = 5;
        CHECK(!ct_equal(a, b, 4));
        b[0] = 0; b[3] = 4;
        CHECK(!ct_equal(a, b, 4));
        CHECK(ct_equal(a, b, 0));
        CHECK(akam_unhex("0A0b0c0D", 4, o) == 0 && o[0] == 10 && o[3] == 13);
        CHECK(akam_unhex("0a0b0c", 4, o) != 0);      /* short */
        CHECK(akam_unhex("0a0b0c0d00", 4, o) != 0);  /* long */
        CHECK(akam_unhex("0a0b0cgd", 4, o) != 0);    /* bad digit */
    }
}

/* ================= vectors, builder ================= */
typedef struct { char k[24], v[4200]; } kv_t;

static char *vecfile;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

typedef struct {
    char name[64], secret[80], key_id[16], method[16], path[160], query[600], body_hex[400], body_fill[32], ts[96],
        nonce[96], cq[1700], canon[2400], sig[80], kind[16], value[300];
    int is_case, is_reject;
} vec_t;

static void vec_field(vec_t *v, const char *k, const char *val) {
#define F(name, field) if (!strcmp(k, name)) { snprintf(v->field, sizeof v->field, "%s", val); return; }
    F("secret", secret) F("key_id", key_id) F("method", method) F("path", path) F("query", query)
    F("body_hex", body_hex) F("body_fill", body_fill) F("timestamp", ts) F("nonce", nonce)
    F("canonical_query", cq) F("canonical", canon) F("signature", sig) F("kind", kind) F("value", value)
    if (!strcmp(k, "case")) { snprintf(v->name, sizeof v->name, "%s", val); v->is_case = 1; }
    if (!strcmp(k, "reject")) { snprintf(v->name, sizeof v->name, "%s", val); v->is_reject = 1; }
#undef F
}

/* Calls cb for every block of the vectors file. */
static void each_vector(void (*cb)(const vec_t *)) {
    char *p = vecfile, *line;
    vec_t v;
    int have = 0;
    memset(&v, 0, sizeof v);
    for (line = strtok(p, "\n"); ; line = strtok(NULL, "\n")) {
        /* strtok skips blank lines, so blocks are delimited by their first key instead ("case"/"reject") */
        if (!line || !strncmp(line, "case:", 5) || !strncmp(line, "reject:", 7)) {
            if (have) cb(&v);
            memset(&v, 0, sizeof v);
            have = 0;
            if (!line) break;
        }
        if (line[0] == '#') continue;
        {
            char *c = strchr(line, ':');
            if (!c) continue;
            *c++ = 0;
            if (*c == ' ') c++;
            vec_field(&v, line, c);
            have = 1;
        }
    }
}

static size_t vec_body(const vec_t *v, uint8_t *out, size_t cap) {
    size_t n = 0;
    if (v->body_hex[0]) {
        n = strlen(v->body_hex) / 2;
        if (n <= cap) akam_unhex(v->body_hex, n, out);
    }
    if (v->body_fill[0]) {
        unsigned b = (unsigned)strtoul(v->body_fill, NULL, 16);
        n = (size_t)atol(strchr(v->body_fill, ':') + 1);
        if (n <= cap) memset(out, (int)b, n);
    }
    return n;
}

static int cases_run, rejects_run;

static void unescape_nl(const char *in, char *out) {
    for (; *in; in++) {
        if (in[0] == '\\' && in[1] == 'n') { *out++ = '\n'; in++; }
        else *out++ = *in;
    }
    *out = 0;
}

static void vec_cb(const vec_t *v) {
    static uint8_t body[110000];
    if (v->is_case) {
        char cq[APISIG_CANON_MAX], canon[APISIG_CANON_MAX], want[APISIG_CANON_MAX], sigh[65];
        uint8_t secret[32], sig[32];
        size_t bl = vec_body(v, body, sizeof body);
        int n;
        cases_run++;
        CHECK(akam_unhex(v->secret, 32, secret) == 0);
        n = apisig_canon_query(v->query, cq, sizeof cq);
        CHECK(n >= 0);
        CHECK_STR(cq, v->cq);
        n = apisig_canon(canon, sizeof canon, v->method, v->path, v->query, body, bl, v->ts, v->nonce, v->key_id);
        CHECK(n > 0);
        unescape_nl(v->canon, want);
        CHECK(n > 0 && strcmp(canon, want) == 0);
        if (strcmp(canon, want)) printf("  case %s\n   got  %s\n   want %s\n", v->name, canon, want);
        apisig_sign(secret, canon, (size_t)(n > 0 ? n : 0), sig);
        hex_of(sig, 32, sigh);
        CHECK_STR(sigh, v->sig);
    } else if (v->is_reject) {
        char out[APISIG_CANON_MAX];
        rejects_run++;
        if (!strcmp(v->kind, "path")) {
            CHECK(!apisig_path_ok(v->value));
            CHECK(apisig_canon(out, sizeof out, "GET", v->value, "", "", 0, "1", "2", "3fa91c07") < 0);
        } else {
            CHECK(apisig_canon_query(v->value, out, sizeof out) < 0);
            CHECK(apisig_canon(out, sizeof out, "GET", "/akam/v1/info", v->value, "", 0, "1", "2", "3fa91c07") < 0);
        }
    }
}

static void test_vectors(const char *path) {
    vecfile = slurp(path, NULL);
    CHECK(vecfile != NULL);
    if (!vecfile) return;
    each_vector(vec_cb);
    CHECK(cases_run >= 16);
    CHECK(rejects_run >= 18);
    free(vecfile);
}

static void test_canon_misc(void) {
    char o[APISIG_CANON_MAX];
    uint8_t body[2] = {1, 2};
    CHECK(apisig_path_ok("/akam/v1/settings/video0.fps"));
    CHECK(!apisig_path_ok(""));
    CHECK(!apisig_path_ok("/a/../b"));
    CHECK(!apisig_path_ok("/a/b..c")); /* any ".." is refused, so client and server cannot disagree */
    CHECK(!apisig_path_ok("/a b"));
    CHECK(!apisig_path_ok("/a%2fb"));
    CHECK(apisig_method_ok("PATCH") && !apisig_method_ok("patch") && !apisig_method_ok("PUT") && !apisig_method_ok(NULL));
    CHECK(apisig_canon(o, 10, "GET", "/a", "", "", 0, "1", "2", "3fa91c07") == -2); /* buffer too small */
    CHECK(apisig_canon(o, sizeof o, "GET", "/a", "", "", 0, "1\n2", "2", "3fa91c07") < 0); /* newline in a line */
    CHECK(apisig_canon(o, sizeof o, "GET", "/a", "", "", 0, "1", "2", "3FA91C07") < 0);    /* key id not lower hex */
    CHECK(apisig_canon(o, sizeof o, "GET", "/a", "", NULL, 3, "1", "2", "3fa91c07") < 0);
    CHECK(apisig_canon(o, sizeof o, "GET", "/a", "", NULL, 0, "1", "2", "3fa91c07") > 0);
    CHECK(apisig_canon(o, sizeof o, "POST", "/a", NULL, body, 2, "1", "2", "3fa91c07") > 0);
    {
        char q[APISIG_QUERY_MAX + 8], c[4096];
        memset(q, 'a', sizeof q - 1);
        q[sizeof q - 1] = 0;
        CHECK(apisig_canon_query(q, c, sizeof c) == -2);
        CHECK(apisig_canon_query("a=1", c, 3) == -2); /* output too small */
        CHECK(apisig_canon_query("a=1", c, 4) == 3);
    }
    {
        char q[200], c[1024];
        int i, p = 0;
        for (i = 0; i < APISIG_QUERY_PARAMS + 1; i++) p += snprintf(q + p, sizeof q - (size_t)p, "%sk%d=1", i ? "&" : "", i);
        CHECK(apisig_canon_query(q, c, sizeof c) == -2); /* too many parameters */
    }
}

/* ================= verifier ================= */
static akeys_t KS;
static const uint8_t SEC_ADMIN[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static void setup_keys(void) {
    akey_t k;
    uint8_t rnd[36];
    int i;
    memset(&KS, 0, sizeof KS);
    memset(&k, 0, sizeof k);
    memcpy(k.id, "3fa91c07", 9); k.scope = AKEY_ADMIN; k.created = 1; memcpy(k.secret, SEC_ADMIN, 32);
    memcpy(k.label, "assistant", 10);
    CHECK(akeys_add(&KS, &k) == 0);
    for (i = 0; i < 36; i++) rnd[i] = (uint8_t)(0x40 + i);
    CHECK(akey_make(&k, AKEY_READ, "dashboard", 2, rnd) == 0);
    CHECK(akeys_add(&KS, &k) == 0);
    for (i = 0; i < 36; i++) rnd[i] = (uint8_t)(0x80 + i);
    CHECK(akey_make(&k, AKEY_WRITE, "script", 3, rnd) == 0);
    CHECK(akeys_add(&KS, &k) == 0);
}

typedef struct {
    char key[9], ts[24], nonce[40], chal[40], sig[72];
    const char *method, *path, *query;
    const char *body;
    int no_ts;
} sreq_t;

static void sign_ts(sreq_t *r, int key_idx, int64_t ts, int n) {
    char canon[APISIG_CANON_MAX], l7[40];
    uint8_t nonce[16], sig[32];
    size_t blen = r->body ? strlen(r->body) : 0;
    int cn;
    memset(nonce, 0, 16);
    nonce[0] = (uint8_t)(n & 255); nonce[1] = (uint8_t)(n >> 8); nonce[15] = 0x77;
    akam_hex(nonce, 16, l7);
    snprintf(r->key, sizeof r->key, "%s", KS.k[key_idx].id);
    snprintf(r->ts, sizeof r->ts, "%lld", (long long)ts);
    snprintf(r->nonce, sizeof r->nonce, "%s", l7);
    r->chal[0] = 0;
    cn = apisig_canon(canon, sizeof canon, r->method, r->path, r->query, r->body, blen, r->ts, r->nonce, r->key);
    apisig_sign(KS.k[key_idx].secret, canon, (size_t)cn, sig);
    akam_hex(sig, 32, r->sig);
}

static void sign_chal(sreq_t *r, int key_idx, const char *chal_hex) {
    char canon[APISIG_CANON_MAX], l[64];
    uint8_t sig[32];
    size_t blen = r->body ? strlen(r->body) : 0;
    int cn;
    snprintf(l, sizeof l, "challenge:%s", chal_hex);
    snprintf(r->key, sizeof r->key, "%s", KS.k[key_idx].id);
    snprintf(r->chal, sizeof r->chal, "%s", chal_hex);
    r->ts[0] = 0; r->nonce[0] = 0;
    cn = apisig_canon(canon, sizeof canon, r->method, r->path, r->query, r->body, blen, l, l, r->key);
    apisig_sign(KS.k[key_idx].secret, canon, (size_t)cn, sig);
    akam_hex(sig, 32, r->sig);
}

static int verify(api_sig_t *s, int clock_set, int64_t wall, uint64_t mono, const sreq_t *r, int need, apisig_result_t *res) {
    apisig_req_t q;
    memset(&q, 0, sizeof q);
    q.method = r->method; q.path = r->path; q.query = r->query;
    q.body = r->body; q.body_len = r->body ? strlen(r->body) : 0;
    q.h_key = r->key;
    q.h_timestamp = r->ts[0] ? r->ts : NULL;
    q.h_nonce = r->nonce[0] ? r->nonce : NULL;
    q.h_challenge = r->chal[0] ? r->chal : NULL;
    q.h_signature = r->sig;
    return apisig_verify(s, &KS, clock_set, wall, mono, &q, need, res);
}

static void test_verify(void) {
    api_sig_t s;
    sreq_t r;
    apisig_result_t res;
    const int64_t T = 1700000000;
    int i;
    unsigned long h0;

    setup_keys();
    memset(&r, 0, sizeof r);
    r.method = "GET"; r.path = "/akam/v1/settings"; r.query = "prefix=video0.";

    /* accept, then replay */
    apisig_init(&s, 60);
    sign_ts(&r, 0, T, 1);
    CHECK(verify(&s, 1, T, 1000, &r, AKEY_READ, &res) == 0 && res.key_idx == 0 && res.scope == AKEY_ADMIN);
    CHECK(verify(&s, 1, T, 1001, &r, AKEY_READ, &res) == 401);
    CHECK_STR(res.error, "replay");
    /* same nonce, other key: independent */
    sign_ts(&r, 1, T, 1);
    CHECK(verify(&s, 1, T, 1002, &r, AKEY_READ, &res) == 0);
    /* a wall-clock step cannot revive a nonce (retention runs on the monotonic clock) */
    sign_ts(&r, 0, T, 1);
    CHECK(verify(&s, 1, T - 50, 5000, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "replay"));
    CHECK(verify(&s, 1, T + 50, 6000, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "replay"));
    /* after the retention (2 x 60 + 5 s = 125 s of monotonic time) the slot is free again, the window still holds it */
    CHECK(apisig_retention_ms(&s) == 125000);
    CHECK(verify(&s, 1, T, 1000 + 124999, &r, AKEY_READ, &res) == 401);
    CHECK(verify(&s, 1, T, 1000 + 125000, &r, AKEY_READ, &res) == 0);

    /* window edges: 60 s accepted, 61 s refused; the skew is reported */
    apisig_init(&s, 60);
    sign_ts(&r, 0, T - 60, 10);
    CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 0);
    sign_ts(&r, 0, T + 60, 11);
    CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 0);
    sign_ts(&r, 0, T - 61, 12);
    CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "clock_skew") && res.skew_s == 61);
    sign_ts(&r, 0, T + 61, 13);
    CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "clock_skew") && res.skew_s == -61);
    apisig_init(&s, 5);   /* clamped to 10 */
    CHECK(s.window_s == 10);
    apisig_init(&s, 9999); /* clamped to 300 */
    CHECK(s.window_s == 300);

    /* failures consume nothing: many bad signatures, then a full cache of good ones still fits */
    apisig_init(&s, 60);
    for (i = 0; i < 200; i++) {
        sign_ts(&r, 0, T, 100 + i);
        r.sig[0] = r.sig[0] == '0' ? '1' : '0';
        CHECK(verify(&s, 1, T, 10, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
    }
    for (i = 0; i < APISIG_NONCE_CAP; i++) {
        sign_ts(&r, 0, T, 1000 + i);
        CHECK(verify(&s, 1, T, 10, &r, AKEY_READ, &res) == 0);
    }
    /* full: fail closed with 429, nothing evicted */
    sign_ts(&r, 0, T, 5000);
    CHECK(verify(&s, 1, T, 11, &r, AKEY_READ, &res) == 429 && !strcmp(res.error, "busy"));
    sign_ts(&r, 0, T, 1000);
    CHECK(verify(&s, 1, T, 12, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "replay")); /* still remembered */
    sign_ts(&r, 0, T, 5000);
    CHECK(verify(&s, 1, T, 10 + 125000, &r, AKEY_READ, &res) == 0); /* expired entries freed */
    /* revoking a key drops its nonces */
    apisig_drop_key(&s, "3fa91c07");
    sign_ts(&r, 0, T, 1001);
    CHECK(verify(&s, 1, T, 10 + 125001, &r, AKEY_READ, &res) == 0);

    /* unknown key and wrong signature look identical and each run exactly one HMAC */
    {
        apisig_result_t ru, rw;
        sreq_t u, w;
        apisig_init(&s, 60);
        w = r; sign_ts(&w, 0, T, 7);
        w.sig[5] = w.sig[5] == 'a' ? 'b' : 'a';
        u = r; sign_ts(&u, 0, T, 8);
        memcpy(u.key, "deadbeef", 9); /* not in the table */
        h0 = s.hmac_calls;
        CHECK(verify(&s, 1, T, 0, &w, AKEY_READ, &rw) == 401);
        CHECK(s.hmac_calls == h0 + 1);
        CHECK(verify(&s, 1, T, 0, &u, AKEY_READ, &ru) == 401);
        CHECK(s.hmac_calls == h0 + 2);
        CHECK(rw.http == ru.http && strcmp(rw.error, ru.error) == 0 && !strcmp(ru.error, "bad_signature"));
        CHECK(ru.key_idx == -1 && rw.key_idx == 0);
        /* pre-signature failures answer the same for a known and an unknown id (no key-id oracle) */
        sign_ts(&u, 0, T - 5000, 9);
        memcpy(u.key, "deadbeef", 9);
        CHECK(verify(&s, 1, T, 0, &u, AKEY_READ, &ru) == 401 && !strcmp(ru.error, "clock_skew"));
        sign_ts(&w, 0, T - 5000, 9);
        CHECK(verify(&s, 1, T, 0, &w, AKEY_READ, &rw) == 401 && !strcmp(rw.error, "clock_skew"));
        CHECK(verify(&s, 0, 0, 0, &u, AKEY_READ, &ru) == 401 && !strcmp(ru.error, "clock_unset"));
        CHECK(verify(&s, 0, 0, 0, &w, AKEY_READ, &rw) == 401 && !strcmp(rw.error, "clock_unset"));
        /* a repeated /hello value is refused, never two live challenges */
        {
            api_sig_t s2;
            uint8_t z[16] = {7};
            char c[33];
            apisig_init(&s2, 60);
            CHECK(apisig_hello(&s2, 0, z, c) == 0 && apisig_hello(&s2, 1, z, c) == -1);
        }
    }

    /* scope matrix; a 403 still consumed its nonce */
    apisig_init(&s, 60);
    {
        static const int need[] = {AKEY_READ, AKEY_WRITE, AKEY_ADMIN};
        int k, nn;
        for (k = 0; k < 3; k++)
            for (nn = 0; nn < 3; nn++) {
                sign_ts(&r, k, T, 2000 + k * 10 + nn);
                i = verify(&s, 1, T, 0, &r, need[nn], &res);
                if (KS.k[k].scope >= need[nn]) CHECK(i == 0);
                else CHECK(i == 403 && !strcmp(res.error, "scope") && res.key_idx == k);
            }
        sign_ts(&r, 1, T, 3000);
        CHECK(verify(&s, 1, T, 0, &r, AKEY_ADMIN, &res) == 403);
        CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "replay"));
    }

    /* challenge mode: clock unset, single use, TTL on the monotonic clock, always accepted */
    {
        uint8_t rnd[16];
        char ch[33], ch2[33];
        char json[100];
        apisig_init(&s, 60);
        for (i = 0; i < 16; i++) rnd[i] = (uint8_t)(0xa0 + i);
        CHECK(apisig_hello(&s, 1000, rnd, ch) == 0);
        CHECK_STR(ch, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
        CHECK(apisig_hello_json(json, sizeof json, 0, ch) > 0);
        CHECK_STR(json, "{\"api\":1,\"clock_set\":false,\"challenge\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\"}");
        sign_chal(&r, 0, ch);
        CHECK(verify(&s, 0, 0, 2000, &r, AKEY_READ, &res) == 0);
        CHECK(verify(&s, 0, 0, 2001, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_challenge")); /* single use */
        /* a timestamp request with the clock unset is told so */
        sign_ts(&r, 0, T, 50);
        CHECK(verify(&s, 0, 0, 2002, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "clock_unset"));
        /* a wrong signature does not burn the challenge */
        CHECK(apisig_hello(&s, 3000, rnd, ch) == 0);
        sign_chal(&r, 0, ch);
        r.sig[0] = r.sig[0] == '0' ? '1' : '0';
        CHECK(verify(&s, 1, T, 3001, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
        sign_chal(&r, 0, ch);
        CHECK(verify(&s, 1, T, 3002, &r, AKEY_READ, &res) == 0); /* challenge mode also with a set clock */
        /* TTL 30 s, monotonic */
        CHECK(apisig_hello(&s, 10000, rnd, ch) == 0);
        sign_chal(&r, 0, ch);
        CHECK(verify(&s, 0, 0, 10000 + APISIG_CHAL_TTL_MS, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_challenge"));
        CHECK(apisig_hello(&s, 50000, rnd, ch) == 0);
        sign_chal(&r, 0, ch);
        CHECK(verify(&s, 0, 0, 50000 + APISIG_CHAL_TTL_MS - 1, &r, AKEY_READ, &res) == 0);
        /* unknown challenge */
        sign_chal(&r, 0, "00000000000000000000000000000000");
        CHECK(verify(&s, 0, 0, 50001, &r, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_challenge"));
        /* the table holds 8, then refuses (fail closed); expiry frees it */
        apisig_init(&s, 60);
        for (i = 0; i < APISIG_CHAL_CAP; i++) { rnd[0] = (uint8_t)i; CHECK(apisig_hello(&s, 0, rnd, ch2) == 0); }
        rnd[0] = 99;
        CHECK(apisig_hello(&s, 1, rnd, ch2) == -1);
        CHECK(apisig_hello(&s, APISIG_CHAL_TTL_MS, rnd, ch2) == 0);
    }

    /* shape errors */
    apisig_init(&s, 60);
    sign_ts(&r, 0, T, 60);
    {
        sreq_t b = r;
        b.method = "get";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400);
        b = r; b.path = "/akam/v1//settings";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400);
        b = r; b.query = "a=%zz";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400);
        b = r; memcpy(b.key, "3FA91C07", 9);
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400 && !strcmp(res.error, "bad_header"));
        b = r; b.sig[3] = 'z';
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400);
        b = r; b.nonce[0] = 0;
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "missing_auth"));
        b = r; b.ts[0] = 0; b.nonce[0] = 0;
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "missing_auth"));
        b = r; memmove(b.ts + 1, b.ts, strlen(b.ts) + 1); b.ts[0] = '0';
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400);
        b = r; snprintf(b.chal, sizeof b.chal, "%s", "00000000000000000000000000000000");
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 400); /* both modes at once */
        b = r; b.body = "{}"; /* body altered after signing */
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
        b = r; b.path = "/akam/v1/state";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
        b = r; b.query = "prefix=video1.";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
        b = r; b.method = "POST";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 401 && !strcmp(res.error, "bad_signature"));
        CHECK(verify(&s, 1, T, 0, &r, AKEY_READ, &res) == 0); /* the pristine one still passes: nothing was consumed */
    }
    /* query order is irrelevant to the signature */
    {
        sreq_t a = r, b = r;
        apisig_init(&s, 60);
        a.query = "b=2&a=1"; b.query = "a=1&b=2";
        sign_ts(&a, 0, T, 70);
        b = a; b.query = "a=1&b=2";
        CHECK(verify(&s, 1, T, 0, &b, AKEY_READ, &res) == 0);
    }
    /* cache invariant of spec 5.5 */
    CHECK(apisig_cache_invariant_ok(APISIG_NONCE_CAP, APISIG_RATE_MILLI_DEFAULT, 125, APISIG_BURST_DEFAULT));
    CHECK(APISIG_RATE_MILLI_DEFAULT == 400 && APISIG_BURST_DEFAULT == 14);
    CHECK(!apisig_cache_invariant_ok(64, 400, 125, 16)); /* the old burst 16 needs 66 slots */
    CHECK(!apisig_cache_invariant_ok(64, 400, 125, 15));
    CHECK(apisig_cache_invariant_ok(64, 380, 125, 16));
}

/* ================= key store ================= */
static char wbuf[AKEY_FILE_MAX + 1];
static int wcalls, wfail;
static int fake_writer(void *ctx, const char *t, size_t n) {
    (void)ctx;
    wcalls++;
    if (wfail) return -1;
    memcpy(wbuf, t, n);
    wbuf[n] = 0;
    return 0;
}

#define LINE(id, sc, cr, lbl) id ":" sc ":" cr ":" "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff:" lbl "\n"

static void test_keys(void) {
    akeys_t k, k2;
    char out[AKEY_FILE_MAX + 1], json[600];
    size_t el = 0;
    int n, i;
    const char *good = LINE("3fa91c07", "admin", "1700000000", "assistant") LINE("0a0b0c0d", "read", "0", "my dashboard");
    akey_t a;
    uint8_t rnd[36];

    CHECK(akeys_parse(&k, good, strlen(good), &el) == 0 && k.n == 2 && k.k[0].scope == AKEY_ADMIN && k.k[1].created == 0);
    CHECK_STR(k.k[1].label, "my dashboard");
    CHECK(k.k[0].secret[0] == 0x00 && k.k[0].secret[31] == 0xff);
    n = akeys_serialize(&k, out, sizeof out);
    CHECK(n == (int)strlen(good) && strcmp(out, good) == 0);
    CHECK(akeys_parse(&k2, out, (size_t)n, NULL) == 0 && k2.n == 2);
    CHECK(akeys_parse(&k, "", 0, NULL) == 0 && k.n == 0);
    {
        char nolf[400];
        snprintf(nolf, sizeof nolf, "%s", LINE("3fa91c07", "admin", "5", "x"));
        nolf[strlen(nolf) - 1] = 0; /* final LF missing: accepted */
        CHECK(akeys_parse(&k, nolf, strlen(nolf), NULL) == 0 && k.n == 1);
    }
    {
        static const char *const bad[] = {
            LINE("3FA91C07", "admin", "1", "x"), LINE("3fa91c0", "admin", "1", "x"), LINE("3fa91c07", "root", "1", "x"),
            LINE("3fa91c07", "admin", "01", "x"), LINE("3fa91c07", "admin", "-1", "x"), LINE("3fa91c07", "admin", "", "x"),
            LINE("3fa91c07", "admin", "123456789012", "x"), LINE("3fa91c07", "admin", "1", ""),
            LINE("3fa91c07", "admin", "1", " x"), LINE("3fa91c07", "admin", "1", "x "), LINE("3fa91c07", "admin", "1", "a:b"),
            LINE("3fa91c07", "admin", "1", "abcdefghijklmnopqrstuvwxy"), LINE("3fa91c07", "admin", "1", "caf\xc3\xa9"),
            "3fa91c07:admin:1:00112233:x\n", "3fa91c07:admin:1\n", "\n",
            "3fa91c07:admin:1:00112233445566778899aabbccddeeff00112233445566778899aabbccddeegg:x\n",
            "3fa91c07:admin:1:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00:x\n"};
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            int r = akeys_parse(&k, bad[i], strlen(bad[i]), &el);
            CHECK(r != 0 && k.n == 0 && el == 1);
            if (r == 0) printf("  accepted bad line %d: %s", i, bad[i]);
        }
    }
    {
        const char *dupid = LINE("3fa91c07", "admin", "1", "a") LINE("3fa91c07", "read", "1", "b");
        const char *duplbl = LINE("3fa91c07", "admin", "1", "a") LINE("0a0b0c0d", "read", "1", "a");
        CHECK(akeys_parse(&k, dupid, strlen(dupid), &el) == AKEYS_E_DUP && el == 2);
        CHECK(akeys_parse(&k, duplbl, strlen(duplbl), &el) == AKEYS_E_DUP && el == 2);
        CHECK(akeys_parse(&k, good, AKEY_FILE_MAX + 1, NULL) == AKEYS_E_TOOBIG);
    }
    /* limits: 8 keys, the 9th refused */
    memset(&k, 0, sizeof k);
    for (i = 0; i < 9; i++) {
        char lbl[16];
        memset(rnd, i + 1, 32);
        rnd[32] = (uint8_t)i; rnd[33] = 1; rnd[34] = 2; rnd[35] = 3;
        snprintf(lbl, sizeof lbl, "k%d", i);
        CHECK(akey_make(&a, i == 0 ? AKEY_ADMIN : AKEY_READ, lbl, 10, rnd) == 0);
        CHECK(akeys_add(&k, &a) == (i < 8 ? 0 : AKEYS_E_TOOBIG));
    }
    n = akeys_serialize(&k, out, sizeof out);
    CHECK(n > 0 && n < AKEY_FILE_MAX);
    CHECK(akeys_parse(&k2, out, (size_t)n, NULL) == 0 && k2.n == 8);
    CHECK(akeys_serialize(&k, out, 50) == AKEYS_E_TOOBIG);

    /* last-admin guard, removal by label or id, force */
    CHECK(akeys_parse(&k, good, strlen(good), NULL) == 0);
    CHECK(akeys_remove(&k, "assistant", 0) == AKEYS_E_LAST_ADMIN && k.n == 2);
    CHECK(akeys_remove(&k, "3fa91c07", 0) == AKEYS_E_LAST_ADMIN);
    CHECK(akeys_remove(&k, "nope", 0) == AKEYS_E_NOTFOUND);
    CHECK(akeys_remove(&k, "0a0b0c0d", 0) == 0 && k.n == 1);
    CHECK(akeys_remove(&k, "assistant", 1) == 0 && k.n == 0);
    /* a second admin lets the first go */
    CHECK(akeys_parse(&k, good, strlen(good), NULL) == 0);
    memset(rnd, 9, 36);
    CHECK(akey_make(&a, AKEY_ADMIN, "second", 5, rnd) == 0 && akeys_add(&k, &a) == 0);
    CHECK(akeys_remove(&k, "assistant", 0) == 0 && akeys_count_admin(&k) == 1);
    /* a label may not equal another key's id (removal by name would hit the wrong key) */
    memset(rnd, 4, 36);
    CHECK(akey_make(&a, AKEY_READ, "0a0b0c0d", 6, rnd) == 0 && akeys_add(&k, &a) == AKEYS_E_DUP);

    /* atomic save through the injected writer */
    CHECK(akeys_parse(&k, good, strlen(good), NULL) == 0);
    memset(rnd, 7, 36);
    CHECK(akey_make(&a, AKEY_WRITE, "tool", 9, rnd) == 0);
    wfail = 1; wcalls = 0; wbuf[0] = 0;
    CHECK(akeys_add_save(&k, &a, fake_writer, NULL) == AKEYS_E_WRITE && wcalls == 1 && k.n == 2); /* unchanged */
    wfail = 0;
    CHECK(akeys_add_save(&k, &a, fake_writer, NULL) == 0 && k.n == 3 && strstr(wbuf, ":write:9:") != NULL);
    CHECK(akeys_add_save(&k, &a, fake_writer, NULL) == AKEYS_E_DUP && wcalls == 2); /* writer not called on a refusal */
    wfail = 1;
    CHECK(akeys_remove_save(&k, "tool", 0, fake_writer, NULL) == AKEYS_E_WRITE && k.n == 3);
    wfail = 0;
    CHECK(akeys_remove_save(&k, "tool", 0, fake_writer, NULL) == 0 && k.n == 2 && strstr(wbuf, "tool") == NULL);
    CHECK(akeys_remove_save(&k, "assistant", 0, fake_writer, NULL) == AKEYS_E_LAST_ADMIN);
    CHECK(akeys_parse(&k2, wbuf, strlen(wbuf), NULL) == 0 && k2.n == 2); /* what was written parses back */
    CHECK(akeys_add_save(&k, &a, NULL, NULL) == AKEYS_E_WRITE);

    /* key generation from injected randomness */
    for (i = 0; i < 36; i++) rnd[i] = (uint8_t)i;
    CHECK(akey_make(&a, AKEY_READ, "t", 1, rnd) == 0 && !strcmp(a.id, "20212223") && a.secret[31] == 31);
    CHECK(akey_make(&a, 0, "t", 1, rnd) != 0 && akey_make(&a, AKEY_READ, "bad:label", 1, rnd) != 0);

    /* the list never carries a secret */
    CHECK(akeys_parse(&k, good, strlen(good), NULL) == 0);
    n = akeys_json_list(&k, json, sizeof json);
    CHECK(n > 0);
    CHECK_STR(json, "{\"keys\":[{\"id\":\"3fa91c07\",\"name\":\"assistant\",\"scope\":\"admin\",\"created\":1700000000},"
                    "{\"id\":\"0a0b0c0d\",\"name\":\"my dashboard\",\"scope\":\"read\",\"created\":0}]}");
    CHECK(strstr(json, "00112233") == NULL);
    CHECK(akeys_json_list(&k, json, 20) == AKEYS_E_TOOBIG);
}

/* ================= settings registry ================= */
static const char *const OPT_MODE[] = {"auto", "on", "off", NULL};
static const char *const G_LED[] = {"led", NULL};
static const char *const G_PT[] = {"pan_tilt", NULL};
static const char *const G_ZONE[] = {"zone4", NULL};

#define NKEY 24
static char store[NKEY][STREG_VAL_MAX];
static const char *order[NKEY * 2];
static size_t norder;
static const char *fail_key;
extern const st_def_t TBL[];
extern const size_t TBL_N;

static int slot(const char *key) {
    size_t i;
    for (i = 0; i < TBL_N; i++)
        if (!strcmp(TBL[i].key, key)) return (int)i;
    return -1;
}
static int h_get(const char *key, char *out, size_t cap) {
    int i = slot(key);
    size_t l;
    if (i < 0) return -1;
    l = strlen(store[i]);
    if (l + 1 > cap) return -1;
    memcpy(out, store[i], l + 1);
    return (int)l;
}
static int h_set(const char *key, const char *val, st_apply_t *eff) {
    int i = slot(key);
    if (i < 0 || (fail_key && !strcmp(fail_key, key))) return -1;
    snprintf(store[i], sizeof store[i], "%s", val);
    order[norder++] = key;
    if (!strcmp(key, "video0.kbps")) *eff = AP_RESTART_COMPONENT; /* the hook may report a re-open */
    return 0;
}
/* min_qp <= max_qp, reading the pending value of the other key */
static int h_qp(const struct streg *r, const char *key, const char *val) {
    char o[STREG_VAL_MAX];
    long a = atol(val), b;
    if (!strcmp(key, "video0.min_qp")) {
        if (streg_peek(r, "video0.max_qp", o, sizeof o) < 0) return -1;
        b = atol(o);
        return a <= b ? 0 : -1;
    }
    if (streg_peek(r, "video0.min_qp", o, sizeof o) < 0) return -1;
    b = atol(o);
    return b <= a ? 0 : -1;
}
static int h_track(const struct streg *r, const char *key, const char *val) {
    char o[STREG_VAL_MAX];
    (void)key;
    if (!strcmp(val, "0")) return 0;
    return (streg_peek(r, "detect.enabled", o, sizeof o) > 0 && !strcmp(o, "1")) ? 0 : -1;
}

#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define H ST_EXPOSE_HTTP
#define HM (ST_EXPOSE_HTTP | ST_EXPOSE_MQTT)
#define M2K 2u
#define M1080 1u
#define CAP_PTZ 1u

const st_def_t TBL[] = {
    /* key, type, lo, hi, opts, def, apply, persist, access, expose, secret, min_scope, model, cap, get, set, check */
    {"mqtt.enabled", ST_BOOL, 0, 0, NULL, "0", AP_LIVE, 1, RW, H, 0, 0, 0, 0, h_get, h_set, NULL},
    {"detect.enabled", ST_BOOL, 0, 0, NULL, "1", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"track.enabled", ST_BOOL, 0, 0, NULL, "0", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, h_track},
    {"video0.kbps", ST_INT, 100, 8000, NULL, "2000", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"video0.fps", ST_INT, 1, 15, NULL, "15", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"video0.min_qp", ST_INT, 10, 51, NULL, "20", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, h_qp},
    {"video0.max_qp", ST_INT, 10, 51, NULL, "40", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, h_qp},
    {"night.mode", ST_ENUM, 0, 0, OPT_MODE, "auto", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"mqtt.base_topic", ST_STRING, 1, 8, NULL, "akam", AP_RESTART_COMPONENT, 1, RW, H, 0, 0, 0, 0, h_get, h_set, NULL},
    {"led.idle", ST_PATTERN, 0, 0, G_LED, "off", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"ptz.park", ST_PATTERN, -180, 180, G_PT, "0:0", AP_LIVE, 1, RW, HM, 0, 0, M2K, CAP_PTZ, h_get, h_set, NULL},
    {"zones.a", ST_PATTERN, 0, 100, G_ZONE, "0:100:0:100", AP_LIVE, 1, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
    {"only1080.flag", ST_BOOL, 0, 0, NULL, "0", AP_LIVE, 1, RW, HM, 0, 0, M1080, 0, h_get, h_set, NULL},
    {"image.brightness", ST_INT, 0, 100, NULL, "50", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL}, /* RAM only */
    {"api.port", ST_INT, 1024, 65535, NULL, "8092", AP_RESTART_COMPONENT, 1, RW, H, 0, AKEY_ADMIN, 0, 0, h_get, h_set, NULL},
    {"sys.uptime", ST_INT, 0, 2000000000, NULL, "0", AP_LIVE, 0, ST_ACC_READ, H, 0, 0, 0, 0, h_get, NULL, NULL},
    {"sys.secretstate", ST_INT, 0, 9, NULL, "0", AP_LIVE, 0, ST_ACC_READ, ST_EXPOSE_MQTT, 0, 0, 0, 0, h_get, NULL, NULL},
    {"mqtt.pass", ST_STRING, 0, 64, NULL, NULL, AP_RESTART_COMPONENT, 0, ST_ACC_WRITE, ST_EXPOSE_MQTT, 1, 0, 0, 0, NULL, h_set, NULL},
    {"fail.key", ST_INT, 0, 9, NULL, "0", AP_LIVE, 1, RW, H, 0, 0, 0, 0, h_get, h_set, NULL},
};
const size_t TBL_N = sizeof TBL / sizeof TBL[0];

static void reset_store(void) {
    size_t i;
    for (i = 0; i < TBL_N; i++) snprintf(store[i], sizeof store[i], "%s", TBL[i].def ? TBL[i].def : "");
    norder = 0;
    fail_key = NULL;
}

static void parse_case(const st_def_t *d, const char *v, int want) {
    int r = streg_validate(d, v);
    if (r != want) printf("  validate %s \"%s\": got %d want %d\n", d->key, v, r, want);
    CHECK(r == want);
}

static void test_parsers(const streg_t *R) {
    const st_def_t *i = streg_find(R, "video0.fps"), *b = streg_find(R, "mqtt.enabled"), *e = streg_find(R, "night.mode"),
                   *s = streg_find(R, "mqtt.base_topic"), *l = streg_find(R, "led.idle"), *p = streg_find(R, "ptz.park"),
                   *z = streg_find(R, "zones.a"), *big = streg_find(R, "sys.uptime");
    CHECK(i && b && e && s && l && p && z && big);
    /* int: min, max, min-1, max+1, overflow, trailing, empty, sign, spaces, control, leading zero */
    parse_case(i, "1", 0); parse_case(i, "15", 0); parse_case(i, "0", ST_E_RANGE); parse_case(i, "16", ST_E_RANGE);
    parse_case(i, "-1", ST_E_RANGE); parse_case(i, "99999999999999999999", ST_E_RANGE);
    parse_case(i, "9223372036854775808", ST_E_RANGE); parse_case(i, "123456789012345678", ST_E_RANGE);
    parse_case(i, "", ST_E_FORMAT); parse_case(i, "+5", ST_E_FORMAT); parse_case(i, "5x", ST_E_FORMAT);
    parse_case(i, "5 ", ST_E_FORMAT); parse_case(i, " 5", ST_E_FORMAT); parse_case(i, "5\n", ST_E_FORMAT);
    parse_case(i, "5\t", ST_E_FORMAT); parse_case(i, "05", ST_E_FORMAT); parse_case(i, "-", ST_E_FORMAT);
    parse_case(i, "1.5", ST_E_FORMAT); parse_case(i, "0x5", ST_E_FORMAT); parse_case(i, "1e1", ST_E_FORMAT);
    parse_case(i, "-0", ST_E_FORMAT); parse_case(i, "\x01", ST_E_FORMAT);
    parse_case(big, "2000000000", 0); parse_case(big, "2000000001", ST_E_RANGE);
    /* bool */
    parse_case(b, "0", 0); parse_case(b, "1", 0); parse_case(b, "2", ST_E_FORMAT); parse_case(b, "true", ST_E_FORMAT);
    parse_case(b, "", ST_E_FORMAT); parse_case(b, "01", ST_E_FORMAT); parse_case(b, "1 ", ST_E_FORMAT); parse_case(b, "-1", ST_E_FORMAT);
    /* enum */
    parse_case(e, "auto", 0); parse_case(e, "off", 0); parse_case(e, "Auto", ST_E_RANGE); parse_case(e, "aut", ST_E_RANGE);
    parse_case(e, "auto ", ST_E_RANGE); parse_case(e, "", ST_E_RANGE);
    /* string: length 1..8, printable ASCII */
    parse_case(s, "a", 0); parse_case(s, "12345678", 0); parse_case(s, "123456789", ST_E_RANGE); parse_case(s, "", ST_E_RANGE);
    parse_case(s, "a\nb", ST_E_FORMAT); parse_case(s, "a\x7f", ST_E_FORMAT); parse_case(s, "caf\xc3\xa9", ST_E_FORMAT);
    parse_case(s, "a b\"\\", 0); /* quotes are legal text; JSON escapes them */
    /* patterns */
    parse_case(l, "on", 0); parse_case(l, "off", 0); parse_case(l, "3:100:200:1000", 0); parse_case(l, "255:10:10:0", 0);
    parse_case(l, "0:100:200:1000", ST_E_RANGE); parse_case(l, "3:9:200:1000", ST_E_RANGE); parse_case(l, "3:100:200:60001", ST_E_RANGE);
    parse_case(l, "3:100:200", ST_E_FORMAT); parse_case(l, "3:100:200:1000:1", ST_E_FORMAT); parse_case(l, "On", ST_E_FORMAT);
    parse_case(l, "3:100::1000", ST_E_FORMAT); parse_case(l, "3:100:200:1000 ", ST_E_FORMAT); parse_case(l, "", ST_E_FORMAT);
    parse_case(p, "0:0", 0); parse_case(p, "-180:180", 0); parse_case(p, "181:0", ST_E_RANGE); parse_case(p, "0:-181", ST_E_RANGE);
    parse_case(p, "0", ST_E_FORMAT); parse_case(p, "0:0:0", ST_E_FORMAT); parse_case(p, "a:b", ST_E_FORMAT);
    parse_case(z, "10:20:30:40", 0); parse_case(z, "20:10:30:40", ST_E_RANGE); parse_case(z, "10:20:40:30", ST_E_RANGE);
    parse_case(z, "10:20:30:101", ST_E_RANGE); parse_case(z, "10:20:30", ST_E_FORMAT);
}

static void test_table_rules(void) {
    streg_t r;
    size_t bad = 99;
    st_def_t t[3];
    static const char *const empty[] = {NULL};
    static const char *const sp[] = {"a b", NULL};
    memset(t, 0, sizeof t);
    t[0] = TBL[0];
    CHECK(streg_init(&r, t, 1, M2K, 0, &bad) == 0);
    t[1] = TBL[0];                                   /* duplicate key */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE && bad == 1);
    t[1] = TBL[3]; t[1].key = "Bad.Key";            /* upper case */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE && bad == 1);
    t[1] = TBL[3]; t[1].key = "k-1";
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[3]; t[1].key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* > 48 */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[3]; t[1].def = "9"; /* default out of bounds */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[3]; t[1].lo = 9; t[1].hi = 1;
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[3]; t[1].set = NULL; /* writable without a setter */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[7]; t[1].opts = empty;
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[7]; t[1].opts = sp; t[1].def = "a b";
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[9]; t[1].opts = NULL;
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[17]; t[1].expose |= ST_EXPOSE_HTTP; /* a secret on HTTP */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[17]; t[1].persist = 1;               /* a persisted secret */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[17]; t[1].get = h_get;               /* a readable secret */
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    t[1] = TBL[17]; t[1].access = RW;
    CHECK(streg_init(&r, t, 2, M2K, 0, &bad) == ST_E_TABLE);
    CHECK(streg_init(&r, t, 0, M2K, 0, &bad) == ST_E_TABLE);
}

#define LT(r, s, ...) streg_load_text(r, s, strlen(s), __VA_ARGS__)

static void test_registry(void) {
    streg_t R;
    char out[8192], v[STREG_VAL_MAX];
    size_t bad = 0, el, ign, failed;
    int n;
    st_result_t res[8];
    const char *k[8], *vv[8];

    CHECK(streg_init(&R, TBL, TBL_N, M2K, CAP_PTZ, &bad) == 0);
    reset_store();
    test_parsers(&R);
    test_table_rules();

    /* availability: model mask and capability */
    CHECK(streg_find(&R, "ptz.park") != NULL);
    CHECK(streg_find(&R, "only1080.flag") == NULL);
    CHECK(streg_get(&R, ST_SRC_HTTP, "only1080.flag", v, sizeof v) == ST_E_UNKNOWN);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "only1080.flag", "1", res) == 422 && res[0].err == ST_E_UNKNOWN);
    {
        streg_t R2, R3;
        CHECK(streg_init(&R2, TBL, TBL_N, M2K, 0, NULL) == 0);        /* 2K without the ptz capability */
        CHECK(streg_find(&R2, "ptz.park") == NULL);
        CHECK(streg_init(&R3, TBL, TBL_N, M1080, CAP_PTZ, NULL) == 0); /* the other model */
        CHECK(streg_find(&R3, "ptz.park") == NULL && streg_find(&R3, "only1080.flag") != NULL);
        n = streg_json_schema(&R2, out, sizeof out);
        CHECK(n > 0 && strstr(out, "ptz.park") == NULL && strstr(out, "video0.fps") != NULL);
    }
    CHECK(streg_find(&R, "nope") == NULL && streg_find(&R, NULL) == NULL);

    /* get */
    CHECK(streg_get(&R, ST_SRC_HTTP, "video0.fps", v, sizeof v) == 2 && !strcmp(v, "15"));
    CHECK(streg_get(&R, ST_SRC_HTTP, "video0.fps", v, 2) == ST_E_GETTER);
    CHECK(streg_get(&R, ST_SRC_HTTP, "sys.secretstate", v, sizeof v) == ST_E_UNKNOWN); /* not exposed over HTTP */
    CHECK(streg_get(&R, ST_SRC_MQTT, "sys.secretstate", v, sizeof v) == 1);
    CHECK(streg_get(&R, ST_SRC_MQTT, "mqtt.enabled", v, sizeof v) == ST_E_UNKNOWN);    /* mqtt.* never exposed over MQTT */
    CHECK(streg_get(&R, ST_SRC_LOCAL, "mqtt.enabled", v, sizeof v) == 1);

    /* secrets: write-only, refused on HTTP, never read, never listed, never saved */
    store[slot("mqtt.pass")][0] = 0;
    CHECK(streg_get(&R, ST_SRC_HTTP, "mqtt.pass", v, sizeof v) == ST_E_UNKNOWN);
    CHECK(streg_get(&R, ST_SRC_MQTT, "mqtt.pass", v, sizeof v) == ST_E_SECRET);
    CHECK(streg_get(&R, ST_SRC_LOCAL, "mqtt.pass", v, sizeof v) == ST_E_SECRET);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "mqtt.pass", "hunter2", res) == 422 && res[0].err == ST_E_UNKNOWN);
    {
        st_def_t t[2];
        streg_t R4;
        t[0] = TBL[0]; t[1] = TBL[17];
        t[1].expose = ST_EXPOSE_MQTT;
        CHECK(streg_init(&R4, t, 2, M2K, 0, NULL) == 0);
        CHECK(streg_check_one(&R4, ST_SRC_HTTP, AKEY_ADMIN, "mqtt.pass", "x") == ST_E_UNKNOWN); /* hidden from HTTP */
        CHECK(streg_check_one(&R4, ST_SRC_MQTT, AKEY_ADMIN, "mqtt.pass", "x") == 0);           /* settable off HTTP */
        CHECK(streg_check_one(&R4, ST_SRC_LOCAL, AKEY_WRITE, "mqtt.pass", "x") == 0);
    }
    CHECK(streg_set(&R, ST_SRC_LOCAL, AKEY_ADMIN, "mqtt.pass", "hunter2", res) == 200);
    CHECK(streg_set(&R, ST_SRC_MQTT, AKEY_ADMIN, "mqtt.pass", "hunter2", res) == 200);
    n = streg_json_schema(&R, out, sizeof out);
    CHECK(n > 0 && !strstr(out, "mqtt.pass") && !strstr(out, "hunter2"));
    n = streg_json_values(&R, "", 0, out, sizeof out);
    CHECK(n > 0 && !strstr(out, "mqtt.pass") && !strstr(out, "hunter2"));
    n = streg_save_text(&R, out, sizeof out);
    CHECK(n >= 0 && !strstr(out, "mqtt.pass") && !strstr(out, "hunter2"));
    {   /* a secret in a PATCH reply is only ever named, never echoed */
        const char *kk[1] = {"mqtt.pass"}, *vq[1] = {"hunter2"};
        st_result_t rr[1];
        CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_ADMIN, 1, kk, vq, rr) == 422);
        n = streg_json_patch_reply(&R, 1, kk, vq, rr, out, sizeof out);
        CHECK(n > 0 && !strstr(out, "hunter2"));
    }

    /* scope and access */
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_READ, "video0.fps", "10", res) == 422 && res[0].err == ST_E_SCOPE);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_WRITE, "video0.fps", "10", res) == 200);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_WRITE, "api.port", "9000", res) == 422 && res[0].err == ST_E_SCOPE);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "api.port", "9000", res) == 200 && res[0].apply == AP_RESTART_COMPONENT);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "sys.uptime", "5", res) == 422 && res[0].err == ST_E_READONLY);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "video0.fps", "16", res) == 422 && res[0].err == ST_E_RANGE);
    CHECK(streg_set(&R, ST_SRC_HTTP, AKEY_ADMIN, "video0.fps", "1x", res) == 422 && res[0].err == ST_E_FORMAT);
    CHECK(streg_set(&R, ST_SRC_MQTT, AKEY_ADMIN, "mqtt.enabled", "1", res) == 422); /* not exposed over MQTT */

    /* all-or-nothing PATCH, registry-order apply, apply modes */
    reset_store();
    k[0] = "video0.fps"; vv[0] = "7";
    k[1] = "detect.enabled"; vv[1] = "0";
    k[2] = "video0.kbps"; vv[2] = "3000";
    k[3] = "mqtt.base_topic"; vv[3] = "cam";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 4, k, vv, res) == 200);
    CHECK(res[0].apply == AP_LIVE && res[2].apply == AP_RESTART_COMPONENT && res[3].apply == AP_RESTART_COMPONENT);
    CHECK(norder == 4 && !strcmp(order[0], "detect.enabled") && !strcmp(order[1], "video0.kbps") &&
          !strcmp(order[2], "video0.fps") && !strcmp(order[3], "mqtt.base_topic")); /* table order, not request order */
    n = streg_json_patch_reply(&R, 4, k, vv, res, out, sizeof out);
    CHECK_STR(out, "{\"results\":{\"video0.fps\":{\"ok\":true,\"apply\":\"applied\",\"value\":\"7\"},"
                   "\"detect.enabled\":{\"ok\":true,\"apply\":\"applied\",\"value\":\"0\"},"
                   "\"video0.kbps\":{\"ok\":true,\"apply\":\"restart_component\",\"value\":\"3000\"},"
                   "\"mqtt.base_topic\":{\"ok\":true,\"apply\":\"restart_component\",\"value\":\"cam\"}}}");
    CHECK(n > 0);
    /* one invalid key: nothing applied */
    reset_store();
    k[0] = "video0.fps"; vv[0] = "7";
    k[1] = "video0.kbps"; vv[1] = "99999";
    k[2] = "nope"; vv[2] = "1";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 3, k, vv, res) == 422);
    CHECK(norder == 0 && !strcmp(store[slot("video0.fps")], "15"));
    CHECK(!res[0].ok && res[0].err == ST_E_NOTAPPLIED && res[1].err == ST_E_RANGE && res[2].err == ST_E_UNKNOWN);
    n = streg_json_patch_reply(&R, 3, k, vv, res, out, sizeof out);
    CHECK(n > 0 && strstr(out, "\"detail\":\"[100,8000]\"") && strstr(out, "\"error\":\"not_applied\"") &&
          strstr(out, "\"error\":\"unknown_key\""));
    /* duplicate key in one patch */
    k[0] = "video0.fps"; vv[0] = "7"; k[1] = "video0.fps"; vv[1] = "8";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 2, k, vv, res) == 422 && res[1].err == ST_E_DUP && norder == 0);
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 0, k, vv, res) == 422);
    /* cross-field: min_qp <= max_qp using the pending values */
    k[0] = "video0.min_qp"; vv[0] = "45";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 1, k, vv, res) == 422 && res[0].err == ST_E_CHECK); /* max is 40 */
    k[1] = "video0.max_qp"; vv[1] = "50";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 2, k, vv, res) == 200);
    k[0] = "video0.max_qp"; vv[0] = "30"; k[1] = "video0.min_qp"; vv[1] = "31";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 2, k, vv, res) == 422 && res[0].err == ST_E_CHECK && res[1].err == ST_E_CHECK);
    CHECK(R.pend_n == 0 && R.pend_k == NULL);
    /* track needs detect */
    reset_store();
    k[0] = "detect.enabled"; vv[0] = "0";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 1, k, vv, res) == 200);
    k[0] = "track.enabled"; vv[0] = "1";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 1, k, vv, res) == 422 && res[0].err == ST_E_CHECK);
    k[1] = "detect.enabled"; vv[1] = "1";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 2, k, vv, res) == 200);
    /* a setter failure: earlier ones stay applied, per-key results, 207; all failing is 422 */
    reset_store();
    fail_key = "video0.fps";
    k[0] = "detect.enabled"; vv[0] = "0"; k[1] = "video0.fps"; vv[1] = "5";
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 2, k, vv, res) == 207);
    CHECK(res[0].ok && !res[1].ok && res[1].err == ST_E_SETTER && !strcmp(store[slot("video0.fps")], "15") &&
          !strcmp(store[slot("detect.enabled")], "0"));
    CHECK(streg_patch(&R, ST_SRC_HTTP, AKEY_WRITE, 1, k + 1, vv + 1, res) == 422 && res[0].err == ST_E_SETTER);
    fail_key = NULL;

    /* persistence text: only persistent, non-default, available, non-secret keys, registry order */
    reset_store();
    strcpy(store[slot("video0.fps")], "7");
    strcpy(store[slot("night.mode")], "off");
    strcpy(store[slot("image.brightness")], "80");   /* RAM only */
    strcpy(store[slot("ptz.park")], "10:20");
    n = streg_save_text(&R, out, sizeof out);
    CHECK_STR(out, "video0.fps=7\nnight.mode=off\nptz.park=10:20\n");
    CHECK(n == (int)strlen(out));
    {
        streg_t R2;   /* ptz absent on this capability set: not saved */
        CHECK(streg_init(&R2, TBL, TBL_N, M2K, 0, NULL) == 0);
        streg_save_text(&R2, out, sizeof out);
        CHECK_STR(out, "video0.fps=7\nnight.mode=off\n");
    }
    CHECK(streg_save_text(&R, out, 10) == ST_E_TOOBIG);
    /* load: round trip, then validation as a whole */
    {
        char saved[512];
        streg_save_text(&R, saved, sizeof saved);
        reset_store();
        norder = 0;
        CHECK(streg_load_text(&R, saved, strlen(saved), &el, &ign, &failed) == 0 && ign == 0 && failed == 0);
        CHECK(norder == 3 && !strcmp(store[slot("video0.fps")], "7") && !strcmp(store[slot("ptz.park")], "10:20"));
        reset_store();
        CHECK(LT(&R, "video0.fps=7\nvideo0.fps=8\n", &el, &ign, &failed) == ST_E_FORMAT && el == 2 && norder == 0);
        CHECK(LT(&R, "video0.fps=7\nvideo0.kbps=1\n", &el, &ign, &failed) == ST_E_FORMAT && el == 2 && norder == 0);
        CHECK(LT(&R, "video0.fps=7\ngarbage\n", &el, &ign, &failed) == ST_E_FORMAT && el == 2 && norder == 0);
        CHECK(LT(&R, "video0.fps=7\n\n", &el, &ign, &failed) == ST_E_FORMAT && norder == 0); /* blank line: strict */
        CHECK(LT(&R, "video0.fps=7\r\n", &el, &ign, &failed) == ST_E_FORMAT && norder == 0);
        CHECK(LT(&R, "video0.min_qp=45\n", &el, &ign, &failed) == ST_E_CHECK && norder == 0);
        CHECK(LT(&R, "video0.min_qp=45\nvideo0.max_qp=50\n", &el, &ign, &failed) == 0 && norder == 2);
        reset_store();
        CHECK(LT(&R, "only1080.flag=1\nimage.brightness=9\nzzz.unknown=3\nmqtt.pass=x\nvideo0.fps=3", &el, &ign, &failed) == 0);
        CHECK(ign == 4 && norder == 1 && !strcmp(store[slot("video0.fps")], "3")); /* no final LF is fine; unknown/RAM/secret ignored */
        CHECK(streg_load_text(&R, out, STREG_FILE_MAX + 1, &el, &ign, &failed) == ST_E_TOOBIG);
        CHECK(LT(&R, "", &el, &ign, &failed) == 0);
        fail_key = "video0.fps"; norder = 0;
        CHECK(LT(&R, "video0.fps=3\nnight.mode=on\n", &el, &ign, &failed) == 0 && failed == 1 && norder == 1);
        fail_key = NULL;
    }

    /* JSON: values, one setting, schema */
    reset_store();
    n = streg_json_values(&R, "video0.", 0, out, sizeof out);
    CHECK_STR(out, "{\"video0.kbps\":\"2000\",\"video0.fps\":\"15\",\"video0.min_qp\":\"20\",\"video0.max_qp\":\"40\"}");
    n = streg_json_values(&R, "", 1, out, sizeof out);
    CHECK_STR(out, "{\"sys.uptime\":\"0\"}");  /* state keys: read-only, HTTP-exposed */
    n = streg_json_values(&R, "", 0, out, sizeof out);
    CHECK(n > 0 && strstr(out, "mqtt.enabled") && !strstr(out, "sys.uptime") && strstr(out, "ptz.park"));
    CHECK(streg_json_values(&R, "", 0, out, 20) == ST_E_TOOBIG);
    n = streg_json_setting(&R, "video0.fps", out, sizeof out);
    CHECK_STR(out, "{\"key\":\"video0.fps\",\"type\":\"int\",\"lo\":1,\"hi\":15,\"default\":\"15\",\"apply\":\"live\","
                   "\"persist\":true,\"access\":\"rw\",\"scope\":\"write\",\"value\":\"15\"}");
    n = streg_json_setting(&R, "night.mode", out, sizeof out);
    CHECK(strstr(out, "\"opts\":[\"auto\",\"on\",\"off\"]") != NULL);
    n = streg_json_setting(&R, "api.port", out, sizeof out);
    CHECK(strstr(out, "\"scope\":\"admin\"") && strstr(out, "\"apply\":\"restart_component\""));
    n = streg_json_setting(&R, "sys.uptime", out, sizeof out);
    CHECK(strstr(out, "\"access\":\"ro\"") && strstr(out, "\"scope\":\"read\"") && strstr(out, "\"persist\":false"));
    n = streg_json_setting(&R, "led.idle", out, sizeof out);
    CHECK(strstr(out, "\"grammar\":\"led\"") && !strstr(out, "\"lo\""));
    n = streg_json_setting(&R, "ptz.park", out, sizeof out);
    CHECK(strstr(out, "\"grammar\":\"pan_tilt\"") && strstr(out, "\"lo\":-180"));
    CHECK(streg_json_setting(&R, "mqtt.pass", out, sizeof out) == ST_E_UNKNOWN && streg_json_setting(&R, "sys.secretstate", out, sizeof out) == ST_E_UNKNOWN);
    CHECK(streg_json_setting(&R, "nope", out, sizeof out) == ST_E_UNKNOWN);
    n = streg_json_schema(&R, out, sizeof out);
    CHECK(n > 0 && !strncmp(out, "{\"api\":1,\"settings\":[{\"key\":\"mqtt.enabled\"", 40) && out[n - 1] == '}');
    CHECK(!strstr(out, "sys.secretstate") && !strstr(out, "mqtt.pass") && strstr(out, "sys.uptime"));
    CHECK(streg_json_schema(&R, out, 100) == ST_E_TOOBIG);
    n = streg_json_schema(&R, out, sizeof out);
    {   /* the schema is itself well formed: every '{' and '[' closes, quotes balance */
        int depth = 0, inq = 0, i;
        for (i = 0; out[i]; i++) {
            if (out[i] == '"') inq = !inq;
            else if (!inq && (out[i] == '{' || out[i] == '[')) depth++;
            else if (!inq && (out[i] == '}' || out[i] == ']')) depth--;
        }
        CHECK(depth == 0 && !inq);
    }
}

/* ================= json_min ================= */
static void test_json(void) {
    jflat_t j;
    char b[64];
    jw_t w;
    size_t i;
    static const struct { const char *in; int rc; } bad[] = {
        {"", JF_E_SYNTAX}, {"{", JF_E_SYNTAX}, {"[]", JF_E_SYNTAX}, {"{}x", JF_E_SYNTAX}, {"{\"a\":}", JF_E_SYNTAX},
        {"{\"a\":1,}", JF_E_SYNTAX}, {"{\"a\" 1}", JF_E_SYNTAX}, {"{a:1}", JF_E_SYNTAX}, {"{\"a\":01}", JF_E_SYNTAX},
        {"{\"a\":-}", JF_E_SYNTAX}, {"{\"a\":1.5}", JF_E_TYPE}, {"{\"a\":1e3}", JF_E_TYPE}, {"{\"a\":null}", JF_E_TYPE},
        {"{\"a\":[1]}", JF_E_TYPE}, {"{\"a\":{}}", JF_E_TYPE}, {"{\"a\":1,\"a\":2}", JF_E_DUP},
        {"{\"a\":\"x\\q\"}", JF_E_SYNTAX}, {"{\"a\":\"\\u00e9\"}", JF_E_STRING}, {"{\"a\":\"\\u0000\"}", JF_E_STRING},
        {"{\"a\":\"\xc3\xa9\"}", JF_E_STRING}, {"{\"a\":\"x\ny\"}", JF_E_STRING}, {"{\"a\":\"abc", JF_E_SYNTAX},
        {"{\"a\":1234567890123456789}", JF_E_LIMIT}, {"{\"a\":tru}", JF_E_SYNTAX}, {"{\"a\":True}", JF_E_SYNTAX}};
    {
        const char *s = " { \"video0.fps\" : \"15\" , \"x\":true,\"y\":false,\"z\":-7 ,\"w\":0,\"n\":-0 } ";
        CHECK(jflat_parse(&j, s, strlen(s)) == 0 && j.n == 6);
        CHECK_STR(j.key[0], "video0.fps"); CHECK_STR(j.val[0], "15");
        CHECK_STR(j.val[1], "1"); CHECK_STR(j.val[2], "0"); CHECK_STR(j.val[3], "-7"); CHECK_STR(j.val[4], "0"); CHECK_STR(j.val[5], "0");
    }
    CHECK(jflat_parse(&j, "{}", 2) == 0 && j.n == 0);
    {
        const char *s = "{\"a\":\"q\\\"\\\\\\/\\n\\u0041\"}";
        CHECK(jflat_parse(&j, s, strlen(s)) == 0 && !strcmp(j.val[0], "q\"\\/\nA"));
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = jflat_parse(&j, bad[i].in, strlen(bad[i].in));
        if (r != bad[i].rc) printf("  json %s: got %d want %d\n", bad[i].in, r, bad[i].rc);
        CHECK(r == bad[i].rc);
    }
    {   /* limits: 33 pairs, long key, long value, pool */
        char s[1400];
        int k, p = 0;
        p += snprintf(s, sizeof s, "{");
        for (k = 0; k < 33; k++) p += snprintf(s + p, sizeof s - (size_t)p, "%s\"k%d\":1", k ? "," : "", k);
        snprintf(s + p, sizeof s - (size_t)p, "}");
        CHECK(jflat_parse(&j, s, strlen(s)) == JF_E_LIMIT);
        memset(s, 'a', 49); s[49] = 0;
        {
            char d[1500];
            snprintf(d, sizeof d, "{\"%s\":1}", s);
            CHECK(jflat_parse(&j, d, strlen(d)) == JF_E_LIMIT);
            s[48] = 0;
            snprintf(d, sizeof d, "{\"%s\":1}", s);
            CHECK(jflat_parse(&j, d, strlen(d)) == 0);
            memset(s, 'b', 129); s[129] = 0;
            snprintf(d, sizeof d, "{\"a\":\"%s\"}", s);
            CHECK(jflat_parse(&j, d, strlen(d)) == JF_E_LIMIT);
        }
    }
    /* writer */
    jw_init(&w, b, sizeof b);
    jw_raw(&w, "{");
    jw_str(&w, "a\"b\\c\n\x7f");
    jw_raw(&w, ":"); jw_long(&w, -12); jw_raw(&w, ","); jw_bool(&w, 1); jw_raw(&w, "}");
    CHECK(jw_end(&w) > 0);
    CHECK_STR(b, "{\"a\\\"b\\\\c\\u000a\\u007f\":-12,true}");
    jw_init(&w, b, 8);
    jw_raw(&w, "0123456789");
    CHECK(jw_end(&w) == -1);
    jw_init(&w, b, 8);
    jw_raw(&w, "0123456");
    CHECK(jw_end(&w) == 7);
    jw_raw(&w, "8");
    CHECK(jw_end(&w) == -1);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s control_api_vectors.txt\n", argv[0]); return 2; }
    test_sha();
    test_vectors(argv[1]);
    test_canon_misc();
    test_verify();
    test_keys();
    test_registry();
    test_json();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
