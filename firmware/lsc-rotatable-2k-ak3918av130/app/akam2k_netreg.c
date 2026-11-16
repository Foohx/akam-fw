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

/* the rules of the scripts: refuse here what akam_net.sh would refuse (band 5 needs a country, and a country cannot be removed while
 * on band 5), so the reply never reports success for a request the script drops. Judged against the stored values of the other key. */
static int h_check(const struct streg *reg, const char *key, const char *val)
{
    char o[16];
    /* the other key: the value of the same PATCH when it has one, else the stored one */
    if (strcmp(key, "net.wifi_band") == 0)
        return strcmp(val, "5") == 0 && streg_peek(reg, "net.wifi_country", o, sizeof o) > 0 && strcmp(o, "none") == 0 ? ST_E_CHECK : 0;
    if (strcmp(key, "net.wifi_country") == 0 && strcmp(val, "none") == 0 && streg_peek(reg, "net.wifi_band", o, sizeof o) > 0 &&
        strcmp(o, "5") == 0)
        return ST_E_CHECK;
    if (strcmp(key, "net.hostname") == 0)
        return a2k_netreg_hostname_ok(val) ? 0 : ST_E_CHECK;
    if (strcmp(key, "net.wifi_country") == 0)
        return strcmp(val, "none") == 0 ||
                       (val[0] >= 'A' && val[0] <= 'Z' && val[1] >= 'A' && val[1] <= 'Z' && val[2] == '\0')
                   ? 0
                   : ST_E_CHECK; /* "none" = no country line (stock behaviour, the default) */
    return 0;
}

static const char *const OPT_BAND[] = {"2.4", "5", NULL};
#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define HM (ST_EXPOSE_HTTP | ST_EXPOSE_MQTT)

const st_def_t a2k_netreg_defs[A2K_NETREG_N] = {
    {"net.hostname", ST_STRING, 1, 32, NULL, "akam-rot2k", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, h_check},
    {"net.wifi_country", ST_STRING, 2, 4, NULL, "none", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, h_check},
    {"net.wifi_band", ST_ENUM, 0, 0, OPT_BAND, "2.4", AP_LIVE, 0, RW, HM, 0, 0, 0, 0, h_get, h_set, h_check},
};
