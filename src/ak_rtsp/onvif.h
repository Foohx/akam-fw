/* ONVIF core glue (specs/onvif.md): the service table on top of http_srv, the SOAP dispatch, the
 * shared authentication gate and the device identity (UUID, MAC, interface address).
 *
 * Flow of one request (runs in the http_srv thread, must not block): find the operation (first element of the SOAP
 * Body) in the service's table, ask the gate for its access class, then call the operation, which fills a response
 * template. Unknown operation: Receiver fault ter:Action / ter:ActionNotSupported (HTTP 500, as ONVIF Core pairs it).
 *
 * Access classes (spec 3.3):
 *   ONVIF_PRE    always open: GetSystemDateAndTime, GetCapabilities, GetServices, GetServiceCapabilities,
 *                GetScopes, GetHostname (the ONVIF pre-auth list of spec 3.1)
 *   ONVIF_READ   needs a login when the RTSP password is ON, open when it is OFF
 *   ONVIF_WRITE  needs a login when ON, REFUSED (ter:NotAuthorized) when OFF
 * A login is a valid WS-Security UsernameToken (PasswordDigest) in the SOAP Header, or a valid HTTP Digest
 * Authorization header. WS-Security failure: Sender fault ter:NotAuthorized, HTTP 400. No token and no or a bad
 * Digest: HTTP 401 with a fresh challenge (realm akam, qop auth). When the htdigest file is invalid every
 * protected request is refused (fail closed). Failures go through the shared limiter (cred.h) and the answer is
 * DEFERRED by the returned delay with http_resp_defer: the poll loop is never blocked.
 *
 * HTTP Digest checks beyond rtsp_auth_digest_verify: the uri must equal the request target (path[?query]) or be that target as an absolute http:// URI (host not compared); qop=auth is required; the
 * nonce must be one this module issued (stateless: issue time + keyed hash, valid ONVIF_NONCE_LIFETIME_US; an unknown or old
 * nonce gets a new challenge with stale=true, no penalty); with qop=auth, nc must increase per nonce (nonces that
 * logged in successfully are tracked in a table of ONVIF_NONCES; an nc that is not higher, e.g. out of order on parallel
 * connections, also gets stale=true without penalty). After CRED_PEER_FREE_FAILS failed logins an address is locked
 * out for a growing time (cred_peer_locked): its credentials are not evaluated meanwhile. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "http_srv.h"
#include "soap.h"

#define ONVIF_MAX_SERVICES 4
#define ONVIF_NONCES 32
#define ONVIF_NONCE_LIFETIME_US (120LL * 1000000)
#define ONVIF_SCRATCH 768

#define ONVIF_NS_ENV "http://www.w3.org/2003/05/soap-envelope"
/* Envelope around a response body (string literals: concatenate in templates). */
#define ONVIF_ENV_OPEN \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
    "<env:Envelope xmlns:env=\"" ONVIF_NS_ENV "\"" \
    " xmlns:ter=\"http://www.onvif.org/ver10/error\"" \
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\"" \
    " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"" \
    " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"" \
    " xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"><env:Body>"
#define ONVIF_ENV_CLOSE "</env:Body></env:Envelope>"

/* Capabilities element of the media and imaging services: the body of their GetServiceCapabilities response and,
 * wrapped in <tds:Capabilities>, of GetServices with IncludeCapability. RTP_TCP is RTP over HTTP tunnelling (not
 * offered), RTP_RTSP_TCP is the interleaved RTP/RTSP/TCP that ak_rtsp serves. */
#define ONVIF_MEDIA_CAPS \
    "<trt:Capabilities SnapshotUri=\"false\" Rotation=\"false\"" \
    " VideoSourceMode=\"false\" OSD=\"false\" TemporaryOSDText=\"false\" EXICompression=\"false\">" \
    "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>" \
    "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"false\" RTP_RTSP_TCP=\"true\"" \
    " NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/></trt:Capabilities>"
#define ONVIF_IMAGING_CAPS \
    "<timg:Capabilities ImageStabilization=\"false\" Presets=\"false\" AdaptablePreset=\"false\"/>"

typedef enum { ONVIF_PRE, ONVIF_READ, ONVIF_WRITE } onvif_class_t;

/* One response per request: a template plus a scratch area for fragments built by the operation. */
typedef struct {
    soap_tpl_t t;
    char scratch[ONVIF_SCRATCH];
} onvif_out_t;

typedef struct onvif_call {
    const http_req_t *req;
    http_resp_t      *resp;
    soap_elem_t       op;     /* the operation element; op.inner holds its arguments */
    onvif_out_t      *out;
} onvif_call_t;

typedef void (*onvif_op_fn)(onvif_call_t *c);
typedef struct { const char *name; onvif_class_t cls; onvif_op_fn fn; } onvif_op_t;

/* Register a service at an exact path before the first onvif_start (the ops table must outlive the server). */
int onvif_add_service(const char *path, const onvif_op_t *ops, size_t n);

/* Register the services (once) and start the HTTP server on `port` (0 = pick one). 0 or -1 (errno). */
int  onvif_start(int port);
void onvif_stop(void);

/* ---- for the operations ---- */
/* Start the response: init the template (a static string, ONVIF_ENV_OPEN ... ONVIF_ENV_CLOSE) and return it for
 * soap_tpl_set / _ref / _set_raw. */
soap_tpl_t *onvif_reply(onvif_call_t *c, const char *tpl);
/* Finish it: 200, SOAP content type, body = the template. */
void onvif_send(onvif_call_t *c);
/* A SOAP fault (receiver 0 = Sender, HTTP 400; 1 = Receiver, HTTP 500). */
void onvif_fault(onvif_call_t *c, int receiver, const char *sub1, const char *sub2, const char *reason);
/* The authentication gate for a class; 1 = go on, 0 = the response is already set (fault, 401). */
int  onvif_gate(onvif_call_t *c, onvif_class_t cls);
/* "http://<local ip>[:port]" the client used (getsockname), for XAddr; falls back to the interface address. */
void onvif_base_url(const http_req_t *req, char *out, size_t cap);

/* ---- identity ---- */
/* The interface used for the device identity: wlan0, then eth0 (tests may force one; NULL = automatic). */
extern const char *onvif_ifname;
/* Name of the interface actually used (onvif_ifname, else wlan0, else eth0). */
const char *onvif_if_name(void);
/* MAC of that interface as "aa:bb:cc:dd:ee:ff"; 0, or -1 (out = "00:00:00:00:00:00"). */
int onvif_mac(char out[18]);
/* Interface IPv4 address text and prefix length (netmask), MTU; 0 or -1. */
int onvif_if_ipv4(char ip[16], int *prefix, int *mtu);
/* Stable device UUID "urn:uuid:xxxxxxxx-xxxx-5xxx-yxxx-xxxxxxxxxxxx", name-based (version 5 style: SHA-1 of a fixed
 * namespace and the lowercase MAC), no randomness: the same on every boot and for WS-Discovery and ONVIF alike. */
void onvif_uuid(char out[46]);

/* Test hook: forget the Digest nonce table. */
void onvif_reset(void);
