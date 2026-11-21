/* ONVIF of the Rotatable 2K own app (opt-in, default OFF): ONVIF Profile S device + media + PTZ and WS-Discovery on top of the shared core in
 * src/net (http_srv, soap, onvif, wsd, cred). The 2K binding of the model hook of onvif.h (onvif_model_services) is here.
 *
 * Enabling: akam2k.conf `onvif=1` (+ optional `onvif_port=N`) -> `--onvif 1 --onvif-port N`; the control API row `onvif.state` (read only: off | on:<port> |
 * no_password | error:<reason>) shows what runs, `onvif.enabled` / `onvif.port` show what was asked. Nothing runs until then: no thread, no socket, no timer.
 *
 * Port: a plain HTTP port, default A2K_ONVIF_PORT (8000: not 554 RTSP, not 8092 control API, not 22 SSH, not the stock's 5000 / 8554 / 6668). Accepted: 1024..65535
 * except the RTSP port in use, the API port, 8092, 8091, 22 and 554. A bad value keeps ONVIF off (logged).
 *
 * Authentication: the STREAM credentials of the RTSP server (the one-line htdigest file of rtsp_auth.h, the same limiter as RTSP): HTTP Digest (realm akam, qop auth,
 * MD5) on every operation but the pre-authentication ones of ONVIF Core (GetSystemDateAndTime, GetCapabilities, GetServices, GetServiceCapabilities, GetScopes, GetHostname,
 * PTZ GetServiceCapabilities). WS-Security UsernameToken PasswordDigest needs the clear text password, which the 2K does not store (only HA1): it is refused (a client falls back to
 * HTTP Digest). ONVIF is NOT STARTED while the stream password is off (state no_password) or its file is invalid (error:auth_broken): an open PTZ on the LAN is never offered.
 * Should the file disappear while running, the core's rule applies: reads open as for RTSP, every PTZ command refused (write class needs a login).
 *
 * Services (all POST, SOAP 1.2): /onvif/device_service, /onvif/media_service, /onvif/ptz_service. Profiles: `stream0` (main, rtsp://<ip>:<rtsp port>/stream0, H.264 Main
 * 2304x1296) and `stream1` (sub, /stream1, H.264 Baseline 640x360), both with the PTZ configuration `ptzcfg` of node `ptznode`.
 *
 * PTZ (onvif_ptz.c): only the generic velocity space (-1..1) for pan / tilt, no zoom, no absolute / relative move. ContinuousMove -> a2k_ptz_vel_manual with the
 * speed mapped to the motor divisor (ptz_vel.h) and the lease = Timeout clamped to 150..2000 ms (default 450 ms); a repeat refreshes the lease; Stop; GotoHomePosition;
 * GetPresets / GotoPreset (the stored patrol presets, token = name); GetStatus (MoveStatus). Refused with a SOAP fault while parked / privacy or calibrating. */
#ifndef AKAM2K_ONVIF_H
#define AKAM2K_ONVIF_H

#include <stddef.h>

#include "patrol.h"
#include "ptz.h"
#include "rtsp_auth.h"
#include "wsd.h"

#ifndef A2K_ONVIF_PORT
#define A2K_ONVIF_PORT 8000
#endif
#define A2K_ONVIF_STATE_MAX 48
#define A2K_ONVIF_PROFILE_N 2
#define A2K_ONVIF_TOK_PROFILE0 "stream0"
#define A2K_ONVIF_TOK_PROFILE1 "stream1"
#define A2K_ONVIF_TOK_PTZCFG "ptzcfg"
#define A2K_ONVIF_TOK_PTZNODE "ptznode"
#define A2K_ONVIF_VEL_SPACE "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"

typedef struct {
    int enabled;                 /* 0 = off */
    int port;                    /* HTTP port, 0 = A2K_ONVIF_PORT */
    int rtsp_port;               /* the RTSP port in use (stream URIs) */
    int api_port;                /* the control API port (0 = none), refused for ONVIF */
    rtsp_auth_db_t *auth;        /* the credentials of the RTSP server (its limiter too) */
} a2k_onvif_cfg_t;

/* 1 when `port` may serve ONVIF given the other ports of this app. */
int a2k_onvif_port_ok(long port, int rtsp_port, int api_port);

/* Brings the service to the state cfg asks for (starts or stops the HTTP server and WS-Discovery). 0 for off / on, -1 for no_password / error (state says why). Never fatal. */
int a2k_onvif_start(const a2k_onvif_cfg_t *cfg);
void a2k_onvif_stop(void);
void a2k_onvif_state(char *out, size_t cap);   /* off | on:<port> | no_password | error:<reason> */
int a2k_onvif_wanted(int *port);               /* what was asked at the start: 1 enabled (and the port), 0 off */

/* The motor side of the PTZ service, replaceable by the host tests (the real ones are in onvif_model.c). */
typedef struct {
    int (*vel)(int sx, int sy, int dx, int dy, int lease_ms);   /* a2k_ptz_vel_manual: 0 or A2K_PTZ_E_* */
    int (*stop)(void);                                          /* a2k_ptz_manual_stop */
    int (*gate)(void);                                          /* a2k_ptz_manual_gate: 0 or A2K_PTZ_E_* */
    int (*home)(void);                                          /* 0 queued, -1 refused */
    int (*goto_preset)(const char *name);                       /* PP_OK, PP_E_NOTFOUND, PP_E_BUSY */
    int (*preset_at)(unsigned i, char name[PP_NAME_MAX + 1], int *pan, int *tilt);
    void (*info)(a2k_ptz_info_t *out);
} a2k_onvif_ptz_ops_t;
void a2k_onvif_set_ptz_ops(const a2k_onvif_ptz_ops_t *ops);
const a2k_onvif_ptz_ops_t *a2k_onvif_real_ptz_ops(void);   /* onvif_model.c */

/* The services (registered through the model table of onvif.h). */
int a2k_onvif_device_register(void);
int a2k_onvif_media_register(void);
int a2k_onvif_ptz_register(void);

/* Facts the device / media services publish (set by a2k_onvif_start from the config; tests may set them directly). */
void a2k_onvif_set_rtsp_port(int port);
int a2k_onvif_rtsp_port(void);

/* TEST ONLY: WS-Discovery bound elsewhere (wsd_test_t); the time source of GetSystemDateAndTime / GetStatus (Unix seconds, default time()). */
void a2k_onvif_test_set_wsd(const wsd_test_t *t);
extern long long (*a2k_onvif_time_fn)(void);

#endif
