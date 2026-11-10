/* ONVIF Device service at /onvif/device_service (specs/07 section 3.1 and 3.2). Operations: GetDeviceInformation,
 * GetCapabilities, GetServices, GetServiceCapabilities, GetSystemDateAndTime, SetSystemDateAndTime (Manual only),
 * GetHostname, SetHostname, GetNetworkInterfaces, GetScopes, GetNTP, SetNTP (refused: no NTP client in the image),
 * GetDNS (read only), GetUsers (the one user), SystemReboot. Access classes in onvif.h.
 *
 * Events are NOT advertised (GetCapabilities has no Events element, GetServices no events service). Media and
 * Imaging XAddrs are advertised (they are the next tasks of the same spec). */
#pragma once
#include <stdint.h>

#ifndef ONVIF_HOSTNAME_FILE
#define ONVIF_HOSTNAME_FILE "/etc/config/akam/hostname"
#endif
#ifndef ONVIF_RESOLV_FILE
#define ONVIF_RESOLV_FILE "/etc/resolv.conf"
#endif
#define ONVIF_MODEL "LSC Smart Indoor (AK3918AV130)"
#define ONVIF_HARDWARE_ID "AK3918AV130"

/* Called by onvif_start. 0 or -1. */
int onvif_device_register(void);

/* ---- hooks, replaceable by tests ---- */
extern const char *onvif_hostname_path;           /* default ONVIF_HOSTNAME_FILE */
extern const char *onvif_resolv_path;             /* default ONVIF_RESOLV_FILE */
extern int64_t (*onvif_time_fn)(void);            /* Unix seconds, default time() */
extern int (*onvif_settime_fn)(int64_t sec);      /* default settimeofday; 0 or -1 */
/* Called after the SystemReboot reply is built (the reboot is the hook's job). Default: one detached thread that
 * waits 1 s (so the reply leaves), then sync() and reboot(RB_AUTOBOOT). */
extern void (*onvif_reboot_fn)(void);

/* The hostname exactly as akam_hostname.sh `show` prints it: the stored valid name, else akam-XXXX (last 4 hex digits
 * of the MAC), else "akam". out >= 33 bytes. */
void onvif_hostname_get(char *out);
/* The helper's rules: RFC 1123 label, 1..32 of A-Za-z0-9-, no leading/trailing -, not all digits. */
int onvif_hostname_valid(const char *name);
