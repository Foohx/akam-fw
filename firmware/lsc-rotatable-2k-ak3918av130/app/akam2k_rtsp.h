/* Multi-client RTSP server of the Rotatable 2K own app (RTP over TCP interleaved, H.264 video, plus an optional L16/8000
 * microphone track, streamid=1, only with --audio 1: akam2k_audio.h).
 *
 *   rtsp://<ip>:<port>/stream0   main (a2k_stream 0)      rtsp://<ip>:<port>/stream1   sub (a2k_stream 1)
 *
 * One accept thread plus one thread per client (at most A2K_RTSP_MAX_CLIENTS). A client never touches the encoder:
 * it reads copies of the encoded frames from the a2k_stream ring (akam2k_stream.h) and writes them through the
 * shared bounded writer (net_tx) with the shared backpressure policy (tx_pace): a slow client loses frames and
 * resynchronises on a keyframe, the capture threads are never blocked. */
#ifndef AKAM2K_RTSP_H
#define AKAM2K_RTSP_H

#include "akam2k_stream.h"

#define A2K_RTSP_MAX_CLIENTS 4

/* Starts listening on `port` (all interfaces) and the accept thread. auth_path: the credentials file of
 * rtsp_auth.h ("user:akam:HA1" on one line; absent = no authentication). Returns 0 or -1 (logged). */
int a2k_rtsp_start(int port, const char *auth_path, a2k_stream_t *streams[2]);
/* Number of connected clients right now. */
int a2k_rtsp_clients(void);
/* Number of clients in PLAY right now (viewing or recording). */
int a2k_rtsp_playing(void);
/* The caller has set a2k_stop: waits (bounded) for the accept thread and every client thread, closes the listener. */
void a2k_rtsp_stop(void);

#endif
