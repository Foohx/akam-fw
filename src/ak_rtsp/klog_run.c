#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "klog.h"

/* I/O half of the kernel log filter, see klog.h. Idle cost: one blocked poll() on /proc/kmsg, no timer
 * unless a summary is pending (then the poll timeout is the time until it is due). */

#define LINE_MAX_BYTES 1000 /* one syslog datagram; longer kernel lines are cut */

typedef struct {
    const char *sock_path;
    int fd; /* datagram socket to syslogd, -1 when not connected */
} sink_t;

static long mono_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec;
}

/* "<prio>kernel: text" is what busybox klogd sends: syslogd files it under kern.<level>. Never blocks and
 * never fails the filter: without syslogd the line is lost (as it would be for klogd) and the connection is
 * retried on the next line. */
static void sink_emit(void *user, int prio, const char *text)
{
    sink_t *s = user;
    if (s->fd < 0) {
        int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", s->sock_path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return; }
        s->fd = fd;
    }
    char buf[LINE_MAX_BYTES + 32];
    int n = snprintf(buf, sizeof(buf), "<%d>kernel: %.*s", prio & 7, LINE_MAX_BYTES, text);
    if (n <= 0) return;
    if (send(s->fd, buf, (size_t)n, MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != EINTR) {
        close(s->fd); /* syslogd restarted or gone: reconnect on the next line */
        s->fd = -1;
    }
}

int klog_run(const char *kmsg_path, const char *sock_path, long window_s)
{
    int in = open(kmsg_path, O_RDONLY | O_CLOEXEC);
    if (in < 0) return 1;

    sink_t sink = { sock_path, -1 };
    klog_t k;
    klog_init(&k, sink_emit, &sink, window_s);

    char chunk[2048], line[LINE_MAX_BYTES + 1];
    size_t ll = 0;
    int rc = 0;
    for (;;) {
        long due = klog_next_due(&k, mono_s());
        struct pollfd p = { in, POLLIN, 0 };
        int r = poll(&p, 1, due < 0 ? -1 : (int)(due > 3600 ? 3600 : due) * 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            rc = 1;
            break;
        }
        if (r == 0) { klog_tick(&k, mono_s()); continue; }
        ssize_t n = read(in, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            rc = 1;
            break;
        }
        if (n == 0) { /* end of a regular file (tests); /proc/kmsg never ends */
            if (ll) { line[ll] = '\0'; klog_feed(&k, line, mono_s()); }
            klog_flush(&k);
            break;
        }
        long now = mono_s();
        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || ll >= LINE_MAX_BYTES) {
                if (ll) { line[ll] = '\0'; klog_feed(&k, line, now); }
                ll = 0;
                if (c == '\n') continue;
            }
            if (c != '\r') line[ll++] = c;
        }
        klog_tick(&k, now);
    }
    close(in);
    if (sink.fd >= 0) close(sink.fd);
    return rc;
}

/* The process shows up as "akam_klog" in ps and /proc/PID/cmdline instead of a second "ak_rtsp", so the
 * health checks that look for ak_rtsp (CPU, pid) keep seeing one process. */
static void rename_process(char **argv)
{
    static const char name[] = "akam_klog";
    size_t room = strlen(argv[0]);
    if (room >= sizeof(name) - 1) {
        memset(argv[0], 0, room);
        memcpy(argv[0], name, sizeof(name) - 1);
    }
    (void)prctl(PR_SET_NAME, (unsigned long)"akam_klog", 0, 0, 0);
}

int klog_main(int argc, char **argv)
{
    const char *kmsg = "/proc/kmsg", *sock = "/dev/log";
    long window = KLOG_WINDOW_S;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--kmsg") && i + 1 < argc) kmsg = argv[++i];
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc) sock = argv[++i];
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) window = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: ak_rtsp klog [--kmsg PATH] [--sock PATH] [--window SECONDS]\n");
            return 2;
        }
    }
    rename_process(argv);
    return klog_run(kmsg, sock, window);
}
