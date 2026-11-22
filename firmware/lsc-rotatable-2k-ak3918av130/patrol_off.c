/* Stand-ins of the PTZ patrol (patrol.c, patrol_run.c, ptz_patrol) of the Rotatable 2K, linked INSTEAD of it when the feature is switched off (AKAM_FEAT_PATROL := 0 in model.mk). The rest of the app
 * (settings, detection, the HTTP and MQTT bindings, main) calls the feature unconditionally; with it off every call answers "not available": a refusal (-1),
 * an empty value or nothing, never a motion and never a file. Generated from the prototypes of the headers: a signature change fails the build here. */
#include <string.h>

#include "patrol.h"
#include "ptz_patrol.h"

int a2k_patrol_active(void)
{
    return 0;
}

void a2k_patrol_get_default(char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
}

int a2k_patrol_get_idle_s(void)
{
    return 0;
}

int a2k_patrol_get_travel_budget(void)
{
    return 0;
}

int a2k_patrol_goto_preset(const char *name)
{
    (void)name;
    return -1;
}

int a2k_patrol_http(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    (void)method;
    (void)sub;
    (void)body;
    (void)len;
    (void)out;
    (void)cap;
    (void)retry_s;
    return 404;
}

void a2k_patrol_note_person(int tracking, int resume_ms)
{
    (void)tracking;
    (void)resume_ms;
}

int a2k_patrol_path(const char *sub)
{
    (void)sub;
    return 0;
}

int a2k_patrol_preset_at(unsigned i, char name[PP_NAME_MAX + 1], int *pan, int *tilt)
{
    (void)i;
    (void)name;
    if (pan) *pan = 0;
    if (tilt) *tilt = 0;
    return -1;
}

int a2k_patrol_run_start(void)
{
    return 0;
}

void a2k_patrol_run_stop(void)
{
}

int a2k_patrol_set_default(const char *name)
{
    (void)name;
    return -1;
}

int a2k_patrol_set_idle_s(int s)
{
    (void)s;
    return -1;
}

int a2k_patrol_set_travel_budget(int steps)
{
    (void)steps;
    return -1;
}

int a2k_patrol_start_route(const char *name)
{
    (void)name;
    return -1;
}

void a2k_patrol_state(char *state, size_t scap, char *route, size_t rcap, int *step)
{
    if (state && scap) state[0] = '\0';
    if (route && rcap) route[0] = '\0';
    if (step) *step = 0;
}

int a2k_patrol_stop_route(void)
{
    return -1;
}

unsigned a2k_patrol_writes(void)
{
    return 0;
}

/* A NAME CHECK, not patrol logic: the settings use it for a button mode name too (mode.apply), so it stays the real rule even with the patrol switched off. */
int pp_name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0, i;
    if (n < 1 || n > PP_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}
