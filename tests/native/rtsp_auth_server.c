/* Minimal RTSP server for tests/rtsp_auth_test.sh: it runs the real
 * rtsp_auth.c state machine (credentials file given as argv[1]) in front of a
 * canned SDP, so that a real Digest client (ffprobe) can be checked against it.
 * Not part of the firmware. Prints "PORT <n>" first, then one line per request:
 *   "<METHOD> <status>"   Usage: rtsp_auth_server FILE */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "rtsp_auth.h"

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void send_all(int fd, const char *s)
{
    size_t n = strlen(s), o = 0;
    while (o < n) {
        ssize_t r = write(fd, s + o, n - o);
        if (r <= 0) return;
        o += (size_t)r;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    rtsp_auth_db_t db;
    rtsp_auth_db_init(&db, argv[1]);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) return 1;
    getsockname(ls, (struct sockaddr *)&a, &al);
    printf("PORT %d\n", ntohs(a.sin_port));
    fflush(stdout);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        rtsp_auth_conn_t conn;
        rtsp_auth_conn_init(&conn);
        char buf[8192];
        size_t have = 0;
        for (;;) {
            char *end;
            while (!(end = memmem(buf, have, "\r\n\r\n", 4))) {
                ssize_t r = read(fd, buf + have, sizeof(buf) - 1 - have);
                if (r <= 0) goto closed;
                have += (size_t)r;
            }
            size_t mlen = (size_t)(end - buf) + 4;
            char msg[8192];
            memcpy(msg, buf, mlen);
            msg[mlen] = '\0';
            memmove(buf, buf + mlen, have - mlen);
            have -= mlen;

            char method[32] = "", uri[512] = "";
            sscanf(msg, "%31s %511s", method, uri);
            long cseq = 0;
            const char *auth = NULL;
            size_t alen = 0;
            for (char *l = strstr(msg, "\r\n"); l && l[2]; l = strstr(l + 2, "\r\n")) {
                if (strncasecmp(l + 2, "CSeq:", 5) == 0) cseq = atol(l + 7);
                if (strncasecmp(l + 2, "Authorization:", 14) == 0) {
                    auth = l + 16;
                    alen = strcspn(auth, "\r");
                }
            }
            rtsp_auth_result_t r;
            rtsp_auth_check(&db, &conn, method, auth, alen, mono_us(), &r);
            if (r.log[0]) fprintf(stderr, "log: %s\n", r.log);
            char out[2048];
            int code;
            if (r.verdict != RTSP_AUTH_ALLOW) {
                code = 401;
                snprintf(out, sizeof(out), "RTSP/1.0 401 Unauthorized\r\nCSeq: %ld\r\n%s%s%s\r\n", cseq,
                         r.challenge[0] ? "WWW-Authenticate: " : "", r.challenge, r.challenge[0] ? "\r\n" : "");
            } else if (strcmp(method, "OPTIONS") == 0) {
                code = 200;
                snprintf(out, sizeof(out),
                         "RTSP/1.0 200 OK\r\nCSeq: %ld\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n", cseq);
            } else if (strcmp(method, "DESCRIBE") == 0) {
                static const char sdp[] = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=t\r\nt=0 0\r\n"
                    "m=video 0 RTP/AVP 96\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:96 H264/90000\r\n"
                    "a=control:streamid=0\r\n";
                code = 200;
                snprintf(out, sizeof(out),
                         "RTSP/1.0 200 OK\r\nCSeq: %ld\r\nContent-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
                         cseq, strlen(sdp), sdp);
            } else {
                code = 461;
                snprintf(out, sizeof(out), "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %ld\r\n\r\n", cseq);
            }
            printf("%s %d\n", method, code);
            fflush(stdout);
            if (r.delay_ms) usleep(r.delay_ms * 1000);
            send_all(fd, out);
            if (r.verdict == RTSP_AUTH_DROP) goto closed;
        }
closed:
        close(fd);
    }
}
