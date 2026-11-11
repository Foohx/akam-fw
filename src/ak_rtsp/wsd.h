/* WS-Discovery responder for ONVIF (spec 07 section 3.1 and 3.2): SOAP 1.2 over UDP, multicast group
 * 239.255.255.250 port 3702. DORMANT until wsd_start(): no socket, no thread, no timer.
 *
 * One thread (niced +10), one poll() loop. It joins the multicast group (checked every 30 s, dropped and joined again every 5 minutes to send a fresh IGMP report, retried every 5 s
 * while it fails, which covers Wi-Fi reconnects; a new join triggers a Hello), answers Probe with ProbeMatches and
 * Resolve with ResolveMatches by UNICAST to the sender only, after a random 0-500 ms delay (WS-Discovery
 * APP_MAX_DELAY) held in a bounded queue of WSD_MAX_PENDING jobs, so the loop never sleeps. Beyond the queue or
 * WSD_RATE_PER_SEC messages per second the request is dropped (anti amplification). Datagrams above 8 KB, anything
 * that is not a Probe/Resolve (our own Hello/Bye/ProbeMatches included) and requests that do not match are ignored
 * silently. A Hello is sent (multicast, after 0-500 ms) at start and a Bye at stop.
 *
 * The XAddr is built by the caller's callback from the local address that routes to the sender (connect+getsockname
 * on a throw-away UDP socket), so the IP is right on Wi-Fi, Ethernet and after a DHCP change. */
#pragma once
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define WSD_PORT         3702
#define WSD_GROUP        "239.255.255.250"
#define WSD_MAX_DGRAM    8192   /* larger datagrams are ignored */
#define WSD_MAX_PENDING  8
#define WSD_RATE_PER_SEC 20
#define WSD_MAX_DELAY_MS 500
#define WSD_SRC_RATE     5      /* matching requests per second per source IP (burst 5) */
#define WSD_SRC_SLOTS    8
#define WSD_REJOIN_MS    (5 * 60 * 1000)   /* drop and re-add the group membership: a fresh IGMP report for snooping switches */
#ifndef WSD_INSTANCE_FILE
#define WSD_INSTANCE_FILE "/etc/config/akam/wsd_instance"   /* boot counter, see wsd_next_instance */
#endif
#define WSD_ID_MAX       96
#define WSD_SCOPES_MAX   1024
#define WSD_TYPES_MAX    128
#define WSD_XADDR_MAX    256

/* Fills `out` (NUL-terminated, space separated URLs) for a client reachable through `local`; returns its length or
 * a negative value to stay silent (e.g. HTTP server down). Called from the wsd thread: must be quick. */
typedef int (*wsd_xaddr_fn)(const struct in_addr *local, char *out, size_t cap, void *ctx);

/* TEST-ONLY hook, leave info.test NULL in production: bind this port (0 = ephemeral, see wsd_port()), and send Hello/Bye to
 * another group address / port (0 = default). A group that is not a multicast address is simply not joined. */
typedef struct { uint16_t port; uint16_t notify_port; uint32_t group; /* network order, 0 = default */ } wsd_test_t;

typedef struct {
    const char *uuid;          /* endpoint reference, "urn:uuid:..." */
    const char *scopes;        /* space separated URIs ("onvif://www.onvif.org/Profile/S ...") */
    const char *types;         /* "dn:NetworkVideoTransmitter tds:Device" (default when NULL); only the dn: and tds:
                                * prefixes are declared in the envelope */
    unsigned    metadata_version;
    wsd_xaddr_fn xaddr;
    void       *ctx;
    uint32_t    mcast_if;      /* interface address (network order) for the group, 0 = INADDR_ANY */
    const wsd_test_t *test;
} wsd_info_t;

/* Strings are copied: the caller may free them. 0 on success (also when the group cannot be joined yet), -1 and
 * errno otherwise; a second call while running is a no-op returning 0. */
int  wsd_start(const wsd_info_t *info);
void wsd_stop(void);          /* sends Bye, joins the thread, closes everything */
int  wsd_port(void);          /* bound port, 0 when stopped */

/* ---- pure parts (tested on the host) ---- */
/* The AppSequence InstanceId for this start: it must grow at every reboot (clients take a smaller or equal one for a
 * stale device). max(previous + 1, wall clock seconds when the clock is set, i.e. >= 2024, else 1), where previous
 * is the decimal in `path` (missing or unreadable = 0). The file is rewritten (tmp + rename, one flash write per
 * start of the responder, i.e. per boot) when `path` is non NULL; a failed write is ignored and the call still returns
 * the value. A clock that is unset on every boot therefore still counts 1, 2, 3 ... */
unsigned wsd_next_instance(const char *path, long long wall_s);
/* Per-source request limiter (WSD_SRC_RATE per second per address, burst WSD_SRC_RATE): 1 = allowed. now in ms,
 * exposed so that the test can drive it with its own clock. */
int  wsd_src_ok(uint32_t ip, long long now_ms);

enum { WSD_K_NONE = 0, WSD_K_PROBE = 1, WSD_K_RESOLVE = 2 };
typedef struct {
    int  kind;
    char msgid[WSD_ID_MAX];
    char types[256];           /* Probe/Types text */
    char scopes[512];          /* Probe/Scopes text */
    char matchby[96];          /* Probe/Scopes/@MatchBy, empty = RFC 3986 */
    char epr[WSD_ID_MAX];      /* Resolve/EndpointReference/Address */
} wsd_msg_t;

/* Returns the kind (WSD_K_NONE for anything we do not answer, including a missing MessageID) or a negative
 * SOAP_* code for malformed or oversized input. */
int  wsd_parse(const char *buf, size_t len, wsd_msg_t *m);
/* 1 when we answer this message. Probe: Types empty or any QName whose local part is NetworkVideoTransmitter or
 * Device; Scopes empty or every probe scope matched (RFC 3986 prefix by default, strcmp0 = exact, other rules never). */
int  wsd_match(const wsd_info_t *info, const wsd_msg_t *m);
int  wsd_scope_match(const char *probe_scope, size_t plen, const char *ours, size_t olen, const char *matchby);

enum { WSD_B_PROBEMATCH, WSD_B_RESOLVEMATCH, WSD_B_HELLO, WSD_B_BYE };
/* Builds one message into out. `relates` is used by the two Match kinds. Returns the length, 0 if it does not fit. */
size_t wsd_build(char *out, size_t cap, int kind, const wsd_info_t *info, const char *xaddrs, const char *msgid,
                 const char *relates, unsigned instance, unsigned msgno);
/* "urn:uuid:xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (random, v4); out >= 46 bytes. */
void wsd_uuid(char *out);

/* Token bucket: WSD_RATE_PER_SEC tokens per second, burst WSD_RATE_PER_SEC. */
typedef struct { long long t_ms; int tokens_x1000; } wsd_rl_t;
void wsd_rl_init(wsd_rl_t *r, long long now_ms);
int  wsd_rl_take(wsd_rl_t *r, long long now_ms);
