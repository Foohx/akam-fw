/* Host unit test of the Rotatable 2K own app's frame ring (akam2k_stream.c): per-stream frame caps, ring depth,
 * the keyframe request after a dropped frame, and the bound on memory. Built with the shared h264_params.c and
 * rtp_clock.c of src/ak_rtsp (compiled unmodified). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/app/akam2k_stream.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* An IDR-looking access unit (start code + NAL type 5) padded to len bytes, or a P slice (type 1). */
static uint8_t *mkau(size_t len, int key)
{
    uint8_t *b = calloc(1, len);
    b[3] = 1;
    b[4] = key ? 0x65 : 0x41;
    return b;
}

int main(void)
{
    static a2k_stream_t m, s;
    uint8_t *au;
    uint64_t cur = 0;
    a2k_frame_t *f;
    int lost, i;

    CHECK(A2K_RING <= 6, "ring depth is at most 6");
    CHECK((size_t)A2K_RING * (A2K_MAX_FRAME_MAIN + A2K_MAX_FRAME_SUB) <= (4u << 20), "worst-case ring memory (both streams) is within 4 MiB");
    CHECK(A2K_MAX_FRAME_MAIN == 512u * 1024 && A2K_MAX_FRAME_SUB == 128u * 1024, "caps are 512 KiB main, 128 KiB sub");

    a2k_stream_init(&m, "stream0", 2304, 1296, 15, 30, 2000, A2K_MAX_FRAME_MAIN);
    a2k_stream_init(&s, "stream1", 640, 360, 15, 30, 512, A2K_MAX_FRAME_SUB);

    au = mkau(A2K_MAX_FRAME_MAIN, 1);
    CHECK(a2k_stream_publish(&m, au, A2K_MAX_FRAME_MAIN) == 0, "main: a frame of exactly the cap is kept");
    CHECK(a2k_stream_take_idr(&m) == 0, "main: no keyframe request after a kept frame");
    CHECK(a2k_stream_publish(&m, au, A2K_MAX_FRAME_MAIN + 1) == -1, "main: one byte over the cap is dropped");
    CHECK(a2k_stream_take_idr(&m) == 1, "main: the drop requests a keyframe");
    CHECK(m.oversized == 1 && m.frames == 1, "main: counters (1 kept, 1 dropped)");
    free(au);

    au = mkau(A2K_MAX_FRAME_SUB + 1, 1);
    CHECK(a2k_stream_publish(&s, au, A2K_MAX_FRAME_SUB) == 0, "sub: a frame of exactly the cap is kept");
    CHECK(a2k_stream_publish(&s, au, A2K_MAX_FRAME_SUB + 1) == -1, "sub: one byte over its (smaller) cap is dropped");
    CHECK(a2k_stream_take_idr(&s) == 1, "sub: the drop requests a keyframe");
    CHECK(a2k_stream_publish(&s, au, 0) == -1 && a2k_stream_publish(&s, NULL, 10) == -1, "empty and NULL input are dropped");
    free(au);

    /* A 200 KB main frame (a normal IDR) is kept by main but would be refused by the sub cap. */
    au = mkau(200u << 10, 1);
    CHECK(a2k_stream_publish(&m, au, 200u << 10) == 0, "main: a 200 KB IDR is kept");
    CHECK(a2k_stream_publish(&s, au, 200u << 10) == -1, "sub: the same 200 KB is over its cap");
    free(au);

    /* The ring keeps only A2K_RING frames: a slow client is told it lost frames and jumps to the oldest one. */
    au = mkau(1000, 0);
    for (i = 0; i < A2K_RING + 3; i++)
        a2k_stream_publish(&m, au, 1000);
    free(au);
    f = a2k_stream_next(&m, &cur, &lost);
    CHECK(f != NULL && lost == 1, "a client behind by more than the ring is told it lost frames");
    CHECK(m.head - cur == A2K_RING - 1, "it got the oldest frame still held (cursor advanced past it)");
    if (f)
        a2k_frame_unref(&m, f);
    {
        unsigned held = 0;
        for (i = 0; i < A2K_RING; i++)
            held += m.ring[i] != NULL;
        CHECK(held == A2K_RING, "exactly A2K_RING frames are held, never more");
    }
    a2k_stream_destroy(&m);
    a2k_stream_destroy(&s);
    printf("rot2k stream unit: %d failed\n", fails);
    return fails != 0;
}
