/* ONVIF compiled out (AKAM_FEAT_ONVIF := 0 in the model.mk): the few symbols the rest of the program still calls, with the answers of an ONVIF that
 * is off. Not linked when the feature is on (the model.mk lists onvif_cfg.c, onvif_device.c ... instead), so the default build is unchanged.
 * onvif_hostname_get is also the hostname getter of MQTT, the settings registry and `GET hostname`: it reads the same file as the real one
 * (the "akam-xxxx" fallback of the real one needs the ONVIF MAC helper, so without the file the name is just "akam"). */
#include <stdio.h>
#include <string.h>
#include "onvif_cfg.h"
#include "onvif_device.h"

const char *onvif_cfg_path = ONVIF_CFG_PATH;
const char *onvif_hostname_path = ONVIF_HOSTNAME_FILE;

int onvif_cfg_load(onvif_cfg_t *out)
{
    out->enabled = 0;
    out->port = 0;
    return 1; /* absent: off */
}

int onvif_svc_reload(char *state, size_t cap)
{
    snprintf(state, cap, "off");
    return 0;
}

void onvif_svc_state(char *state, size_t cap) { snprintf(state, cap, "off"); }
void onvif_svc_stop(void) {}

void onvif_hostname_get(char *out)
{
    char line[64] = "";
    FILE *f = fopen(onvif_hostname_path, "re");
    if (f) {
        if (fgets(line, sizeof(line), f)) line[strcspn(line, "\r\n")] = '\0';
        fclose(f);
    }
    if (line[0] && strlen(line) <= 32) memcpy(out, line, strlen(line) + 1);
    else snprintf(out, 33, "akam");
}
