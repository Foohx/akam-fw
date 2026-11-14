#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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
    long quiet_until_ms; /* no waiting for room before this time (a wait just timed out) */
} sink_t;

static long mono_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec;
}

#define SEND_WAIT_MS   300  /* longest wait for room in the syslogd socket for one line */
#define SEND_QUIET_MS 1000  /* after a wait that timed out, lines are dropped without waiting for this long */

static long mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* "<prio>kernel: text" is what busybox klogd sends: syslogd files it under kern.<level>. Never blocks for
 * long and never fails the filter. A full socket (the unix datagram queue holds about 11 messages, a boot
 * dmesg or an oops fills it at once) is waited for with poll(POLLOUT), bounded; when syslogd is stuck for
 * longer, lines are dropped quickly instead of stalling the reader, and the core counts them. Without
 * syslogd the line is lost (as it would be for klogd) and the connection is retried on the next line.
 * Returns 1 when the line was queued to syslogd. */
static int sink_emit(void *user, int prio, const char *text)
{
    sink_t *s = user;
    char buf[LINE_MAX_BYTES + 32];
    int n = snprintf(buf, sizeof(buf), "<%d>kernel: %.*s", prio & 7, LINE_MAX_BYTES, text);
    if (n <= 0) return 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s->fd < 0) {
            int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return 0;
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", s->sock_path);
            if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return 0; }
            s->fd = fd;
        }
        for (;;) {
            if (send(s->fd, buf, (size_t)n, MSG_DONTWAIT) >= 0) return 1;
            if (errno == EINTR) continue;
            if (errno != EAGAIN) break;
            long now = mono_ms();
            if (now < s->quiet_until_ms) return 0; /* syslogd was stuck a moment ago: do not stall again */
            struct pollfd p = { s->fd, POLLOUT, 0 };
            int r = poll(&p, 1, SEND_WAIT_MS);
            if (r > 0) continue;
            if (r < 0 && errno == EINTR) continue;
            s->quiet_until_ms = mono_ms() + SEND_QUIET_MS;
            return 0; /* still full: this line is lost, the socket is fine */
        }
        close(s->fd); /* syslogd restarted (stale socket): reconnect and send this same line once more */
        s->fd = -1;
    }
    return 0;
}

int klog_run(const char *kmsg_path, const char *sock_path, long window_s)
{
    int in = open(kmsg_path, O_RDONLY | O_CLOEXEC);
    if (in < 0) return 1;

    sink_t sink = { sock_path, -1, 0 };
    klog_t k;
    klog_init(&k, sink_emit, &sink, window_s);

    char chunk[2048], line[LINE_MAX_BYTES + 1];
    size_t ll = 0;
    int discard = 0;
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
            if (discard) { /* the rest of a line that was cut at the cap: dropped up to its newline */
                if (c == '\n') discard = 0;
                continue;
            }
            if (c == '\n') {
                if (ll) { line[ll] = '\0'; klog_feed(&k, line, now); }
                ll = 0;
                continue;
            }
            if (ll >= LINE_MAX_BYTES) {
                line[ll] = '\0';
                klog_feed(&k, line, now);
                ll = 0;
                discard = 1;
                continue;
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
    char kmsg_buf[256] = "/proc/kmsg", sock_buf[256] = "/dev/log";
    const char *kmsg = kmsg_buf, *sock = sock_buf;
    long window = KLOG_WINDOW_S;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--kmsg") && i + 1 < argc) snprintf(kmsg_buf, sizeof(kmsg_buf), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc) snprintf(sock_buf, sizeof(sock_buf), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) window = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: ak_rtsp klog [--kmsg PATH] [--sock PATH] [--window SECONDS]\n");
            return 2;
        }
    }
    /* same exe as the streamer: `kill -USR1 $(pidof ak_rtsp)` (frame grab) must not reach this process */
    signal(SIGUSR1, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    rename_process(argv);
    for (int i = 1; i < argc; i++) memset(argv[i], 0, strlen(argv[i])); /* cmdline reads just "akam_klog" */
    return klog_run(kmsg, sock, window);
}
