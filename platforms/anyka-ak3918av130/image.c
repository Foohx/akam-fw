#include "image.h"

#include "isp.h"
#include "log.h"

void a2k_image_defaults(a2k_image_cfg_t *c)
{
    c->brightness = c->contrast = c->sharpness = -1;
    c->mains_hz = 50;
}

int a2k_image_effect_value(int pct)
{
    if (pct < 0 || pct > 100)
        return -1000;
    return (pct == 100 ? 99 : pct) - 50;
}

static int pct_ok(int v) { return v == -1 || (v >= 0 && v <= 100); }

int a2k_image_cfg_ok(const a2k_image_cfg_t *c)
{
    return pct_ok(c->brightness) && pct_ok(c->contrast) && pct_ok(c->sharpness) && (c->mains_hz == 50 || c->mains_hz == 60);
}

static a2k_image_cfg_t g_last; /* what the last valid a2k_image_apply sent (RAM): the replay after a day / night switch */
static int g_last_ok;

int a2k_image_apply(const a2k_image_cfg_t *c)
{
    static const struct { int type; const char *name; } fx[3] = {
        { A2K_FX_BRIGHTNESS, "brightness" }, { A2K_FX_CONTRAST, "contrast" }, { A2K_FX_SHARPNESS, "sharpness" }
    };
    const int v[3] = { c->brightness, c->contrast, c->sharpness };
    int fails = 0;
    if (!a2k_image_cfg_ok(c)) {
        a2k_log("image: invalid settings, nothing applied");
        return 1;
    }
    g_last = *c;
    g_last_ok = 1;
    for (int i = 0; i < 3; i++) {
        if (v[i] < 0)
            continue;
        if (a2k_isp_effect(fx[i].type, a2k_image_effect_value(v[i])) != 0) {
            a2k_log("image: %s %d not applied", fx[i].name, v[i]);
            fails++;
        }
    }
    if (a2k_isp_effect(A2K_FX_MAINS_HZ, c->mains_hz) != 0) {
        a2k_log("image: mains %d Hz not applied", c->mains_hz);
        fails++;
    }
    a2k_log("image: brightness %d, contrast %d, sharpness %d (-1 = conf file), mains %d Hz, %d failure(s)", c->brightness,
            c->contrast, c->sharpness, c->mains_hz, fails);
    return fails;
}

int a2k_image_replay(void)
{
    /* Nothing was set by the user or akam.conf (all three -1): the conf set the switch reloaded is what is wanted, nothing is sent. */
    if (!g_last_ok || (g_last.brightness < 0 && g_last.contrast < 0 && g_last.sharpness < 0))
        return 0;
    return a2k_image_apply(&g_last);
}
