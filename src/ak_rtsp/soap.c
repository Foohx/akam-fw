/* Bounded SOAP 1.2 scanner and template engine; see soap.h. */
#include <string.h>

#include "soap.h"

/* ------------------------------------------------------------------ markup tokenizer */

enum { K_START, K_END, K_EMPTY, K_COMMENT, K_CDATA };

typedef struct {
    int         kind;
    soap_span_t name;   /* element name; for K_CDATA the data */
    soap_span_t attrs;
    const char *start;  /* the '<' */
    const char *next;   /* first byte after the markup */
} markup_t;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* First occurrence of the `m` bytes of `pat` in [p, end), or NULL. */
static const char *find_str(const char *p, const char *end, const char *pat, size_t m)
{
    if ((size_t)(end - p) < m) return NULL;
    const char *last = end - m;
    for (; p <= last; p++)
        if (*p == pat[0] && memcmp(p, pat, m) == 0) return p;
    return NULL;
}

/* Parse the markup that starts at p (*p == '<'). 0 or SOAP_MALFORMED. */
static int parse_markup(const char *p, const char *end, markup_t *m)
{
    m->start = p;
    if ((size_t)(end - p) >= 4 && memcmp(p, "<!--", 4) == 0) {
        const char *e = find_str(p + 4, end, "-->", 3);
        if (!e) return SOAP_MALFORMED;
        m->kind = K_COMMENT;
        m->next = e + 3;
        return 0;
    }
    if ((size_t)(end - p) >= 9 && memcmp(p, "<![CDATA[", 9) == 0) {
        const char *e = find_str(p + 9, end, "]]>", 3);
        if (!e) return SOAP_MALFORMED;
        m->kind = K_CDATA;
        m->name.p = p + 9;
        m->name.n = (size_t)(e - (p + 9));
        m->next = e + 3;
        return 0;
    }
    if ((size_t)(end - p) >= 2 && p[1] == '?') {      /* processing instruction / xml declaration */
        const char *e = find_str(p + 2, end, "?>", 2);
        if (!e) return SOAP_MALFORMED;
        m->kind = K_COMMENT;
        m->next = e + 2;
        return 0;
    }
    if ((size_t)(end - p) >= 2 && p[1] == '!') return SOAP_MALFORMED;   /* DOCTYPE, ENTITY: refused */

    const char *lim = ((size_t)(end - p) > SOAP_MAX_TAG) ? p + SOAP_MAX_TAG : end;
    const char *q = p + 1;
    int closing = 0;
    if (q < lim && *q == '/') { closing = 1; q++; }
    const char *ns = q;
    while (q < lim && !is_ws(*q) && *q != '>' && *q != '/' && *q != '<' && *q != '"' && *q != '\'' && *q != '=') q++;
    if (q == ns || (size_t)(q - ns) > SOAP_MAX_NAME) return SOAP_MALFORMED;
    m->name.p = ns;
    m->name.n = (size_t)(q - ns);
    const char *as = q;
    int empty = 0;
    for (;;) {                                    /* attributes, quote aware, up to '>' */
        if (q >= lim) return SOAP_MALFORMED;
        char c = *q;
        if (c == '"' || c == '\'') {
            q++;
            while (q < lim && *q != c) q++;
            if (q >= lim) return SOAP_MALFORMED;
            q++;
        } else if (c == '<') {
            return SOAP_MALFORMED;
        } else if (c == '>') {
            break;
        } else if (c == '/' && q + 1 < lim && q[1] == '>') {
            empty = 1;
            break;
        } else {
            q++;
        }
    }
    m->attrs.p = as;
    m->attrs.n = (size_t)(q - as);
    m->next = q + (empty ? 2 : 1);
    if (closing) {
        if (empty) return SOAP_MALFORMED;
        for (size_t i = 0; i < m->attrs.n; i++)
            if (!is_ws(as[i])) return SOAP_MALFORMED;   /* only blanks may follow the name of an end tag */
        m->kind = K_END;
    } else {
        m->kind = empty ? K_EMPTY : K_START;
    }
    return 0;
}

/* Next element tag at or after *pp (character data, comments, CDATA, PIs skipped). 1 got one, 0 end, <0 error. */
static int next_tag(const char **pp, const char *end, markup_t *m)
{
    const char *p = *pp;
    for (;;) {
        p = memchr(p, '<', (size_t)(end - p));
        if (!p) { *pp = end; return 0; }
        int r = parse_markup(p, end, m);
        if (r < 0) return r;
        p = m->next;
        if (m->kind == K_COMMENT || m->kind == K_CDATA) continue;
        *pp = p;
        return 1;
    }
}

/* Local part of a qualified name. */
static soap_span_t local_of(soap_span_t q)
{
    for (size_t i = q.n; i > 0; i--)
        if (q.p[i - 1] == ':') { q.p += i; q.n -= i; break; }
    return q;
}

static int span_is(soap_span_t s, const char *lit)
{
    size_t n = strlen(lit);
    return s.n == n && memcmp(s.p, lit, n) == 0;
}

/* Fill `e` from the start tag `m`; for a non-empty element find the matching end tag. */
static int finish_elem(const markup_t *m, const char *end, soap_elem_t *e)
{
    e->name = m->name;
    e->attrs = m->attrs;
    e->empty = (m->kind == K_EMPTY);
    e->inner.p = m->next;
    e->inner.n = 0;
    if (e->empty) return SOAP_OK;
    int depth = 1;
    const char *p = m->next;
    markup_t t;
    for (;;) {
        int r = next_tag(&p, end, &t);
        if (r < 0) return r;
        if (r == 0) return SOAP_MALFORMED;        /* truncated */
        if (t.kind == K_START) {
            if (++depth > SOAP_MAX_DEPTH) return SOAP_TOODEEP;
        } else if (t.kind == K_END && --depth == 0) {
            e->inner.n = (size_t)(t.start - m->next);
            return SOAP_OK;
        }
    }
}

int soap_is(const soap_elem_t *e, const char *local) { return span_is(local_of(e->name), local); }

int soap_find(soap_span_t range, const char *local, soap_elem_t *out)
{
    const char *p = range.p, *end = range.p + range.n;
    int depth = 0;
    markup_t m;
    for (;;) {
        int r = next_tag(&p, end, &m);
        if (r < 0) return r;
        if (r == 0) return SOAP_NOTFOUND;
        if (m.kind == K_END) {
            if (--depth < 0) return SOAP_MALFORMED;
            continue;
        }
        if (span_is(local_of(m.name), local)) return finish_elem(&m, end, out);
        if (m.kind == K_START && ++depth > SOAP_MAX_DEPTH) return SOAP_TOODEEP;
    }
}

int soap_body_op(const char *xml, size_t len, soap_elem_t *op)
{
    const char *p = xml, *end = xml + len;
    int depth = 0;
    markup_t m;
    for (;;) {
        int r = next_tag(&p, end, &m);
        if (r < 0) return r;
        if (r == 0) return SOAP_NOTFOUND;
        if (m.kind == K_END) {
            if (--depth < 0) return SOAP_MALFORMED;
            continue;
        }
        if (depth == 0 && !span_is(local_of(m.name), "Envelope")) return SOAP_MALFORMED;
        if (depth == 1 && span_is(local_of(m.name), "Body")) {
            if (m.kind == K_EMPTY) return SOAP_NOTFOUND;
            r = next_tag(&p, end, &m);
            if (r < 0) return r;
            if (r == 0) return SOAP_MALFORMED;
            if (m.kind == K_END) return SOAP_NOTFOUND;
            return finish_elem(&m, end, op);
        }
        if (m.kind == K_START && ++depth > SOAP_MAX_DEPTH) return SOAP_TOODEEP;
    }
}

/* ------------------------------------------------------------------ text and attributes */

typedef struct { char *out; size_t cap, n; int over; } sink_t;

static void put(sink_t *s, char c)
{
    if (s->n + 1 < s->cap) s->out[s->n++] = c;
    else s->over = 1;
}

static void put_utf8(sink_t *s, unsigned long cp)
{
    if (cp < 0x80) { put(s, (char)cp); }
    else if (cp < 0x800) { put(s, (char)(0xC0 | (cp >> 6))); put(s, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        put(s, (char)(0xE0 | (cp >> 12))); put(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(s, (char)(0x80 | (cp & 0x3F)));
    } else {
        put(s, (char)(0xF0 | (cp >> 18))); put(s, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put(s, (char)(0x80 | ((cp >> 6) & 0x3F))); put(s, (char)(0x80 | (cp & 0x3F)));
    }
}

/* p points just after '&'. Decode one reference; returns bytes consumed (';' included) or 0. */
static size_t entity(const char *p, const char *end, sink_t *s)
{
    size_t room = (size_t)(end - p) < 12 ? (size_t)(end - p) : 12;
    size_t k = 0;
    while (k < room && p[k] != ';') k++;
    if (k == 0 || k >= room) return 0;
    if (p[0] == '#') {
        unsigned long cp = 0;
        size_t i = 1;
        int hex = (k > 1 && p[1] == 'x');
        if (hex) i = 2;
        if (i >= k) return 0;
        for (; i < k; i++) {
            int d;
            char c = p[i];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned long)d;
            if (cp > 0x10FFFF) return 0;
        }
        if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        put_utf8(s, cp);
        return k + 1;
    }
    soap_span_t n = { p, k };
    if (span_is(n, "lt")) put(s, '<');
    else if (span_is(n, "gt")) put(s, '>');
    else if (span_is(n, "amp")) put(s, '&');
    else if (span_is(n, "quot")) put(s, '"');
    else if (span_is(n, "apos")) put(s, '\'');
    else return 0;
    return k + 1;
}

static int finish_sink(sink_t *s, int flags)
{
    if (s->over) { s->out[s->cap - 1] = 0; return SOAP_TRUNC; }
    s->out[s->n] = 0;
    if (flags & SOAP_TRIM) {
        size_t a = 0, b = s->n;
        while (a < b && is_ws(s->out[a])) a++;
        while (b > a && is_ws(s->out[b - 1])) b--;
        memmove(s->out, s->out + a, b - a);
        s->n = b - a;
        s->out[s->n] = 0;
    }
    return (int)s->n;
}

int soap_text(const soap_elem_t *e, char *out, size_t cap, int flags)
{
    if (!cap) return SOAP_TRUNC;
    sink_t s = { out, cap, 0, 0 };
    const char *p = e->inner.p, *end = e->inner.p + e->inner.n;
    while (p < end) {
        char c = *p;
        if (c == '<') {
            markup_t m;
            int r = parse_markup(p, end, &m);
            if (r < 0) return r;
            if (m.kind == K_CDATA)
                for (size_t i = 0; i < m.name.n; i++) put(&s, m.name.p[i]);
            p = m.next;
        } else if (c == '&') {
            size_t k = entity(p + 1, end, &s);
            if (!k) return SOAP_MALFORMED;
            p += 1 + k;
        } else {
            put(&s, c);
            p++;
        }
        if (s.over) break;
    }
    return finish_sink(&s, flags);
}

int soap_attr(const soap_elem_t *e, const char *name, char *out, size_t cap)
{
    if (!cap) return SOAP_TRUNC;
    const char *p = e->attrs.p, *end = e->attrs.p + e->attrs.n;
    for (;;) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) return SOAP_NOTFOUND;
        const char *ns = p;
        while (p < end && !is_ws(*p) && *p != '=') p++;
        soap_span_t an = { ns, (size_t)(p - ns) };
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != '=') return SOAP_MALFORMED;
        p++;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || (*p != '"' && *p != '\'')) return SOAP_MALFORMED;
        char q = *p++;
        const char *vs = p;
        while (p < end && *p != q) p++;
        if (p >= end) return SOAP_MALFORMED;
        const char *ve = p++;
        if (an.n >= 5 && memcmp(an.p, "xmlns", 5) == 0 && (an.n == 5 || an.p[5] == ':')) continue;
        if (!span_is(local_of(an), name)) continue;
        sink_t s = { out, cap, 0, 0 };
        while (vs < ve) {
            if (*vs == '&') {
                size_t k = entity(vs + 1, ve, &s);
                if (!k) return SOAP_MALFORMED;
                vs += 1 + k;
            } else if (*vs == '<') {
                return SOAP_MALFORMED;
            } else {
                put(&s, *vs++);
            }
        }
        return finish_sink(&s, 0);
    }
}

/* ------------------------------------------------------------------ templates */

void soap_tpl_init(soap_tpl_t *t, const char *tpl)
{
    memset(t, 0, sizeof(*t));
    t->tpl = tpl;
}

static int tpl_add(soap_tpl_t *t, const char *name, const char *val, int raw)
{
    if (t->nvar >= SOAP_TPL_MAXVARS) return -1;
    t->var[t->nvar].name = name;
    t->var[t->nvar].val = val;
    t->var[t->nvar].raw = raw;
    t->nvar++;
    return 0;
}

int soap_tpl_set(soap_tpl_t *t, const char *name, const char *val)
{
    size_t n = strlen(val) + 1;
    if (n > SOAP_TPL_POOL - t->pool_used || t->nvar >= SOAP_TPL_MAXVARS) return -1;
    char *d = t->pool + t->pool_used;
    memcpy(d, val, n);
    t->pool_used += n;
    return tpl_add(t, name, d, 0);
}

int soap_tpl_ref(soap_tpl_t *t, const char *name, const char *val) { return tpl_add(t, name, val, 0); }
int soap_tpl_set_raw(soap_tpl_t *t, const char *name, const char *val) { return tpl_add(t, name, val, 1); }

/* The reader: writes up to cap bytes of the template into buf. Its state is only the four rd_t fields, so
 * soap_tpl_len can run it on a scratch state without touching the object. */
typedef struct { size_t tpos; const char *vp; int vraw; const char *esc; } rd_t;

static size_t rd_run(const soap_tpl_t *t, rd_t *r, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap) {
        if (r->esc) {
            buf[n++] = *r->esc++;
            if (!*r->esc) r->esc = NULL;
            continue;
        }
        if (r->vp) {
            char c = *r->vp;
            if (!c) { r->vp = NULL; continue; }
            r->vp++;
            if (r->vraw) { buf[n++] = c; continue; }
            const char *e = NULL;
            switch (c) {
            case '&': e = "&amp;"; break;
            case '<': e = "&lt;"; break;
            case '>': e = "&gt;"; break;
            case '"': e = "&quot;"; break;
            case '\'': e = "&apos;"; break;
            default: break;
            }
            if (e) { buf[n++] = e[0]; r->esc = e + 1; }
            else if ((unsigned char)c >= 0x20 || c == '\t' || c == '\n' || c == '\r') buf[n++] = c;
            continue;                              /* other C0 controls are not XML: dropped */
        }
        char c = t->tpl[r->tpos];
        if (!c) break;
        if (c == '{' && t->tpl[r->tpos + 1] == '{') {
            const char *ns = t->tpl + r->tpos + 2;
            const char *ne = strstr(ns, "}}");
            if (ne && (size_t)(ne - ns) < SOAP_MAX_NAME) {
                size_t nl = (size_t)(ne - ns);
                for (int i = 0; i < t->nvar; i++)
                    if (strlen(t->var[i].name) == nl && memcmp(t->var[i].name, ns, nl) == 0) {
                        r->vp = t->var[i].val;
                        r->vraw = t->var[i].raw;
                        break;
                    }
                r->tpos = (size_t)(ne - t->tpl) + 2;
                continue;
            }
        }
        buf[n++] = c;
        r->tpos++;
    }
    return n;
}

size_t soap_tpl_len(const soap_tpl_t *t)
{
    rd_t r = { 0, NULL, 0, NULL };
    char scratch[64];
    size_t total = 0, n;
    while ((n = rd_run(t, &r, scratch, sizeof(scratch))) > 0) total += n;
    return total;
}

size_t soap_tpl_read(void *ctx, char *buf, size_t cap)
{
    soap_tpl_t *t = ctx;
    rd_t r = { t->tpos, t->vp, t->vraw, t->esc };
    size_t n = rd_run(t, &r, buf, cap);
    t->tpos = r.tpos; t->vp = r.vp; t->vraw = r.vraw; t->esc = r.esc;
    return n;
}

/* ------------------------------------------------------------------ faults */

static const char FAULT_TPL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:ter=\"http://www.onvif.org/ver10/error\" xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<env:Body><env:Fault><env:Code><env:Value>env:{{code}}</env:Value>"
    "<env:Subcode><env:Value>{{sub1}}</env:Value>{{open2}}{{sub2}}{{close2}}</env:Subcode></env:Code>"
    "<env:Reason><env:Text xml:lang=\"en\">{{reason}}</env:Text></env:Reason></env:Fault></env:Body></env:Envelope>";

void soap_fault_init(soap_tpl_t *t, int receiver, const char *sub1, const char *sub2, const char *reason)
{
    char buf[160];
    soap_tpl_init(t, FAULT_TPL);
    soap_tpl_ref(t, "code", receiver ? "Receiver" : "Sender");
    soap_tpl_set(t, "sub1", sub1 ? sub1 : "ter:Action");
    if (sub2) {
        soap_tpl_set_raw(t, "open2", "<env:Subcode><env:Value>");
        soap_tpl_set(t, "sub2", sub2);
        soap_tpl_set_raw(t, "close2", "</env:Value></env:Subcode>");
    }
    size_t n = reason ? strlen(reason) : 0;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (n) memcpy(buf, reason, n);
    buf[n] = 0;
    soap_tpl_set(t, "reason", buf);
}

int soap_fault_status(int receiver) { return receiver ? 500 : 400; }
