/* Registry rows of the network settings of the 2K (specs/control-api.md section 2): net.hostname, net.wifi_country,
 * net.wifi_band. DATA ONLY and NOT wired to the HTTP API yet. The app never spawns a process (anti-brick scan), so the
 * get/set hooks call a backend the caller installs with a2k_netreg_set_backend(): the future wiring runs
 * `akam_net.sh hostname show|set NAME` / `akam_net.sh wifi show|set country XX|set band B` (overlay/akam/akam_net.sh,
 * which validates again, persists in /config/akam/hostname and /config/akam/net, and owns the Wi-Fi dead-man). The values
 * are therefore not persisted in the registry file (persist = 0): the script files are the single source of truth.
 * Apply mode AP_LIVE: the change is in effect when the set hook returns (hostname) or is confirmed by the dead-man
 * within 60 s (Wi-Fi; the script reverts a change that loses the network). */
#ifndef AKAM2K_NETREG_H
#define AKAM2K_NETREG_H
#include <stddef.h>

#include "settings_reg.h"

#define A2K_NETREG_N 3

extern const st_def_t a2k_netreg_defs[A2K_NETREG_N];

/* get: write the current text of key into out (cap bytes), return its length or < 0.
 * set: apply val for key, return 0 or < 0. */
typedef struct {
    int (*get)(const char *key, char *out, size_t cap);
    int (*set)(const char *key, const char *val);
} a2k_netreg_backend_t;

void a2k_netreg_set_backend(const a2k_netreg_backend_t *b);

/* RFC 1123 label as akam_hostname.sh enforces it (1..32 of A-Za-z0-9-, no leading/trailing '-', not all digits);
 * used by the check hook so the registry refuses what the script would refuse. 1 = valid. */
int a2k_netreg_hostname_ok(const char *s);

#endif
