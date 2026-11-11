/* Host test of rtsp_busy.c (G06): the 503 answer for a second RTSP client, over real loopback sockets. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rtsp_busy.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static int listen_on(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 5) < 0) return -1;
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

static int dial(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* One service pass the way the session loop does it. */
static void pass(rtsp_busy_t *b, int lfd, int timeout_ms)
{
    struct pollfd p[RTSP_BUSY_NFDS];
    rtsp_busy_fds(b, lfd, p);
    int w = rtsp_busy_wait_ms(b, now_ms(), timeout_ms);
    poll(p, RTSP_BUSY_NFDS, w);
    rtsp_busy_service(b, lfd, p, now_ms());
}

/* Run passes until the client has an answer or EOF, at most limit_ms. Returns the answer length. */
static size_t run_until_answer(rtsp_busy_t *b, int lfd, int cfd, char *out, size_t cap, int limit_ms)
{
    size_t n = 0;
    int64_t t0 = now_ms();
    out[0] = '\0';
    while (now_ms() - t0 < limit_ms) {
        pass(b, lfd, 20);
        ssize_t r = recv(cfd, out + n, cap - 1 - n, MSG_DONTWAIT);
        if (r > 0) { n += (size_t)r; out[n] = '\0'; }
        if (r == 0) break;
        if (n && strstr(out, "\r\n\r\n")) break;
    }
    return n;
}

int main(void)
{
    char out[512];

    /* the answer builder */
    {
        const char *req = "OPTIONS rtsp://cam/ RTSP/1.0\r\nCSeq: 7\r\nUser-Agent: x\r\n\r\n";
        size_t n = rtsp_busy_reply(req, strlen(req), out, sizeof(out));
        CHECK(n > 0 && n == strlen(out));
        CHECK(!strncmp(out, "RTSP/1.0 503 Service Unavailable\r\nCSeq: 7\r\n", 42));
        CHECK(strstr(out, "Retry-After: ") && strstr(out, "go2rtc") && strstr(out, "Connection: close\r\n\r\n"));
        const char *r2 = "DESCRIBE rtsp://cam/ RTSP/1.0\r\ncseq:   12\r\n\r\n";
        rtsp_busy_reply(r2, strlen(r2), out, sizeof(out));
        CHECK(strstr(out, "CSeq: 12\r\n") != NULL);
        rtsp_busy_reply("garbage", 7, out, sizeof(out));   /* no CSeq: none invented */
        CHECK(!strncmp(out, "RTSP/1.0 503 Service Unavailable\r\nRetry-After", 44) && !strstr(out, "CSeq"));
        const char *r3 = "X-CSeq: 9\r\n\r\n";                /* only a header at the start of a line counts */
        rtsp_busy_reply(r3, strlen(r3), out, sizeof(out));
        CHECK(!strstr(out, "CSeq"));
        CHECK(rtsp_busy_reply(req, strlen(req), out, 20) == 0); /* too small */
    }

    int port, lfd = listen_on(&port);
    CHECK(lfd >= 0);
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK); /* as main.c does: accept() must never park the video loop */
    rtsp_busy_t b;
    rtsp_busy_init(&b);
    int fds0 = count_fds();

    /* spurious POLLIN (connection reset before accept): returns at once, nothing is kept */
    {
        struct pollfd p[RTSP_BUSY_NFDS];
        rtsp_busy_fds(&b, lfd, p);
        p[0].revents = POLLIN;
        int64_t t0 = now_ms();
        rtsp_busy_service(&b, lfd, p, now_ms());
        CHECK(now_ms() - t0 < 100 && count_fds() == fds0);
    }

    /* a prober that sends OPTIONS gets the 503 at once and is closed */
    {
        int c = dial(port);
        CHECK(c >= 0);
        const char *req = "OPTIONS rtsp://cam/ RTSP/1.0\r\nCSeq: 3\r\n\r\n";
        CHECK(send(c, req, strlen(req), 0) == (ssize_t)strlen(req));
        int64_t t0 = now_ms();
        size_t n = run_until_answer(&b, lfd, c, out, sizeof(out), 2000);
        CHECK(n > 0 && !strncmp(out, "RTSP/1.0 503 ", 13) && strstr(out, "CSeq: 3\r\n"));
        CHECK(now_ms() - t0 < 300);
        /* the server side closed: EOF follows */
        char t[16];
        int eof = 0;
        for (int i = 0; i < 20 && !eof; i++) { pass(&b, lfd, 10); eof = recv(c, t, sizeof(t), MSG_DONTWAIT) == 0; }
        CHECK(eof);
        close(c);
    }
    CHECK(count_fds() == fds0);

    /* a silent client (no request) is answered and closed after RTSP_BUSY_WAIT_MS, never held */
    {
        int c = dial(port);
        int64_t t0 = now_ms();
        size_t n = run_until_answer(&b, lfd, c, out, sizeof(out), 3000);
        int64_t dt = now_ms() - t0;
        CHECK(n > 0 && !strncmp(out, "RTSP/1.0 503 ", 13));
        CHECK(dt >= RTSP_BUSY_WAIT_MS - 50 && dt < RTSP_BUSY_WAIT_MS + 400);
        close(c);
    }
    CHECK(count_fds() == fds0);

    /* a client that connects and leaves is dropped without leaking */
    {
        int c = dial(port);
        close(c);
        for (int i = 0; i < 10; i++) pass(&b, lfd, 10);
    }
    CHECK(count_fds() == fds0);
    CHECK(rtsp_busy_wait_ms(&b, now_ms(), 77) == 77); /* nothing waiting: the caller's timeout is kept */

    /* a flood: at most RTSP_BUSY_MAX wait at once, the rest is closed on accept, no fd is left behind */
    {
        int c[10];
        for (int i = 0; i < 10; i++) { c[i] = dial(port); pass(&b, lfd, 0); } /* the backlog is only 5 deep */
        int64_t t0 = now_ms();
        int answered = 0, closed_silent = 0;
        while (now_ms() - t0 < 1500) pass(&b, lfd, 20);
        for (int i = 0; i < 10; i++) {
            char t[400];
            ssize_t r = recv(c[i], t, sizeof(t), MSG_DONTWAIT);
            if (r > 0 && !strncmp(t, "RTSP/1.0 503", 12)) answered++;
            else if (r == 0) closed_silent++;
            close(c[i]);
        }
        CHECK(answered + closed_silent == 10);
        CHECK(answered >= 3);
    }
    CHECK(count_fds() == fds0);

    /* the busy slots are released at the end of a session */
    {
        int c = dial(port);
        for (int i = 0; i < 3; i++) pass(&b, lfd, 10);
        CHECK(b.fd[0] >= 0);
        rtsp_busy_close_all(&b);
        CHECK(b.fd[0] < 0);
        char t[64] = "";
        ssize_t r = recv(c, t, sizeof(t) - 1, MSG_DONTWAIT);
        CHECK(r > 12 && !strncmp(t, "RTSP/1.0 503", 12)); /* answered, not closed in silence */
        close(c);
    }
    CHECK(count_fds() == fds0);

    /* an invalid listen fd (-1, as in unit builds) is ignored by poll */
    {
        struct pollfd p[RTSP_BUSY_NFDS];
        rtsp_busy_fds(&b, -1, p);
        CHECK(poll(p, RTSP_BUSY_NFDS, 10) == 0);
        rtsp_busy_service(&b, -1, p, now_ms());
    }

    close(lfd);
    if (g_fail) { printf("rtsp_busy_test: %d failure(s)\n", g_fail); return 1; }
    printf("rtsp_busy_test: all passed\n");
    return 0;
}
