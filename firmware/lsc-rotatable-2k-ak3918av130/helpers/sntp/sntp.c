/* akam2k_sntp -- one-shot SNTP client, the "ntpd" of the Rotatable 2K card (BusyBox there has no ntpd applet).
 *
 *   akam2k_sntp [-n] [-q] -p SERVER [-p SERVER ...]
 *
 * Takes the subset of BusyBox ntpd arguments that src/scripts/akam_ntp.sh uses (-n and -q are
 * accepted and implied: foreground, one shot). Tries the servers in order, 3 s each (name resolution counts against
 * a global 40 s alarm), sends one SNTPv4 client request on UDP 123 from an ephemeral port, accepts only a reply that
 * passes sntp_parse_reply (mode 4, stratum 1..15, origin equal to what we sent) from the address we asked, then
 * steps the clock with settimeofday() when it is off by 20 ms or more.
 * Exit 0: the clock is right (stepped, or already within 20 ms). Exit 1: no server gave a usable answer, or the
 * time could not be set. Exit 2: usage. No resident process, no listening socket, no file, no ctype.
 * The camera libc has a 32-bit time_t: NTP seconds are handled in int64 and the target must fit before 2038. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sntp_core.h"

#define MAX_SERVERS 8
#define PER_SERVER_MS 3000
#define GLOBAL_ALARM_S 40
#define STEP_MIN_US 20000

static int64_t now_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return -1;
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* The low 16 bits of the transmit fraction are unpredictable to an off-path sender: a murmur-style mix of the
 * microsecond clock, the monotonic nanosecond clock, the pid and a stack address (ASLR). No file is read. */
static uint32_t rnd32(void)
{
    struct timeval tv;
    struct timespec ts;
    uint32_t v;
    int local;
    gettimeofday(&tv, NULL);
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = (uint32_t)tv.tv_usec ^ ((uint32_t)ts.tv_nsec * 2654435761u) ^ ((uint32_t)getpid() * 40503u) ^
        (uint32_t)(uintptr_t)&local ^ (uint32_t)tv.tv_sec;
    v ^= v >> 16;
    v *= 0x85ebca6bu;
    v ^= v >> 13;
    v *= 0xc2b2ae35u;
    v ^= v >> 16;
    return v;
}

/* Query one address. 0 = clock handled, -1 = try the next one. */
static int query(const struct sockaddr_in *sa, const char *name)
{
    uint8_t out[SNTP_PKT_LEN], in[128];
    struct sntp_reply r;
    struct pollfd p;
    int64_t t1, t2, t3, t4, off, delay, deadline;
    uint64_t sent;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (const struct sockaddr *)sa, sizeof *sa) != 0) { /* also filters replies by source */
        close(fd);
        return -1;
    }
    t1 = now_us();
    if (t1 < 0) {
        close(fd);
        return -1;
    }
    sent = (sntp_us_to_ntp(t1) & ~0xffffull) | (rnd32() & 0xffffu);
    sntp_build_request(out, sent);
    if (send(fd, out, sizeof out, 0) != (ssize_t)sizeof out) {
        close(fd);
        return -1;
    }
    deadline = t1 + (int64_t)PER_SERVER_MS * 1000;
    for (;;) {
        int64_t left = (deadline - now_us()) / 1000;
        ssize_t n;
        enum sntp_err e;
        if (left <= 0) {
            fprintf(stderr, "akam2k_sntp: %s: timeout\n", name);
            break;
        }
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (int)left) <= 0)
            continue;
        n = recv(fd, in, sizeof in, 0);
        if (n < 0)
            continue;
        t4 = now_us();
        e = sntp_parse_reply(in, (size_t)n, sent, &r);
        if (e != SNTP_OK) {
            fprintf(stderr, "akam2k_sntp: %s: reply refused (%d)\n", name, (int)e);
            continue; /* a bad packet does not end the wait: the real reply may still come */
        }
        if (sntp_ntp_to_us(r.rx_raw, &t2) != 0 || sntp_ntp_to_us(r.tx_raw, &t3) != 0 ||
            t3 < SNTP_MIN_UNIX * 1000000) {
            fprintf(stderr, "akam2k_sntp: %s: server time out of range\n", name);
            break;
        }
        sntp_offset(t1, t2, t3, t4, &off, &delay);
        if (delay < 0 || delay > 10 * 1000000LL) {
            fprintf(stderr, "akam2k_sntp: %s: implausible delay\n", name);
            break;
        }
        close(fd);
        if (off > -STEP_MIN_US && off < STEP_MIN_US) {
            printf("akam2k_sntp: %s: offset %lld us, no step\n", name, (long long)off);
            return 0;
        }
        {
            struct timeval tv;
            int32_t sec, usec;
            if (sntp_target(now_us(), off, &sec, &usec) != 0) {
                fprintf(stderr, "akam2k_sntp: %s: target time not representable\n", name);
                return -1;
            }
            tv.tv_sec = sec;
            tv.tv_usec = usec;
            if (settimeofday(&tv, NULL) != 0) {
                fprintf(stderr, "akam2k_sntp: settimeofday: %s\n", strerror(errno));
                return -1;
            }
        }
        printf("akam2k_sntp: %s: stepped by %lld us (stratum %u)\n", name, (long long)off, r.stratum);
        return 0;
    }
    close(fd);
    return -1;
}

static int try_server(const char *name)
{
    struct addrinfo hints, *res, *a;
    int rc = -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(name, "123", &hints, &res) != 0 || !res) {
        fprintf(stderr, "akam2k_sntp: %s: cannot resolve\n", name);
        return -1;
    }
    for (a = res; a && rc != 0; a = a->ai_next)
        if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(struct sockaddr_in))
            rc = query((const struct sockaddr_in *)(const void *)a->ai_addr, name);
    freeaddrinfo(res);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: akam2k_sntp [-n] [-q] -p SERVER [-p SERVER ...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *srv[MAX_SERVERS];
    int ns = 0, i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-q"))
            continue;
        if (!strcmp(argv[i], "-p") && i + 1 < argc && ns < MAX_SERVERS && argv[i + 1][0] != '\0' &&
            argv[i + 1][0] != '-')
            srv[ns++] = argv[++i];
        else
            return usage();
    }
    if (ns == 0)
        return usage();
    alarm(GLOBAL_ALARM_S);
    for (i = 0; i < ns; i++)
        if (try_server(srv[i]) == 0)
            return 0;
    return 1;
}
