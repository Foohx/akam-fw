/* ONVIF on/off wiring (specs/onvif.md, specs/roadmap.md): the opt-in flag file and the runtime start/stop of
 * the HTTP server (onvif.c + http_srv.c) and WS-Discovery (wsd.c).
 *
 * Flag file /etc/config/akam/onvif, written by akam_onvif.sh: exactly two lines, `enabled=1|0` and `port=N`, any
 * order, LF (a trailing CR is tolerated like the helper does), N a whole number 1..65535 except 21-24, 554 and 8091
 * (the ports of the other Akam services). Absent file = off. Anything else (unknown or repeated key, missing key,
 * bad number, a third line, file over ONVIF_CFG_MAX bytes) = off, with one log line.
 *
 * Nothing runs until onvif_svc_reload(): OFF costs no thread, no socket and no timer. ON costs two threads (HTTP and
 * WS-Discovery), both asleep until traffic arrives: the HTTP poll has no timeout when no connection is open, the
 * WS-Discovery loop wakes every 30 s (5 s while the multicast join fails).
 *
 * States (onvif_svc_state, "GET onvif.state"): off | on:<port> | no_password | error:<reason>.
 *   no_password: enabled, the RTSP password is ON (or its file is broken) but there are no valid clear-text
 *                credentials for the WS-Security login (cred.h): nothing is started. When the RTSP password is OFF
 *                ONVIF does start: reads are open and the writes are refused (spec 3.3, onvif.c).
 *   error:port_in_use | error:permission_denied | error:http_failed | error:discovery_failed.
 * The device name scope comes from the hostname at the time of the reload (a hostname change applies at the next
 * reload or start). The camera IP is not captured: XAddrs are built per request from the address that routes to
 * the client. */
#pragma once
#include <stddef.h>
#include "wsd.h"

#ifndef ONVIF_CFG_PATH /* tests point onvif_cfg_path elsewhere */
#define ONVIF_CFG_PATH "/etc/config/akam/onvif"
#endif
#define ONVIF_CFG_MAX 64
#define ONVIF_STATE_MAX 48

typedef struct { int enabled; int port; } onvif_cfg_t;

extern const char *onvif_cfg_path;
/* TEST-ONLY: passed to wsd_start (bind an unprivileged port, notify elsewhere). NULL in production. */
extern const wsd_test_t *onvif_svc_wsd_test;

/* 1 when `port` may be used for the HTTP server (1..65535, not 21-24, 554, 8091). */
int onvif_cfg_port_ok(long port);
/* Strict parse of the file content. 0 = valid (*out filled), -1 = invalid. */
int onvif_cfg_parse(const char *text, size_t len, onvif_cfg_t *out);
/* Read the file: 0 valid, 1 absent (*out = off), -1 invalid or unreadable (*out = off, one log line). */
int onvif_cfg_load(onvif_cfg_t *out);
/* Percent-encode `in` for a URI path segment (unreserved characters stay). Returns the length, truncating at cap. */
size_t onvif_cfg_urlenc(const char *in, char *out, size_t cap);

/* Boot and `ONVIF reload`: re-read the file and bring the services to the state it asks for (start, stop, restart
 * on a port change; WS-Discovery is restarted on every reload so the name scope is rebuilt). Video is untouched.
 * Fills `state` (>= ONVIF_STATE_MAX) with the new state and returns 0 for off/on, -1 for no_password/error. Thread
 * safe (serialised). */
int  onvif_svc_reload(char *state, size_t cap);
void onvif_svc_state(char *state, size_t cap);
/* Stop everything (Bye is sent). Safe when off. */
void onvif_svc_stop(void);
