/* Registry rows of the user-file options (MQTT, ONVIF, stream password, NTP, microphone): see cfgreg.h. Pure data, grammars and cross checks. */
#include "cfgreg.h"

#include <string.h>

#include "api_keys.h"

static const a2k_cfgreg_backend_t *backend;

void a2k_cfgreg_set_backend(const a2k_cfgreg_backend_t *b) { backend = b; }

static int h_get(const char *key, char *out, size_t cap)
{
    return backend && backend->get ? backend->get(key, out, cap) : -1;
}

static int h_set(const char *key, const char *val, st_apply_t *eff)
{
    (void)eff;
    return backend && backend->set && backend->set(key, val) == 0 ? 0 : -1;
}

int a2k_cfgreg_user_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 1 || n > 32 || (n == 1 && s[0] == '-'))
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '@' || c == '+' || c == '-'))
            return 0;
    }
    return 1;
}

int a2k_cfgreg_host_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 1 || n > 63)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

/* a host name (labels of letters, digits and inner hyphens, at most 253 characters) or a dotted IPv4 address: the grammar of akam_ntp.sh */
static int ntp_name_ok(const char *s, size_t n)
{
    size_t i, lab = 0;
    if (n < 1 || n > 253)
        return 0;
    for (i = 0; i <= n; i++) {
        char c = i < n ? s[i] : '.';
        if (c == '.') {
            if (lab == 0 || lab > 63 || s[i - 1] == '-')
                return 0;
            lab = 0;
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
        if (lab == 0 && c == '-')
            return 0;
        lab++;
    }
    return 1;
}

int a2k_cfgreg_ntp_ok(const char *list)
{
    int cnt = 0;
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!ntp_name_ok(p, n) || ++cnt > 3)
            return 0;
        if (!e)
            break;
        p = e + 1;
        if (!*p)
            return 0; /* a trailing space */
    }
    return cnt >= 1;
}

int a2k_cfgreg_secret_ok(const char *s, size_t min)
{
    size_t i, n = strlen(s);
    if (n < min || n > 128)
        return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
            return 0;
    return 1;
}

/* the value of another key after this PATCH: the pending one, else the stored one (a write-only key has no stored value to show: < 0) */
static int other(const struct streg *reg, const char *key, char *out, size_t cap) { return streg_peek(reg, key, out, cap); }

/* the stream password exists after this PATCH: a user is set (pending, else stored) and not emptied; a new user always comes with its password
 * (checked on rtsp.user), and a stored user means a stored hash */
static int auth_after(const struct streg *reg)
{
    char u[40];
    return streg_peek(reg, "rtsp.user", u, sizeof u) > 0;
}

static int h_check(const struct streg *reg, const char *key, const char *val)
{
    char o[48], cur[48];
    if (strcmp(key, "mqtt.host") == 0)
        return val[0] == '\0' || a2k_cfgreg_host_ok(val) ? 0 : ST_E_CHECK;
    if (strcmp(key, "mqtt.user") == 0)
        return val[0] == '\0' || a2k_cfgreg_user_ok(val) ? 0 : ST_E_CHECK;
    if (strcmp(key, "mqtt.enabled") == 0)
        return val[0] == '1' && !(other(reg, "mqtt.host", o, sizeof o) > 0) ? ST_E_CHECK : 0; /* nothing to connect to */
    if (strcmp(key, "mqtt.password") == 0) {
        if (val[0] == '\0')
            return 0; /* no password */
        if (!a2k_cfgreg_secret_ok(val, 1))
            return ST_E_CHECK;
        return other(reg, "mqtt.user", o, sizeof o) > 0 ? 0 : ST_E_CHECK; /* a password needs a user name */
    }
    if (strcmp(key, "ntp.servers") == 0)
        return val[0] == '\0' || a2k_cfgreg_ntp_ok(val) ? 0 : ST_E_CHECK;
    if (strcmp(key, "rtsp.user") == 0) {
        int pw = streg_peek(reg, "rtsp.password", o, sizeof o) > 0;
        if (val[0] == '\0')
            return pw ? ST_E_CHECK : 0; /* "" removes the password: it cannot come with a password */
        if (!a2k_cfgreg_user_ok(val))
            return ST_E_CHECK;
        if (pw)
            return 0;
        /* the same user as the stored one is a no-op that keeps the password; another user needs its password in the same PATCH (only the password makes the hash) */
        return h_get("rtsp.user", cur, sizeof cur) > 0 && strcmp(cur, val) == 0 && h_get("rtsp.auth", o, sizeof o) > 0 && strcmp(o, "on") == 0 ? 0 : ST_E_CHECK;
    }
    if (strcmp(key, "rtsp.password") == 0) {
        if (!a2k_cfgreg_secret_ok(val, 8))
            return ST_E_CHECK; /* 8 to 128 characters; an empty one is refused: rtsp.user="" removes the password */
        return streg_peek(reg, "rtsp.user", o, sizeof o) > 0 ? 0 : ST_E_CHECK; /* a user is needed (stored or in this PATCH) */
    }
    if (strcmp(key, "onvif.enabled") == 0)
        return val[0] == '1' && !auth_after(reg) ? ST_E_CHECK : 0; /* ONVIF logs in with the stream credentials: no password, no ONVIF */
    return 0;
}

#define RW (ST_ACC_READ | ST_ACC_WRITE)
#define RO ST_ACC_READ
#define HTTP ST_EXPOSE_HTTP

/* key, type, lo, hi, opts, def, apply, persist, access, expose, secret, min_scope, model_mask, cap_req, get, set, check */
const st_def_t a2k_cfgreg_defs[A2K_CFGREG_N] = {
    {"audio.mic_enable", ST_BOOL, 0, 0, NULL, "0", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"mqtt.enabled", ST_BOOL, 0, 0, NULL, "0", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"mqtt.host", ST_STRING, 0, 63, NULL, "", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"mqtt.port", ST_INT, 1, 65535, NULL, "1883", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"mqtt.user", ST_STRING, 0, 32, NULL, "", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"mqtt.password", ST_STRING, 0, 128, NULL, NULL, AP_RESTART_COMPONENT, 0, ST_ACC_WRITE, HTTP, ST_SECRET_WO, AKEY_ADMIN, 0, 0, NULL, h_set, h_check},
    {"onvif.enabled", ST_BOOL, 0, 0, NULL, "0", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"onvif.port", ST_INT, 1024, 65535, NULL, "8000", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"rtsp.user", ST_STRING, 0, 32, NULL, "", AP_LIVE, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"rtsp.password", ST_STRING, 0, 128, NULL, NULL, AP_LIVE, 0, ST_ACC_WRITE, HTTP, ST_SECRET_WO, AKEY_ADMIN, 0, 0, NULL, h_set, h_check},
    {"ntp.servers", ST_STRING, 0, 200, NULL, "", AP_RESTART_COMPONENT, 0, RW, HTTP, 0, AKEY_ADMIN, 0, 0, h_get, h_set, h_check},
    {"rtsp.auth", ST_STRING, 1, 8, NULL, "off", AP_LIVE, 0, RO, HTTP, 0, 0, 0, 0, h_get, NULL, NULL},
    {"cfg.apply", ST_STRING, 1, 16, NULL, "idle", AP_LIVE, 0, RO, HTTP, 0, 0, 0, 0, h_get, NULL, NULL},
};
