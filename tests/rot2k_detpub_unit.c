/* Host unit test of the person-box publication logic (akam2k_detpub.c): rate limit, change detection, the empty list when the last
 * person leaves, privacy / parked / off, stable ids, the JSON format, and that a burst of lists never evicts another event type
 * (api_events.c rings). No camera. */
#include <stdio.h>
#include <string.h>

#include "api_events.h"
#include "akam2k_detpub.h"

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const struct a2k_dp_in P1 = { 100, 20, 139, 119, 900, 0, 0 };

static int upd(a2k_dp_t *p, int64_t t, int hz, int blocked, const struct a2k_dp_in *b, int n, char *js)
{
    return a2k_dp_update(p, t, 1700000000000LL + t, hz, blocked, b, n, 1, 1200, 340, js, A2K_DP_JSON);
}

static void test_format(void)
{
    a2k_dp_t p;
    char js[A2K_DP_JSON];
    struct a2k_dp_in b[2] = { { 100, 20, 139, 119, 900, 0, 1 }, { 0, 0, 319, 191, 700, 1, 0 } };
    int n;
    a2k_dp_init(&p);
    n = a2k_dp_update(&p, 1000, 1700000000123LL, 2, 0, b, 2, 1, 1200, 340, js, sizeof js);
    CHECK(n > 0 && (size_t)n == strlen(js));
    CHECK(!strcmp(js, "{\"ts_ms\":1700000000123,\"w\":320,\"h\":192,\"pan\":1200,\"tilt\":340,\"boxes\":["
                      "{\"x\":312,\"y\":104,\"w\":125,\"h\":520,\"score\":900,\"id\":0,\"target\":true,\"ignored\":false},"
                      "{\"x\":0,\"y\":0,\"w\":1000,\"h\":1000,\"score\":700,\"id\":1,\"target\":false,\"ignored\":true}]}"));
    /* position unknown: no pan / tilt */
    a2k_dp_init(&p);
    n = a2k_dp_update(&p, 1000, 5, 2, 0, b, 1, 0, 0, 0, js, sizeof js);
    CHECK(n > 0 && !strstr(js, "pan") && !strstr(js, "tilt") && strstr(js, "\"ts_ms\":5,\"w\":320,\"h\":192,\"boxes\":[{"));
    /* the biggest list fits the buffer and the registry value */
    {
        struct a2k_dp_in m[A2K_DP_MAX];
        int i;
        for (i = 0; i < A2K_DP_MAX; i++) {
            m[i].x1 = i * 30, m[i].y1 = 0, m[i].x2 = i * 30 + 29, m[i].y2 = 191, m[i].score = 1000, m[i].ignored = 1, m[i].target = 1;
        }
        a2k_dp_init(&p);
        n = a2k_dp_update(&p, 1000, 1700000000123LL, 2, 0, m, A2K_DP_MAX + 3, 1, 100000, 100000, js, sizeof js);
        CHECK(n > 0 && n <= A2K_EVT_DDATA); /* n > MAX is cut to MAX */
        CHECK(a2k_dp_update(&p, 5000, 1, 2, 0, m, 8, 1, 1, 1, js, 20) == 0); /* a buffer too small publishes nothing */
    }
}

static void test_rate_and_change(void)
{
    a2k_dp_t p;
    char js[A2K_DP_JSON];
    struct a2k_dp_in b = P1;
    int64_t t;
    int pubs = 0;
    a2k_dp_init(&p);
    CHECK(upd(&p, 1000, 2, 0, &b, 1, js) > 0); /* first sight: at once */
    CHECK(upd(&p, 1200, 2, 0, &b, 1, js) == 0);
    /* unchanged while present: the 1 s keepalive, not every inference */
    for (t = 1200; t <= 6000; t += 200)
        if (upd(&p, t, 2, 0, &b, 1, js) > 0)
            pubs++;
    CHECK(pubs == 5); /* 2000, 3000, 4000, 5000, 6000 */
    /* a move over the threshold publishes, but not faster than 500 ms at 2 Hz */
    b.x1 += 20, b.x2 += 20; /* 62 per-mille */
    CHECK(upd(&p, 6200, 2, 0, &b, 1, js) == 0);
    CHECK(upd(&p, 6400, 2, 0, &b, 1, js) == 0);
    CHECK(upd(&p, 6500, 2, 0, &b, 1, js) > 0);
    /* a small jitter (under 15 per-mille) is no change */
    b.x1 += 1, b.x2 += 1;
    CHECK(upd(&p, 7100, 2, 0, &b, 1, js) == 0);
    /* a flag flip is a change */
    b.ignored = 1;
    CHECK(upd(&p, 6700, 2, 0, &b, 1, js) == 0); /* rate limited ... */
    CHECK(upd(&p, 7000, 2, 0, &b, 1, js) > 0); /* ... then due */
    b.target = 1;
    CHECK(upd(&p, 8100, 2, 0, &b, 1, js) > 0 && strstr(js, "\"target\":true"));
    /* never more than 5 per second, even at publish_hz 10 */
    a2k_dp_init(&p);
    pubs = 0;
    for (t = 0; t < 10000; t += 20) {
        struct a2k_dp_in m = P1;
        m.x1 += (int)(t / 20 % 3) * 40, m.x2 += (int)(t / 20 % 3) * 40; /* moves at every publication */
        if (upd(&p, t, 10, 0, &m, 1, js) > 0)
            pubs++;
    }
    CHECK(pubs <= 50 && pubs >= 49);
    /* hz 1: one a second at most */
    a2k_dp_init(&p);
    pubs = 0;
    for (t = 0; t < 10000; t += 100) {
        struct a2k_dp_in m = P1;
        m.x1 += (int)(t / 100 % 2) * 40, m.x2 += (int)(t / 100 % 2) * 40;
        if (upd(&p, t, 1, 0, &m, 1, js) > 0)
            pubs++;
    }
    CHECK(pubs == 10);
    /* a new box count is a change */
    a2k_dp_init(&p);
    {
        struct a2k_dp_in two[2] = { P1, { 200, 30, 239, 129, 800, 0, 0 } };
        CHECK(upd(&p, 1000, 2, 0, two, 1, js) > 0);
        CHECK(upd(&p, 1500, 2, 0, two, 2, js) > 0 && strstr(js, "\"id\":1"));
    }
}

static void test_empty_on_leave(void)
{
    a2k_dp_t p;
    char js[A2K_DP_JSON];
    struct a2k_dp_in b = P1;
    a2k_dp_init(&p);
    CHECK(upd(&p, 1000, 2, 0, NULL, 0, js) == 0); /* nobody ever: nothing */
    CHECK(upd(&p, 1200, 2, 0, NULL, 0, js) == 0);
    CHECK(upd(&p, 2000, 2, 0, &b, 1, js) > 0);
    CHECK(upd(&p, 2200, 2, 0, NULL, 0, js) == 0); /* leaves, but within the rate limit: waits */
    CHECK(upd(&p, 2500, 2, 0, NULL, 0, js) > 0 && strstr(js, "\"boxes\":[]}"));
    CHECK(upd(&p, 2700, 2, 0, NULL, 0, js) == 0); /* the empty list is sent once */
    CHECK(upd(&p, 9000, 2, 0, NULL, 0, js) == 0);
    /* a person who came and went inside the rate limit before it was ever published needs no empty list */
    a2k_dp_init(&p);
    CHECK(upd(&p, 1000, 2, 0, &b, 1, js) > 0);
    CHECK(upd(&p, 1100, 2, 0, NULL, 0, js) == 0);
    CHECK(upd(&p, 1500, 2, 0, NULL, 0, js) > 0); /* it was published: the empty one follows */
    CHECK(upd(&p, 1600, 2, 0, &b, 1, js) == 0);  /* and the rate limit holds for the next arrival too */
    CHECK(upd(&p, 2000, 2, 0, &b, 1, js) > 0);
}

static void test_blocked_off_ids(void)
{
    a2k_dp_t p;
    char js[A2K_DP_JSON];
    struct a2k_dp_in b = P1;
    a2k_dp_init(&p);
    CHECK(upd(&p, 1000, 0, 0, &b, 1, js) == 0); /* publish_hz 0 = off */
    CHECK(upd(&p, 5000, 0, 0, &b, 1, js) == 0);
    CHECK(upd(&p, 6000, 2, 1, &b, 1, js) == 0); /* privacy / parked */
    CHECK(upd(&p, 8000, 2, 1, &b, 1, js) == 0);
    CHECK(upd(&p, 9000, 2, 0, &b, 1, js) > 0);  /* back: published at once */
    CHECK(upd(&p, 9200, 2, 1, NULL, 0, js) == 0); /* blocked while a person was published: no empty list either */
    CHECK(upd(&p, 12000, 2, 0, NULL, 0, js) == 0);
    /* ids: kept while the person stays near, a new one for a new person; never reused */
    a2k_dp_init(&p);
    CHECK(upd(&p, 1000, 2, 0, &b, 1, js) > 0 && strstr(js, "\"id\":0"));
    b.x1 += 20, b.x2 += 20;
    CHECK(upd(&p, 2000, 2, 0, &b, 1, js) > 0 && strstr(js, "\"id\":0"));
    CHECK(upd(&p, 3000, 2, 0, NULL, 0, js) > 0);
    CHECK(upd(&p, 4000, 2, 0, &b, 1, js) > 0 && strstr(js, "\"id\":1")); /* he came back after the empty list: a new id */
    {
        struct a2k_dp_in two[2] = { b, { 250, 40, 289, 139, 700, 0, 0 } };
        CHECK(upd(&p, 5000, 2, 0, two, 2, js) > 0 && strstr(js, "\"id\":1") && strstr(js, "\"id\":2"));
    }
    /* hz and the latest list */
    a2k_dp_set_hz(99);
    CHECK(a2k_dp_hz() == A2K_DP_HZ_MAX);
    a2k_dp_set_hz(-3);
    CHECK(a2k_dp_hz() == 0);
    a2k_dp_set_hz(A2K_DP_HZ_DEFAULT);
    {
        char o[A2K_DP_JSON];
        CHECK(a2k_dp_latest(o, sizeof o) > 0 && !strcmp(o, "{\"ts_ms\":0,\"boxes\":[]}"));
        a2k_dp_set_latest(js);
        CHECK(a2k_dp_latest(o, sizeof o) == (int)strlen(js) && !strcmp(o, js));
        CHECK(a2k_dp_latest(o, 10) == -1);
        a2k_dp_clear_latest();
        CHECK(!strcmp((a2k_dp_latest(o, sizeof o), o), "{\"ts_ms\":0,\"boxes\":[]}"));
    }
}

static void test_ring_isolation(void)
{
    static char big[12288];
    a2k_dp_t p;
    char js[A2K_DP_JSON];
    struct a2k_dp_in m[A2K_DP_MAX];
    int i, count = 0, nd = 0;
    char *q;
    for (i = 0; i < A2K_DP_MAX; i++) {
        m[i].x1 = i * 30, m[i].y1 = 0, m[i].x2 = i * 30 + 29, m[i].y2 = 191, m[i].score = 1000, m[i].ignored = 1, m[i].target = 1;
    }
    a2k_apievt_reset();
    a2k_api_event_button();
    a2k_api_event("setting", "{\"key\":\"led.mode\"}");
    a2k_dp_init(&p);
    for (i = 0; i < 200; i++) { /* ten minutes of full lists, as fast as the limiter lets them */
        int n = a2k_dp_update(&p, 1000 + i * 250, 1700000000000LL, 10, 0, m, A2K_DP_MAX - (i & 1), 1, 100000, 100000, js, sizeof js);
        if (n > 0)
            a2k_api_event(A2K_EVT_DETECTIONS, js);
    }
    CHECK(a2k_apievt_json(0, big, sizeof big, &count) > 0);
    CHECK(strstr(big, "\"type\":\"button\"") && strstr(big, "\"type\":\"setting\""));
    for (q = big; (q = strstr(q, "\"type\":\"detections\"")) != NULL; q++)
        nd++;
    CHECK(nd == A2K_EVT_DRING);
}

int main(void)
{
    test_format();
    test_rate_and_change();
    test_empty_on_leave();
    test_blocked_off_ids();
    test_ring_isolation();
    printf("%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
