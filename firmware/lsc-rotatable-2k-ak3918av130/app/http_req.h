/* Strict HTTP/1.1 request-head parser of the local control API (specs/control-api.md 4.1). Pure: no I/O, no allocation, no
 * global state; fuzzed under ASan/UBSan (tests/rot2k_api_fuzz.c). The head is parsed IN PLACE (the caller owns a mutable
 * buffer) and the result points into it. Only what the API needs is kept; everything else is validated and dropped.
 * Rules: CRLF line ends only (a bare CR or LF is 400, so two parsers cannot disagree on a request boundary), no NUL, no
 * control byte or byte >= 0x80 anywhere, no obsolete line folding, at most HREQ_MAX_HEADERS headers, the five X-Akam-*
 * headers and Content-Length at most once. The request target is NOT decoded: the raw '+' stays '+' (it is signed as %2B
 * by the client), a '%' stays as sent (api_sig rejects it in the path and decodes it in the query). */
#ifndef AKAM_HTTP_REQ_H
#define AKAM_HTTP_REQ_H
#include <stddef.h>

#define HREQ_HEAD_MAX 4096   /* request line + headers + the blank line */
#define HREQ_BODY_MAX 8192
#define HREQ_PATH_MAX 128
#define HREQ_QUERY_MAX 512
#define HREQ_MAX_HEADERS 40
#define HREQ_VALUE_MAX 160   /* any one X-Akam-* header value */

typedef struct {
    char *method;            /* upper-case letters, 3..7 */
    char *path;              /* starts with '/', <= HREQ_PATH_MAX, no '?' or '#' */
    char *query;             /* raw text after '?', NULL when there is none */
    const char *key, *timestamp, *nonce, *challenge, *signature; /* X-Akam-*, NULL when absent */
    long content_length;     /* -1 when absent */
    int chunked;             /* a Transfer-Encoding header was sent (the API answers 411) */
    int expect;              /* an Expect header was sent (the API answers 417) */
    size_t head_len;         /* bytes of the head including the final CRLFCRLF */
    int http10;              /* the request said HTTP/1.0 */
} hreq_t;

/* Looks for the end of the head in buf[0..len): the offset just after the first CRLFCRLF, 0 when it has not arrived yet,
 * -1 when HREQ_HEAD_MAX bytes came without one (answer 431), -2 when a line ends in a bare LF (answer 400). */
long hreq_head_end(const char *buf, size_t len);

/* Parses buf[0..head_len) (head_len from hreq_head_end). The buffer is modified (NULs). 0 on success, else the HTTP status:
 * 400 malformed, 414 target too long, 431 too many headers or a value too long, 413 Content-Length over HREQ_BODY_MAX,
 * 505 not HTTP/1.0 or 1.1. */
int hreq_parse(char *buf, size_t head_len, hreq_t *r);

#endif
