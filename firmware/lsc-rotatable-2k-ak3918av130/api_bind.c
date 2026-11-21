/* See api_bind.h. */
#include "api_bind.h"

#include <string.h>

#include "button_rt.h"
#include "log.h"
#include "mqtt_cfg.h" /* A2K_APP_VERSION */
#include "patrol.h"
#include "settings.h"
#include "store.h"

static void log_line(const char *line) { a2k_log("%s", line); }

static const streg_t *be_reg(void) { return a2k_settings_reg(); }
static void be_keys(int n) { a2k_settings_set_keys(n); }

static const a2k_api_backend_t g_backend = {
    be_reg, a2k_settings_exec, a2k_settings_idle, a2k_settings_action_count, a2k_settings_action, a2k_settings_action_find, be_keys,
};

/* PTZ presets and patrol routes (specs/ptz-patrol.md 7): /ptz/<rest> */
static int ptz_match(const char *sub) { return !strncmp(sub, "/ptz/", 5) && a2k_patrol_path(sub + 5); }
static int ptz_handle(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    return a2k_patrol_http(method, sub + 5, body, len, out, cap, retry_s);
}
/* the programmable button: bindings and modes (specs/control-api.md) */
static int btn_match(const char *sub) { return a2k_btn_path(sub + 1); }
static int btn_handle(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    return a2k_btn_http(method, sub + 1, body, len, out, cap, retry_s);
}

static const a2k_api_ext_t g_ext[] = { { ptz_match, ptz_handle }, { btn_match, btn_handle } };

void a2k_api_env_2k(a2k_api_env_t *env)
{
    memset(env, 0, sizeof *env);
    env->keys_path = A2K_API_KEYS;
    env->audit_path = A2K_API_AUDIT;
    env->model = "lsc-rotatable-2k";
    env->version = A2K_APP_VERSION;
    env->log = log_line;
    env->store_read = a2k_store_read;
    env->store_write = a2k_store_write_mode;
    env->backend = &g_backend;
    env->ext = g_ext;
    env->n_ext = sizeof g_ext / sizeof g_ext[0];
}
