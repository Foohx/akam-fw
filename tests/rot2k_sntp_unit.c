/* Host unit test of the pure part of akam2k_sntp (sntp/akam2k_sntp_core.c): request layout, reply validation (mode,
 * version, leap, stratum, origin, zero transmit), the NTP era handling around 2036 and 2038, timestamp round trips,
 * the offset/delay formulas and the 32-bit settimeofday target. Prints "rot2k sntp unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "akam2k_sntp_core.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* A valid server reply to a request that carried `sent`. */
static void good_reply(uint8_t *p, uint64_t sent, uint64_t rx, uint64_t tx)
{
    memset(p, 0, SNTP_PKT_LEN);
    p[0] = (0 << 6) | (4 << 3) | 4;
    p[1] = 2;
    put64(p + 24, sent);
    put64(p + 32, rx);
    put64(p + 40, tx);
}

int main(void)
{
    uint8_t q[SNTP_PKT_LEN], r[SNTP_PKT_LEN + 16];
    struct sntp_reply rep;
    const uint64_t sent = 0xe8a1b2c3d4e5f607ull;
    int64_t us, off, delay;
    int32_t s, u;

    sntp_build_request(q, sent);
    CHECK(q[0] == 0x23, "request: LI 0, VN 4, mode 3 (0x23)");
    CHECK(q[1] == 0 && q[2] == 0 && q[3] == 0, "request: stratum, poll, precision zero");
    {
        int zero = 1;
        for (int i = 4; i < 40; i++)
            if (q[i])
                zero = 0;
        CHECK(zero, "request: bytes 4..39 are zero (no reference or origin data leaked)");
    }
    CHECK(q[40] == 0xe8 && q[47] == 0x07 && q[44] == 0xd4, "request: transmit timestamp big endian at 40..47");

    good_reply(r, sent, 0x1111111122222222ull, 0xe8a1b2c500000001ull);
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK, "reply: valid packet accepted");
    CHECK(rep.rx_raw == 0x1111111122222222ull && rep.tx_raw == 0xe8a1b2c500000001ull && rep.stratum == 2,
          "reply: t2, t3 and stratum extracted");
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN + 16, sent, &rep) == SNTP_OK, "reply: extension bytes after 48 are ignored");
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN - 1, sent, &rep) == SNTP_E_SHORT, "reply: 47 bytes refused");
    CHECK(sntp_parse_reply(r, 0, sent, &rep) == SNTP_E_SHORT, "reply: empty refused");
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent ^ 1, &rep) == SNTP_E_ORIGIN, "reply: origin off by one bit refused");
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, 0, &rep) == SNTP_E_ORIGIN, "reply: origin zero refused");

    good_reply(r, sent, 1, 2);
    r[0] = (0 << 6) | (4 << 3) | 3;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_MODE, "reply: mode 3 (our own request echoed) refused");
    r[0] = (0 << 6) | (4 << 3) | 5;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_MODE, "reply: broadcast mode 5 refused");
    r[0] = (0 << 6) | (4 << 3) | 0;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_MODE, "reply: mode 0 refused");
    r[0] = (0 << 6) | (2 << 3) | 4;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_VERSION, "reply: version 2 refused");
    r[0] = (0 << 6) | (3 << 3) | 4;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK, "reply: version 3 accepted");
    r[0] = (3 << 6) | (4 << 3) | 4;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_LEAP, "reply: leap 3 (unsynchronised) refused");
    r[0] = (1 << 6) | (4 << 3) | 4;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK, "reply: leap second warning accepted");
    r[0] = (0 << 6) | (4 << 3) | 4;
    r[1] = 0;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_STRATUM, "reply: stratum 0 (kiss-o'-death) refused");
    r[1] = 16;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_STRATUM, "reply: stratum 16 refused");
    r[1] = 1;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK, "reply: stratum 1 accepted");
    r[1] = 15;
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK, "reply: stratum 15 accepted");
    good_reply(r, sent, 5, 0);
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_XMT_ZERO, "reply: zero transmit timestamp refused");
    good_reply(r, sent, 0, 5);
    CHECK(sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_E_RX_ZERO, "reply: zero receive timestamp refused");

    /* fuzz: random 48 byte packets never crash and are accepted only when they happen to be valid replies */
    srand(7);
    {
        int accepted = 0;
        for (int i = 0; i < 20000; i++) {
            for (int k = 0; k < SNTP_PKT_LEN; k++)
                r[k] = (uint8_t)rand();
            if (sntp_parse_reply(r, SNTP_PKT_LEN, sent, &rep) == SNTP_OK)
                accepted++;
        }
        CHECK(accepted == 0, "reply: random packets are not accepted (origin must match)");
    }

    /* NTP eras: 2026-11-15 00:00:00 UTC = unix 1794700800 = ntp 4003689600 (top bit set, era 0) */
    CHECK(sntp_ntp_to_us(((uint64_t)4003689600ull) << 32, &us) == 0 && us == 1794700800LL * 1000000, "era 0: 2026-11-15 converts");
    CHECK(sntp_ntp_to_us(((uint64_t)SNTP_UNIX_TO_NTP) << 32, &us) == 0 && us == 0, "era 0: 1970-01-01 is unix 0");
    CHECK(sntp_ntp_to_us(((uint64_t)SNTP_UNIX_TO_NTP - 1) << 32, &us) == -1, "era 0: one second before 1970 refused");
    CHECK(sntp_ntp_to_us(0xffffffffull << 32, &us) == 0 && us == (0xffffffffLL - SNTP_UNIX_TO_NTP) * 1000000,
          "era 0: last second of 2036-02-07 06:28:15");
    /* era 1: ntp seconds 0 = 2036-02-07 06:28:16 = unix 2085978496 */
    CHECK(sntp_ntp_to_us(0, &us) == 0 && us == 2085978496LL * 1000000, "era 1: NTP seconds 0 is 2036-02-07 06:28:16");
    CHECK(sntp_ntp_to_us(((uint64_t)100) << 32, &us) == 0 && us == 2085978596LL * 1000000, "era 1: seconds 100");
    CHECK(sntp_ntp_to_us(0x7fffffffull << 32, &us) == 0 && us == (0x7fffffffLL + 0x100000000LL - SNTP_UNIX_TO_NTP) * 1000000,
          "era 1: pivot 2^31-1 is year 2104");
    CHECK(sntp_ntp_to_us(0x80000000ull << 32, &us) == -1, "pivot: 2^31 (year 1968 in era 0) is before 1970 and refused");
    CHECK(sntp_ntp_to_us((((uint64_t)4003689600ull) << 32) | 0x80000000u, &us) == 0 && us == 1794700800LL * 1000000 + 500000,
          "fraction 0x80000000 is half a second");

    /* round trips over era 0 (the request side only builds 1970..2036 values) */
    {
        int bad = 0;
        for (int i = 0; i < 100000; i++) {
            int64_t t = (((int64_t)rand() << 16) ^ rand()) % (2085978496LL * 1000000 / 1000) * 1000 + rand() % 1000, back;
            if (sntp_ntp_to_us(sntp_us_to_ntp(t), &back) != 0 || back - t > 1 || t - back > 1)
                bad++;
        }
        CHECK(bad == 0, "round trip unix us -> ntp -> unix us within 1 us (100000 random times, 1970..2036)");
    }
    CHECK(sntp_us_to_ntp(0) == ((uint64_t)SNTP_UNIX_TO_NTP << 32), "request side: unix 0 is the 1970 NTP second, fraction 0");
    CHECK((sntp_us_to_ntp(999999) & 0xffffffffu) == (uint32_t)(((uint64_t)999999 << 32) / 1000000u), "request side: fraction scaling");

    /* offset and delay: server ahead by exactly 10 s, 40 ms each way, 2 ms server processing */
    {
        int64_t t1 = 1000000000LL, t2 = t1 + 10000000LL + 40000, t3 = t2 + 2000, t4 = t1 + 40000 + 2000 + 40000;
        sntp_offset(t1, t2, t3, t4, &off, &delay);
        CHECK(off == 10000000LL, "offset: server 10 s ahead with symmetric path = exactly +10 s");
        CHECK(delay == 80000, "delay: round trip minus server processing = 80 ms");
        /* clock 1970-ish: offset of a whole 56 years */
        t1 = 5000000LL;
        t2 = 1794700800LL * 1000000 + 100;
        t3 = t2 + 50;
        t4 = t1 + 300;
        sntp_offset(t1, t2, t3, t4, &off, &delay);
        CHECK(off == ((t2 - t1) + (t3 - t4)) / 2 && off > 1794000000LL * 1000000, "offset: 56 year step does not overflow");
        CHECK(delay == 250, "delay: 300 us round trip, 50 us server time");
        sntp_offset(t1, t1 - 5000000, t1 - 5000000, t1, &off, &delay);
        CHECK(off == -5000000 && delay == 0, "offset: server behind gives a negative offset");
    }

    /* 32-bit settimeofday target */
    CHECK(sntp_target(5000000LL, 1794700800LL * 1000000 + 250000 - 5000000LL, &s, &u) == 0 && s == 1794700800 && u == 250000,
          "target: 1970 clock + 56 year offset = 2026-11-15.25 (fits in int32)");
    CHECK(sntp_target(1794700800LL * 1000000, -500000, &s, &u) == 0 && s == 1794700799 && u == 500000,
          "target: negative offset borrows a second correctly");
    CHECK(sntp_target(1794700800LL * 1000000, 0x7fffffffLL * 1000000 - 1794700800LL * 1000000, &s, &u) == 0 && s == 0x7fffffff,
          "target: 2038-01-19 03:14:07 is the last value accepted");
    CHECK(sntp_target(1794700800LL * 1000000, 0x80000000LL * 1000000 - 1794700800LL * 1000000, &s, &u) == -1,
          "target: 2038-01-19 03:14:08 does not fit a 32-bit time_t and is refused");
    CHECK(sntp_target(0, 2085978496LL * 1000000, &s, &u) == 0 && s == 2085978496, "target: 2036-02-07 era 1 value fits in int32 and is accepted");
    CHECK(sntp_target(0, 2300000000LL * 1000000, &s, &u) == -1, "target: a 2042 value is refused, not wrapped negative");
    CHECK(sntp_target(1794700800LL * 1000000, -1794700800LL * 1000000, &s, &u) == -1, "target: 1970 result refused");

    printf("rot2k sntp unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
