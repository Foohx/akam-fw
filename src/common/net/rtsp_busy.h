#pragma once
/* Second RTSP client while one is being served (G06).
 *
 * The server streams to one client at a time, from inside handle_client(), so main()'s accept loop does not run
 * during a session. A second client would connect (the kernel completes the handshake in the backlog) and then
 * wait in silence for hours. Instead the session loop adds the listening socket to its own poll set and, with
 * this module, answers the newcomer at once with "RTSP/1.0 503 Service Unavailable" and closes it.
 *
 * Why in the session loop and not in a helper thread: no extra thread or stack (45 MB RAM), no race with the
 * accept() of main(), no extra wakeup while nobody connects (the sockets only join a poll that already runs),
 * and the work per newcomer is a few non-blocking syscalls. Nothing here ever blocks: sockets are
 * non-blocking, at most RTSP_BUSY_MAX newcomers wait for their request, and each is closed after
 * RTSP_BUSY_WAIT_MS whether or not it sent one. The active session and its frame timing are not touched.
 *
 * Several viewers: restream with go2rtc (one connection from go2rtc, N viewers behind it). */
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#define RTSP_BUSY_MAX     3     /* newcomers waiting for their request at once; more are closed on accept */
#define RTSP_BUSY_WAIT_MS 500   /* wait for a request this long, then answer anyway */
#define RTSP_BUSY_REQ_MAX 384   /* bytes of the request kept (only CSeq is read) */
#define RTSP_BUSY_NFDS    (1 + RTSP_BUSY_MAX)

typedef struct {
    int     fd[RTSP_BUSY_MAX];     /* -1 = free */
    size_t  len[RTSP_BUSY_MAX];
    int64_t t0_ms[RTSP_BUSY_MAX];
    char    buf[RTSP_BUSY_MAX][RTSP_BUSY_REQ_MAX];
} rtsp_busy_t;

void rtsp_busy_init(rtsp_busy_t *b);
/* Fill p[RTSP_BUSY_NFDS]: p[0] = listen socket, p[1+i] = waiting newcomer i (fd -1 when free: poll ignores it). */
void rtsp_busy_fds(const rtsp_busy_t *b, int listen_fd, struct pollfd *p);
/* After poll(): accept a pending connection (p[0]), read requests, answer and close the ready or expired ones. */
void rtsp_busy_service(rtsp_busy_t *b, int listen_fd, const struct pollfd *p, int64_t now_ms);
/* poll() timeout: max_ms, or less when a waiting newcomer expires sooner (never negative). */
int  rtsp_busy_wait_ms(const rtsp_busy_t *b, int64_t now_ms, int max_ms);
/* End of the session: answer 503 to every waiting newcomer and close it (never a silent close). */
void rtsp_busy_close_all(rtsp_busy_t *b);

/* The 503 answer for a request prefix (CSeq is echoed when found). Returns the length written (< cap), 0 if cap is too small. */
size_t rtsp_busy_reply(const char *req, size_t req_len, char *out, size_t cap);
