/* Registry rows of the network settings: see akam2k_netreg.h. Pure data and three thin hooks; no I/O of its own. */
#include "akam2k_netreg.h"

#include <string.h>

static const a2k_netreg_backend_t *backend;

void a2k_netreg_set_backend(const a2k_netreg_backend_t *b)
{
    backend = b;
}

int a2k_netreg_hostname_ok(const char *s)
{
    size_t i, n = s ? strlen(s) : 0;
    int digit_only = 1;

    if (n < 1 || n > 32 || s[0] == '-' || s[n - 1] == '-')
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')
            continue;
        digit_only = 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'))
            return 0;
    }
    return !digit_only;
}

static int h_get(const char *key, char *out, size_t cap)
{
    return backend && backend->get ? backend->get(key, out, cap) : ST_E_GETTER;
}

static int h_set(const char *key, const char *val, st_apply_t *effective)
{
    (void)effective; /* AP_LIVE as declared */
    return backend && backend->set && backend->set(key, val) == 0 ? 0 : ST_E_SETTER;
}

/* the string rules of the scripts: refuse here what akam_net.sh would refuse */
static int h_check(const struct streg *reg, const char *key, const char *val)
{
    (void)reg;
    if (strcmp(key, "net.hostname") == 0)
        return a2k_netreg_hostname_ok(val) ? 0 : ST_E_CHECK;
    if (strcmp(key, "net.wifi_country") == 0)
        return val[0] >= 'A' && val[0] <= 'Z' && val[1] >= 'A' && val[1] <= 'Z' && val[2] == '\0' ? 0 : ST_E_CHECK;
    return 0;
}

static const char *const OPT_BAND[] = {"auto", "2.4", "5", NULL};
#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define HM (ST_EXPOSE_HTTP | ST_EXPOSE_MQTT)

const st_def_t a2k_netreg_defs[A2K_NETREG_N] = {
    {"net.hostname", ST_STRING, 1, 32, NULL, "akam-rot2k", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, h_check},
    {"net.wifi_country", ST_STRING, 2, 2, NULL, "FR", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, h_check},
    {"net.wifi_band", ST_ENUM, 0, 0, OPT_BAND, "auto", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, NULL},
};
