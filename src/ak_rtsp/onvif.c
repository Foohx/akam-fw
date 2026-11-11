/* ONVIF core glue, see onvif.h. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "b64.h"
#include "cred.h"
#include "onvif.h"
#include "onvif_device.h"
#include "onvif_imaging.h"
#include "onvif_media.h"
#include "sha1.h"

/* ------------------------------------------------------------------------ services */

typedef struct { const char *path; const onvif_op_t *ops; size_t n; } service_t;
static service_t g_svc[ONVIF_MAX_SERVICES];
static int g_nsvc;
static int g_registered;

int onvif_add_service(const char *path, const onvif_op_t *ops, size_t n)
{
    if (g_registered || g_nsvc >= ONVIF_MAX_SERVICES || !path || !ops) return -1;
    g_svc[g_nsvc++] = (service_t){ path, ops, n };
    return 0;
}

/* ------------------------------------------------------------------------ responses */

soap_tpl_t *onvif_reply(onvif_call_t *c, const char *tpl)
{
    soap_tpl_init(&c->out->t, tpl);
    return &c->out->t;
}

void onvif_send(onvif_call_t *c)
{
    http_resp_set(c->resp, 200, SOAP_CONTENT_TYPE);
    http_resp_body_stream(c->resp, soap_tpl_len(&c->out->t), soap_tpl_read, &c->out->t);
}

void onvif_fault(onvif_call_t *c, int receiver, const char *sub1, const char *sub2, const char *reason)
{
    soap_fault_init(&c->out->t, receiver, sub1, sub2, reason);
    http_resp_set(c->resp, soap_fault_status(receiver), SOAP_CONTENT_TYPE);
    http_resp_body_stream(c->resp, soap_tpl_len(&c->out->t), soap_tpl_read, &c->out->t);
}

/* ------------------------------------------------------------------------ Digest nonces */

typedef struct {
    char nonce[RTSP_AUTH_NONCE_HEX + 1];
    int64_t issued_us;
    uint32_t last_nc;
    int used;
} nonce_ent_t;
static nonce_ent_t g_nonce[ONVIF_NONCES];
static pthread_mutex_t g_nonce_mu = PTHREAD_MUTEX_INITIALIZER;

void onvif_reset(void)
{
    pthread_mutex_lock(&g_nonce_mu);
    memset(g_nonce, 0, sizeof(g_nonce));
    pthread_mutex_unlock(&g_nonce_mu);
}

/* Issue a nonce into out (RTSP_AUTH_NONCE_HEX + 1); frees an expired slot, else replaces the oldest one. */
static int nonce_issue(char *out)
{
    int64_t now = cred_mono_us();
    if (rtsp_auth_new_nonce(out) != 0) return -1;
    pthread_mutex_lock(&g_nonce_mu);
    nonce_ent_t *slot = NULL;
    for (int i = 0; i < ONVIF_NONCES; i++) {
        nonce_ent_t *e = &g_nonce[i];
        if (e->used && now - e->issued_us > ONVIF_NONCE_LIFETIME_US) e->used = 0;
        if (!e->used) { slot = e; break; }
        if (!slot || e->issued_us < slot->issued_us) slot = e;
    }
    memcpy(slot->nonce, out, RTSP_AUTH_NONCE_HEX + 1);
    slot->issued_us = now;
    slot->last_nc = 0;
    slot->used = 1;
    pthread_mutex_unlock(&g_nonce_mu);
    return 0;
}

enum { NC_OK, NC_UNKNOWN, NC_REPLAY };

/* Is d->nonce one of ours and alive; with qop, is nc higher than the last one used (and remember it). */
static int nonce_use(const rtsp_auth_digest_t *d)
{
    int64_t now = cred_mono_us();
    int rc = NC_UNKNOWN;
    pthread_mutex_lock(&g_nonce_mu);
    for (int i = 0; i < ONVIF_NONCES; i++) {
        nonce_ent_t *e = &g_nonce[i];
        if (!e->used || strcmp(e->nonce, d->nonce) != 0) continue;
        if (now - e->issued_us > ONVIF_NONCE_LIFETIME_US) { e->used = 0; break; }
        if (d->qop) {
            char *end;
            unsigned long nc = strtoul(d->nc, &end, 16);
            if (*end != '\0' || nc == 0 || nc > 0xffffffffUL || (uint32_t)nc <= e->last_nc) { rc = NC_REPLAY; break; }
            e->last_nc = (uint32_t)nc;
        }
        rc = NC_OK;
        break;
    }
    pthread_mutex_unlock(&g_nonce_mu);
    return rc;
}

static void challenge(onvif_call_t *c, int stale, unsigned delay_ms)
{
    char nonce[RTSP_AUTH_NONCE_HEX + 1], v[192];
    http_resp_t *r = c->resp;
    http_resp_set(r, 401, NULL);
    r->body_len = 0;
    r->produce = NULL;
    if (nonce_issue(nonce) != 0) {      /* no random source: no challenge to offer, refuse */
        r->close = 1;
        return;
    }
    snprintf(v, sizeof(v), "Digest realm=\"%s\", qop=\"auth\", nonce=\"%s\", algorithm=MD5%s", RTSP_AUTH_REALM, nonce,
             stale ? ", stale=true" : "");
    http_resp_header(r, "WWW-Authenticate", v);
    if (delay_ms) http_resp_defer(r, delay_ms);
}

static void not_authorized(onvif_call_t *c, unsigned delay_ms)
{
    onvif_fault(c, 0, "ter:NotAuthorized", NULL, "Sender not authorized");
    if (delay_ms) http_resp_defer(c->resp, delay_ms);
}

/* ------------------------------------------------------------------------ authentication gate */

/* The UsernameToken of the SOAP Header (never one inside the Body, where an operation argument could be named so). */
static int find_token(const onvif_call_t *c, soap_elem_t *ut)
{
    soap_elem_t h;
    soap_span_t doc = { c->req->body, c->req->body_len };
    return soap_find(doc, "Header", &h) == SOAP_OK && soap_find(h.inner, "UsernameToken", ut) == SOAP_OK;
}

/* WS-Security UsernameToken of the Header. 0 = none, 1 = valid, -1 = present and refused. */
static int wsse_check(onvif_call_t *c, unsigned *delay)
{
    soap_elem_t ut, u, p, n, cr;
    if (!find_token(c, &ut)) return 0;
    char user[CRED_USER_MAX + 2] = "", nonce[B64_ENCODED_LEN(CRED_NONCE_RAW_MAX) + 2] = "", created[CRED_CREATED_MAX + 2] = "",
         digest[48] = "", type[128] = "";
    int ok = 1;
    ok &= soap_find(ut.inner, "Username", &u) == SOAP_OK && soap_text(&u, user, sizeof(user), SOAP_TRIM) >= 0;
    ok &= soap_find(ut.inner, "Nonce", &n) == SOAP_OK && soap_text(&n, nonce, sizeof(nonce), SOAP_TRIM) >= 0;
    ok &= soap_find(ut.inner, "Created", &cr) == SOAP_OK && soap_text(&cr, created, sizeof(created), SOAP_TRIM) >= 0;
    ok &= soap_find(ut.inner, "Password", &p) == SOAP_OK && soap_text(&p, digest, sizeof(digest), SOAP_TRIM) >= 0;
    /* Only PasswordDigest: a clear-text password over HTTP is never accepted. */
    if (ok) {
        int tl = soap_attr(&p, "Type", type, sizeof(type));
        static const char want[] = "#PasswordDigest";
        ok &= tl >= (int)sizeof(want) - 1 && strcmp(type + tl - (sizeof(want) - 1), want) == 0;
    }
    if (!ok) {                          /* malformed token: a failed login (counted) */
        cred_check_wsse("", "", "", "", delay);
        return -1;
    }
    return cred_check_wsse(user, nonce, created, digest, delay) ? 1 : -1;
}

/* The request target as the client wrote it: path[?query]. */
static int target_of(const http_req_t *r, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s%s%s", r->path, r->query ? "?" : "", r->query ? r->query : "");
    return n > 0 && (size_t)n < cap;
}

/* The digest uri is the request target, or the same as an absolute URI (http://host[:port]target): the host part is
 * not compared (the client may use any name or address of the camera). */
static int uri_matches(const char *uri, const char *target)
{
    if (strcmp(uri, target) == 0) return 1;
    if (strncasecmp(uri, "http://", 7) != 0) return 0;
    const char *p = uri + 7;
    size_t a = strcspn(p, "/?#@ ");
    if (a == 0 || p[a] != '/') return 0;
    return strcmp(p + a, target) == 0;
}

int onvif_gate(onvif_call_t *c, onvif_class_t cls)
{
    if (cls == ONVIF_PRE) return 1;
    rtsp_auth_mode_t mode = cred_auth_mode();
    if (mode == RTSP_AUTH_OFF) {
        if (cls == ONVIF_READ) return 1;
        not_authorized(c, 0);               /* state changes need the password to be on */
        return 0;
    }
    unsigned delay = 0;
    soap_elem_t probe;
    int has_token = find_token(c, &probe);
    if (mode == RTSP_AUTH_BROKEN) {         /* fail closed */
        if (has_token) not_authorized(c, 0); else challenge(c, 0, 0);
        return 0;
    }
    if (has_token) {
        int w = wsse_check(c, &delay);
        if (w > 0) return 1;
        not_authorized(c, delay);
        return 0;
    }
    const char *hdr = c->req->authorization;
    if (!hdr) { challenge(c, 0, 0); return 0; }
    rtsp_auth_digest_t d;
    if (!cred_check_http_digest(c->req->method, hdr, strlen(hdr), &d, &delay)) { challenge(c, 0, delay); return 0; }
    char target[512];
    if (!target_of(c->req, target, sizeof(target)) || !uri_matches(d.uri, target)) {
        cred_note_failure(&delay);          /* right credentials for another URI: replay or confusion */
        challenge(c, 0, delay);
        return 0;
    }
    if (!d.qop) {                           /* no nc to count: a captured request could be replayed, ask again with qop */
        challenge(c, 0, 0);
        return 0;
    }
    switch (nonce_use(&d)) {
    case NC_OK: return 1;
    case NC_REPLAY:
        cred_note_failure(&delay);
        challenge(c, 0, delay);
        return 0;
    default:                                /* not ours or too old: a client that simply has to retry */
        challenge(c, 1, 0);
        return 0;
    }
}

/* ------------------------------------------------------------------------ dispatch */

static void handler(const http_req_t *req, http_resp_t *resp, void *user)
{
    const service_t *s = user;
    onvif_call_t call = { req, resp, { { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, 0 }, NULL };
    call.out = http_resp_alloc(resp, sizeof(onvif_out_t));
    if (!call.out) { http_resp_set(resp, 503, NULL); return; }
    if (soap_body_op(req->body, req->body_len, &call.op) != SOAP_OK) {
        onvif_fault(&call, 0, "ter:InvalidArgs", NULL, "Malformed SOAP request");
        return;
    }
    for (size_t i = 0; i < s->n; i++) {
        if (!soap_is(&call.op, s->ops[i].name)) continue;
        if (!onvif_gate(&call, s->ops[i].cls)) return;
        s->ops[i].fn(&call);
        if (!resp->status) onvif_fault(&call, 1, "ter:Action", NULL, "Internal error");
        return;
    }
    onvif_fault(&call, 0, "ter:ActionNotSupported", NULL, "Operation not supported");
}

int onvif_start(int port)
{
    if (!g_registered) {
        if (onvif_device_register() != 0 || onvif_media_register() != 0 || onvif_imaging_register() != 0) return -1;
        for (int i = 0; i < g_nsvc; i++)
            if (http_srv_route(g_svc[i].path, HTTP_M_POST, handler, &g_svc[i]) != 0) return -1;
        g_registered = 1;
    }
    return http_srv_start(port);
}

void onvif_stop(void) { http_srv_stop(); }

/* ------------------------------------------------------------------------ identity */

const char *onvif_ifname;

static int if_query(unsigned long req, const char *name, struct ifreq *ifr)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    memset(ifr, 0, sizeof(*ifr));
    snprintf(ifr->ifr_name, sizeof(ifr->ifr_name), "%s", name);
    int rc = ioctl(s, req, ifr);
    close(s);
    return rc;
}

const char *onvif_if_name(void)
{
    struct ifreq r;
    if (onvif_ifname) return onvif_ifname;
    if (if_query(SIOCGIFHWADDR, "wlan0", &r) == 0) return "wlan0";
    if (if_query(SIOCGIFHWADDR, "eth0", &r) == 0) return "eth0";
    return "wlan0";
}

int onvif_mac(char out[18])
{
    struct ifreq r;
    if (if_query(SIOCGIFHWADDR, onvif_if_name(), &r) != 0) {
        strcpy(out, "00:00:00:00:00:00");
        return -1;
    }
    const unsigned char *m = (const unsigned char *)r.ifr_hwaddr.sa_data;
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

int onvif_if_ipv4(char ip[16], int *prefix, int *mtu)
{
    struct ifreq r;
    const char *n = onvif_if_name();
    if (if_query(SIOCGIFADDR, n, &r) != 0) return -1;
    struct in_addr a = ((struct sockaddr_in *)&r.ifr_addr)->sin_addr;
    if (!inet_ntop(AF_INET, &a, ip, 16)) return -1;
    *prefix = 24;
    if (if_query(SIOCGIFNETMASK, n, &r) == 0) {
        uint32_t m = ntohl(((struct sockaddr_in *)&r.ifr_netmask)->sin_addr.s_addr);
        int p = 0;
        while (p < 32 && (m & (0x80000000u >> p))) p++;
        *prefix = p;
    }
    *mtu = 1500;
    if (if_query(SIOCGIFMTU, n, &r) == 0 && r.ifr_mtu > 0) *mtu = r.ifr_mtu;
    return 0;
}

void onvif_uuid(char out[46])
{
    static const uint8_t ns[16] = { 0x4f, 0x1b, 0x8c, 0x62, 0xe3, 0x7a, 0x4d, 0x05,
                                    0xa9, 0x21, 0x7c, 0xd3, 0x0e, 0x58, 0xb6, 0x94 };
    char mac[18];
    uint8_t h[SHA1_DIGEST_LEN];
    sha1_ctx c;
    onvif_mac(mac);
    sha1_init(&c);
    sha1_update(&c, ns, sizeof(ns));
    sha1_update(&c, mac, strlen(mac));
    sha1_final(&c, h);
    h[6] = (uint8_t)((h[6] & 0x0f) | 0x50);   /* version 5 */
    h[8] = (uint8_t)((h[8] & 0x3f) | 0x80);   /* RFC 4122 variant */
    snprintf(out, 46, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2],
             h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
}

void onvif_base_url(const http_req_t *req, char *out, size_t cap)
{
    char ip[16] = "";
    int port = http_srv_port();
    if (req && req->local.sin_family == AF_INET && req->local.sin_addr.s_addr != 0)
        inet_ntop(AF_INET, &req->local.sin_addr, ip, sizeof(ip));
    if (!ip[0]) {
        int p, m;
        if (onvif_if_ipv4(ip, &p, &m) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");
    }
    if (port == 80 || port == 0) snprintf(out, cap, "http://%s", ip);
    else snprintf(out, cap, "http://%s:%d", ip, port);
}
