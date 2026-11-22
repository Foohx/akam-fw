/* Stand-ins of the scene detection (scene.c, scene_check) of the Rotatable 2K, linked INSTEAD of it when the feature is switched off (AKAM_FEAT_SCENE := 0 in model.mk). The rest of the app
 * (settings, detection, the HTTP and MQTT bindings, main) calls the feature unconditionally; with it off every call answers "not available": a refusal (-1),
 * an empty value or nothing, never a motion and never a file. Generated from the prototypes of the headers: a signature change fails the build here. */
#include <string.h>

#include "scene.h"
#include "scene_check.h"

void a2k_scene_feed(const uint8_t *y, unsigned w, unsigned h, unsigned stride)
{
    (void)y;
    (void)w;
    (void)h;
    (void)stride;
}

const a2k_ptz_scene_t *a2k_scene_hooks(void)
{
    return NULL;
}

void a2k_scene_set_drift_ref(int on)
{
    (void)on;
}

void a2k_scene_set_min(int permille)
{
    (void)permille;
}

int sc_parse_min(const char *s)
{
    (void)s;
    return 0; /* accepted and ignored: scene_min left in akam2k.conf must not stop the app of a build without the scene check */
}
