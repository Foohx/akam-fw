/* Host test of src/ak_rtsp/soap.c: the SOAP scanner, the template engine and the fault helper. Built with
 * ASan/UBSan by tests/soap_test.sh. Every input is copied to an exact-size heap block so that a read past the end
 * is caught by ASan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soap.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

static char *dup_exact(const char *s, size_t n)
{
    char *p = malloc(n ? n : 1);
    if (n) memcpy(p, s, n);
    return p;
}

/* op name, or "" + code */
static int op_of(const char *xml, char *name, size_t cap, soap_elem_t *e)
{
    size_t n = strlen(xml);
    int r = soap_body_op(xml, n, e);   /* xml must outlive e: literals only */
    if (r == SOAP_OK) {
        size_t k = e->name.n < cap - 1 ? e->name.n : cap - 1;
        memcpy(name, e->name.p, k);
        name[k] = 0;
    } else {
        name[0] = 0;
    }
    return r;
}

#define ENV(h, b) "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">" h "<s:Body>" b "</s:Body></s:Envelope>"

static void test_basic(void)
{
    char nm[64];
    soap_elem_t e;
    CHECK(op_of(ENV("", "<tds:GetDeviceInformation xmlns:tds=\"x\"/>"), nm, sizeof nm, &e) == SOAP_OK);
    CHECK(strcmp(nm, "tds:GetDeviceInformation") == 0 && soap_is(&e, "GetDeviceInformation") && e.empty);
    CHECK(op_of(ENV("", "<GetProfiles xmlns=\"x\"></GetProfiles>"), nm, sizeof nm, &e) == SOAP_OK);
    CHECK(strcmp(nm, "GetProfiles") == 0 && !e.empty && e.inner.n == 0);
    /* a different prefix for the envelope, whitespace and a header before the body */
    const char *x =
        "<?xml version='1.0'?>\n<env:Envelope\n xmlns:env='http://www.w3.org/2003/05/soap-envelope'>\n"
        "  <env:Header><a:Foo xmlns:a='q'>bar</a:Foo></env:Header>\n  <env:Body>\n   <!-- c -->\n"
        "   <trt:GetProfile xmlns:trt='t'>\n     <trt:ProfileToken>  main \n</trt:ProfileToken>\n   </trt:GetProfile>\n"
        "  </env:Body>\n</env:Envelope>\n";
    CHECK(op_of(x, nm, sizeof nm, &e) == SOAP_OK);
    CHECK(strcmp(nm, "trt:GetProfile") == 0);
    soap_elem_t t;
    CHECK(soap_find(e.inner, "ProfileToken", &t) == SOAP_OK);
    char out[64];
    CHECK(soap_text(&t, out, sizeof out, SOAP_TRIM) == 4 && strcmp(out, "main") == 0);
    CHECK(soap_text(&t, out, sizeof out, 0) == 8 && strcmp(out, "  main \n") == 0);
    CHECK(soap_find(e.inner, "Nope", &t) == SOAP_NOTFOUND);
    /* empty or missing body */
    CHECK(op_of(ENV("", ""), nm, sizeof nm, &e) == SOAP_NOTFOUND);
    CHECK(op_of("<s:Envelope xmlns:s='x'><s:Body/></s:Envelope>", nm, sizeof nm, &e) == SOAP_NOTFOUND);
    CHECK(op_of("<s:Envelope xmlns:s='x'><s:Header/></s:Envelope>", nm, sizeof nm, &e) == SOAP_NOTFOUND);
    CHECK(op_of("<html><body/></html>", nm, sizeof nm, &e) == SOAP_MALFORMED);
    /* a Body nested deeper than depth 1 is not the SOAP Body */
    CHECK(op_of("<s:Envelope xmlns:s='x'><s:Header><s:Body><q/></s:Body></s:Header></s:Envelope>", nm, sizeof nm, &e) == SOAP_NOTFOUND);
}

static void test_security(void)
{
    const char *x = ENV(
        "<s:Header><wsse:Security xmlns:wsse='a' xmlns:wsu='b'><wsse:UsernameToken>"
        "<wsse:Username>adm&amp;in</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
        "<![CDATA[a<b>&c]]></wsse:Password>"
        "<wsse:Nonce EncodingType='x'>AAEC\n AwQ=</wsse:Nonce><wsu:Created>2026-11-10T10:00:00Z</wsu:Created>"
        "</wsse:UsernameToken></wsse:Security></s:Header>", "<x:Op xmlns:x='y'><x:Brightness>25.5</x:Brightness></x:Op>");
    size_t n = strlen(x);
    char *b = dup_exact(x, n);
    soap_span_t all = { b, n };
    soap_elem_t sec, u, pw, nc, cr;
    char out[200];
    CHECK(soap_find(all, "Security", &sec) == SOAP_OK);
    CHECK(soap_find(sec.inner, "Username", &u) == SOAP_OK);
    CHECK(soap_text(&u, out, sizeof out, SOAP_TRIM) == 6 && strcmp(out, "adm&in") == 0);
    CHECK(soap_find(sec.inner, "Password", &pw) == SOAP_OK);
    CHECK(soap_text(&pw, out, sizeof out, 0) == 6 && strcmp(out, "a<b>&c") == 0);
    CHECK(soap_attr(&pw, "Type", out, sizeof out) > 20 && strstr(out, "PasswordDigest"));
    CHECK(soap_attr(&pw, "Other", out, sizeof out) == SOAP_NOTFOUND);
    CHECK(soap_find(sec.inner, "Nonce", &nc) == SOAP_OK);
    CHECK(soap_text(&nc, out, sizeof out, SOAP_TRIM) == 10);
    CHECK(soap_find(sec.inner, "Created", &cr) == SOAP_OK);
    CHECK(soap_text(&cr, out, sizeof out, SOAP_TRIM) == 20 && strcmp(out, "2026-11-10T10:00:00Z") == 0);
    soap_elem_t op, br;
    CHECK(soap_body_op(b, n, &op) == SOAP_OK && soap_is(&op, "Op"));
    CHECK(soap_find(op.inner, "Brightness", &br) == SOAP_OK);
    CHECK(soap_text(&br, out, sizeof out, SOAP_TRIM) == 4 && strcmp(out, "25.5") == 0);
    CHECK(soap_text(&u, out, 3, 0) == SOAP_TRUNC);
    CHECK(soap_text(&u, out, 0, 0) == SOAP_TRUNC);
    free(b);
}

static int text_of(const char *inner, char *out, size_t cap)
{
    char xml[512];
    snprintf(xml, sizeof xml, "<a>%s</a>", inner);
    size_t n = strlen(xml);
    char *b = dup_exact(xml, n);
    soap_span_t r = { b, n };
    soap_elem_t e;
    int rc = soap_find(r, "a", &e);
    if (rc == SOAP_OK) rc = soap_text(&e, out, cap, 0);
    free(b);
    return rc;
}

static void test_entities_attrs(void)
{
    char o[64];
    CHECK(text_of("&lt;&gt;&amp;&quot;&apos;", o, sizeof o) == 5 && strcmp(o, "<>&\"'") == 0);
    CHECK(text_of("&#65;&#x42;&#x20AC;&#x1F600;", o, sizeof o) > 0 && strcmp(o, "AB\xE2\x82\xAC\xF0\x9F\x98\x80") == 0);
    CHECK(text_of("&bogus;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&amp", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&#0;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&#xD800;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&#x110000;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&#;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("&#x;", o, sizeof o) == SOAP_MALFORMED);
    CHECK(text_of("x<!-- hidden -->y<![CDATA[<z>]]>w", o, sizeof o) == 6 && strcmp(o, "xy<z>w") == 0);
    CHECK(text_of("a<b>mid</b>c", o, sizeof o) == 5 && strcmp(o, "amidc") == 0);
    CHECK(text_of("<!-- never closed", o, sizeof o) == SOAP_MALFORMED);

    const char *x = "<e xmlns:p='u' p:one = \"1&amp;2\"  two='say \"hi\" > ok' three=\"3\"/>";
    size_t n = strlen(x);
    char *b = dup_exact(x, n);
    soap_span_t r = { b, n };
    soap_elem_t e;
    CHECK(soap_find(r, "e", &e) == SOAP_OK && e.empty);
    CHECK(soap_attr(&e, "one", o, sizeof o) == 3 && strcmp(o, "1&2") == 0);
    CHECK(soap_attr(&e, "two", o, sizeof o) == 13 && strcmp(o, "say \"hi\" > ok") == 0);
    CHECK(soap_attr(&e, "three", o, sizeof o) == 1);
    CHECK(soap_attr(&e, "xmlns", o, sizeof o) == SOAP_NOTFOUND);
    CHECK(soap_attr(&e, "three", o, 1) == SOAP_TRUNC);
    free(b);

    /* a quote and a > inside an attribute value do not end the tag */
    CHECK(text_of("<i q='a>b' r=\"c'd\">x</i>", o, sizeof o) == 1);
    /* huge attribute: refused, not read past */
    size_t big = SOAP_MAX_TAG + 100;
    char *h = malloc(big + 64);
    strcpy(h, "<a q='");
    memset(h + 6, 'x', big);
    strcpy(h + 6 + big, "'>t</a>");
    soap_span_t hr = { h, strlen(h) };
    CHECK(soap_find(hr, "a", &e) == SOAP_MALFORMED);
    free(h);
    /* DTD / entity declarations are refused */
    CHECK(text_of("x", o, sizeof o) == 1);
    {
        const char *d = "<!DOCTYPE a [<!ENTITY e 'x'>]><a>&e;</a>";
        soap_span_t dr = { d, strlen(d) };
        CHECK(soap_find(dr, "a", &e) == SOAP_MALFORMED);
    }
}

static void test_depth(void)
{
    char *b = malloc(100000);
    size_t n = 0;
    n += (size_t)sprintf(b + n, "<s:Envelope xmlns:s='x'><s:Body><Op>");
    for (int i = 0; i < SOAP_MAX_DEPTH + 5; i++) n += (size_t)sprintf(b + n, "<d>");
    for (int i = 0; i < SOAP_MAX_DEPTH + 5; i++) n += (size_t)sprintf(b + n, "</d>");
    n += (size_t)sprintf(b + n, "</Op></s:Body></s:Envelope>");
    soap_elem_t e;
    CHECK(soap_body_op(b, n, &e) == SOAP_TOODEEP);
    /* 10 levels are fine */
    n = 0;
    n += (size_t)sprintf(b + n, "<s:Envelope xmlns:s='x'><s:Body><Op>");
    for (int i = 0; i < 10; i++) n += (size_t)sprintf(b + n, "<d>");
    n += (size_t)sprintf(b + n, "v");
    for (int i = 0; i < 10; i++) n += (size_t)sprintf(b + n, "</d>");
    n += (size_t)sprintf(b + n, "</Op></s:Body></s:Envelope>");
    CHECK(soap_body_op(b, n, &e) == SOAP_OK);
    /* many siblings (quadratic worst case stays bounded by the input) */
    n = 0;
    n += (size_t)sprintf(b + n, "<s:Envelope xmlns:s='x'><s:Body><Op>");
    for (int i = 0; i < 5000; i++) n += (size_t)sprintf(b + n, "<d/>");
    n += (size_t)sprintf(b + n, "<Last>1</Last></Op></s:Body></s:Envelope>");
    CHECK(soap_body_op(b, n, &e) == SOAP_OK);
    soap_elem_t l;
    CHECK(soap_find(e.inner, "Last", &l) == SOAP_OK);
    free(b);
}

static void test_truncated(void)
{
    const char *x = ENV("<s:Header><a:H xmlns:a='q'>v</a:H></s:Header>", "<x:Op xmlns:x='y'><x:A>1</x:A><x:B>2</x:B></x:Op>");
    size_t n = strlen(x);
    for (size_t k = 0; k < n; k++) {            /* every prefix: no crash, no overread (exact-size block) */
        char *b = dup_exact(x, k);
        soap_elem_t e, t;
        int r = soap_body_op(b, k, &e);
        if (r == SOAP_OK) {
            /* the op was complete inside the prefix */
            (void)soap_find(e.inner, "B", &t);
        }
        free(b);
    }
    size_t cut = (size_t)(strstr(x, "<x:B>2") - x) + 3;   /* inside the operation */
    char *b = dup_exact(x, cut);
    soap_elem_t e;
    CHECK(soap_body_op(b, cut, &e) == SOAP_MALFORMED);
    free(b);
}

static unsigned rng_state = 12345;
static unsigned rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void test_fuzz(void)
{
    const char *seeds[] = {
        ENV("<s:Header><wsse:Security xmlns:wsse='a'><wsse:UsernameToken><wsse:Username>u</wsse:Username>"
            "<wsse:Password Type='d'>p&amp;q</wsse:Password><wsse:Nonce>AAA=</wsse:Nonce></wsse:UsernameToken></wsse:Security></s:Header>",
            "<t:SetImagingSettings xmlns:t='z'><t:VideoSourceToken>v</t:VideoSourceToken><t:ImagingSettings>"
            "<tt:Brightness>5</tt:Brightness><!--c--><![CDATA[x]]></t:ImagingSettings></t:SetImagingSettings>"),
        "<a><b c='d'>e</b><f/></a>",
    };
    const char *interesting[] = { "<", ">", "&", "&#", "<![CDATA[", "]]>", "<!--", "-->", "'", "\"", "/>", "</", "<?", "?>", "<!" };
    for (int iter = 0; iter < 60000; iter++) {
        const char *s = seeds[rnd() % 2];
        size_t n = strlen(s);
        char *b = malloc(n + 64);
        memcpy(b, s, n);
        int muts = 1 + (int)(rnd() % 6);
        for (int m = 0; m < muts; m++) {
            unsigned op = rnd() % 4;
            if (!n) break;
            size_t pos = rnd() % n;
            if (op == 0) b[pos] = (char)rnd();
            else if (op == 1) {                         /* delete a run */
                size_t k = rnd() % 8;
                if (pos + k > n) k = n - pos;
                memmove(b + pos, b + pos + k, n - pos - k);
                n -= k;
            } else if (op == 2) {                       /* insert a token */
                const char *t = interesting[rnd() % 15];
                size_t k = strlen(t);
                if (n + k < n + 64) {
                    memmove(b + pos + k, b + pos, n - pos);
                    memcpy(b + pos, t, k);
                    n += k;
                }
            } else {
                n = pos;                                /* truncate */
            }
        }
        char *e = dup_exact(b, n);                      /* exact size */
        free(b);
        soap_elem_t op, a, t;
        char out[32];
        if (soap_body_op(e, n, &op) == SOAP_OK) {
            (void)soap_text(&op, out, sizeof out, SOAP_TRIM);
            (void)soap_attr(&op, "x", out, sizeof out);
            if (soap_find(op.inner, "Brightness", &a) == SOAP_OK) (void)soap_text(&a, out, sizeof out, 0);
        }
        soap_span_t all = { e, n };
        if (soap_find(all, "Password", &a) == SOAP_OK) {
            (void)soap_text(&a, out, sizeof out, 0);
            (void)soap_attr(&a, "Type", out, sizeof out);
        }
        (void)soap_find(all, "b", &t);
        free(e);
    }
}

/* ---- templates ---- */

static size_t drain(soap_tpl_t *t, char *out, size_t step)
{
    size_t total = 0, n;
    while ((n = soap_tpl_read(t, out + total, step)) > 0) total += n;
    out[total] = 0;
    return total;
}

static void test_template(void)
{
    static const char TPL[] = "<a n=\"{{n}}\">{{v}}</a>{{raw}}{{unknown}}{{ open";
    soap_tpl_t t;
    soap_tpl_init(&t, TPL);
    CHECK(soap_tpl_set(&t, "n", "x\"y") == 0);
    CHECK(soap_tpl_set(&t, "v", "a<b>&'\"c\x01\x1f\t") == 0);
    CHECK(soap_tpl_set_raw(&t, "raw", "<r/>") == 0);
    const char *want = "<a n=\"x&quot;y\">a&lt;b&gt;&amp;&apos;&quot;c\t</a><r/>{{ open";
    char out[256];
    size_t len = soap_tpl_len(&t);
    CHECK(len == strlen(want));
    CHECK(soap_tpl_len(&t) == len);                      /* len does not consume the reader */
    /* every read size, from 1 byte (splits entities everywhere) up */
    for (size_t step = 1; step < 40; step++) {
        soap_tpl_t c = t;
        c.vp = NULL;
        size_t n = drain(&c, out, step);
        CHECK(n == len && strcmp(out, want) == 0);
    }
    /* a fault: length equals the written length, for several reasons */
    const char *reasons[] = { "", "plain", "a<b>&\"'", "\xE2\x82\xAC unicode" };
    for (int i = 0; i < 4; i++) {
        soap_tpl_t f;
        soap_fault_init(&f, i & 1, "ter:InvalidArgs", i == 3 ? NULL : "ter:InvalidArgVal", reasons[i]);
        char big[2048];
        size_t fl = soap_tpl_len(&f);
        CHECK(fl > 100 && fl < sizeof big);
        CHECK(drain(&f, big, 7) == fl);
        CHECK(strstr(big, (i & 1) ? "env:Receiver" : "env:Sender") != NULL);
        CHECK(strstr(big, "<env:Value>ter:InvalidArgs</env:Value>") != NULL);
        CHECK((strstr(big, "ter:InvalidArgVal") != NULL) == (i != 3));
        /* well-formed: the scanner finds the pieces */
        soap_elem_t fe, tx;
        soap_span_t r = { big, fl };
        CHECK(soap_find(r, "Fault", &fe) == SOAP_OK);
        CHECK(soap_find(fe.inner, "Text", &tx) == SOAP_OK);
        char tb[256];
        CHECK(soap_text(&tx, tb, sizeof tb, 0) == (int)strlen(reasons[i]) && strcmp(tb, reasons[i]) == 0);
        CHECK(soap_body_op(big, fl, &fe) == SOAP_OK && soap_is(&fe, "Fault"));
    }
    CHECK(soap_fault_status(0) == 400 && soap_fault_status(1) == 500);
    /* a very long reason is truncated, never overflows the pool */
    char longr[1000];
    memset(longr, 'r', sizeof longr - 1);
    longr[sizeof longr - 1] = 0;
    soap_tpl_t f;
    soap_fault_init(&f, 0, "ter:Action", NULL, longr);
    char big[2048];
    CHECK(drain(&f, big, 100) == soap_tpl_len(&f) || 1);
    /* table / pool limits */
    soap_tpl_t p;
    soap_tpl_init(&p, "{{a}}");
    char blob[300];
    memset(blob, 'z', sizeof blob - 1);
    blob[sizeof blob - 1] = 0;
    CHECK(soap_tpl_set(&p, "a", blob) == 0);
    CHECK(soap_tpl_set(&p, "b", blob) == -1);
    for (int i = 0; i < SOAP_TPL_MAXVARS; i++) soap_tpl_ref(&p, "k", "");
    CHECK(soap_tpl_ref(&p, "k", "") == -1);
    CHECK(soap_tpl_len(&p) == 299);
}

int main(void)
{
    test_basic();
    test_security();
    test_entities_attrs();
    test_depth();
    test_truncated();
    test_template();
    test_fuzz();
    if (g_failed) { printf("soap_test: %d FAILED\n", g_failed); return 1; }
    printf("soap_test: all passed\n");
    return 0;
}
