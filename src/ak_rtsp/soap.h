/* Bounded SOAP 1.2 scanner and response template engine for the ONVIF services (specs/onvif.md).
 * No XML library, no DOM, no allocation: every function works on a (pointer, length) range and never reads
 * outside it. Namespace prefixes are ignored (elements and attributes are matched by local name). DTDs
 * (<!DOCTYPE, <!ENTITY) are refused. Pure logic, tested on the host (tests/native/soap_test.c). */
#pragma once
#include <stddef.h>

#define SOAP_MAX_DEPTH 32   /* element nesting accepted while scanning */
#define SOAP_MAX_TAG   4096 /* one start/end tag, attributes included */
#define SOAP_MAX_NAME  128

enum {
    SOAP_OK        = 0,
    SOAP_NOTFOUND  = 1,  /* well-formed so far, the element / attribute is not there */
    SOAP_MALFORMED = -1, /* syntax error, truncated document, bad entity, DTD, oversized tag */
    SOAP_TOODEEP   = -2, /* nesting above SOAP_MAX_DEPTH */
    SOAP_TRUNC     = -3  /* output buffer too small */
};

typedef struct { const char *p; size_t n; } soap_span_t;

/* An element found by the scanner. All spans point into the caller's buffer. */
typedef struct {
    soap_span_t name;   /* qualified name as written (prefix included) */
    soap_span_t attrs;  /* raw attribute text of the start tag */
    soap_span_t inner;  /* raw content between the start and the end tag (empty for <x/>) */
    int         empty;  /* self-closing */
} soap_elem_t;

/* The first child element of the first Body element of the Envelope (the SOAP operation).
 * SOAP_NOTFOUND when the Body is empty or missing. */
int soap_body_op(const char *xml, size_t len, soap_elem_t *op);

/* The first element (any depth) whose local name is `local`, searched inside `range`. */
int soap_find(soap_span_t range, const char *local, soap_elem_t *out);

/* True when the local name of `e` is `local`. */
int soap_is(const soap_elem_t *e, const char *local);

/* Text of an element: character data and CDATA, comments dropped, child tags dropped (their text kept), the five
 * predefined entities and numeric references decoded (to UTF-8). NUL-terminated. SOAP_TRIM strips leading and
 * trailing XML whitespace (use it for tokens, not for passwords). Returns the length, or a negative SOAP_* code. */
#define SOAP_TRIM 1
int soap_text(const soap_elem_t *e, char *out, size_t cap, int flags);

/* Attribute value by local name (either quote style, entities decoded). Returns its length, SOAP_NOTFOUND, or an
 * error code. */
int soap_attr(const soap_elem_t *e, const char *name, char *out, size_t cap);

/* ---- response templates ----
 * A static template with {{name}} placeholders. Values are XML-escaped on output (& < > " ' and the C0 controls
 * that XML forbids are dropped), unless set with soap_tpl_set_raw. The output length is computed first without
 * producing the body (for Content-Length), then the body is produced in pieces of any size (the resumable
 * soap_tpl_read matches http_srv's producer callback), so no full-size response buffer exists.
 * The object is self-contained (small fixed pool for copied values), so it can live in http_resp_alloc memory. */
#define SOAP_TPL_MAXVARS 12
#define SOAP_TPL_POOL    512

typedef struct soap_tpl {
    const char *tpl;
    struct { const char *name; const char *val; int raw; } var[SOAP_TPL_MAXVARS];
    int    nvar;
    char   pool[SOAP_TPL_POOL];
    size_t pool_used;
    /* reader state */
    size_t      tpos;
    const char *vp;   /* value being emitted */
    int         vraw;
    const char *esc;  /* rest of an entity being emitted */
} soap_tpl_t;

void soap_tpl_init(soap_tpl_t *t, const char *tpl);
/* Copy `val` (escaped on output). -1 when the pool or the variable table is full. */
int  soap_tpl_set(soap_tpl_t *t, const char *name, const char *val);
/* Keep the pointer (must outlive the response), escaped on output. */
int  soap_tpl_ref(soap_tpl_t *t, const char *name, const char *val);
/* Keep the pointer, output as is (trusted XML fragment). */
int  soap_tpl_set_raw(soap_tpl_t *t, const char *name, const char *val);
/* Exact number of bytes the whole template produces; does not disturb the reader. Unknown placeholders are empty. */
size_t soap_tpl_len(const soap_tpl_t *t);
/* Producer: next up to `cap` bytes (ctx is a soap_tpl_t *); 0 at the end. */
size_t soap_tpl_read(void *ctx, char *buf, size_t cap);

/* ---- SOAP 1.2 faults ----
 * ONVIF core 5.11 / SOAP 1.2 HTTP binding: a Sender fault is HTTP 400, a Receiver fault is 500. Subcodes are
 * written with the usual prefixes (ter: is declared in the template), e.g. "ter:InvalidArgs" then
 * "ter:InvalidArgVal", or "ter:ActionNotSupported", "ter:NotAuthorized". `sub2` may be NULL. */
#define SOAP_CONTENT_TYPE "application/soap+xml; charset=utf-8"
void soap_fault_init(soap_tpl_t *t, int receiver, const char *sub1, const char *sub2, const char *reason);
int  soap_fault_status(int receiver);
