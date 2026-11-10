/* Host test of src/ak_rtsp/rtp_in.c: RTP receive unpacker of the audio
 * backchannel. Build/run: tests/rtp_in_test.sh (ASan + UBSan). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g711.h"
#include "rtp_in.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* Builds a packet: version 2, no padding/extension/CSRC unless asked. */
static size_t mk(uint8_t *p, int pt, uint16_t seq, uint32_t ssrc, const uint8_t *pl, size_t n)
{
    memset(p, 0, 12);
    p[0] = 0x80;
    p[1] = (uint8_t)pt;
    p[2] = (uint8_t)(seq >> 8); p[3] = (uint8_t)seq;
    p[8] = (uint8_t)(ssrc >> 24); p[9] = (uint8_t)(ssrc >> 16);
    p[10] = (uint8_t)(ssrc >> 8); p[11] = (uint8_t)ssrc;
    memcpy(p + 12, pl, n);
    return 12 + n;
}

static int16_t pcm[RTP_IN_MAX_SAMPLES];

static void test_valid(void)
{
    printf("# valid packets\n");
    rtp_in_t r; rtp_in_init(&r);
    uint8_t pl[160], pkt[400];
    for (int i = 0; i < 160; i++) pl[i] = (uint8_t)(i * 3);
    size_t n = 0; int pt = -1;

    size_t len = mk(pkt, 0, 100, 0x11223344, pl, 160);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && n == 160 && pt == 0, "PCMU accepted");
    int same = 1;
    for (int i = 0; i < 160; i++) if (pcm[i] != g711_ulaw_decode(pl[i])) same = 0;
    CHECK(same, "PCMU payload decoded with mu-law");

    len = mk(pkt, 8, 101, 0x11223344, pl, 160);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && pt == 8 && n == 160, "PCMA accepted");
    same = 1;
    for (int i = 0; i < 160; i++) if (pcm[i] != g711_alaw_decode(pl[i])) same = 0;
    CHECK(same, "PCMA payload decoded with A-law");
    CHECK(r.packets == 2 && r.samples == 320 && r.lost == 0 && r.dropped == 0, "counters");

    pkt[1] |= 0x80; /* marker bit must not matter */
    pkt[2] = 0; pkt[3] = 102;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && pt == 8, "marker bit ignored");

    len = mk(pkt, 0, 103, 0x11223344, pl, 1);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && n == 1, "one-sample payload");
}

static void test_sequence(void)
{
    printf("# sequence\n");
    rtp_in_t r; rtp_in_init(&r);
    uint8_t pl[8] = {0}, pkt[64];
    size_t n; int pt;
    uint16_t s = 65533;
    for (int i = 0; i < 6; i++, s++) {
        size_t len = mk(pkt, 0, s, 7, pl, 8);
        CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK, "sequence wrap 65535 -> 0 accepted");
    }
    CHECK(r.packets == 6 && r.lost == 0, "no loss across the wrap");

    size_t len = mk(pkt, 0, (uint16_t)(s - 1), 7, pl, 8);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_DUP, "same sequence: duplicate");
    len = mk(pkt, 0, (uint16_t)(s - 5), 7, pl, 8);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_DUP, "older sequence: dropped");
    CHECK(r.dropped == 2 && r.dropped_by[RTP_IN_DROP_DUP] == 2, "duplicates counted");

    len = mk(pkt, 0, (uint16_t)(s + 3), 7, pl, 8); /* 3 missing */
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && r.lost == 3, "gap of 3: accepted, 3 lost");

    /* new SSRC restarts the tracking, also with a lower sequence */
    len = mk(pkt, 0, 5, 8, pl, 8);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && r.lost == 3, "new SSRC: followed from scratch");
    len = mk(pkt, 0, 6, 8, pl, 8);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK, "and continues");

    /* same SSRC, sequence restarted far away */
    len = mk(pkt, 0, 40000, 8, pl, 8);
    unsigned long lost = r.lost;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && r.lost == lost, "far jump: resync, no loss counted");
}

static void test_bad(void)
{
    printf("# malformed packets\n");
    rtp_in_t r; rtp_in_init(&r);
    uint8_t pl[40], pkt[RTP_IN_MAX_PACKET + 64];
    memset(pl, 0x55, sizeof(pl));
    size_t n = 99; int pt = -1;
    size_t len;

    CHECK(rtp_in_feed(&r, pkt, 0, pcm, &n, &pt) == RTP_IN_DROP_SHORT, "empty");
    CHECK(rtp_in_feed(&r, pkt, 11, pcm, &n, &pt) == RTP_IN_DROP_SHORT, "shorter than the fixed header");

    len = mk(pkt, 0, 1, 1, pl, 40); pkt[0] = 0x40;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_VERSION, "version 1");
    pkt[0] = 0xC0;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_VERSION, "version 3");

    len = mk(pkt, 0, 1, 1, pl, 40);
    CHECK(rtp_in_feed(&r, pkt, 12, pcm, &n, &pt) == RTP_IN_DROP_EMPTY, "header only: no audio");

    /* CSRC list */
    len = mk(pkt, 0, 2, 1, pl, 40); pkt[0] = 0x82; /* CC = 2: payload starts at 20 */
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && n == 32, "CSRC list skipped (32 samples left)");
    len = mk(pkt, 0, 3, 1, pl, 4); pkt[0] = 0x8F; /* CC = 15 needs 60 bytes of CSRC */
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_SHORT, "CSRC list longer than the packet");

    /* extension */
    len = mk(pkt, 0, 4, 1, pl, 40);
    memmove(pkt + 12 + 8, pkt + 12, 40);
    pkt[0] = 0x90; pkt[12] = 0xBE; pkt[13] = 0xDE; pkt[14] = 0; pkt[15] = 1; /* 1 word of extension */
    memset(pkt + 16, 0, 4);
    CHECK(rtp_in_feed(&r, pkt, len + 8, pcm, &n, &pt) == RTP_IN_OK && n == 40, "extension skipped");
    pkt[15] = 200; /* declares 800 bytes */
    CHECK(rtp_in_feed(&r, pkt, len + 8, pcm, &n, &pt) == RTP_IN_DROP_SHORT, "extension longer than the packet");
    CHECK(rtp_in_feed(&r, pkt, 14, pcm, &n, &pt) == RTP_IN_DROP_SHORT, "extension header cut");

    /* padding */
    len = mk(pkt, 0, 5, 1, pl, 40); pkt[0] = 0xA0; pkt[len - 1] = 8;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_OK && n == 32, "padding removed");
    pkt[len - 1] = 0;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PADDING, "padding length 0");
    pkt[len - 1] = 41;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PADDING, "padding larger than the payload");
    pkt[len - 1] = 40;
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_EMPTY, "padding eats the whole payload");

    /* payload types */
    len = mk(pkt, 96, 6, 1, pl, 40);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PT, "dynamic PT dropped");
    len = mk(pkt, 13, 6, 1, pl, 40);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PT, "comfort noise PT 13 dropped");
    len = mk(pkt, 101, 6, 1, pl, 4);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PT, "telephone-event dropped");
    len = mk(pkt, 97, 6, 1, pl, 40);
    CHECK(rtp_in_feed(&r, pkt, len, pcm, &n, &pt) == RTP_IN_DROP_PT, "L16 PT 97 dropped");

    /* size */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x80;
    pkt[3] = 7;
    CHECK(rtp_in_feed(&r, pkt, RTP_IN_MAX_PACKET, pcm, &n, &pt) == RTP_IN_OK && n == RTP_IN_MAX_PACKET - 12,
          "largest accepted packet");
    CHECK(rtp_in_feed(&r, pkt, RTP_IN_MAX_PACKET + 1, pcm, &n, &pt) == RTP_IN_DROP_BIG, "one byte over the limit");
    CHECK(rtp_in_feed(&r, pkt, 65535, pcm, &n, &pt) == RTP_IN_DROP_BIG, "huge length (no read past the buffer)");

    unsigned long sum = 0;
    for (int i = 0; i <= RTP_IN_DROP_DUP; i++) sum += r.dropped_by[i];
    CHECK(sum == r.dropped && r.dropped > 10, "every drop is counted once, by reason");
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void test_fuzz(void)
{
    printf("# fuzz (sanitizers watch the bounds)\n");
    rtp_in_t r; rtp_in_init(&r);
    int ok = 1;
    for (int i = 0; i < 200000; i++) {
        size_t len = rnd() % (RTP_IN_MAX_PACKET + 1);
        if (rnd() % 4 == 0) len = rnd() % 40;
        uint8_t *buf = malloc(len ? len : 1);
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        if (len >= 12 && rnd() % 2) { buf[0] = (uint8_t)((buf[0] & 0x3F) | 0x80); buf[1] &= 0x08 | (uint8_t)(rnd() % 2 ? 0x80 : 0); }
        size_t n = 0; int pt = -1;
        rtp_in_result_t res = rtp_in_feed(&r, buf, len, pcm, &n, &pt);
        if (res == RTP_IN_OK && (n == 0 || n > len || (pt != 0 && pt != 8))) ok = 0;
        free(buf);
    }
    CHECK(ok, "200000 random packets: never a bad count");
    CHECK(r.packets + r.dropped == 200000, "every packet accepted or counted as dropped");
}

int main(void)
{
    test_valid();
    test_sequence();
    test_bad();
    test_fuzz();
    printf(fails ? "\nFAILED: %d\n" : "\nrtp_in_test: all passed\n", fails);
    return fails ? 1 : 0;
}
