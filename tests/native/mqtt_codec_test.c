/* Host test of src/ak_rtsp/mqtt.c: the MQTT 3.1.1 codec. Golden bytes (written by hand from the specification, not
 * produced by the code under test), remaining-length edge cases, every encoder against every too-small buffer,
 * malformed and hostile input, and a deterministic fuzz loop. Run under ASan/UBSan: every buffer below is an exact
 * size heap block, so one byte too many or too few is a report. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static void expect_bytes(int line, const uint8_t *got, int n, const uint8_t *want, size_t wn)
{
    if (n != (int)wn || memcmp(got, want, wn)) {
        fprintf(stderr, "FAIL line %d: got %d bytes:", line, n);
        for (int i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", got[i]);
        fprintf(stderr, "\n   want %zu bytes:", wn);
        for (size_t i = 0; i < wn && i < 64; i++) fprintf(stderr, " %02x", want[i]);
        fprintf(stderr, "\n");
        g_failed++;
    }
}
#define GOLDEN(got, n, ...) do { static const uint8_t w_[] = { __VA_ARGS__ }; expect_bytes(__LINE__, (got), (n), w_, sizeof(w_)); } while (0)

/* ------------------------------------------------------------------ remaining length */
static void test_varlen(void)
{
    static const struct { uint32_t v; int n; uint8_t b[4]; } t[] = {
        { 0, 1, { 0x00 } }, { 1, 1, { 0x01 } }, { 127, 1, { 0x7f } }, { 128, 2, { 0x80, 0x01 } },
        { 129, 2, { 0x81, 0x01 } }, { 16383, 2, { 0xff, 0x7f } }, { 16384, 3, { 0x80, 0x80, 0x01 } },
        { 2097151, 3, { 0xff, 0xff, 0x7f } }, { 2097152, 4, { 0x80, 0x80, 0x80, 0x01 } },
        { 268435455, 4, { 0xff, 0xff, 0xff, 0x7f } },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        uint8_t o[4];
        uint32_t back = 12345;
        CHECK(mqtt_varlen_enc(o, sizeof(o), t[i].v) == t[i].n);
        CHECK(memcmp(o, t[i].b, (size_t)t[i].n) == 0);
        CHECK(mqtt_varlen_dec(t[i].b, (size_t)t[i].n, &back) == t[i].n && back == t[i].v);
        /* every proper prefix is "need more" */
        for (int k = 0; k < t[i].n; k++) CHECK(mqtt_varlen_dec(t[i].b, (size_t)k, &back) == 0);
        /* the encoder refuses a buffer one byte short */
        uint8_t *tiny = malloc((size_t)t[i].n - 1 + 1);
        CHECK(mqtt_varlen_enc(tiny, (size_t)t[i].n - 1, t[i].v) == -1);
        free(tiny);
        /* trailing bytes are not consumed */
        uint8_t more[5] = { 0 };
        memcpy(more, t[i].b, (size_t)t[i].n);
        more[t[i].n] = 0x55;
        CHECK(mqtt_varlen_dec(more, (size_t)t[i].n + 1, &back) == t[i].n);
    }
    uint8_t o[5];
    CHECK(mqtt_varlen_enc(o, sizeof(o), 268435456u) == -1);
    CHECK(mqtt_varlen_enc(o, sizeof(o), 0xffffffffu) == -1);
    uint32_t v = 0;
    static const uint8_t overlong[] = { 0x80, 0x80, 0x80, 0x80, 0x01 };   /* a fifth byte */
    CHECK(mqtt_varlen_dec(overlong, sizeof(overlong), &v) == -1);
    static const uint8_t all_cont[] = { 0xff, 0xff, 0xff, 0xff };         /* four continuation bytes, never ends */
    CHECK(mqtt_varlen_dec(all_cont, sizeof(all_cont), &v) == -1);
    static const uint8_t nonshort0[] = { 0x80, 0x00 };                    /* 0 written in two bytes */
    CHECK(mqtt_varlen_dec(nonshort0, 2, &v) == -1);
    static const uint8_t nonshort1[] = { 0xff, 0x80, 0x00 };
    CHECK(mqtt_varlen_dec(nonshort1, 3, &v) == -1);
    CHECK(mqtt_varlen_dec(overlong, 0, &v) == 0);
}

/* ------------------------------------------------------------------ strings */
static void test_utf8(void)
{
    CHECK(mqtt_utf8_ok("", 0));
    CHECK(mqtt_utf8_ok("akam/cam-01/status", 18));
    CHECK(mqtt_utf8_ok("\xc3\xa9", 2));                       /* U+00E9 */
    CHECK(mqtt_utf8_ok("\xe2\x82\xac", 3));                   /* U+20AC */
    CHECK(mqtt_utf8_ok("\xf0\x9f\x98\x80", 4));               /* U+1F600 */
    CHECK(!mqtt_utf8_ok("a\0b", 3));                          /* U+0000 is forbidden */
    CHECK(!mqtt_utf8_ok("\xc0\x80", 2));                      /* overlong NUL */
    CHECK(!mqtt_utf8_ok("\xc1\xbf", 2));                      /* overlong */
    CHECK(!mqtt_utf8_ok("\xe0\x80\x80", 3));                  /* overlong */
    CHECK(!mqtt_utf8_ok("\xed\xa0\x80", 3));                  /* surrogate U+D800 */
    CHECK(!mqtt_utf8_ok("\xf4\x90\x80\x80", 4));              /* above U+10FFFF */
    CHECK(!mqtt_utf8_ok("\xf8\x88\x80\x80\x80", 5));
    CHECK(!mqtt_utf8_ok("\xc3", 1));                          /* truncated */
    CHECK(!mqtt_utf8_ok("\xe2\x82", 2));
    CHECK(!mqtt_utf8_ok("\x80", 1));                          /* stray continuation */
    CHECK(!mqtt_utf8_ok("\xc3\x28", 2));                      /* bad continuation */
    CHECK(!mqtt_utf8_ok("\xff", 1));
    char *big = malloc(70000);
    memset(big, 'a', 70000);
    CHECK(mqtt_utf8_ok(big, 65535));
    CHECK(!mqtt_utf8_ok(big, 65536));
    free(big);
    CHECK(mqtt_topic_name_ok("a/b", 3));
    CHECK(!mqtt_topic_name_ok("", 0));
    CHECK(!mqtt_topic_name_ok("a/+/b", 5));
    CHECK(!mqtt_topic_name_ok("a/#", 3));
    CHECK(!mqtt_topic_name_ok("a/\xff", 3));
}

/* ------------------------------------------------------------------ encoders */
#define BUF(name, n) uint8_t *name = malloc(n)

/* Every cap below the real length must give -1 and write nothing past cap (exact-size heap block). */
static void short_buffers(int (*enc)(uint8_t *, size_t, void *), void *arg, int want)
{
    for (int cap = 0; cap < want; cap++) {
        uint8_t *b = malloc((size_t)cap ? (size_t)cap : 1);
        CHECK(enc(b, (size_t)cap, arg) == -1);
        free(b);
    }
    /* the body is built after the longest fixed header and then moved down: 4 bytes of slack (see mqtt.h) */
    uint8_t *b = malloc((size_t)want + 4);
    CHECK(enc(b, (size_t)want + 4, arg) == want);
    free(b);
}

static int enc_connect_cb(uint8_t *o, size_t cap, void *a) { return mqtt_enc_connect(o, cap, a); }

static void test_encoders(void)
{
    mqtt_conn_t full = { "c1", "u", "p", 60, 1, "a/s", "offline", 1 };
    BUF(o, 256);
    int n = mqtt_enc_connect(o, 256, &full);
    GOLDEN(o, n, 0x10, 0x22, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xe6, 0x00, 0x3c, 0x00, 0x02, 'c', '1',
           0x00, 0x03, 'a', '/', 's', 0x00, 0x07, 'o', 'f', 'f', 'l', 'i', 'n', 'e', 0x00, 0x01, 'u', 0x00, 0x01, 'p');
    short_buffers(enc_connect_cb, &full, n);

    mqtt_conn_t min = { "x", NULL, NULL, 0, 1, NULL, NULL, 0 };
    n = mqtt_enc_connect(o, 256, &min);
    GOLDEN(o, n, 0x10, 0x0d, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x00, 0x00, 0x01, 'x');
    short_buffers(enc_connect_cb, &min, n);

    mqtt_conn_t user_only = { "x", "bob", "", 30, 0, NULL, NULL, 0 };       /* clean=0, user, empty password */
    n = mqtt_enc_connect(o, 256, &user_only);
    GOLDEN(o, n, 0x10, 0x12, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x80, 0x00, 0x1e, 0x00, 0x01, 'x', 0x00, 0x03, 'b', 'o', 'b');
    mqtt_conn_t will_noretain = { "x", NULL, NULL, 10, 1, "t", "", 0 };      /* will with an empty message */
    n = mqtt_enc_connect(o, 256, &will_noretain);
    GOLDEN(o, n, 0x10, 0x12, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x06, 0x00, 0x0a, 0x00, 0x01, 'x', 0x00, 0x01, 't', 0x00, 0x00);

    /* refusals */
    mqtt_conn_t bad;
    bad = (mqtt_conn_t){ "x", NULL, "pw", 60, 1, NULL, NULL, 0 };           /* password without user */
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ "", NULL, NULL, 60, 1, NULL, NULL, 0 };            /* empty client id */
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ NULL, NULL, NULL, 60, 1, NULL, NULL, 0 };
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ "x", NULL, NULL, 60, 1, "a/+", "m", 0 };           /* wildcard in the will topic */
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ "x", NULL, NULL, 60, 1, "", "m", 0 };              /* empty will topic */
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ "x\xff", NULL, NULL, 60, 1, NULL, NULL, 0 };       /* bad UTF-8 */
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    bad = (mqtt_conn_t){ "x", "b\xc3", NULL, 60, 1, NULL, NULL, 0 };
    CHECK(mqtt_enc_connect(o, 256, &bad) == -1);
    CHECK(mqtt_enc_connect(o, 5, &full) == -1);                             /* cap <= header room */
    free(o);

    /* a password with every byte value except NUL is binary data, not text */
    char pw[128];
    for (int i = 0; i < 127; i++) pw[i] = (char)(i + 1);
    pw[127] = '\0';
    mqtt_conn_t bin = { "x", "u", pw, 60, 1, NULL, NULL, 0 };
    BUF(ob, 400);
    n = mqtt_enc_connect(ob, 400, &bin);
    CHECK(n > 0 && memcmp(ob + n - 127, pw, 127) == 0);
    free(ob);

    /* PUBLISH */
    BUF(p, 64);
    n = mqtt_enc_publish(p, 64, "a/b", "hi", 2, 1);
    GOLDEN(p, n, 0x31, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i');
    n = mqtt_enc_publish(p, 64, "a/b", "hi", 2, 0);
    GOLDEN(p, n, 0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i');
    n = mqtt_enc_publish(p, 64, "t", NULL, 0, 1);                           /* empty retained payload: clears a retained message */
    GOLDEN(p, n, 0x31, 0x03, 0x00, 0x01, 't');
    CHECK(mqtt_enc_publish(p, 64, "a/+", "x", 1, 0) == -1);
    CHECK(mqtt_enc_publish(p, 64, "a/#", "x", 1, 0) == -1);
    CHECK(mqtt_enc_publish(p, 64, "", "x", 1, 0) == -1);
    CHECK(mqtt_enc_publish(p, 64, NULL, "x", 1, 0) == -1);
    free(p);
    for (int cap = 0; cap < 9; cap++) {
        uint8_t *b = malloc(cap ? (size_t)cap : 1);
        CHECK(mqtt_enc_publish(b, (size_t)cap, "a/b", "hi", 2, 1) == -1);
        free(b);
    }
    /* remaining length edge cases through a real PUBLISH: body = 2 + 1 + payload */
    static const size_t plen[] = { 124, 125, 126, 16380, 16381, 16382 };       /* body 127, 128, 129, 16383, 16384, 16385 */
    static const int vb[] = { 1, 2, 2, 2, 3, 3 };                              /* bytes of the remaining length */
    for (size_t i = 0; i < sizeof(plen) / sizeof(plen[0]); i++) {
        size_t cap = plen[i] + 3 + 5;
        uint8_t *b = malloc(cap);
        uint8_t *pl = malloc(plen[i]);
        memset(pl, 'p', plen[i]);
        n = mqtt_enc_publish(b, cap, "t", pl, plen[i], 0);
        CHECK(n == 1 + vb[i] + 3 + (int)plen[i]);
        mqtt_pkt_t pk;
        CHECK(mqtt_frame(b, (size_t)n, 70000, &pk) == n && pk.blen == plen[i] + 3 && pk.type == MQTT_PUBLISH);
        free(b);
        free(pl);
    }

    /* SUBSCRIBE, PUBACK, PINGREQ, DISCONNECT */
    BUF(s, 32);
    n = mqtt_enc_subscribe(s, 32, 1, "a/+/set");
    GOLDEN(s, n, 0x82, 0x0c, 0x00, 0x01, 0x00, 0x07, 'a', '/', '+', '/', 's', 'e', 't', 0x00);
    n = mqtt_enc_subscribe(s, 32, 0x1234, "t");
    GOLDEN(s, n, 0x82, 0x06, 0x12, 0x34, 0x00, 0x01, 't', 0x00);
    CHECK(mqtt_enc_subscribe(s, 32, 0, "t") == -1);                         /* identifier 0 is not allowed */
    CHECK(mqtt_enc_subscribe(s, 32, 1, "") == -1);
    CHECK(mqtt_enc_subscribe(s, 32, 1, NULL) == -1);
    CHECK(mqtt_enc_subscribe(s, 32, 1, "\xff") == -1);
    for (int cap = 0; cap < 8; cap++) {
        uint8_t *b = malloc(cap ? (size_t)cap : 1);
        CHECK(mqtt_enc_subscribe(b, (size_t)cap, 0x1234, "t") == -1);
        free(b);
    }
    n = mqtt_enc_puback(s, 32, 0xabcd);
    GOLDEN(s, n, 0x40, 0x02, 0xab, 0xcd);
    n = mqtt_enc_pingreq(s, 32);
    GOLDEN(s, n, 0xc0, 0x00);
    n = mqtt_enc_disconnect(s, 32);
    GOLDEN(s, n, 0xe0, 0x00);
    CHECK(mqtt_enc_puback(s, 3, 1) == -1 && mqtt_enc_pingreq(s, 1) == -1 && mqtt_enc_disconnect(s, 0) == -1);
    free(s);
}

/* ------------------------------------------------------------------ framing and decoders */
static uint8_t *g_keep;
static int frame(const uint8_t *b, size_t n, size_t maxb, mqtt_pkt_t *p)
{
    free(g_keep);                                   /* the previous call's copy */
    g_keep = calloc(n ? n : 1, 1);                  /* exact size: a read past n is a report */
    if (n) memcpy(g_keep, b, n);
    return mqtt_frame(g_keep, n, maxb, p);
}

static void test_frame(void)
{
    mqtt_pkt_t p;
    static const uint8_t connack[] = { 0x20, 0x02, 0x01, 0x00 };
    CHECK(frame(connack, 4, 100, &p) == 4 && p.type == MQTT_CONNACK && p.blen == 2 && p.total == 4);
    int sp = 0, rc = -1;
    CHECK(mqtt_dec_connack(&p, &sp, &rc) == 0 && sp == 1 && rc == 0);
    for (size_t k = 0; k < 4; k++) CHECK(frame(connack, k, 100, &p) == 0);     /* every prefix: need more */
    static const uint8_t refused[] = { 0x20, 0x02, 0x00, 0x05 };
    frame(refused, 4, 100, &p);
    CHECK(mqtt_dec_connack(&p, &sp, &rc) == 0 && sp == 0 && rc == 5);
    static const uint8_t bad_ack_flags[] = { 0x20, 0x02, 0x02, 0x00 };           /* reserved bits of the ack flags */
    frame(bad_ack_flags, 4, 100, &p);
    CHECK(mqtt_dec_connack(&p, &sp, &rc) == -1);
    static const uint8_t short_ack[] = { 0x20, 0x01, 0x00 };
    CHECK(frame(short_ack, 3, 100, &p) == 3 && mqtt_dec_connack(&p, &sp, &rc) == -1);

    /* reserved types and wrong flags */
    static const uint8_t t0[] = { 0x00, 0x00 }, t15[] = { 0xf0, 0x00 };
    CHECK(frame(t0, 2, 100, &p) == -1 && frame(t15, 2, 100, &p) == -1);
    static const uint8_t sub_bad[] = { 0x80, 0x03, 0, 1, 0 };                   /* SUBSCRIBE needs flags 0010 */
    CHECK(frame(sub_bad, 5, 100, &p) == -1);
    static const uint8_t ping_bad[] = { 0xc1, 0x00 };
    CHECK(frame(ping_bad, 2, 100, &p) == -1);
    static const uint8_t pingresp[] = { 0xd0, 0x00 };
    CHECK(frame(pingresp, 2, 100, &p) == 2 && p.type == MQTT_PINGRESP && p.blen == 0);
    static const uint8_t pubrel[] = { 0x62, 0x02, 0x00, 0x01 };
    CHECK(frame(pubrel, 4, 100, &p) == 4 && p.type == 6);
    static const uint8_t pubrel_bad[] = { 0x60, 0x02, 0x00, 0x01 };
    CHECK(frame(pubrel_bad, 4, 100, &p) == -1);

    /* remaining length: non-shortest, five bytes, and a length bigger than max_body */
    static const uint8_t nonshort[] = { 0x30, 0x80, 0x00 };
    CHECK(frame(nonshort, 3, 100, &p) == -1);
    static const uint8_t five[] = { 0x30, 0x80, 0x80, 0x80, 0x80, 0x01 };
    CHECK(frame(five, 6, 100, &p) == -1);
    static const uint8_t big[] = { 0x30, 0xff, 0xff, 0xff, 0x7f };               /* 268435455 */
    CHECK(frame(big, 5, 600, &p) == -2 && p.total == 5 + 268435455u && p.blen == 268435455u);
    static const uint8_t over[] = { 0x30, 0x81, 0x04 };                          /* 513 > 512 */
    CHECK(frame(over, 3, 512, &p) == -2 && p.total == 3 + 513u);
    static const uint8_t fits[] = { 0x30, 0x80, 0x04 };                          /* 512 == max_body: needs the body */
    CHECK(frame(fits, 3, 512, &p) == 0);
    static const uint8_t cont[] = { 0x30, 0x80 };                                /* the length itself is cut */
    CHECK(frame(cont, 2, 512, &p) == 0);
    CHECK(frame(cont, 1, 512, &p) == 0);

    /* two packets back to back: only the first is consumed */
    static const uint8_t two[] = { 0xd0, 0x00, 0x20, 0x02, 0x00, 0x00 };
    CHECK(frame(two, 6, 100, &p) == 2 && p.type == MQTT_PINGRESP);

    /* SUBACK and PUBACK */
    static const uint8_t suback[] = { 0x90, 0x03, 0x00, 0x01, 0x00 };
    uint16_t pid = 0;
    CHECK(frame(suback, 5, 100, &p) == 5 && mqtt_dec_suback(&p, &pid, &rc) == 0 && pid == 1 && rc == 0);
    static const uint8_t suback_fail[] = { 0x90, 0x03, 0x00, 0x01, 0x80 };
    CHECK(frame(suback_fail, 5, 100, &p) == 5 && mqtt_dec_suback(&p, &pid, &rc) == 0 && rc == 0x80);
    static const uint8_t suback_bad[] = { 0x90, 0x03, 0x00, 0x01, 0x07 };
    CHECK(frame(suback_bad, 5, 100, &p) == 5 && mqtt_dec_suback(&p, &pid, &rc) == -1);
    static const uint8_t suback_two[] = { 0x90, 0x04, 0x00, 0x01, 0x00, 0x00 };
    CHECK(frame(suback_two, 6, 100, &p) == 6 && mqtt_dec_suback(&p, &pid, &rc) == -1);   /* we send one filter */
    static const uint8_t puback[] = { 0x40, 0x02, 0x12, 0x34 };
    CHECK(frame(puback, 4, 100, &p) == 4 && mqtt_dec_puback(&p, &pid) == 0 && pid == 0x1234);
}

static void test_publish_dec(void)
{
    mqtt_pkt_t p;
    mqtt_pub_t m;
    static const uint8_t q0[] = { 0x31, 0x08, 0x00, 0x03, 'a', '/', 'b', 'h', 'i', '!' };
    CHECK(frame(q0, sizeof(q0), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == 0);
    CHECK(m.qos == 0 && m.retain == 1 && m.dup == 0 && m.tlen == 3 && !memcmp(m.topic, "a/b", 3) && m.plen == 3 &&
          !memcmp(m.payload, "hi!", 3));
    static const uint8_t q1[] = { 0x3a, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x2a, 'o', 'k' };   /* qos 1, dup */
    CHECK(frame(q1, sizeof(q1), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == 0);
    CHECK(m.qos == 1 && m.dup == 1 && m.retain == 0 && m.pid == 42 && m.plen == 2);
    static const uint8_t empty[] = { 0x30, 0x03, 0x00, 0x01, 't' };
    CHECK(frame(empty, sizeof(empty), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == 0 && m.plen == 0);
    static const uint8_t q3[] = { 0x36, 0x05, 0x00, 0x01, 't', 0x00, 0x01 };                     /* qos 3 */
    CHECK(frame(q3, sizeof(q3), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t nopid[] = { 0x32, 0x03, 0x00, 0x01, 't' };                              /* qos 1 without pid */
    CHECK(frame(nopid, sizeof(nopid), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t pid0[] = { 0x32, 0x05, 0x00, 0x01, 't', 0x00, 0x00 };
    CHECK(frame(pid0, sizeof(pid0), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t toolong[] = { 0x30, 0x04, 0x00, 0x09, 't', 'x' };                       /* topic past the body */
    CHECK(frame(toolong, sizeof(toolong), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t notopic[] = { 0x30, 0x02, 0x00, 0x00 };
    CHECK(frame(notopic, sizeof(notopic), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t onebyte[] = { 0x30, 0x01, 0x00 };
    CHECK(frame(onebyte, sizeof(onebyte), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t wild[] = { 0x30, 0x05, 0x00, 0x03, 'a', '/', '#' };
    CHECK(frame(wild, sizeof(wild), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    static const uint8_t badutf[] = { 0x30, 0x04, 0x00, 0x02, 0xc3, 0x28 };
    CHECK(frame(badutf, sizeof(badutf), 100, &p) > 0 && mqtt_dec_publish(&p, &m) == -1);
    mqtt_pkt_t notpub = { MQTT_PINGRESP, 0, NULL, 0, 2 };
    CHECK(mqtt_dec_publish(&notpub, &m) == -1);
}

/* ------------------------------------------------------------------ backoff */
static void test_backoff(void)
{
    for (unsigned attempt = 0; attempt < 40; attempt++) {
        for (uint32_t r = 0; r < 200; r++) {
            unsigned d = mqtt_backoff_ms(attempt, r * 2654435761u, 5000, 60000);
            CHECK(d >= 4000 && d <= 60000);
        }
    }
    /* grows by doubling before the cap (compare with the same jitter) */
    CHECK(mqtt_backoff_ms(0, 20, 5000, 60000) == 5000);      /* rnd % 41 == 20 -> exactly 100 % */
    CHECK(mqtt_backoff_ms(1, 20, 5000, 60000) == 10000);
    CHECK(mqtt_backoff_ms(2, 20, 5000, 60000) == 20000);
    CHECK(mqtt_backoff_ms(3, 20, 5000, 60000) == 40000);
    CHECK(mqtt_backoff_ms(4, 20, 5000, 60000) == 60000);     /* capped */
    CHECK(mqtt_backoff_ms(1000, 20, 5000, 60000) == 60000);  /* no overflow */
    CHECK(mqtt_backoff_ms(0, 0, 5000, 60000) == 4000);       /* -20 % */
    CHECK(mqtt_backoff_ms(0, 40, 5000, 60000) == 6000);      /* +20 % */
    CHECK(mqtt_backoff_ms(10, 40, 5000, 60000) == 60000);    /* the cap is hard */
    CHECK(mqtt_backoff_ms(0, 7, 50, 400) >= 40 && mqtt_backoff_ms(9, 7, 50, 400) <= 400);
    (void)mqtt_backoff_ms(5, 0xffffffffu, 0xffffffffu, 0xffffffffu);                          /* huge values: no UB (UBSan) */
}

/* ------------------------------------------------------------------ fuzz */
static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void feed(const uint8_t *b, size_t n)
{
    uint8_t *c = calloc(n ? n : 1, 1);
    if (n) memcpy(c, b, n);
    mqtt_pkt_t p;
    int r = mqtt_frame(c, n, 300, &p);
    CHECK(r <= 0 || (size_t)r <= n);
    if (r > 0) {
        CHECK((size_t)r == p.total && p.body + p.blen <= c + n);
        int a, bb;
        uint16_t pid;
        mqtt_pub_t m;
        (void)mqtt_dec_connack(&p, &a, &bb);
        (void)mqtt_dec_suback(&p, &pid, &a);
        (void)mqtt_dec_puback(&p, &pid);
        if (mqtt_dec_publish(&p, &m) == 0) {
            CHECK(m.topic >= (const char *)c && (const uint8_t *)m.topic + m.tlen <= c + n);
            CHECK(m.payload >= c && m.payload + m.plen <= c + n);
            CHECK(m.tlen > 0 && !memchr(m.topic, '+', m.tlen) && !memchr(m.topic, '#', m.tlen));
        }
    }
    free(c);
}

static void test_fuzz(void)
{
    uint8_t seed[] = { 0x31, 0x08, 0x00, 0x03, 'a', '/', 'b', 'h', 'i', '!' };
    uint8_t buf[320];
    /* pure random bytes, biased toward small first bytes so that every type is hit */
    for (int it = 0; it < 30000; it++) {
        size_t n = rnd() % 40;
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
        if (n) buf[0] = (uint8_t)((rnd() % 16) << 4 | (rnd() % 16));
        feed(buf, n);
    }
    /* every single-byte mutation and every truncation of a real PUBLISH */
    for (size_t i = 0; i < sizeof(seed); i++) {
        for (int v = 0; v < 256; v++) {
            uint8_t m[sizeof(seed)];
            memcpy(m, seed, sizeof(seed));
            m[i] = (uint8_t)v;
            feed(m, sizeof(m));
        }
        feed(seed, i);
    }
    /* all the packets this module encodes, mutated at random */
    uint8_t pk[256];
    mqtt_conn_t c = { "c1", "u", "p", 60, 1, "a/s", "offline", 1 };
    int n = mqtt_enc_connect(pk, sizeof(pk), &c);
    for (int it = 0; it < 20000; it++) {
        uint8_t m[256];
        memcpy(m, pk, (size_t)n);
        for (int k = 0; k < 3; k++) m[rnd() % (unsigned)n] = (uint8_t)rnd();
        feed(m, (size_t)n - rnd() % 4);
    }
    /* remaining length byte sweep with a long, zero-filled body behind it */
    memset(buf, 0, sizeof(buf));
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b += 15) {
            buf[0] = 0x30; buf[1] = (uint8_t)a; buf[2] = (uint8_t)b;
            feed(buf, sizeof(buf));
        }
}

int main(void)
{
    test_varlen();
    test_utf8();
    test_encoders();
    test_frame();
    test_publish_dec();
    test_backoff();
    test_fuzz();
    free(g_keep);
    if (g_failed) { fprintf(stderr, "mqtt_codec_test: %d FAILED\n", g_failed); return 1; }
    printf("mqtt_codec_test: OK\n");
    return 0;
}
