/* See rtsp_busy.h. */
#include "rtsp_busy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

void rtsp_busy_init(rtsp_busy_t *b)
{
    memset(b, 0, sizeof(*b));
    for (int i = 0; i < RTSP_BUSY_MAX; i++) b->fd[i] = -1;
}

void rtsp_busy_fds(const rtsp_busy_t *b, int listen_fd, struct pollfd *p)
{
    p[0].fd = listen_fd;
    p[0].events = POLLIN;
    p[0].revents = 0;
    for (int i = 0; i < RTSP_BUSY_MAX; i++) {
        p[1 + i].fd = b->fd[i];
        p[1 + i].events = POLLIN;
        p[1 + i].revents = 0;
    }
}

size_t rtsp_busy_reply(const char *req, size_t req_len, char *out, size_t cap)
{
    char cseq[16] = "";
    for (size_t i = 0; i + 5 < req_len; i++) {
        if ((i == 0 || req[i - 1] == '\n') && strncasecmp(req + i, "CSeq:", 5) == 0) {
            size_t j = i + 5, k = 0;
            while (j < req_len && (req[j] == ' ' || req[j] == '\t')) j++;
            while (j < req_len && req[j] >= '0' && req[j] <= '9' && k < sizeof(cseq) - 1) cseq[k++] = req[j++];
            cseq[k] = '\0';
            break;
        }
    }
    int n = snprintf(out, cap,
                     "RTSP/1.0 503 Service Unavailable\r\n"
                     "%s%s%s"
                     "Retry-After: 10\r\n"
                     "Reason: one viewer at a time, restream with go2rtc for several\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     cseq[0] ? "CSeq: " : "", cseq, cseq[0] ? "\r\n" : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void answer_and_close(rtsp_busy_t *b, int i)
{
    char out[320], junk[256];
    size_t n = rtsp_busy_reply(b->buf[i], b->len[i], out, sizeof(out));
    if (n) (void)send(b->fd[i], out, n, MSG_DONTWAIT | MSG_NOSIGNAL); /* ~200 bytes into an empty send buffer */
    (void)recv(b->fd[i], junk, sizeof(junk), MSG_DONTWAIT);           /* leave no unread bytes: close() would RST */
    close(b->fd[i]);
    b->fd[i] = -1;
    b->len[i] = 0;
}

static int request_complete(const char *s, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') return 1;
    return 0;
}

void rtsp_busy_service(rtsp_busy_t *b, int listen_fd, const struct pollfd *p, int64_t now_ms)
{
    for (int i = 0; i < RTSP_BUSY_MAX; i++) {
        if (b->fd[i] < 0) continue;
        int done = 0;
        if (p[1 + i].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = recv(b->fd[i], b->buf[i] + b->len[i], RTSP_BUSY_REQ_MAX - b->len[i], MSG_DONTWAIT);
            if (r > 0) {
                b->len[i] += (size_t)r;
                done = b->len[i] >= RTSP_BUSY_REQ_MAX || request_complete(b->buf[i], b->len[i]);
            } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                close(b->fd[i]); /* the newcomer left: nothing to answer */
                b->fd[i] = -1;
                b->len[i] = 0;
                continue;
            }
        }
        if (done || now_ms - b->t0_ms[i] >= RTSP_BUSY_WAIT_MS) answer_and_close(b, i);
    }
    if (p[0].revents & POLLIN) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < RTSP_BUSY_MAX; i++)
            if (b->fd[i] < 0) { slot = i; break; }
        if (slot < 0) { close(fd); return; } /* flood: refuse without reading */
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        b->fd[slot] = fd;
        b->len[slot] = 0;
        b->t0_ms[slot] = now_ms;
    }
}

int rtsp_busy_wait_ms(const rtsp_busy_t *b, int64_t now_ms, int max_ms)
{
    int w = max_ms;
    for (int i = 0; i < RTSP_BUSY_MAX; i++) {
        if (b->fd[i] < 0) continue;
        int64_t left = RTSP_BUSY_WAIT_MS - (now_ms - b->t0_ms[i]);
        if (left < 0) left = 0;
        if (left < w) w = (int)left;
    }
    return w;
}

void rtsp_busy_close_all(rtsp_busy_t *b)
{
    for (int i = 0; i < RTSP_BUSY_MAX; i++) {
        if (b->fd[i] >= 0) close(b->fd[i]);
        b->fd[i] = -1;
        b->len[i] = 0;
    }
}
