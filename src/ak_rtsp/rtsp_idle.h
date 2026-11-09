#pragma once
/* Pre-PLAY idle policy of the single-client RTSP server.
 *
 * One client at a time: a LAN host that connects and stays silent, or never
 * gets to PLAY, would hold the only slot forever. Before PLAY the connection
 * is dropped when
 *   - no complete RTSP request arrived for RTSP_IDLE_PRE_PLAY_MS, or
 *   - PLAY was not reached within RTSP_IDLE_SETUP_MAX_MS of the connection.
 * Partial request bytes do not count as activity (a client trickling one byte
 * a minute is still caught by the overall cap).
 * While playing there is NO idleness drop: over TCP, players may send nothing
 * at all; the stream ends on a write stall, disconnect, BYE or TEARDOWN.
 *
 * Pure state machine: the caller passes monotonic microseconds, so
 * tests/rtsp_idle_test.sh needs neither sleeps nor sockets. */
#include <stdint.h>

#define RTSP_IDLE_PRE_PLAY_MS 20000 /* silence allowed before PLAY */
#define RTSP_IDLE_SETUP_MAX_MS 60000 /* total time allowed to reach PLAY */

typedef struct {
    int64_t start_us;    /* connection accepted */
    int64_t activity_us; /* last complete request */
} rtsp_idle_t;

void rtsp_idle_init(rtsp_idle_t *s, int64_t now_us);
/* A complete RTSP request was received. */
void rtsp_idle_request(rtsp_idle_t *s, int64_t now_us);
/* NULL to keep the client, otherwise the drop reason. */
const char *rtsp_idle_check(const rtsp_idle_t *s, int is_playing, int64_t now_us);
/* Milliseconds poll() may sleep before the next possible verdict (> 0, <= max_ms). */
int rtsp_idle_wait_ms(const rtsp_idle_t *s, int is_playing, int64_t now_us, int max_ms);
