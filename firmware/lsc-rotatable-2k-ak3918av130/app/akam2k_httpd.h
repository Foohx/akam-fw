/* The HTTP server of the local control API (specs/control-api.md 4.1): ONE thread (64 KiB stack, below the video threads), poll
 * based, plain HTTP/1.1, `Connection: close`, no keep-alive, no pipelining, no chunked uploads. Hard limits: 4 connections (3 per peer),
 * head 4 KiB, body 8 KiB, 5 s without progress and 10 s per request while reading, replies written with a 2 s bound. A GET /events that has
 * nothing to report yet is HELD (long poll, up to 25 s) and answered when an event arrives (akam2k_apievt.h wakes the thread through a
 * pipe) or at its deadline. The listener exists only while at least one API key is installed (nothing listens on an unprovisioned
 * camera); the key file is re-read about once a second, so the on-camera tool opens and closes the port without a restart.
 * This file is the only API file that creates a socket, binds, listens or accepts (the anti-brick scan lists it next to
 * akam2k_rtsp.c). */
#ifndef AKAM2K_HTTPD_H
#define AKAM2K_HTTPD_H

#include "akam2k_api.h"

#define A2K_HTTPD_MAX_CONNS 4
#define A2K_HTTPD_MAX_PER_PEER 3
#define A2K_HTTPD_READ_IDLE_MS 5000
#define A2K_HTTPD_REQUEST_MS 10000
#define A2K_HTTPD_WRITE_MS 2000

/* Starts the thread. port 0 = an ephemeral port (tests); loopback_only binds 127.0.0.1 (tests), else 0.0.0.0. env as a2k_api_init.
 * 0 started, -1 failed (logged; the rest of the app goes on). */
int a2k_httpd_start(int port, int loopback_only, const a2k_api_env_t *env);
/* Closes the listener and every connection, joins the thread. Idempotent. */
void a2k_httpd_stop(void);
/* The port the listener is bound to now, 0 while closed (no key). */
int a2k_httpd_port(void);

#endif
