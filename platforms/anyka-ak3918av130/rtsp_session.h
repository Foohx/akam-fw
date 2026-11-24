/* Multi-client RTSP server of the Rotatable 2K own app (RTP over TCP interleaved, H.264 video, plus an optional L16/8000
 * microphone track, streamid=1, only with --audio 1: audio.h).
 *
 *   rtsp://<ip>:<port>/stream0   main (a2k_stream 0)      rtsp://<ip>:<port>/stream1   sub (a2k_stream 1)
 *
 * One accept thread plus one thread per client (at most A2K_RTSP_MAX_CLIENTS). A client never touches the encoder:
 * it reads copies of the encoded frames from the a2k_stream ring (stream.h) and writes them through the
 * shared bounded writer (net_tx) with the shared backpressure policy (tx_pace): a slow client loses frames and
 * resynchronises on a keyframe, the capture threads are never blocked. */
#ifndef AKAM2K_RTSP_H
#define AKAM2K_RTSP_H

#include "stream.h"
#include "rtsp_auth.h"

#define A2K_RTSP_MAX_CLIENTS 4
/* The one place the default RTSP port is set: 554, the same canonical URL as the 1080p (rtsp://<ip>:554/stream0).
 * akam.conf `port` (-p) overrides it; the stock yy_cam's 8554 is not served. */
#ifndef A2K_RTSP_PORT
#define A2K_RTSP_PORT 554
#endif

/* Starts listening on `port` (all interfaces) and the accept thread. auth_path: the credentials file of
 * rtsp_auth.h ("user:akam:HA1" on one line; absent = no authentication). Returns 0 or -1 (logged). */
int a2k_rtsp_start(int port, const char *auth_path, a2k_stream_t *streams[2]);
/* The credentials of this server (rtsp_auth.h): ONVIF logs in against the same file and shares its failure limiter (cred_init). Valid after a2k_rtsp_start. */
rtsp_auth_db_t *a2k_rtsp_auth_db(void);
/* Number of connected clients right now. */
int a2k_rtsp_clients(void);
/* Number of clients in PLAY right now (viewing or recording). */
int a2k_rtsp_playing(void);
/* The caller has set a2k_stop: waits (bounded) for the accept thread and every client thread, closes the listener. */
void a2k_rtsp_stop(void);

#endif
