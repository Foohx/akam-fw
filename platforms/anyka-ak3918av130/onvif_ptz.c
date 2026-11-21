/* ONVIF PTZ service of the Rotatable 2K (ver20 PTZ, namespace tptz) at /onvif/ptz_service. See onvif_services.h for the contract.
 *
 * Operations: GetServiceCapabilities (open), GetNodes, GetNode, GetConfigurations, GetConfiguration, GetConfigurationOptions, GetStatus, GetPresets (read class);
 * ContinuousMove, Stop, GotoHomePosition, GotoPreset (write class). Every other PTZ operation (AbsoluteMove, RelativeMove, SetPreset, RemovePreset, SetHomePosition, tours...)
 * answers the core's ter:ActionNotSupported: the node lists ONLY the generic velocity space for pan / tilt (no zoom, no position spaces).
 *
 * Faults (SOAP 1.2): unknown ProfileToken -> Sender ter:InvalidArgVal / ter:NoProfile; unknown PresetToken -> ter:InvalidArgVal / ter:NoToken; a velocity space other than the generic one,
 * or a non-zero Zoom -> ter:InvalidArgVal / ter:SpaceNotSupported; a component outside -1..1 or not a number -> ter:InvalidArgVal / ter:InvalidVelocity; a bad Timeout ->
 * ter:InvalidArgVal / ter:InvalidTimeout; the head cannot move now (parked / privacy, calibrating or its position unknown, controller down) -> Receiver ter:Action /
 * ter:PTZUnavailable with the reason in the text (the subcode is Akam's: the ONVIF list has none for "refused by state").
 * No ctype.h (see ctype_compat.h). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "onvif_services.h"
#include "onvif.h"
#include "ptz_vel.h"

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE
#define NSPTZ "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""

static const a2k_onvif_ptz_ops_t *g_ops;
void a2k_onvif_set_ptz_ops(const a2k_onvif_ptz_ops_t *ops) { g_ops = ops; }

static int arg_text(onvif_call_t *c, const char *local, char *out, size_t cap)
{
    soap_elem_t e;
    if (soap_find(c->op.inner, local, &e) != SOAP_OK)
        return -1;
    return soap_text(&e, out, cap, SOAP_TRIM) >= 0 ? 0 : -1;
}

static void fault_noprofile(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoProfile", "The requested profile token does not exist");
}

static int need_profile(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "ProfileToken", tok, sizeof tok) != 0 || (strcmp(tok, A2K_ONVIF_TOK_PROFILE0) && strcmp(tok, A2K_ONVIF_TOK_PROFILE1))) {
        fault_noprofile(c);
        return 0;
    }
    return 1;
}

/* the head cannot take a motion command now: the SOAP fault, never a silent success */
static void refuse(onvif_call_t *c, int rc)
{
    const char *why = rc == A2K_PTZ_E_PARKED ? "PTZ is refused while the camera is parked (privacy mode)"
                      : rc == A2K_PTZ_E_NOTREADY ? "PTZ is not ready (calibrating or position unknown)"
                                                 : "PTZ is not available";
    onvif_fault(c, 1, "ter:Action", "ter:PTZUnavailable", why);
}

/* ---- fragments ---- */
#define SPACES_VEL \
    "<tt:ContinuousPanTiltVelocitySpace><tt:URI>" A2K_ONVIF_VEL_SPACE "</tt:URI>" \
    "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>" \
    "</tt:ContinuousPanTiltVelocitySpace>"
#define NODE_BODY \
    "<tt:Name>" A2K_ONVIF_TOK_PTZNODE "</tt:Name><tt:SupportedPTZSpaces>" SPACES_VEL "</tt:SupportedPTZSpaces>" \
    "<tt:MaximumNumberOfPresets>{{maxp}}</tt:MaximumNumberOfPresets><tt:HomeSupported>true</tt:HomeSupported>"
#define CONFIG_BODY \
    "<tt:Name>" A2K_ONVIF_TOK_PTZCFG "</tt:Name><tt:UseCount>2</tt:UseCount><tt:NodeToken>" A2K_ONVIF_TOK_PTZNODE "</tt:NodeToken>" \
    "<tt:DefaultContinuousPanTiltVelocitySpace>" A2K_ONVIF_VEL_SPACE "</tt:DefaultContinuousPanTiltVelocitySpace>" \
    "<tt:DefaultPTZTimeout>PT0.45S</tt:DefaultPTZTimeout>"

static void set_maxp(soap_tpl_t *t) { soap_tpl_ref(t, "maxp", "16"); } /* PP_PRESETS */

/* ---- read operations ---- */

static void op_service_caps(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<tptz:GetServiceCapabilitiesResponse " NSPTZ "><tptz:Capabilities EFlip=\"false\" Reverse=\"false\" "
                "GetCompatibleConfigurations=\"false\" MoveStatus=\"true\" StatusPosition=\"false\"/></tptz:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_nodes(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN "<tptz:GetNodesResponse " NSPTZ "><tptz:PTZNode token=\"" A2K_ONVIF_TOK_PTZNODE
                                "\" FixedHomePosition=\"false\" GeoMove=\"false\">" NODE_BODY "</tptz:PTZNode></tptz:GetNodesResponse>" ENV_CLOSE);
    set_maxp(t);
    onvif_send(c);
}

static void op_get_node(onvif_call_t *c)
{
    char tok[32];
    soap_tpl_t *t;
    if (arg_text(c, "NodeToken", tok, sizeof tok) != 0 || strcmp(tok, A2K_ONVIF_TOK_PTZNODE) != 0) {
        onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoEntity", "The requested node token does not exist");
        return;
    }
    t = onvif_reply(c, ENV_OPEN "<tptz:GetNodeResponse " NSPTZ "><tptz:PTZNode token=\"" A2K_ONVIF_TOK_PTZNODE
                    "\" FixedHomePosition=\"false\" GeoMove=\"false\">" NODE_BODY "</tptz:PTZNode></tptz:GetNodeResponse>" ENV_CLOSE);
    set_maxp(t);
    onvif_send(c);
}

static void op_get_configurations(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<tptz:GetConfigurationsResponse " NSPTZ "><tptz:PTZConfiguration token=\"" A2K_ONVIF_TOK_PTZCFG "\">" CONFIG_BODY
                "</tptz:PTZConfiguration></tptz:GetConfigurationsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void fault_noconfig(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoConfig", "The requested configuration token does not exist");
}

static void op_get_configuration(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "PTZConfigurationToken", tok, sizeof tok) != 0 || strcmp(tok, A2K_ONVIF_TOK_PTZCFG) != 0) {
        fault_noconfig(c);
        return;
    }
    onvif_reply(c, ENV_OPEN "<tptz:GetConfigurationResponse " NSPTZ "><tptz:PTZConfiguration token=\"" A2K_ONVIF_TOK_PTZCFG "\">" CONFIG_BODY
                "</tptz:PTZConfiguration></tptz:GetConfigurationResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_options(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "ConfigurationToken", tok, sizeof tok) != 0 || strcmp(tok, A2K_ONVIF_TOK_PTZCFG) != 0) {
        fault_noconfig(c);
        return;
    }
    onvif_reply(c, ENV_OPEN "<tptz:GetConfigurationOptionsResponse " NSPTZ "><tptz:PTZConfigurationOptions><tt:Spaces>" SPACES_VEL "</tt:Spaces>"
                "<tt:PTZTimeout><tt:Min>PT0.15S</tt:Min><tt:Max>PT2S</tt:Max></tt:PTZTimeout></tptz:PTZConfigurationOptions>"
                "</tptz:GetConfigurationOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_status(onvif_call_t *c)
{
    a2k_ptz_info_t i;
    time_t now = (time_t)(a2k_onvif_time_fn ? a2k_onvif_time_fn() : (long long)time(NULL));
    struct tm u;
    soap_tpl_t *t;
    if (!need_profile(c))
        return;
    memset(&i, 0, sizeof i);
    if (g_ops && g_ops->info)
        g_ops->info(&i);
    gmtime_r(&now, &u);
    snprintf(c->out->scratch, ONVIF_SCRATCH, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
    t = onvif_reply(c, ENV_OPEN "<tptz:GetStatusResponse " NSPTZ "><tptz:PTZStatus><tt:MoveStatus><tt:PanTilt>{{mv}}</tt:PanTilt></tt:MoveStatus>"
                    "<tt:UtcTime>{{utc}}</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>" ENV_CLOSE);
    /* while a motion runs the position is not published as settled (known = 0) in the moved state; a calibration moves the head too */
    soap_tpl_ref(t, "mv", i.state == HAL_PTZ_CALIBRATING || (i.state == HAL_PTZ_MOVED && !i.known) ? "MOVING" : "IDLE");
    soap_tpl_ref(t, "utc", c->out->scratch);
    onvif_send(c);
}

static void op_get_presets(onvif_call_t *c)
{
    char *s = c->out->scratch, name[PP_NAME_MAX + 1];
    size_t off = 0;
    unsigned k;
    int pan, tilt;
    soap_tpl_t *t;
    if (!need_profile(c))
        return;
    s[0] = '\0';
    for (k = 0; g_ops && g_ops->preset_at && k < PP_PRESETS && g_ops->preset_at(k, name, &pan, &tilt) == 0; k++) {
        size_t j;
        int n;
        for (j = 0; name[j]; j++) /* stored names are a-z 0-9 _ already (pp_name_ok): anything else is not listed (it goes into XML) */
            if (!((name[j] >= 'a' && name[j] <= 'z') || (name[j] >= '0' && name[j] <= '9') || name[j] == '_'))
                break;
        if (name[j] || !name[0])
            continue;
        n = snprintf(s + off, ONVIF_SCRATCH - off, "<tptz:Preset token=\"%s\"><tt:Name>%s</tt:Name></tptz:Preset>", name, name);
        if (n < 0 || off + (size_t)n >= ONVIF_SCRATCH)
            break;
        off += (size_t)n;
    }
    t = onvif_reply(c, ENV_OPEN "<tptz:GetPresetsResponse " NSPTZ ">{{presets}}</tptz:GetPresetsResponse>" ENV_CLOSE);
    soap_tpl_set_raw(t, "presets", s);
    onvif_send(c);
}

/* ---- motion ---- */

static void fault_space(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:SpaceNotSupported", "Only the generic velocity space (-1..1, pan / tilt) is supported, no zoom");
}
static void fault_velocity(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidVelocity", "The velocity must be numbers in -1..1");
}

/* soap_attr returns the value length, and SOAP_NOTFOUND (1) for an absent attribute: a one character value ("1") returns the same number. An absent attribute leaves the
 * buffer untouched, so it is cleared first: a one character value then shows in out[0]. 1 = absent, 0 = found (out filled), -1 = malformed. */
static int attr_get(const soap_elem_t *e, const char *name, char *out, size_t cap)
{
    int n;
    out[0] = '\0';
    n = soap_attr(e, name, out, cap);
    if (n == SOAP_NOTFOUND && out[0] == '\0')
        return 1;
    return n < 0 ? -1 : 0;
}

/* A velocity component attribute: -1 absent, 0 ok (*milli), -2 bad (not a number / out of range). */
static int vel_attr(const soap_elem_t *e, const char *name, int *milli)
{
    char v[24];
    int n = attr_get(e, name, v, sizeof v);
    if (n == 1)
        return -1;
    if (n < 0 || ptzv_parse_milli(v, milli) != 0)
        return -2;
    return 0;
}

static int space_ok(const soap_elem_t *e)
{
    char sp[160];
    int n = attr_get(e, "space", sp, sizeof sp);
    if (n == 1)
        return 1;
    return n == 0 && !strcmp(sp, A2K_ONVIF_VEL_SPACE);
}

static void op_continuous_move(onvif_call_t *c)
{
    soap_elem_t vel, pt, zm, to;
    int px = 0, py = 0, rc, lease = PTZV_LEASE_DEF_MS;
    long ms;
    ptzv_cmd_t m;
    if (!need_profile(c))
        return;
    if (soap_find(c->op.inner, "Velocity", &vel) != SOAP_OK) {
        onvif_fault(c, 0, "ter:InvalidArgs", NULL, "Velocity is required");
        return;
    }
    if (soap_find(vel.inner, "Zoom", &zm) == SOAP_OK) { /* no zoom: a zero is accepted (many clients always send it), anything else is refused */
        int z = 0, r = vel_attr(&zm, "x", &z);
        if (r == -2) { fault_velocity(c); return; }
        if (r == 0 && z != 0) { fault_space(c); return; }
    }
    if (soap_find(vel.inner, "PanTilt", &pt) != SOAP_OK) {
        onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidVelocity", "The velocity has no PanTilt");
        return;
    }
    if (!space_ok(&pt)) { fault_space(c); return; }
    {
        int rx = vel_attr(&pt, "x", &px), ry = vel_attr(&pt, "y", &py);
        if (rx == -2 || ry == -2 || rx == -1 || ry == -1) { fault_velocity(c); return; }
    }
    if (soap_find(c->op.inner, "Timeout", &to) == SOAP_OK) {
        char d[40];
        if (soap_text(&to, d, sizeof d, SOAP_TRIM) < 0 || ptzv_parse_duration_ms(d, &ms) != 0) {
            onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidTimeout", "Timeout is an xsd:duration (PT0.5S)");
            return;
        }
        lease = ptzv_lease_clamp(ms); /* 0 s (and anything below 150 ms) -> 150 ms, above 2 s -> 2 s */
        if (ms == 0)
            lease = PTZV_LEASE_MIN_MS;
    }
    if (!g_ops || !g_ops->vel) {
        refuse(c, A2K_PTZ_E_REFUSED);
        return;
    }
    if (!ptzv_map(px, py, &m)) { /* a velocity inside the dead zone is a stop */
        (void)g_ops->stop();
    } else {
        rc = g_ops->vel(m.sx, m.sy, m.dx, m.dy, lease);
        if (rc != 0) {
            refuse(c, rc);
            return;
        }
    }
    onvif_reply(c, ENV_OPEN "<tptz:ContinuousMoveResponse " NSPTZ "/>" ENV_CLOSE);
    onvif_send(c);
}

static void op_stop(onvif_call_t *c)
{
    char v[8];
    int pantilt = 1;
    if (!need_profile(c))
        return;
    if (arg_text(c, "PanTilt", v, sizeof v) == 0) {
        if (!strcmp(v, "false") || !strcmp(v, "0"))
            pantilt = 0;
        else if (strcmp(v, "true") && strcmp(v, "1")) {
            onvif_fault(c, 0, "ter:InvalidArgs", NULL, "PanTilt is a boolean");
            return;
        }
    }
    if (pantilt && g_ops && g_ops->stop)
        (void)g_ops->stop(); /* always succeeds: nothing to stop is not an error */
    onvif_reply(c, ENV_OPEN "<tptz:StopResponse " NSPTZ "/>" ENV_CLOSE);
    onvif_send(c);
}

static void op_goto_home(onvif_call_t *c)
{
    int rc;
    if (!need_profile(c))
        return;
    if (!g_ops || !g_ops->gate || !g_ops->home) {
        refuse(c, A2K_PTZ_E_REFUSED);
        return;
    }
    rc = g_ops->gate();
    if (rc != 0) {
        refuse(c, rc);
        return;
    }
    if (g_ops->home() != 0) {
        refuse(c, A2K_PTZ_E_REFUSED);
        return;
    }
    onvif_reply(c, ENV_OPEN "<tptz:GotoHomePositionResponse " NSPTZ "/>" ENV_CLOSE);
    onvif_send(c);
}

static void op_goto_preset(onvif_call_t *c)
{
    char tok[PP_NAME_MAX + 8];
    int rc;
    if (!need_profile(c))
        return;
    if (arg_text(c, "PresetToken", tok, sizeof tok) != 0 || strlen(tok) > PP_NAME_MAX) {
        onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoToken", "The requested preset token does not exist");
        return;
    }
    if (!g_ops || !g_ops->gate || !g_ops->goto_preset) {
        refuse(c, A2K_PTZ_E_REFUSED);
        return;
    }
    rc = g_ops->gate();
    if (rc != 0) {
        refuse(c, rc);
        return;
    }
    rc = g_ops->goto_preset(tok);
    if (rc == PP_E_NOTFOUND) {
        onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoToken", "The requested preset token does not exist");
        return;
    }
    if (rc != 0) {
        refuse(c, A2K_PTZ_E_REFUSED);
        return;
    }
    onvif_reply(c, ENV_OPEN "<tptz:GotoPresetResponse " NSPTZ "/>" ENV_CLOSE);
    onvif_send(c);
}

static const onvif_op_t k_ptz_ops[] = {
    { "GetServiceCapabilities", ONVIF_PRE, op_service_caps },
    { "GetNodes", ONVIF_READ, op_get_nodes },
    { "GetNode", ONVIF_READ, op_get_node },
    { "GetConfigurations", ONVIF_READ, op_get_configurations },
    { "GetConfiguration", ONVIF_READ, op_get_configuration },
    { "GetConfigurationOptions", ONVIF_READ, op_get_options },
    { "GetStatus", ONVIF_READ, op_get_status },
    { "GetPresets", ONVIF_READ, op_get_presets },
    { "ContinuousMove", ONVIF_WRITE, op_continuous_move },
    { "Stop", ONVIF_WRITE, op_stop },
    { "GotoHomePosition", ONVIF_WRITE, op_goto_home },
    { "GotoPreset", ONVIF_WRITE, op_goto_preset },
};

int a2k_onvif_ptz_register(void)
{
    return onvif_add_service("/onvif/ptz_service", k_ptz_ops, sizeof k_ptz_ops / sizeof k_ptz_ops[0]);
}
