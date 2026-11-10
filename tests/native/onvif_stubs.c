/* Stand-ins for the camera side of the ONVIF Media and Imaging services (audio_out.c, control.c), shared by
 * tests/native/onvif_test.c and onvif_media_test.c. State is plain globals the tests set and inspect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"

int g_stub_talk;                 /* audio_out_talk_available() */
int g_stub_volume = 50;          /* audio.out_volume */
int g_stub_isp[4] = { 0, 0, 0, 0 };   /* brightness, contrast, saturation, sharpness */
char g_stub_night[8] = "auto";
int g_stub_fail_key;             /* 1: control_apply_setting refuses */
char g_stub_log[64][48];         /* "key=value/pN" in call order (N = persist argument) */
int g_stub_nlog;
int g_stub_persist_calls;

int audio_out_talk_available(void) { return g_stub_talk; }

int audio_out_ctl_get(const char *key, char *out, size_t cap)
{
    if (strcmp(key, "audio.out_volume") != 0) return -1;
    snprintf(out, cap, "audio.out_volume=%d", g_stub_volume);
    return 0;
}

static int *isp_slot(const char *key)
{
    if (!strcmp(key, "isp.brightness")) return &g_stub_isp[0];
    if (!strcmp(key, "isp.contrast")) return &g_stub_isp[1];
    if (!strcmp(key, "isp.saturation")) return &g_stub_isp[2];
    if (!strcmp(key, "isp.sharpness")) return &g_stub_isp[3];
    return NULL;
}

int control_get_setting(const char *key, char *out, size_t cap)
{
    int *s = isp_slot(key);
    if (s) { snprintf(out, cap, "%d", *s); return 0; }
    if (!strcmp(key, "night.mode")) { snprintf(out, cap, "%s", g_stub_night); return 0; }
    return -1;
}

int control_apply_setting(const char *key, const char *value, int persist, char *msg, size_t cap)
{
    if (g_stub_nlog < 64) snprintf(g_stub_log[g_stub_nlog++], sizeof(g_stub_log[0]), "%s=%s/p%d", key, value, persist);
    if (g_stub_fail_key) { snprintf(msg, cap, "ERR refused"); return -1; }
    int *s = isp_slot(key);
    if (s) {
        int v = atoi(value);
        if (v < -50 || v > 50) { snprintf(msg, cap, "ERR range"); return -1; }
        *s = v;
    } else if (!strcmp(key, "night.mode")) {
        snprintf(g_stub_night, sizeof(g_stub_night), "%s", value);
    } else {
        snprintf(msg, cap, "ERR unknown");
        return -1;
    }
    snprintf(msg, cap, "OK %s=%s", key, value);
    return 0;
}

int control_persist_picture(void)
{
    g_stub_persist_calls++;
    return 0;
}
