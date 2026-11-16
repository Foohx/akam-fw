/* Host loopback test of the Rotatable 2K RTSP server (akam2k_rtsp.c, REAL server and SmolRTSP, hardware modules stubbed):
 * /stream0 and /stream1 are served, any other path is 404, and the client cap holds. The port is a free loopback one
 * (554 needs root); the default constant is checked below. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "akam2k_aring.h"
#include "akam2k_audio.h"
#include "akam2k_cap.h"
#include "akam2k_log.h"
#include "akam2k_ptz.h"
#include "akam2k_rtsp.h"

volatile sig_atomic_t a2k_stop;
void a2k_log(const char *fmt, ...) { (void)fmt; }
int a2k_audio_up(void) { return 0; }
a2k_aring_t *a2k_audio_ring(void) { return NULL; }
int a2k_ptz_privacy_active(void) { return 0; }

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int free_port(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t l = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), p;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0)
        return 0;
    getsockname(fd, (struct sockaddr *)&a, &l);
    p = ntohs(a.sin_port);
    close(fd);
    return p;
}

static int dial(int port)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct timeval tv = { 0, 400000 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int rtsp(int fd, const char *method, int port, const char *path, int cseq)
{
    char req[256], rsp[2048];
    ssize_t n = 0;
    int st = -1;
    snprintf(req, sizeof req, "%s rtsp://127.0.0.1:%d%s RTSP/1.0\r\nCSeq: %d\r\n\r\n", method, port, path, cseq);
    size_t got = 0;
    if (write(fd, req, strlen(req)) < 0 && errno != EPIPE && errno != ECONNRESET)
        return -1;
    /* SmolRTSP writes an answer in several small pieces: collect until the line goes quiet (0.4 s) or closes. */
    while (got < sizeof rsp - 1 && (n = read(fd, rsp + got, sizeof rsp - 1 - got)) > 0)
        got += (size_t)n;
    if (got == 0)
        return -1;
    rsp[got] = 0;
    sscanf(rsp, "RTSP/1.0 %d", &st);
    return st;
}

/* Admitted: OPTIONS answers 200 (returns the fd). Over the cap: 503 at once (returns -503). */
static int admitted(int port)
{
    int fd = dial(port), st;
    if (fd < 0)
        return -1;
    st = rtsp(fd, "OPTIONS", port, "/stream0", 1);
    if (st != 200)
        close(fd);
    return st == 200 ? fd : -st;
}

int main(void)
{
    static a2k_stream_t m, s;
    a2k_stream_t *sp[2] = { &m, &s };
    static const uint8_t au[] = { 0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x28, 0xAB, 0x40, 0x3C, 0x01, 0, 0, 1, 0x68, 0xEE, 0x3C, 0x80,
                                  0, 0, 0, 1, 0x65, 0x80, 0x81, 0x82 };
    int port = free_port(), i, fd, fds[8], n = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(A2K_RTSP_PORT == 554, "the default RTSP port constant is 554, the same URL as the 1080p");
    a2k_stream_init(&m, "stream0", 2304, 1296, 15, 30, 2000, A2K_MAX_FRAME_MAIN);
    a2k_stream_init(&s, "stream1", 640, 360, 15, 30, 512, A2K_MAX_FRAME_SUB);
    a2k_stream_publish(&m, au, sizeof au);
    a2k_stream_publish(&s, au, sizeof au);

    CHECK(port > 0 && a2k_rtsp_start(port, "/nonexistent/rtsp.conf", sp) == 0, "start: one listener");
    fd = dial(port);
    CHECK(fd >= 0, "the port accepts a connection");
    if (fd >= 0) {
        CHECK(rtsp(fd, "DESCRIBE", port, "/stream0", 1) == 200, "DESCRIBE /stream0 is 200");
        CHECK(rtsp(fd, "DESCRIBE", port, "/stream1", 2) == 200, "DESCRIBE /stream1 is 200");
        CHECK(rtsp(fd, "DESCRIBE", port, "/stream2", 3) == 404, "DESCRIBE /stream2 is 404 (strict paths)");
        CHECK(rtsp(fd, "DESCRIBE", port, "/", 4) == 404, "DESCRIBE / is 404 (strict paths)");
        close(fd);
    }
    usleep(300000);
    CHECK(a2k_rtsp_clients() == 0, "the probe client is gone");
    for (i = 0; i < A2K_RTSP_MAX_CLIENTS; i++) {
        fd = admitted(port);
        CHECK(fd >= 0, "client within the cap is admitted");
        if (fd >= 0)
            fds[n++] = fd;
    }
    CHECK(admitted(port) == -503, "over the cap: 503");
    for (i = 0; i < n; i++)
        close(fds[i]);

    a2k_stop = 1;
    a2k_rtsp_stop();
    CHECK(dial(port) < 0, "stop closes the listener");
    printf("rot2k rtsp ports unit: %d failed\n", fails);
    return fails != 0;
}
