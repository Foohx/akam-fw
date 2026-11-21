/* Host unit test of the network registry rows (netreg.c): table valid, strict values, the check hooks, the set
 * and get hooks through a fake backend. Prints "rot2k netreg unit: N failed". */
#include <stdio.h>
#include <string.h>

#include "netreg.h"
#include "api_keys.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static char last_key[64], last_val[64];
static const char *mock_country = "FR", *mock_band = "2.4";
static int nset, set_rc;

static int b_get(const char *key, char *out, size_t cap)
{
    const char *v = !strcmp(key, "net.hostname") ? "cam-a" : !strcmp(key, "net.wifi_country") ? mock_country : mock_band;
    if (strlen(v) + 1 > cap)
        return ST_E_TOOBIG;
    strcpy(out, v);
    return (int)strlen(v);
}

static int b_set(const char *key, const char *val)
{
    snprintf(last_key, sizeof last_key, "%s", key);
    snprintf(last_val, sizeof last_val, "%s", val);
    nset++;
    return set_rc;
}

int main(void)
{
    streg_t r;
    st_result_t res;
    size_t bad = 99;
    char out[STREG_VAL_MAX];
    static const a2k_netreg_backend_t be = {b_get, b_set};
    int n;

    CHECK(streg_init(&r, a2k_netreg_defs, A2K_NETREG_N, 1u, 0, &bad) == 0, "the three rows form a valid table");
    CHECK(streg_find(&r, "net.hostname") && streg_find(&r, "net.wifi_country") && streg_find(&r, "net.wifi_band"), "all keys found");
    CHECK(streg_find(&r, "net.hostname")->apply == AP_LIVE && streg_find(&r, "net.wifi_band")->apply == AP_LIVE, "apply live");
    CHECK(!streg_find(&r, "net.hostname")->persist && !streg_find(&r, "net.wifi_band")->persist, "not persisted in the registry file (the scripts own it)");

    a2k_netreg_set_backend(&be);
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "Cam-B", &res) == 200 || res.ok == 0, "uppercase hostname reaches the check");
    n = nset;
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "cam-b", &res) == 200 && res.ok && res.apply == AP_LIVE, "valid hostname applied live");
    CHECK(nset == n + 1 && !strcmp(last_key, "net.hostname") && !strcmp(last_val, "cam-b"), "backend got the hostname");

    n = nset;
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "-bad", &res) != 200 && res.err == ST_E_CHECK, "leading dash refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "bad-", &res) != 200, "trailing dash refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "12345", &res) != 200, "digits only refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "a_b", &res) != 200, "underscore refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "", &res) != 200, "empty refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "abcdefghijklmnopqrstuvwxyz0123456", &res) != 200, "33 chars refused");
    CHECK(nset == n, "no refused value reached the backend");
    CHECK(a2k_netreg_hostname_ok("a") && a2k_netreg_hostname_ok("a1") && a2k_netreg_hostname_ok("1a") && !a2k_netreg_hostname_ok(NULL), "hostname_ok edge cases");

    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "DE", &res) == 200 && !strcmp(last_val, "DE"), "country DE");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "de", &res) != 200, "lowercase country refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "FRA", &res) != 200, "three letters refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "F1", &res) != 200, "digit refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "5", &res) == 200 && !strcmp(last_val, "5"), "band 5");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "2.4", &res) == 200, "band 2.4");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "6", &res) != 200, "band 6 refused");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "auto", &res) != 200 && res.err == ST_E_RANGE, "band auto refused (removed: 2.4 or 5)");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "", &res) != 200, "empty band refused");

    n = nset;
    mock_country = "none";
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "5", &res) != 200 && res.err == ST_E_CHECK && nset == n, "band 5 refused while the country is none");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "2.4", &res) == 200, "band 2.4 without a country");
    mock_country = "FR";
    mock_band = "5";
    n = nset;
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "none", &res) != 200 && res.err == ST_E_CHECK && nset == n, "country none refused while on band 5");
    mock_band = "2.4";
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_country", "none", &res) == 200, "country none on band 2.4");

    set_rc = -1;
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.wifi_band", "5", &res) != 200 && res.err == ST_E_SETTER, "a failing backend is a setter error");
    set_rc = 0;
    CHECK(streg_get(&r, ST_SRC_HTTP, "net.hostname", out, sizeof out) == 5 && !strcmp(out, "cam-a"), "get goes through the backend");
    a2k_netreg_set_backend(NULL);
    CHECK(streg_get(&r, ST_SRC_HTTP, "net.hostname", out, sizeof out) < 0, "no backend: get fails cleanly");
    CHECK(streg_set(&r, ST_SRC_HTTP, AKEY_WRITE, "net.hostname", "cam-c", &res) != 200, "no backend: set fails cleanly");

    printf("rot2k netreg unit: %d failed\n", fails);
    return fails != 0;
}
