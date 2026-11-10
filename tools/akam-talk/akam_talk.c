/* akam-talk: send audio to the speaker of an Akam camera over its RTSP port, with the
 * ONVIF audio backchannel (specs/07-spec-standard-protocols.md section 1).
 *
 *   akam-talk [options] rtsp://USER@HOST[:PORT]/PATH
 *
 * One TCP connection, like go2rtc: DESCRIBE with "Require: www.onvif.org/ver20/backchannel"
 * (Digest login), SETUP of the send-only audio track (RTP interleaved in the RTSP
 * connection), PLAY, then G.711 RTP packets every 20 ms, then RTCP BYE and TEARDOWN.
 * Audio comes from stdin or a file (raw 8 kHz mono signed 16-bit little-endian, a WAV
 * file with that format, or already G.711), or from a generated test tone.
 *
 * The password is read from the environment (AKAM_RTSP_PASSWORD) or from a file
 * (--password-file); it is never taken from the command line and never printed.
 *
 * Portable C99 (Linux, macOS, Windows through winsock); MIT licence like the rest of
 * Akam; reuses md5.c and g711.c of the camera code. Exit codes: 0 done, 1 usage,
 * 2 network, 3 the camera refuses talk (551 or no backchannel in the SDP),
 * 4 login refused, 5 protocol error. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define sock_errno() WSAGetLastError()
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static int64_t now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart * 1000 / f.QuadPart);
}
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#define sock_errno() errno
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#endif

#include "g711.h"
#include "md5.h"

#define BACKCHANNEL_TAG "www.onvif.org/ver20/backchannel"
#define PTIME_MS 20
#define SAMPLES_PER_PACKET (8 * PTIME_MS) /* 8 kHz */
#define KEEPALIVE_MS 25000
#define RX_CAP 16384

enum { EXIT_OK = 0, EXIT_USAGE = 1, EXIT_NET = 2, EXIT_REFUSED = 3, EXIT_LOGIN = 4, EXIT_PROTO = 5 };

static volatile sig_atomic_t g_stop;
static int g_verbose;

static void on_signal(int s) { (void)s; g_stop = 1; }

static void vlog(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("akam-talk: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int fail(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("akam-talk: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return code;
}

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

/* strcasestr is not portable (missing on Windows): a small own version. */
static const char *ci_find(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl) return h;
    for (; *h; h++)
        if (!strncasecmp(h, n, nl)) return h;
    return NULL;
}

/* ---------------------------------------------------------------- connection */

typedef struct {
    sock_t fd;
    char host[256];
    char port[8];
    char base[512];        /* rtsp://host:port/path, no credentials */
    char user[64];
    char pass[128];
    char nonce[128];
    char realm[64];
    int  qop_auth;
    unsigned nc;
    char session[64];
    int  cseq;
    char rx[RX_CAP];
    size_t rxn;
} conn_t;

static int send_all(sock_t fd, const void *p, size_t n)
{
    const char *s = p;
    while (n) {
#ifdef _WIN32
        int r = send(fd, s, (int)n, 0);
#else
        ssize_t r = send(fd, s, n, 0
#ifdef MSG_NOSIGNAL
                         | MSG_NOSIGNAL
#endif
                         );
#endif
        if (r <= 0) return -1;
        s += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Waits for data: 1 readable, 0 timeout, -1 error. */
static int wait_readable(sock_t fd, int ms)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select((int)fd + 1, &rs, NULL, NULL, &tv);
    return r < 0 ? -1 : r;
}

static int rx_more(conn_t *c, int ms)
{
    if (c->rxn >= sizeof(c->rx)) return -1;
    int w = wait_readable(c->fd, ms);
    if (w <= 0) return w;
#ifdef _WIN32
    int r = recv(c->fd, c->rx + c->rxn, (int)(sizeof(c->rx) - c->rxn), 0);
#else
    ssize_t r = recv(c->fd, c->rx + c->rxn, sizeof(c->rx) - c->rxn, 0);
#endif
    if (r <= 0) return -1;
    c->rxn += (size_t)r;
    return 1;
}

static void rx_drop(conn_t *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rxn - n);
    c->rxn -= n;
}

static const char *find_hdr_end(const char *b, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return b + i;
    return NULL;
}

static int is_status_line(const char *b, size_t n) { return n >= 5 && !memcmp(b, "RTSP/", 5); }

/* Takes the next complete item from the receive buffer: an interleaved frame
 * (discarded) or a response (copied to out). Returns 1 for a response, 0 when
 * more bytes are needed, -1 on garbage. */
static int rx_take_response(conn_t *c, char *out, size_t cap)
{
    for (;;) {
        while (c->rxn && (c->rx[0] == '\r' || c->rx[0] == '\n')) rx_drop(c, 1);
        if (!c->rxn) return 0;
        if (c->rx[0] == '$') {
            if (c->rxn < 4) return 0;
            size_t len = ((size_t)(uint8_t)c->rx[2] << 8) | (uint8_t)c->rx[3];
            if (c->rxn < 4 + len) return 0;
            rx_drop(c, 4 + len);
            continue;
        }
        if (c->rxn >= 5 && !is_status_line(c->rx, c->rxn)) return -1;
        if (c->rxn < 5) return 0;
        const char *he = find_hdr_end(c->rx, c->rxn);
        if (!he) return c->rxn >= sizeof(c->rx) ? -1 : 0;
        size_t hl = (size_t)(he - c->rx) + 4, bl = 0;
        for (size_t i = 0; i + 15 < hl; i++)
            if ((i == 0 || c->rx[i - 1] == '\n') && !strncasecmp(c->rx + i, "Content-Length:", 15)) {
                bl = (size_t)strtoul(c->rx + i + 15, NULL, 10);
                break;
            }
        if (hl + bl > sizeof(c->rx)) return -1;
        if (c->rxn < hl + bl) return 0;
        size_t total = hl + bl < cap - 1 ? hl + bl : cap - 1;
        memcpy(out, c->rx, total);
        out[total] = '\0';
        rx_drop(c, hl + bl);
        return 1;
    }
}

static int status_of(const char *resp)
{
    int code = 0;
    return sscanf(resp, "RTSP/%*d.%*d %d", &code) == 1 ? code : -1;
}

/* Value of a response header (case-insensitive name), or NULL. */
static const char *header(const char *resp, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = strchr(resp, '\n');
    const char *end = strstr(resp, "\r\n\r\n");
    while (p && p < (end ? end : resp + strlen(resp))) {
        p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strcspn(p, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return out;
        }
        p = strchr(p, '\n');
    }
    return NULL;
}

/* ---------------------------------------------------------------- Digest */

static int param(const char *h, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = h; (p = ci_find(p, key)); p += kl) {
        if (p != h && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (p[kl] != '=') continue;
        p += kl + 1;
        int q = (*p == '"');
        if (q) p++;
        size_t n = q ? strcspn(p, "\"") : strcspn(p, ", \t\r\n");
        if (n >= cap) n = cap - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

static void parse_challenge(conn_t *c, const char *resp)
{
    char h[512];
    if (!header(resp, "WWW-Authenticate", h, sizeof(h))) return;
    if (strncasecmp(h, "Digest", 6) != 0) return;
    param(h, "realm", c->realm, sizeof(c->realm));
    param(h, "nonce", c->nonce, sizeof(c->nonce));
    char q[32] = "";
    c->qop_auth = param(h, "qop", q, sizeof(q)) && strstr(q, "auth") != NULL;
    c->nc = 0;
}

/* Authorization header line (with CRLF), or "" before the first challenge. */
static void auth_line(conn_t *c, const char *method, const char *uri, char *out, size_t cap)
{
    out[0] = '\0';
    if (!c->nonce[0]) return;
    char ha1[33], resp[33], nc[9], cn[17];
    md5_hex_cat(ha1, c->user, ":", c->realm, ":", c->pass, (char *)NULL);
    if (c->qop_auth) {
        snprintf(nc, sizeof(nc), "%08x", ++c->nc);
        snprintf(cn, sizeof(cn), "%08x%08x", (unsigned)now_ms() * 2654435761u, (unsigned)rand());
        char ha2[33];
        md5_hex_cat(ha2, method, ":", uri, (char *)NULL);
        md5_hex_cat(resp, ha1, ":", c->nonce, ":", nc, ":", cn, ":", "auth", ":", ha2, (char *)NULL);
        snprintf(out, cap, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", qop=auth, nc=%s, cnonce=\"%s\"\r\n", c->user, c->realm, c->nonce, uri, resp, nc, cn);
    } else {
        char ha2[33];
        md5_hex_cat(ha2, method, ":", uri, (char *)NULL);
        md5_hex_cat(resp, ha1, ":", c->nonce, ":", ha2, (char *)NULL);
        snprintf(out, cap, "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\"\r\n", c->user, c->realm, c->nonce, uri, resp);
    }
}

/* One request/response, answering a 401 once. extra: header lines with CRLF. */
static int request(conn_t *c, const char *method, const char *uri, const char *extra, char *resp, size_t cap)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        char ah[640], req[2048];
        auth_line(c, method, uri, ah, sizeof(ah));
        int n = snprintf(req, sizeof(req), "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: akam-talk\r\n", method, uri, ++c->cseq);
        if (c->session[0]) n += snprintf(req + n, sizeof(req) - (size_t)n, "Session: %s\r\n", c->session);
        n += snprintf(req + n, sizeof(req) - (size_t)n, "%s%s\r\n", ah, extra ? extra : "");
        if (n <= 0 || (size_t)n >= sizeof(req)) return -1;
        if (send_all(c->fd, req, (size_t)n) < 0) return -1;
        int64_t deadline = now_ms() + 8000;
        for (;;) {
            int t = rx_take_response(c, resp, cap);
            if (t < 0) return -1;
            if (t > 0) break;
            if (now_ms() > deadline) return -1;
            if (rx_more(c, 200) < 0) return -1;
        }
        int code = status_of(resp);
        if (code == 401 && attempt == 0 && c->pass[0]) {
            parse_challenge(c, resp);
            if (c->nonce[0]) continue;
        }
        return code;
    }
    return 401;
}

/* ---------------------------------------------------------------- URL, connection */

static int parse_url(conn_t *c, const char *url)
{
    if (strncasecmp(url, "rtsp://", 7) != 0) return -1;
    const char *p = url + 7;
    const char *slash = p + strcspn(p, "/");
    const char *at = NULL;
    for (const char *q = p; q < slash; q++) if (*q == '@') at = q;
    const char *host = p;
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        size_t ul = (size_t)((colon ? colon : at) - p);
        if (ul >= sizeof(c->user)) return -1;
        memcpy(c->user, p, ul);
        c->user[ul] = '\0';
        if (colon) {
            size_t pl = (size_t)(at - colon - 1);
            if (pl >= sizeof(c->pass)) return -1;
            memcpy(c->pass, colon + 1, pl);
            c->pass[pl] = '\0';
            fprintf(stderr, "akam-talk: a password in the URL is visible to other users of this computer; "
                    "prefer AKAM_RTSP_PASSWORD\n");
        }
        host = at + 1;
    }
    const char *hend = host + strcspn(host, ":/");
    if (host[0] == '[') { /* [v6]:port */
        hend = strchr(host, ']');
        if (!hend) return -1;
        hend++;
    }
    size_t hl = (size_t)(hend - (host[0] == '[' ? host + 1 : host)) - (host[0] == '[' ? 1 : 0);
    const char *hs = host[0] == '[' ? host + 1 : host;
    if (hl == 0 || hl >= sizeof(c->host)) return -1;
    memcpy(c->host, hs, hl);
    c->host[hl] = '\0';
    snprintf(c->port, sizeof(c->port), "554");
    if (*hend == ':') {
        size_t pl = strcspn(hend + 1, "/");
        if (pl == 0 || pl >= sizeof(c->port)) return -1;
        memcpy(c->port, hend + 1, pl);
        c->port[pl] = '\0';
    }
    const char *path = slash;
    if (host[0] == '[')
        snprintf(c->base, sizeof(c->base), "rtsp://[%s]:%s%s", c->host, c->port, path);
    else
        snprintf(c->base, sizeof(c->base), "rtsp://%s:%s%s", c->host, c->port, path);
    size_t bl = strlen(c->base);
    while (bl > 7 && c->base[bl - 1] == '/') c->base[--bl] = '\0';
    return 0;
}

static int connect_to(conn_t *c)
{
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c->host, c->port, &hints, &res) != 0) return -1;
    c->fd = SOCK_BAD;
    for (ai = res; ai; ai = ai->ai_next) {
        c->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->fd == SOCK_BAD) continue;
        if (connect(c->fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        sock_close(c->fd);
        c->fd = SOCK_BAD;
    }
    freeaddrinfo(res);
    if (c->fd == SOCK_BAD) return -1;
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return 0;
}

/* ---------------------------------------------------------------- SDP */

/* Finds the backchannel media section: m=audio with a=sendonly. Returns 1 and
 * fills control (the a=control value) and the offered payload types. */
static int sdp_backchannel(const char *sdp, char *control, size_t cap, int *has_pcmu, int *has_pcma)
{
    const char *p = sdp;
    while ((p = strstr(p, "m=audio")) != NULL) {
        const char *next = strstr(p + 7, "\nm=");
        size_t len = next ? (size_t)(next - p) : strlen(p);
        char *sec = malloc(len + 1);
        if (!sec) return 0;
        memcpy(sec, p, len);
        sec[len] = '\0';
        int found = 0;
        if (strstr(sec, "a=sendonly")) {
            const char *cp = strstr(sec, "a=control:");
            if (cp) {
                cp += 10;
                size_t n = strcspn(cp, "\r\n");
                if (n >= cap) n = cap - 1;
                memcpy(control, cp, n);
                control[n] = '\0';
                *has_pcmu = strstr(sec, "PCMU/8000") != NULL;
                *has_pcma = strstr(sec, "PCMA/8000") != NULL;
                found = 1;
            }
        }
        free(sec);
        if (found) return 1;
        if (!next) break;
        p = next + 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- audio source */

typedef enum { FMT_AUTO, FMT_S16LE, FMT_MULAW, FMT_ALAW, FMT_WAV } fmt_t;

typedef struct {
    FILE *f;
    fmt_t fmt;
    int tone;          /* generated: samples left, or -1 for none */
    long tone_left;
    unsigned tone_pos;
    long wav_left;     /* bytes of audio data left in a WAV file */
    uint8_t peek[12];  /* bytes read to recognise the format, served first */
    size_t peek_n, peek_pos;
} src_t;

static size_t src_read(src_t *s, void *buf, size_t n)
{
    uint8_t *o = buf;
    size_t got = 0;
    while (got < n && s->peek_pos < s->peek_n) o[got++] = s->peek[s->peek_pos++];
    if (got < n) got += fread(o + got, 1, n - got, s->f);
    return got;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Skips a WAV header; 0 = ok (8 kHz mono 16-bit PCM), -1 = another format. */
static int wav_open(src_t *s, const uint8_t *h)
{
    if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return -1;
    int got_fmt = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, s->f) != 8) return -1;
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[16];
            if (len < 16 || fread(f, 1, 16, s->f) != 16) return -1;
            if (le16(f) != 1 || le16(f + 2) != 1 || le32(f + 4) != 8000 || le16(f + 14) != 16) return -2;
            got_fmt = 1;
            for (uint32_t k = 16; k < len + (len & 1); k++) if (fgetc(s->f) == EOF) return -1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!got_fmt) return -1;
            s->wav_left = (long)(len == 0 || len == 0xffffffffu || len > 0x7fffffffu ? 0x7fffffff : len); /* streamed WAV: until EOF */
            return 0;
        } else {
            for (uint32_t k = 0; k < len + (len & 1); k++) if (fgetc(s->f) == EOF) return -1;
        }
    }
}

/* Next 20 ms as G.711 bytes in the chosen codec. Returns the byte count (0 at the end). */
static size_t next_packet(src_t *s, int pcmu, uint8_t *out)
{
    if (s->tone >= 0) {
        /* 1 kHz at 8 kHz repeats every 8 samples; -16 dBFS like the camera's own beep */
        static const int16_t k[8] = { 0, 3673, 5194, 3673, 0, -3673, -5194, -3673 };
        size_t n = 0;
        while (n < SAMPLES_PER_PACKET && s->tone_left > 0) {
            int16_t v = k[s->tone_pos++ & 7];
            out[n++] = pcmu ? g711_ulaw_encode(v) : g711_alaw_encode(v);
            s->tone_left--;
        }
        return n;
    }
    if (s->fmt == FMT_MULAW || s->fmt == FMT_ALAW) {
        return src_read(s, out, SAMPLES_PER_PACKET);
    }
    uint8_t raw[SAMPLES_PER_PACKET * 2];
    size_t want = sizeof(raw);
    if (s->fmt == FMT_WAV && (long)want > s->wav_left) want = (size_t)s->wav_left & ~(size_t)1;
    size_t got = want ? src_read(s, raw, want) : 0;
    if (s->fmt == FMT_WAV) s->wav_left -= (long)got;
    size_t n = got / 2;
    for (size_t i = 0; i < n; i++) {
        int16_t v = (int16_t)le16(raw + 2 * i);
        out[i] = pcmu ? g711_ulaw_encode(v) : g711_alaw_encode(v);
    }
    return n;
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
    fputs("usage: akam-talk [options] rtsp://USER@HOST[:PORT]/PATH\n"
          "  Sends audio to the speaker of the camera (ONVIF audio backchannel).\n"
          "  Password: AKAM_RTSP_PASSWORD in the environment, or --password-file FILE.\n"
          "  User: in the address, or --user NAME, or AKAM_RTSP_USER (default akam).\n"
          "  --input FILE|-       audio source (default: standard input)\n"
          "  --format F           auto (default: WAV if it starts with RIFF, else s16le), s16le, wav, mulaw, alaw\n"
          "                       s16le/wav: 8 kHz mono signed 16-bit; mulaw/alaw: already G.711 at 8 kHz\n"
          "  --tone [SECONDS]     send a 1 kHz test tone instead (default 2 s)\n"
          "  --codec pcmu|pcma    G.711 flavour to send (default pcmu, when the camera offers it)\n"
          "  --password-file F    read the password from the first line of F\n"
          "  --realtime-off       do not pace to real time (for tests only)\n"
          "  -v                   explain what happens on standard error\n", stderr);
}

static int read_password_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *r = fgets(out, (int)cap, f);
    fclose(f);
    if (!r) return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

int main(int argc, char **argv)
{
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.fd = SOCK_BAD;
    src_t src;
    memset(&src, 0, sizeof(src));
    src.tone = -1;
    src.fmt = FMT_AUTO;
    const char *input = "-", *url = NULL, *codec_opt = NULL, *pwfile = NULL;
    int paced = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return EXIT_OK; }
        else if (!strcmp(a, "-v")) g_verbose = 1;
        else if (!strcmp(a, "--realtime-off")) paced = 0;
        else if (!strcmp(a, "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(a, "--password-file") && i + 1 < argc) pwfile = argv[++i];
        else if (!strcmp(a, "--codec") && i + 1 < argc) codec_opt = argv[++i];
        else if (!strcmp(a, "--user") && i + 1 < argc) snprintf(c.user, sizeof(c.user), "%s", argv[++i]);
        else if (!strcmp(a, "--format") && i + 1 < argc) {
            const char *f = argv[++i];
            if      (!strcmp(f, "auto"))  src.fmt = FMT_AUTO;
            else if (!strcmp(f, "s16le")) src.fmt = FMT_S16LE;
            else if (!strcmp(f, "wav"))   src.fmt = FMT_WAV;
            else if (!strcmp(f, "mulaw")) src.fmt = FMT_MULAW;
            else if (!strcmp(f, "alaw"))  src.fmt = FMT_ALAW;
            else { usage(); return EXIT_USAGE; }
        } else if (!strcmp(a, "--tone")) {
            double secs = 2.0;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') secs = atof(argv[++i]);
            if (secs <= 0 || secs > 3600) return fail(EXIT_USAGE, "--tone wants 0 to 3600 seconds");
            src.tone = 1;
            src.tone_left = (long)(secs * 8000);
        } else if (a[0] == '-' && a[1]) { usage(); return EXIT_USAGE; }
        else if (!url) url = a;
        else { usage(); return EXIT_USAGE; }
    }
    if (!url) { usage(); return EXIT_USAGE; }
    if (parse_url(&c, url) != 0) return fail(EXIT_USAGE, "not an rtsp:// address");
    if (!c.pass[0]) {
        const char *pw = getenv("AKAM_RTSP_PASSWORD");
        if (pwfile) {
            if (read_password_file(pwfile, c.pass, sizeof(c.pass)) != 0) return fail(EXIT_USAGE, "cannot read the password file");
        } else if (pw) {
            snprintf(c.pass, sizeof(c.pass), "%s", pw);
        }
    }
    if (!c.user[0]) {
        const char *u = getenv("AKAM_RTSP_USER");
        snprintf(c.user, sizeof(c.user), "%s", u && *u ? u : "akam");
    }
    if (codec_opt && strcmp(codec_opt, "pcmu") && strcmp(codec_opt, "pcma")) return fail(EXIT_USAGE, "--codec wants pcmu or pcma");

    /* audio source */
    if (src.tone < 0) {
        if (!strcmp(input, "-")) {
            src.f = stdin;
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
#endif
        } else {
            src.f = fopen(input, "rb");
            if (!src.f) return fail(EXIT_USAGE, "cannot open %s: %s", input, strerror(errno));
        }
        if (src.fmt == FMT_AUTO || src.fmt == FMT_WAV) {
            src.peek_n = fread(src.peek, 1, sizeof(src.peek), src.f);
            if (src.peek_n == 12 && !memcmp(src.peek, "RIFF", 4)) {
                int w = wav_open(&src, src.peek);
                src.peek_n = 0; /* the 12 bytes were the header */
                if (w == -2) return fail(EXIT_USAGE, "the WAV file must be 8000 Hz, mono, 16-bit PCM "
                                         "(convert: ffmpeg -i in.wav -ar 8000 -ac 1 -c:a pcm_s16le out.wav)");
                if (w != 0) return fail(EXIT_USAGE, "unreadable WAV header");
                src.fmt = FMT_WAV;
            } else if (src.fmt == FMT_WAV) {
                return fail(EXIT_USAGE, "not a WAV file");
            } else {
                src.fmt = FMT_S16LE; /* the bytes read stay in the peek buffer and are sent first */
            }
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail(EXIT_NET, "winsock failed to start");
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    srand((unsigned)(now_ms() ^ (int64_t)(uintptr_t)&c));

    if (connect_to(&c) != 0) return fail(EXIT_NET, "cannot connect to %s port %s", c.host, c.port);
    vlog("connected to %s:%s", c.host, c.port);

    static char resp[RX_CAP];
    char extra[256], v[256];

    /* DESCRIBE with the backchannel requirement */
    snprintf(extra, sizeof(extra), "Require: " BACKCHANNEL_TAG "\r\nAccept: application/sdp\r\n");
    int code = request(&c, "DESCRIBE", c.base, extra, resp, sizeof(resp));
    if (code == 551)
        return fail(EXIT_REFUSED, "the camera does not offer talk: the RTSP password must be on (./akam rtsp-auth set), "
                    "audio out enabled and audio.talk_enable=1");
    if (code == 401) {
        if (!c.pass[0]) return fail(EXIT_LOGIN, "the camera asks for a password: set AKAM_RTSP_PASSWORD");
        return fail(EXIT_LOGIN, "login refused (wrong user or password)");
    }
    if (code < 0) return fail(EXIT_NET, "no answer to DESCRIBE");
    if (code != 200) return fail(EXIT_PROTO, "DESCRIBE answered %d", code);

    char control[256];
    int has_pcmu = 0, has_pcma = 0;
    const char *sdp = strstr(resp, "\r\n\r\n");
    if (!sdp || !sdp_backchannel(sdp + 4, control, sizeof(control), &has_pcmu, &has_pcma))
        return fail(EXIT_REFUSED, "the camera sent no audio backchannel in its description");
    int pcmu = has_pcmu;
    if (codec_opt) {
        pcmu = !strcmp(codec_opt, "pcmu");
        if ((pcmu && !has_pcmu) || (!pcmu && !has_pcma)) return fail(EXIT_REFUSED, "the camera does not offer %s", codec_opt);
    } else if (!has_pcmu && !has_pcma) {
        return fail(EXIT_REFUSED, "the camera offers neither PCMU nor PCMA");
    }
    vlog("backchannel track %s, sending %s", control, pcmu ? "PCMU" : "PCMA");

    char base[512];
    snprintf(base, sizeof(base), "%s", c.base);
    if (header(resp, "Content-Base", v, sizeof(v))) {
        snprintf(base, sizeof(base), "%s", v);
        size_t bl = strlen(base);
        while (bl > 7 && base[bl - 1] == '/') base[--bl] = '\0';
    }
    char track[768];
    if (!strncasecmp(control, "rtsp://", 7)) snprintf(track, sizeof(track), "%s", control);
    else snprintf(track, sizeof(track), "%s/%s", base, control);

    snprintf(extra, sizeof(extra), "Require: " BACKCHANNEL_TAG "\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    code = request(&c, "SETUP", track, extra, resp, sizeof(resp));
    if (code == 551 || code == 404) return fail(EXIT_REFUSED, "the camera refused the talk track (%d)", code);
    if (code != 200) return fail(EXIT_PROTO, "SETUP answered %d", code);
    int rtp_ch = 0, rtcp_ch = 1;
    if (header(resp, "Transport", v, sizeof(v))) {
        const char *il = strstr(v, "interleaved=");
        if (il) sscanf(il + 12, "%d-%d", &rtp_ch, &rtcp_ch);
    }
    if (!header(resp, "Session", v, sizeof(v))) return fail(EXIT_PROTO, "SETUP gave no Session");
    v[strcspn(v, ";")] = '\0';
    snprintf(c.session, sizeof(c.session), "%.63s", v);
    vlog("session set up, RTP on channel %d", rtp_ch);

    snprintf(extra, sizeof(extra), "Require: " BACKCHANNEL_TAG "\r\nRange: npt=0.000-\r\n");
    code = request(&c, "PLAY", base, extra, resp, sizeof(resp));
    if (code != 200) return fail(EXIT_PROTO, "PLAY answered %d", code);
    vlog("talking");

    /* RTP out */
    uint16_t seq = (uint16_t)rand();
    uint32_t ts = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    uint32_t ssrc = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    int64_t t0 = now_ms(), last_ka = t0;
    long sent = 0, packets = 0;
    int rc = EXIT_OK;
    uint8_t frame[4 + 12 + SAMPLES_PER_PACKET];

    while (!g_stop) {
        uint8_t *pl = frame + 16;
        size_t n = next_packet(&src, pcmu, pl);
        if (n == 0) break;
        frame[0] = '$';
        frame[1] = (uint8_t)rtp_ch;
        frame[2] = 0;
        frame[3] = (uint8_t)(12 + n);
        frame[4] = 0x80;
        frame[5] = (uint8_t)((pcmu ? 0 : 8) | (packets == 0 ? 0x80 : 0)); /* marker on the first packet */
        frame[6] = (uint8_t)(seq >> 8); frame[7] = (uint8_t)seq;
        frame[8] = (uint8_t)(ts >> 24); frame[9] = (uint8_t)(ts >> 16); frame[10] = (uint8_t)(ts >> 8); frame[11] = (uint8_t)ts;
        frame[12] = (uint8_t)(ssrc >> 24); frame[13] = (uint8_t)(ssrc >> 16); frame[14] = (uint8_t)(ssrc >> 8); frame[15] = (uint8_t)ssrc;
        if (send_all(c.fd, frame, 16 + n) < 0) { rc = fail(EXIT_NET, "the camera closed the connection"); break; }
        seq++;
        ts += (uint32_t)n;
        sent += (long)n;
        packets++;

        /* pace: packet k leaves at t0 + k * 20 ms (a late live source is not slowed down) */
        if (paced) {
            int64_t due = t0 + packets * PTIME_MS, now = now_ms();
            if (due > now) sleep_ms((int)(due - now));
            else if (now - due > 200) t0 = now - packets * PTIME_MS; /* live source that fell behind: resync */
        }
        /* drain whatever the camera sent (keep-alive answers, RTCP) and notice a closed connection */
        while (c.rxn < sizeof(c.rx) && wait_readable(c.fd, 0) > 0) {
            if (rx_more(&c, 0) < 0) { rc = fail(EXIT_NET, "the camera closed the connection"); g_stop = 1; break; }
        }
        char junk[1024];
        while (rx_take_response(&c, junk, sizeof(junk)) > 0) {
            int sc = status_of(junk);
            if (sc >= 400) vlog("camera answered %d to a keep-alive", sc);
        }
        if (now_ms() - last_ka >= KEEPALIVE_MS) {
            char ka[512];
            int kn = snprintf(ka, sizeof(ka), "GET_PARAMETER %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n", base, ++c.cseq, c.session);
            if (send_all(c.fd, ka, (size_t)kn) < 0) { rc = fail(EXIT_NET, "the camera closed the connection"); break; }
            last_ka = now_ms();
        }
    }

    /* end: RTCP BYE, TEARDOWN */
    if (rc == EXIT_OK || g_stop) {
        uint8_t bye[4 + 8] = { '$', (uint8_t)rtcp_ch, 0, 8, 0x81, 203, 0, 1, (uint8_t)(ssrc >> 24), (uint8_t)(ssrc >> 16), (uint8_t)(ssrc >> 8), (uint8_t)ssrc };
        (void)send_all(c.fd, bye, sizeof(bye));
        snprintf(extra, sizeof(extra), "Require: " BACKCHANNEL_TAG "\r\n");
        (void)request(&c, "TEARDOWN", base, extra, resp, sizeof(resp));
    }
    vlog("sent %ld samples in %ld packets", sent, packets);
    sock_close(c.fd);
    if (src.f && src.f != stdin) fclose(src.f);
    return rc;
}
