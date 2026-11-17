/* See http_req.h. */
#include "http_req.h"

#include <string.h>

long hreq_head_end(const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r'))
            return -2; /* a bare LF: answered 400 at once, not after the read timeout */
        if (i >= 3 && buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r')
            return i + 1 <= HREQ_HEAD_MAX ? (long)(i + 1) : -1;
    }
    return len >= HREQ_HEAD_MAX ? -1 : 0;
}

static int is_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '^' ||
           c == '`' || c == '|' || c == '~';
}

static int ieq(const char *a, const char *b) /* case-insensitive, b is lower case */
{
    for (; *a && *b; a++, b++) {
        char c = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (c != *b)
            return 0;
    }
    return *a == *b;
}

/* Request line "METHOD SP target SP HTTP/1.x" in line[0..n). */
static int parse_request_line(char *line, size_t n, hreq_t *r)
{
    char *sp1, *sp2, *q;
    size_t i, mlen, tlen;
    line[n] = '\0';
    sp1 = memchr(line, ' ', n);
    if (!sp1)
        return 400;
    sp2 = memchr(sp1 + 1, ' ', n - (size_t)(sp1 + 1 - line));
    if (!sp2 || memchr(sp2 + 1, ' ', n - (size_t)(sp2 + 1 - line)))
        return 400;
    *sp1 = '\0';
    *sp2 = '\0';
    mlen = strlen(line);
    if (mlen < 3 || mlen > 7)
        return 400;
    for (i = 0; i < mlen; i++)
        if (line[i] < 'A' || line[i] > 'Z')
            return 400;
    r->method = line;
    tlen = strlen(sp1 + 1);
    if (tlen == 0 || sp1[1] != '/')
        return 400;
    if (tlen > HREQ_PATH_MAX + 1 + HREQ_QUERY_MAX)
        return 414;
    for (i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)sp1[1 + i];
        if (c <= 0x20 || c >= 0x7f || c == '#')
            return 400;
    }
    r->path = sp1 + 1;
    q = strchr(r->path, '?');
    if (q) {
        *q = '\0';
        r->query = q + 1;
        if (strlen(r->query) > HREQ_QUERY_MAX)
            return 414;
    }
    if (strlen(r->path) > HREQ_PATH_MAX)
        return 414;
    if (strcmp(sp2 + 1, "HTTP/1.1") == 0)
        r->http10 = 0;
    else if (strcmp(sp2 + 1, "HTTP/1.0") == 0)
        r->http10 = 1;
    else
        return strncmp(sp2 + 1, "HTTP/", 5) == 0 ? 505 : 400;
    return 0;
}

static int set_once(const char **slot, char *val)
{
    if (*slot)
        return 400;
    if (strlen(val) > HREQ_VALUE_MAX)
        return 431;
    *slot = val;
    return 0;
}

/* One header line (NUL terminated, no CRLF). */
static int parse_header(char *line, size_t n, hreq_t *r)
{
    char *colon, *val;
    size_t i;
    if (n == 0 || line[0] == ' ' || line[0] == '\t')
        return 400; /* obsolete folding, or an empty line before the end */
    colon = memchr(line, ':', n);
    if (!colon || colon == line)
        return 400;
    *colon = '\0';
    for (i = 0; line + i < colon; i++)
        if (!is_tchar((unsigned char)line[i]))
            return 400;
    val = colon + 1;
    while (*val == ' ' || *val == '\t')
        val++;
    for (i = strlen(val); i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'); i--)
        val[i - 1] = '\0';
    if (ieq(line, "x-akam-key"))
        return set_once(&r->key, val);
    if (ieq(line, "x-akam-timestamp"))
        return set_once(&r->timestamp, val);
    if (ieq(line, "x-akam-nonce"))
        return set_once(&r->nonce, val);
    if (ieq(line, "x-akam-challenge"))
        return set_once(&r->challenge, val);
    if (ieq(line, "x-akam-signature"))
        return set_once(&r->signature, val);
    if (ieq(line, "content-length")) {
        long v = 0;
        if (r->content_length >= 0 || *val == '\0' || strlen(val) > 9)
            return 400;
        for (i = 0; val[i]; i++) {
            if (val[i] < '0' || val[i] > '9')
                return 400;
            v = v * 10 + (val[i] - '0');
        }
        if (v > HREQ_BODY_MAX)
            return 413;
        r->content_length = v;
    } else if (ieq(line, "transfer-encoding")) {
        r->chunked = 1;
    } else if (ieq(line, "expect")) {
        r->expect = 1;
    }
    return 0;
}

int hreq_parse(char *buf, size_t head_len, hreq_t *r)
{
    size_t pos = 0, i;
    int nh = 0, rc, first = 1;
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    r->head_len = head_len;
    if (head_len < 4 || head_len > HREQ_HEAD_MAX || memcmp(buf + head_len - 4, "\r\n\r\n", 4) != 0)
        return 400;
    /* every CR is followed by LF and every LF preceded by CR; no NUL; no byte >= 0x80 anywhere */
    for (i = 0; i < head_len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0 || c >= 0x80)
            return 400;
        if (c == '\r' && (i + 1 >= head_len || buf[i + 1] != '\n'))
            return 400;
        if (c == '\n' && (i == 0 || buf[i - 1] != '\r'))
            return 400;
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t')
            return 400;
    }
    while (pos < head_len - 2) { /* the last CRLF is the blank line */
        char *line = buf + pos, *eol = memchr(line, '\r', head_len - pos);
        size_t n = (size_t)(eol - line);
        pos += n + 2;
        line[n] = '\0';
        if (first) {
            if ((rc = parse_request_line(line, n, r)) != 0)
                return rc;
            first = 0;
            continue;
        }
        if (++nh > HREQ_MAX_HEADERS)
            return 431;
        if ((rc = parse_header(line, n, r)) != 0)
            return rc;
    }
    return first ? 400 : 0;
}
