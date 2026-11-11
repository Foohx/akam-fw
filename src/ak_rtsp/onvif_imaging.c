/* ONVIF Imaging service, see onvif_imaging.h. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "control.h"
#include "onvif.h"
#include "onvif_imaging.h"

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE
#define TOK_VSRC "vsrc"
#define LEVEL_MIN (-50)
#define LEVEL_MAX 50

/* ------------------------------------------------------------------------ debounced flash write */

static int64_t default_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t (*onvif_imaging_now_ms)(void) = default_now_ms;
int onvif_imaging_thread = 1;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pending;           /* picture values changed in RAM, not yet written */
static int64_t g_deadline_ms;   /* write when the clock reaches this */
static int g_running;           /* the flush thread exists */
static unsigned g_flushes;

unsigned onvif_imaging_flushes(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned n = g_flushes;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void flush_now(void)
{
    if (control_persist_picture() != 0) printf("[onvif] imaging: picture settings not saved\n");
}

int onvif_imaging_poll(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_pending || onvif_imaging_now_ms() < g_deadline_ms) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    g_pending = 0;
    g_flushes++;
    pthread_mutex_unlock(&g_mu);
    flush_now();
    return 1;
}

int onvif_imaging_flush_pending(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_pending) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    g_pending = 0;
    g_flushes++;
    pthread_mutex_unlock(&g_mu);
    flush_now();
    return 1;
}

static void *flush_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(250000);
        onvif_imaging_poll();
        pthread_mutex_lock(&g_mu);
        if (!g_pending) {
            g_running = 0;
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
        pthread_mutex_unlock(&g_mu);
    }
}

/* A picture value changed: (re)arm the quiet-time timer, start the thread when none runs. With `force` (ONVIF
 * ForcePersistence) the write is done now. */
static void picture_changed(int force)
{
    pthread_mutex_lock(&g_mu);
    if (force) {
        g_pending = 0;
        g_flushes++;
        pthread_mutex_unlock(&g_mu);
        flush_now();
        return;
    }
    g_pending = 1;
    g_deadline_ms = onvif_imaging_now_ms() + ONVIF_IMAGING_PERSIST_MS;
    int start = onvif_imaging_thread && !g_running;
    if (start) g_running = 1;
    pthread_mutex_unlock(&g_mu);
    if (!start) return;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 32 * 1024);
    if (pthread_create(&th, &at, flush_thread, NULL) != 0) {
        pthread_mutex_lock(&g_mu);
        g_running = 0;      /* the next request polls instead (onvif_imaging_poll in every operation) */
        pthread_mutex_unlock(&g_mu);
    }
    pthread_attr_destroy(&at);
}

/* ------------------------------------------------------------------------ helpers */

static const struct { const char *elem; const char *key; } k_levels[] = {
    { "Brightness", "isp.brightness" },
    { "ColorSaturation", "isp.saturation" },
    { "Contrast", "isp.contrast" },
    { "Sharpness", "isp.sharpness" },
};
#define NLEVELS 4

/* xsd:float text to the nearest integer, without floating point: [sign]digits[.digits]; no exponent, no spaces. */
static int parse_level(const char *s, int *out)
{
    int neg = 0, n = 0;
    long v = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (*s < '0' || *s > '9') return -1;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (++n > 6) return -1;
        v = v * 10 + (*s - '0');
    }
    if (*s == '.') {
        s++;
        int first = -1;
        for (; *s >= '0' && *s <= '9'; s++)
            if (first < 0) first = *s - '0';
        if (first >= 5) v++;
    }
    if (*s) return -1;
    if (neg) v = -v;
    if (v < LEVEL_MIN || v > LEVEL_MAX) return -1;
    *out = (int)v;
    return 0;
}

static int arg_text(soap_span_t range, const char *local, char *out, size_t cap)
{
    soap_elem_t e;
    if (soap_find(range, local, &e) != SOAP_OK) return -1;
    return soap_text(&e, out, cap, SOAP_TRIM) >= 0 ? 0 : -1;
}

static int need_source(onvif_call_t *c)
{
    char tok[24];
    if (arg_text(c->op.inner, "VideoSourceToken", tok, sizeof(tok)) != 0 || strcmp(tok, TOK_VSRC) != 0) {
        onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoSource", "The requested video source token does not exist");
        return 0;
    }
    return 1;
}

static const char *ircut_of(const char *mode)
{
    if (!strcmp(mode, "day")) return "ON";
    if (!strcmp(mode, "night")) return "OFF";
    return "AUTO";
}

/* ------------------------------------------------------------------------ operations */

static void op_service_caps(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization=\"false\" Presets=\"false\""
        " AdaptablePreset=\"false\"/></timg:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_settings(onvif_call_t *c)
{
    onvif_imaging_poll();
    if (!need_source(c)) return;
    char v[NLEVELS][12], mode[12];
    for (int i = 0; i < NLEVELS; i++)
        if (control_get_setting(k_levels[i].key, v[i], sizeof(v[i])) != 0) snprintf(v[i], sizeof(v[i]), "0");
    if (control_get_setting("night.mode", mode, sizeof(mode)) != 0) snprintf(mode, sizeof(mode), "auto");
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<timg:GetImagingSettingsResponse><timg:ImagingSettings>"
        "<tt:Brightness>{{b}}</tt:Brightness><tt:ColorSaturation>{{s}}</tt:ColorSaturation>"
        "<tt:Contrast>{{c}}</tt:Contrast><tt:IrCutFilter>{{ir}}</tt:IrCutFilter>"
        "<tt:Sharpness>{{sh}}</tt:Sharpness></timg:ImagingSettings></timg:GetImagingSettingsResponse>" ENV_CLOSE);
    soap_tpl_set(t, "b", v[0]);
    soap_tpl_set(t, "s", v[1]);
    soap_tpl_set(t, "c", v[2]);
    soap_tpl_set(t, "sh", v[3]);
    soap_tpl_ref(t, "ir", ircut_of(mode));
    onvif_send(c);
}

#define RANGE50 "<tt:Min>-50</tt:Min><tt:Max>50</tt:Max>"
static void op_get_options(onvif_call_t *c)
{
    if (!need_source(c)) return;
    onvif_reply(c, ENV_OPEN
        "<timg:GetOptionsResponse><timg:ImagingOptions>"
        "<tt:Brightness>" RANGE50 "</tt:Brightness><tt:ColorSaturation>" RANGE50 "</tt:ColorSaturation>"
        "<tt:Contrast>" RANGE50 "</tt:Contrast>"
        "<tt:IrCutFilterModes>ON</tt:IrCutFilterModes><tt:IrCutFilterModes>OFF</tt:IrCutFilterModes>"
        "<tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes>"
        "<tt:Sharpness>" RANGE50 "</tt:Sharpness></timg:ImagingOptions></timg:GetOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void settings_invalid(onvif_call_t *c, const char *why)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:SettingsInvalid", why);
}

static void op_set_settings(onvif_call_t *c)
{
    onvif_imaging_poll();
    if (!need_source(c)) return;
    soap_elem_t is, e;
    if (soap_find(c->op.inner, "ImagingSettings", &is) != SOAP_OK) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidArgVal", "ImagingSettings missing");
        return;
    }
    static const char *const unsupported[] = { "WhiteBalance", "Exposure", "Focus", "WideDynamicRange",
                                               "BacklightCompensation" };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++)
        if (soap_find(is.inner, unsupported[i], &e) == SOAP_OK) {
            settings_invalid(c, "Unsupported imaging setting");
            return;
        }
    /* Validate everything first: nothing is applied when one value is wrong. */
    int want[NLEVELS], have_lvl[NLEVELS];
    char tx[16];
    const char *ircut_mode = NULL;
    for (int i = 0; i < NLEVELS; i++) {
        have_lvl[i] = 0;
        if (soap_find(is.inner, k_levels[i].elem, &e) != SOAP_OK) continue;
        if (soap_text(&e, tx, sizeof(tx), SOAP_TRIM) < 1 || parse_level(tx, &want[i]) != 0) {
            settings_invalid(c, "Value out of range (-50 to 50) or not a number");
            return;
        }
        have_lvl[i] = 1;
    }
    if (soap_find(is.inner, "IrCutFilter", &e) == SOAP_OK) {
        if (soap_text(&e, tx, sizeof(tx), SOAP_TRIM) < 1) { settings_invalid(c, "Bad IrCutFilter"); return; }
        if (!strcmp(tx, "ON")) ircut_mode = "day";
        else if (!strcmp(tx, "OFF")) ircut_mode = "night";
        else if (!strcmp(tx, "AUTO")) ircut_mode = "auto";
        else { settings_invalid(c, "IrCutFilter must be ON, OFF or AUTO"); return; }
    }
    int force = 0;
    if (arg_text(c->op.inner, "ForcePersistence", tx, sizeof(tx)) == 0)
        force = !strcmp(tx, "true") || !strcmp(tx, "1");

    char msg[160], cur[16], val[16];
    int changed = 0;
    for (int i = 0; i < NLEVELS; i++) {
        if (!have_lvl[i]) continue;
        snprintf(val, sizeof(val), "%d", want[i]);
        if (control_get_setting(k_levels[i].key, cur, sizeof(cur)) == 0 && !strcmp(cur, val)) continue;
        if (control_apply_setting(k_levels[i].key, val, 0, msg, sizeof(msg)) != 0) {
            if (changed) picture_changed(force);    /* what was applied is kept, like a failed 8091 SET */
            onvif_fault(c, 1, "ter:Action", NULL, "The camera refused the setting");
            return;
        }
        changed = 1;
    }
    if (changed) picture_changed(force);
    if (ircut_mode) {
        if (control_get_setting("night.mode", cur, sizeof(cur)) != 0 || strcmp(cur, ircut_mode) != 0) {
            /* discrete choice: persisted at once (night.c writes only when the mode changed) */
            if (control_apply_setting("night.mode", ircut_mode, 1, msg, sizeof(msg)) != 0) {
                onvif_fault(c, 1, "ter:Action", NULL, "The camera refused the setting");
                return;
            }
        }
    }
    onvif_reply(c, ENV_OPEN "<timg:SetImagingSettingsResponse/>" ENV_CLOSE);
    onvif_send(c);
}

static const onvif_op_t k_ops[] = {
    { "GetServiceCapabilities", ONVIF_PRE, op_service_caps },
    { "GetImagingSettings", ONVIF_READ, op_get_settings },
    { "GetOptions", ONVIF_READ, op_get_options },
    { "SetImagingSettings", ONVIF_WRITE, op_set_settings },
};

int onvif_imaging_register(void)
{
    return onvif_add_service("/onvif/imaging_service", k_ops, sizeof(k_ops) / sizeof(k_ops[0]));
}
