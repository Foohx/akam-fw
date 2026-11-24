/* Host unit test of the user-file registry rows (cfgreg.c): valid table, write-only rows (never read, never echoed, admin only over HTTP),
 * grammars (the ones of the strict MQTT file parser, mqtt_cfg.h, and of akam_ntp.sh), cross-field checks through a fake backend.
 * Prints "rot2k cfgreg unit: N failed". */
#include <stdio.h>
#include <string.h>

#include "cfgreg.h"
#include "api_keys.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static char qk[16][32], qv[16][140];
static int nq;
static char stored_user[40] = "", stored_auth[8] = "off";

static int b_get(const char *key, char *out, size_t cap)
{
    const char *v = !strcmp(key, "rtsp.user") ? stored_user : !strcmp(key, "rtsp.auth") ? stored_auth : "";
    if (strlen(v) + 1 > cap)
        return ST_E_TOOBIG;
    strcpy(out, v);
    return (int)strlen(v);
}

static int b_set(const char *key, const char *val)
{
    if (nq >= 16)
        return -1;
    snprintf(qk[nq], sizeof qk[0], "%s", key);
    snprintf(qv[nq], sizeof qv[0], "%s", val);
    nq++;
    return 0;
}

static int patch(streg_t *r, int scope, size_t n, const char *const *k, const char *const *v, st_result_t *res)
{
    nq = 0;
    return streg_patch(r, ST_SRC_HTTP, scope, n, k, v, res);
}

static int one(streg_t *r, int scope, const char *k, const char *v)
{
    st_result_t res[1];
    return patch(r, scope, 1, &k, &v, res);
}

int main(void)
{
    streg_t r;
    size_t bad = 99;
    char out[STREG_VAL_MAX], body[4096];
    static const a2k_cfgreg_backend_t be = { b_get, b_set };
    st_result_t res[8];
    const char *k8[8], *v8[8];
    int i;

    CHECK(streg_init(&r, a2k_cfgreg_defs, A2K_CFGREG_N, 1u, 0, &bad) == 0, "the rows form a valid table");
    a2k_cfgreg_set_backend(&be);

    /* write-only rows */
    CHECK(streg_find(&r, "mqtt.password")->secret == ST_SECRET_WO && streg_find(&r, "rtsp.password")->secret == ST_SECRET_WO, "the two passwords are write-only rows");
    CHECK(streg_get(&r, ST_SRC_HTTP, "mqtt.password", out, sizeof out) < 0 && streg_get(&r, ST_SRC_LOCAL, "rtsp.password", out, sizeof out) < 0, "a write-only row cannot be read");
    CHECK(streg_json_setting(&r, "mqtt.password", out, sizeof out) < 0, "...nor shown as a setting");
    CHECK(streg_json_values(&r, "", 0, body, sizeof body) > 0 && !strstr(body, "password"), "...nor listed with the values");
    CHECK(streg_json_schema(&r, body, sizeof body) > 0 && !strstr(body, "password"), "...nor in the schema");
    CHECK(one(&r, AKEY_WRITE, "mqtt.password", "abc") != 200, "a write key cannot set a password (admin only)");
    CHECK(one(&r, AKEY_WRITE, "mqtt.host", "x.lan") != 200 && one(&r, AKEY_WRITE, "ntp.servers", "a.b") != 200 && one(&r, AKEY_WRITE, "audio.mic_enable", "1") != 200, "every user-file row needs an admin key");
    CHECK(streg_set(&r, ST_SRC_MQTT, AKEY_ADMIN, "mqtt.host", "x.lan", &res[0]) != 200, "not reachable over MQTT");

    /* MQTT */
    CHECK(one(&r, AKEY_ADMIN, "mqtt.host", "broker.lan") == 200 && nq == 1 && !strcmp(qk[0], "mqtt.host") && !strcmp(qv[0], "broker.lan"), "host reaches the backend");
    CHECK(one(&r, AKEY_ADMIN, "mqtt.host", "bad host") != 200 && one(&r, AKEY_ADMIN, "mqtt.host", "a_b") != 200 && one(&r, AKEY_ADMIN, "mqtt.host", "") == 200, "host grammar; an empty host forgets the broker");
    CHECK(one(&r, AKEY_ADMIN, "mqtt.port", "0") != 200 && one(&r, AKEY_ADMIN, "mqtt.port", "65536") != 200 && one(&r, AKEY_ADMIN, "mqtt.port", "1883") == 200, "port range");
    CHECK(one(&r, AKEY_ADMIN, "mqtt.enabled", "1") != 200, "enabling needs a host (stored or in the same PATCH)");
    k8[0] = "mqtt.host"; v8[0] = "h.lan"; k8[1] = "mqtt.enabled"; v8[1] = "1";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) == 200 && nq == 2, "...in the same PATCH it does");
    CHECK(one(&r, AKEY_ADMIN, "mqtt.user", "-") != 200 && one(&r, AKEY_ADMIN, "mqtt.user", "a b") != 200 && one(&r, AKEY_ADMIN, "mqtt.user", "ha_1@x.y+z") == 200, "user grammar");
    CHECK(one(&r, AKEY_ADMIN, "mqtt.password", "secret") != 200, "a password needs a user");
    k8[0] = "mqtt.user"; v8[0] = "ha"; k8[1] = "mqtt.password"; v8[1] = "sec ret";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) == 200 && nq == 2, "...with the user in the same PATCH it is queued");
    CHECK(streg_json_patch_reply(&r, 2, k8, v8, res, body, sizeof body) > 0 && strstr(body, "mqtt.password") && !strstr(body, "sec ret") && strstr(body, "\"value\":\"ha\""), "the reply echoes the user but never the password");
    v8[1] = "bad\tctl";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) != 200, "a control character in a password is refused");
    CHECK(a2k_cfgreg_host_ok("broker.lan") && a2k_cfgreg_host_ok("1.2.3.4") && a2k_cfgreg_host_ok("a-b.c") && !a2k_cfgreg_host_ok("a_b") && !a2k_cfgreg_host_ok("bad host") && !a2k_cfgreg_host_ok("") && !a2k_cfgreg_host_ok("0123456789012345678901234567890123456789012345678901234567890123"), "host: 1 to 63 of A-Z a-z 0-9 . -");
    CHECK(a2k_cfgreg_user_ok("ha") && a2k_cfgreg_user_ok("u_1@x.y+z-") && !a2k_cfgreg_user_ok("-") && !a2k_cfgreg_user_ok("") && !a2k_cfgreg_user_ok("a:b") && !a2k_cfgreg_user_ok("0123456789012345678901234567890123"), "user: 1 to 32 of A-Z a-z 0-9 . _ @ + -, not a lone -");

    /* stream password and ONVIF */
    stored_user[0] = '\0';
    strcpy(stored_auth, "off");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.user", "bob") != 200, "a user without a password is refused");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.password", "longenough") != 200, "a password without a user is refused");
    k8[0] = "rtsp.user"; v8[0] = "bob"; k8[1] = "rtsp.password"; v8[1] = "short";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) != 200, "a password under 8 characters is refused");
    v8[1] = "long enough";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) == 200 && nq == 2, "user and password together");
    strcpy(stored_user, "bob");
    strcpy(stored_auth, "on");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.password", "another long") == 200, "a new password keeps the stored user");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.user", "bob") == 200, "the same user again is a no-op");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.user", "alice") != 200, "another user needs its password");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.user", "") == 200, "an empty user removes the password");
    k8[0] = "rtsp.user"; v8[0] = ""; k8[1] = "rtsp.password"; v8[1] = "long enough";
    CHECK(patch(&r, AKEY_ADMIN, 2, k8, v8, res) != 200, "...but not together with a password");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.password", "") != 200, "an empty password is refused (use an empty user)");
    CHECK(one(&r, AKEY_ADMIN, "onvif.enabled", "1") == 200, "ONVIF on with a stored stream password");
    stored_user[0] = '\0';
    strcpy(stored_auth, "off");
    CHECK(one(&r, AKEY_ADMIN, "onvif.enabled", "1") != 200, "ONVIF on without a stream password is refused");
    k8[0] = "rtsp.user"; v8[0] = "bob"; k8[1] = "rtsp.password"; v8[1] = "long enough"; k8[2] = "onvif.enabled"; v8[2] = "1";
    CHECK(patch(&r, AKEY_ADMIN, 3, k8, v8, res) == 200, "...with the password in the same PATCH it is accepted");
    CHECK(one(&r, AKEY_ADMIN, "onvif.port", "80") != 200 && one(&r, AKEY_ADMIN, "onvif.port", "8001") == 200, "ONVIF port 1024..65535");

    /* NTP and the microphone */
    CHECK(a2k_cfgreg_ntp_ok("pool.ntp.org") && a2k_cfgreg_ntp_ok("a.b c.d 1.2.3.4") && !a2k_cfgreg_ntp_ok("a b c d") && !a2k_cfgreg_ntp_ok("-x") && !a2k_cfgreg_ntp_ok("a..b") && !a2k_cfgreg_ntp_ok("a b ") && !a2k_cfgreg_ntp_ok("a  b") && !a2k_cfgreg_ntp_ok("x_y"), "the NTP grammar");
    CHECK(one(&r, AKEY_ADMIN, "ntp.servers", "pool.ntp.org time.example.net") == 200 && one(&r, AKEY_ADMIN, "ntp.servers", "") == 200 && one(&r, AKEY_ADMIN, "ntp.servers", "a.b c.d e.f g.h") != 200, "ntp.servers rows");
    CHECK(one(&r, AKEY_ADMIN, "audio.mic_enable", "1") == 200 && one(&r, AKEY_ADMIN, "audio.mic_enable", "2") != 200, "audio.mic_enable");
    CHECK(streg_find(&r, "audio.mic_enable")->def[0] == '0' && streg_find(&r, "mqtt.enabled")->def[0] == '0' && streg_find(&r, "onvif.enabled")->def[0] == '0', "microphone, MQTT and ONVIF are opt-in");
    CHECK(one(&r, AKEY_ADMIN, "rtsp.auth", "on") != 200 && one(&r, AKEY_ADMIN, "cfg.apply", "ok") != 200, "the state rows are read only");
    {
        int none_persisted = 1;
        for (i = 0; i < A2K_CFGREG_N; i++)
            if (a2k_cfgreg_defs[i].persist)
                none_persisted = 0;
        CHECK(none_persisted, "no row is persisted in the registry file (the files are the single source)");
    }
    a2k_cfgreg_set_backend(NULL);
    CHECK(one(&r, AKEY_ADMIN, "mqtt.host", "x.lan") != 200, "no backend: set fails cleanly");

    printf("rot2k cfgreg unit: %d failed\n", fails);
    return fails != 0;
}
