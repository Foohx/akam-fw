#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "ctl_gate.h"

int ctl_peer_is_loopback(const struct sockaddr *sa)
{
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        return ((ntohl(a->sin_addr.s_addr) >> 24) & 0xff) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        const unsigned char *b = a->sin6_addr.s6_addr;
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (IN6_IS_ADDR_LOOPBACK(&a->sin6_addr)) return 1;
        return memcmp(b, mapped, 12) == 0 && b[12] == 127;
    }
    return 0;
}

int ctl_write_allowed(int is_loopback, int debug_on)
{
    return is_loopback || debug_on;
}

int ctl_is_write_cmd(const char *cmd)
{
    return cmd && (!strcasecmp(cmd, "SET") || !strcasecmp(cmd, "SAVE") || !strcasecmp(cmd, "RESET"));
}

int ctl_debug_active(ctl_debug_t *d, int64_t now_ms)
{
    if (d->on && now_ms >= d->until_ms) d->on = 0;
    return d->on;
}

int ctl_debug_remaining_s(ctl_debug_t *d, int64_t now_ms)
{
    if (!ctl_debug_active(d, now_ms)) return 0;
    return (int)((d->until_ms - now_ms + 999) / 1000);
}

void ctl_debug_command(ctl_debug_t *d, int64_t now_ms, int is_loopback,
                       const char *arg, const char *arg2, char *out, size_t cap)
{
    if (!is_loopback) {
        snprintf(out, cap, "ERR DEBUG is loopback-only, use akam_debug.sh on the camera");
    } else if (arg && !strcasecmp(arg, "off") && !arg2) {
        d->on = 0;
        snprintf(out, cap, "OK ctl.debug=0");
    } else if (arg && !strcasecmp(arg, "status") && !arg2) {
        if (ctl_debug_active(d, now_ms))
            snprintf(out, cap, "ctl.debug=1 remaining=%d", ctl_debug_remaining_s(d, now_ms));
        else
            snprintf(out, cap, "ctl.debug=0");
    } else if (arg && !strcasecmp(arg, "on")) {
        long min = CTL_DEBUG_DEF_MIN;
        if (arg2) {
            char *end;
            min = strtol(arg2, &end, 10);
            if (*arg2 == '\0' || *end != '\0' || min < 1 || min > CTL_DEBUG_MAX_MIN) {
                snprintf(out, cap, "ERR DEBUG minutes must be 1..%d", CTL_DEBUG_MAX_MIN);
                return;
            }
        }
        d->on = 1;
        d->until_ms = now_ms + min * 60000;
        snprintf(out, cap, "OK ctl.debug=1 minutes=%ld", min);
    } else {
        snprintf(out, cap, "ERR DEBUG on [minutes] | off | status");
    }
}

int ctl_idle_limit_s(int is_loopback, int debug_on)
{
    return (is_loopback || debug_on) ? CTL_IDLE_LOCAL_S : CTL_IDLE_REMOTE_S;
}

int ctl_session_limit_s(int is_loopback, int debug_on)
{
    return (is_loopback || debug_on) ? 0 : CTL_SESSION_REMOTE_S;
}

int ctl_slots_acquire(ctl_slots_t *s, int is_loopback)
{
    int remote = 0;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) remote += s->used[i] == 2;
    if (!is_loopback && remote >= CTL_MAX_REMOTE) return -1;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++)
        if (!s->used[i]) { s->used[i] = is_loopback ? 1 : 2; return i; }
    return -1;
}

void ctl_slots_release(ctl_slots_t *s, int idx)
{
    if (idx >= 0 && idx < CTL_MAX_CLIENTS) s->used[idx] = 0;
}

int ctl_slots_count(const ctl_slots_t *s)
{
    int n = 0;
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) n += s->used[i] ? 1 : 0;
    return n;
}

/* "ak_rtsp ctl <line...>": send one line to the control port on 127.0.0.1 and print the
 * answer (without the LOG lines the port pushes by itself). Used by akam_debug.sh and the
 * other helper scripts, so they do not depend on the BusyBox nc. Runs before any hardware
 * init. Exit 0 when the first reply line is not an ERR, 1 otherwise (also: no connection,
 * no reply). Ends at the "." of a LIST, or 250 ms after the last data once a reply line
 * arrived (1 s while none did), 3 s at most. */
int ctl_client_run(unsigned short port, int argc, char **argv, FILE *out)
{
    char line[512];
    size_t len = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (len + n + 2 > sizeof(line)) { fprintf(stderr, "ctl: line too long\n"); return 1; }
        if (i) line[len++] = ' ';
        memcpy(line + len, argv[i], n);
        len += n;
    }
    line[len++] = '\n';

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("ctl: socket"); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("ctl: connect 127.0.0.1"); return 1; }
    if (write(fd, line, len) != (ssize_t)len) { perror("ctl: write"); return 1; }

    char   buf[1024], pend[1024];
    size_t plen = 0;
    int    first = 1, rc = 1, got_line = 0, done = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long left = 3000 - ((long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) break;
        struct pollfd p = { fd, POLLIN, 0 };
        long gap = got_line ? 250 : 1000;
        if (poll(&p, 1, left > gap ? (int)gap : (int)left) <= 0) break;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') { if (plen < sizeof(pend) - 1) pend[plen++] = buf[i]; continue; }
            pend[plen] = '\0';
            plen = 0;
            if (!strncmp(pend, "LOG ", 4)) continue;
            if (first) { rc = strncmp(pend, "ERR", 3) ? 0 : 1; first = 0; }
            got_line = 1;
            fputs(pend, out), fputc('\n', out);
            if (!strcmp(pend, ".")) done = 1;   /* end of a LIST */
        }
        if (done) break;
    }
    close(fd);
    return rc;
}
