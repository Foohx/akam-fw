/* ONVIF of the Rotatable 2K: the service controller (start / stop / state), the model binding of the shared core, the Device and Media services.
 * See onvif_services.h. The PTZ service is in onvif_ptz.c. Responses are static templates (soap.h); the few runtime facts are substituted.
 * No ctype.h (the camera's libc lacks its locale glue, see ctype_compat.h). */
#define _GNU_SOURCE
#include "onvif_services.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "mqtt_cfg.h" /* A2K_APP_VERSION */
#include "video.h"
#include "cred.h"
#include "http_srv.h"
#include "onvif.h"
#include "onvif_common.h"
#include "wsd.h"

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE

#define A2K_ONVIF_MODEL "LSC Smart Rotatable 2K (AK3918AV130)"
#define A2K_ONVIF_HARDWARE_ID "AK3918AV130"

/* ================================================================================= controller */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_state[A2K_ONVIF_STATE_MAX] = "off";
static int g_running, g_port, g_want, g_want_port;
static int g_rtsp_port = 554;
static const wsd_test_t *g_wsd_test;
long long (*a2k_onvif_time_fn)(void);

static long long now_unix(void) { return a2k_onvif_time_fn ? a2k_onvif_time_fn() : (long long)time(NULL); }

void a2k_onvif_set_rtsp_port(int port) { g_rtsp_port = port > 0 && port < 65536 ? port : 554; }
void a2k_onvif_test_set_wsd(const wsd_test_t *t) { g_wsd_test = t; }

int a2k_onvif_port_ok(long port, int rtsp_port, int api_port)
{
    if (port < 1024 || port > 65535)
        return 0;
    if (port == 22 || port == 554 || port == 8091 || port == 8092 || port == rtsp_port || (api_port && port == api_port))
        return 0;
    return 1;
}

static void hostname(char *out, size_t cap)
{
    char h[64];
    size_t i;
    h[0] = '\0';
    if (gethostname(h, sizeof h - 1) != 0 || !h[0])
        snprintf(h, sizeof h, "akam-rot2k");
    for (i = 0; h[i]; i++) /* a name goes into XML and a URI: letters, digits and - only */
        if (!((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9') || h[i] == '-'))
            h[i] = '-';
    snprintf(out, cap, "%s", h);
}

static int xaddr_cb(const struct in_addr *local, char *out, size_t cap, void *ctx)
{
    int port = http_srv_port(), n;
    char ip[INET_ADDRSTRLEN];
    (void)ctx;
    if (port == 0 || !inet_ntop(AF_INET, local, ip, sizeof ip))
        return -1;
    n = snprintf(out, cap, "http://%s:%d/onvif/device_service", ip, port);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static int start_wsd(void)
{
    char host[64], scopes[400], uuid[46];
    wsd_info_t info;
    hostname(host, sizeof host);
    snprintf(scopes, sizeof scopes,
             "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/Profile/S onvif://www.onvif.org/type/ptz "
             "onvif://www.onvif.org/name/%s onvif://www.onvif.org/hardware/" A2K_ONVIF_HARDWARE_ID " onvif://www.onvif.org/location/",
             host);
    onvif_uuid(uuid);
    memset(&info, 0, sizeof info);
    info.uuid = uuid;
    info.scopes = scopes;
    info.metadata_version = 1;
    info.xaddr = xaddr_cb;
    info.test = g_wsd_test;
    return wsd_start(&info);
}

static void stop_locked(void)
{
    if (!g_running)
        return;
    wsd_stop();
    onvif_stop();
    g_running = 0;
    g_port = 0;
}

void a2k_onvif_stop(void)
{
    pthread_mutex_lock(&g_lock);
    stop_locked();
    snprintf(g_state, sizeof g_state, "off");
    pthread_mutex_unlock(&g_lock);
}

void a2k_onvif_state(char *out, size_t cap)
{
    pthread_mutex_lock(&g_lock);
    snprintf(out, cap, "%s", g_state);
    pthread_mutex_unlock(&g_lock);
}

int a2k_onvif_wanted(int *port)
{
    int w;
    pthread_mutex_lock(&g_lock);
    w = g_want;
    if (port)
        *port = g_want_port;
    pthread_mutex_unlock(&g_lock);
    return w;
}

int a2k_onvif_start(const a2k_onvif_cfg_t *cfg)
{
    int port, rc = 0, e;
    pthread_mutex_lock(&g_lock);
    g_want = cfg->enabled ? 1 : 0;
    g_want_port = cfg->port ? cfg->port : A2K_ONVIF_PORT;
    if (!cfg->enabled) {
        stop_locked();
        snprintf(g_state, sizeof g_state, "off");
        goto out;
    }
    port = g_want_port;
    stop_locked();
    a2k_onvif_set_rtsp_port(cfg->rtsp_port);
    if (!a2k_onvif_port_ok(port, cfg->rtsp_port, cfg->api_port)) {
        snprintf(g_state, sizeof g_state, "error:bad_port");
        a2k_log("onvif: port %d is not allowed (1024..65535, not the RTSP port, the API port, 8091, 8092, 22 or 554): ONVIF stays off", port);
        rc = -1;
        goto out;
    }
    if (!cfg->auth) {
        snprintf(g_state, sizeof g_state, "error:auth_broken");
        rc = -1;
        goto out;
    }
    cred_init(NULL, cfg->auth); /* the htdigest file and the failure limiter of the RTSP server */
    switch (cred_auth_mode()) {
    case RTSP_AUTH_OFF:
        snprintf(g_state, sizeof g_state, "no_password");
        a2k_log("onvif: enabled but the stream password is not set (the rtsp.conf credentials file is absent): ONVIF is not started (no PTZ without a login)");
        rc = -1;
        goto out;
    case RTSP_AUTH_BROKEN:
        snprintf(g_state, sizeof g_state, "error:auth_broken");
        a2k_log("onvif: the stream credentials file is invalid: ONVIF is not started");
        rc = -1;
        goto out;
    default:
        break;
    }
    errno = 0;
    if (onvif_start(port) != 0) {
        e = errno;
        snprintf(g_state, sizeof g_state, "error:%s", e == EADDRINUSE ? "port_in_use" : e == EACCES ? "permission_denied" : "http_failed");
        a2k_log("onvif: HTTP server on port %d failed: %s", port, strerror(e));
        rc = -1;
        goto out;
    }
    errno = 0;
    if (start_wsd() != 0) {
        e = errno;
        onvif_stop();
        snprintf(g_state, sizeof g_state, "error:discovery_failed");
        a2k_log("onvif: WS-Discovery failed: %s", strerror(e));
        rc = -1;
        goto out;
    }
    g_running = 1;
    g_port = port;
    snprintf(g_state, sizeof g_state, "on:%d", port);
    a2k_log("onvif: on, HTTP port %d, profiles stream0 / stream1, PTZ velocity control, Digest login with the stream credentials", port);
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* ================================================================================= model binding */

const onvif_register_fn onvif_model_services[] = { a2k_onvif_device_register, a2k_onvif_media_register, a2k_onvif_ptz_register, NULL };

/* ================================================================================= helpers */

static int arg_text(onvif_call_t *c, const char *local, char *out, size_t cap)
{
    soap_elem_t e;
    if (soap_find(c->op.inner, local, &e) != SOAP_OK)
        return -1;
    return soap_text(&e, out, cap, SOAP_TRIM) >= 0 ? 0 : -1;
}

/* "stream0" / "stream1" -> 0 / 1, else -1 */
static int profile_index(const char *tok)
{
    if (!strcmp(tok, A2K_ONVIF_TOK_PROFILE0)) return 0;
    if (!strcmp(tok, A2K_ONVIF_TOK_PROFILE1)) return 1;
    return -1;
}


static int need_profile(onvif_call_t *c)
{
    char tok[32];
    int i;
    if (arg_text(c, "ProfileToken", tok, sizeof tok) != 0 || (i = profile_index(tok)) < 0) {
        onvif_fault_noprofile(c);
        return -1;
    }
    return i;
}

/* ================================================================================= Device service */

#define DEV_CAPS \
    "<tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>" \
    "<tds:Security TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" X.509Token=\"false\" " \
    "SAMLToken=\"false\" KerberosToken=\"false\" RELToken=\"false\"/>" \
    "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" SystemBackup=\"false\" SystemLogging=\"false\" " \
    "FirmwareUpgrade=\"false\"/></tds:Capabilities>"

#define PTZ_CAPS \
    "<tptz:Capabilities xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" EFlip=\"false\" Reverse=\"false\" " \
    "GetCompatibleConfigurations=\"false\" MoveStatus=\"true\" StatusPosition=\"false\"/>"
#define MEDIA_CAPS \
    "<trt:Capabilities SnapshotUri=\"false\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\" TemporaryOSDText=\"false\" EXICompression=\"false\">" \
    "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"2\"/>" \
    "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"false\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/>" \
    "</trt:Capabilities>"

static void op_device_information(onvif_call_t *c)
{
    char mac[18], serial[13];
    soap_tpl_t *t;
    int i, j = 0;
    onvif_mac(mac);
    for (i = 0; i < 17; i++)
        if (mac[i] != ':')
            serial[j++] = mac[i];
    serial[12] = '\0';
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetDeviceInformationResponse><tds:Manufacturer>Akam</tds:Manufacturer><tds:Model>{{model}}</tds:Model>"
        "<tds:FirmwareVersion>{{fw}}</tds:FirmwareVersion><tds:SerialNumber>{{serial}}</tds:SerialNumber>"
        "<tds:HardwareId>{{hw}}</tds:HardwareId></tds:GetDeviceInformationResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "model", A2K_ONVIF_MODEL);
    soap_tpl_ref(t, "fw", A2K_APP_VERSION);
    soap_tpl_set(t, "serial", serial);
    soap_tpl_ref(t, "hw", A2K_ONVIF_HARDWARE_ID);
    onvif_send(c);
}

static void op_capabilities(onvif_call_t *c)
{
    char base[64];
    soap_tpl_t *t;
    onvif_base_url(c->req, base, sizeof base);
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetCapabilitiesResponse><tds:Capabilities>"
        "<tt:Device><tt:XAddr>{{base}}/onvif/device_service</tt:XAddr>"
        "<tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
        "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
        "<tt:System><tt:DiscoveryResolve>true</tt:DiscoveryResolve><tt:DiscoveryBye>true</tt:DiscoveryBye>"
        "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
        "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
        "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tt:SupportedVersions></tt:System>"
        "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
        "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
        "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
        "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>"
        "<tt:Media><tt:XAddr>{{base}}/onvif/media_service</tt:XAddr><tt:StreamingCapabilities>"
        "<tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>false</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
        "</tt:StreamingCapabilities></tt:Media>"
        "<tt:PTZ><tt:XAddr>{{base}}/onvif/ptz_service</tt:XAddr></tt:PTZ>"
        "</tds:Capabilities></tds:GetCapabilitiesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "base", base);
    onvif_send(c);
}

static void op_services(onvif_call_t *c)
{
    char base[64], inc[8];
    soap_elem_t e;
    soap_tpl_t *t;
    int with;
    inc[0] = '\0';
    onvif_base_url(c->req, base, sizeof base);
    if (soap_find(c->op.inner, "IncludeCapability", &e) == SOAP_OK)
        (void)soap_text(&e, inc, sizeof inc, SOAP_TRIM);
    with = !strcmp(inc, "true") || !strcmp(inc, "1");
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetServicesResponse>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/device_service</tds:XAddr>{{cd}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/media_service</tds:XAddr>{{cm}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/ptz_service</tds:XAddr>{{cp}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "</tds:GetServicesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "base", base);
    soap_tpl_set_raw(t, "cd", with ? DEV_CAPS : "");
    soap_tpl_set_raw(t, "cm", with ? "<tds:Capabilities>" MEDIA_CAPS "</tds:Capabilities>" : "");
    soap_tpl_set_raw(t, "cp", with ? "<tds:Capabilities>" PTZ_CAPS "</tds:Capabilities>" : "");
    onvif_send(c);
}

static void op_service_capabilities(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<tds:GetServiceCapabilitiesResponse>" DEV_CAPS "</tds:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_time(onvif_call_t *c)
{
    time_t now = (time_t)now_unix();
    struct tm u;
    soap_tpl_t *t;
    char *s = c->out->scratch;
    gmtime_r(&now, &u);
    snprintf(s, ONVIF_SCRATCH,
             "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
             "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime>",
             u.tm_hour, u.tm_min, u.tm_sec, u.tm_year + 1900, u.tm_mon + 1, u.tm_mday);
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>Manual</tt:DateTimeType>"
        "<tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
        "{{times}}</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>" ENV_CLOSE);
    soap_tpl_set_raw(t, "times", s);
    onvif_send(c);
}

static void op_get_hostname(onvif_call_t *c)
{
    char name[64];
    soap_tpl_t *t;
    hostname(name, sizeof name);
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>{{name}}</tt:Name>"
        "</tds:HostnameInformation></tds:GetHostnameResponse>" ENV_CLOSE);
    soap_tpl_set(t, "name", name);
    onvif_send(c);
}

static void op_scopes(onvif_call_t *c)
{
    char name[64];
    soap_tpl_t *t;
    hostname(name, sizeof name);
    t = onvif_reply(c, ENV_OPEN
        "<tds:GetScopesResponse>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/S</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/ptz</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/" A2K_ONVIF_HARDWARE_ID "</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/{{name}}</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/location/</tt:ScopeItem></tds:Scopes>"
        "</tds:GetScopesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "name", name);
    onvif_send(c);
}


static void op_get_protocols(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetNetworkProtocolsResponse>"
        "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{{http}}</tt:Port></tds:NetworkProtocols>"
        "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{{rtsp}}</tt:Port></tds:NetworkProtocols>"
        "</tds:GetNetworkProtocolsResponse>" ENV_CLOSE);
    snprintf(c->out->scratch, ONVIF_SCRATCH, "%d", http_srv_port());
    soap_tpl_set(t, "http", c->out->scratch);
    snprintf(c->out->scratch, ONVIF_SCRATCH, "%d", g_rtsp_port);
    soap_tpl_set(t, "rtsp", c->out->scratch);
    onvif_send(c);
}

static const onvif_op_t k_dev_ops[] = {
    { "GetSystemDateAndTime", ONVIF_PRE, op_get_time },
    { "GetCapabilities", ONVIF_PRE, op_capabilities },
    { "GetServices", ONVIF_PRE, op_services },
    { "GetServiceCapabilities", ONVIF_PRE, op_service_capabilities },
    { "GetScopes", ONVIF_PRE, op_scopes },
    { "GetHostname", ONVIF_PRE, op_get_hostname },
    { "GetDeviceInformation", ONVIF_READ, op_device_information },
    { "GetDiscoveryMode", ONVIF_READ, onvif_op_get_discovery_mode },
    { "GetNetworkProtocols", ONVIF_READ, op_get_protocols },
};

int a2k_onvif_device_register(void)
{
    return onvif_add_service("/onvif/device_service", k_dev_ops, sizeof k_dev_ops / sizeof k_dev_ops[0]);
}

/* ================================================================================= Media service */

#define TOK_VSRC "vsrc"
#define TOK_VSRCCFG "vsrccfg"
#define SRC_W "2304"
#define SRC_H "1296"

#define VSRCCFG_BODY \
    "<tt:Name>" TOK_VSRCCFG "</tt:Name><tt:UseCount>2</tt:UseCount><tt:SourceToken>" TOK_VSRC "</tt:SourceToken>" \
    "<tt:Bounds x=\"0\" y=\"0\" width=\"" SRC_W "\" height=\"" SRC_H "\"/>"
/* N = 0 main (2304x1296 Main), 1 sub (640x360 Main; Baseline only if the encoder refused Main, not reflected here); the frame rate, GOP and bit rate are the live video settings ({{fpsN}} {{gopN}} {{kbpsN}}) */
#define VENCCFG_BODY(N, W, H, PROF) \
    "<tt:Name>venccfg" #N "</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>" \
    "<tt:Resolution><tt:Width>" W "</tt:Width><tt:Height>" H "</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality>" \
    "<tt:RateControl><tt:FrameRateLimit>{{fps" #N "}}</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>" \
    "<tt:BitrateLimit>{{kbps" #N "}}</tt:BitrateLimit></tt:RateControl>" \
    "<tt:H264><tt:GovLength>{{gop" #N "}}</tt:GovLength><tt:H264Profile>" PROF "</tt:H264Profile></tt:H264>" \
    "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>" \
    "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout>"
#define PTZCFG_BODY \
    "<tt:Name>" A2K_ONVIF_TOK_PTZCFG "</tt:Name><tt:UseCount>2</tt:UseCount><tt:NodeToken>" A2K_ONVIF_TOK_PTZNODE "</tt:NodeToken>" \
    "<tt:DefaultContinuousPanTiltVelocitySpace>" A2K_ONVIF_VEL_SPACE "</tt:DefaultContinuousPanTiltVelocitySpace>" \
    "<tt:DefaultPTZTimeout>PT0.45S</tt:DefaultPTZTimeout>"
#define PROFILE_BODY(N, TOK, W, H, PROF) \
    "<tt:Name>" TOK "</tt:Name>" \
    "<tt:VideoSourceConfiguration token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY "</tt:VideoSourceConfiguration>" \
    "<tt:VideoEncoderConfiguration token=\"venccfg" #N "\">" VENCCFG_BODY(N, W, H, PROF) "</tt:VideoEncoderConfiguration>" \
    "<tt:PTZConfiguration token=\"" A2K_ONVIF_TOK_PTZCFG "\">" PTZCFG_BODY "</tt:PTZConfiguration>"
#define PROFILE0 PROFILE_BODY(0, A2K_ONVIF_TOK_PROFILE0, "2304", "1296", "Main")
#define PROFILE1 PROFILE_BODY(1, A2K_ONVIF_TOK_PROFILE1, "640", "360", "Main")

/* {{fpsN}} {{gopN}} {{kbpsN}} of both streams (6 of the 12 template variables); the strings live in the response scratch area. */
static void venc_vars(onvif_call_t *c, soap_tpl_t *t)
{
    char *s = c->out->scratch;
    size_t off = 0;
    static const char *const nm[2][3] = { { "fps0", "gop0", "kbps0" }, { "fps1", "gop1", "kbps1" } };
    int chn;
    for (chn = 0; chn < 2; chn++) {
        a2k_vid_t v;
        int vals[3], k;
        a2k_vid_get(chn, &v);
        vals[0] = v.v[A2K_VP_FPS];
        vals[1] = v.v[A2K_VP_GOP];
        vals[2] = v.v[A2K_VP_KBPS];
        for (k = 0; k < 3; k++) {
            int n = snprintf(s + off, ONVIF_SCRATCH - off, "%d", vals[k]);
            if (n < 0 || off + (size_t)n + 1 >= ONVIF_SCRATCH)
                return;
            soap_tpl_ref(t, nm[chn][k], s + off);
            off += (size_t)n + 1;
        }
    }
}

static void op_get_profiles(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetProfilesResponse>"
        "<trt:Profiles fixed=\"true\" token=\"" A2K_ONVIF_TOK_PROFILE0 "\">" PROFILE0 "</trt:Profiles>"
        "<trt:Profiles fixed=\"true\" token=\"" A2K_ONVIF_TOK_PROFILE1 "\">" PROFILE1 "</trt:Profiles>"
        "</trt:GetProfilesResponse>" ENV_CLOSE);
    venc_vars(c, t);
    onvif_send(c);
}

static void op_get_profile(onvif_call_t *c)
{
    soap_tpl_t *t;
    int i = need_profile(c);
    if (i < 0)
        return;
    if (i == 0)
        t = onvif_reply(c, ENV_OPEN "<trt:GetProfileResponse><trt:Profile fixed=\"true\" token=\"" A2K_ONVIF_TOK_PROFILE0 "\">" PROFILE0
                        "</trt:Profile></trt:GetProfileResponse>" ENV_CLOSE);
    else
        t = onvif_reply(c, ENV_OPEN "<trt:GetProfileResponse><trt:Profile fixed=\"true\" token=\"" A2K_ONVIF_TOK_PROFILE1 "\">" PROFILE1
                        "</trt:Profile></trt:GetProfileResponse>" ENV_CLOSE);
    venc_vars(c, t);
    onvif_send(c);
}

static void op_get_video_sources(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"" TOK_VSRC "\"><tt:Framerate>{{fps0}}</tt:Framerate>"
        "<tt:Resolution><tt:Width>" SRC_W "</tt:Width><tt:Height>" SRC_H "</tt:Height></tt:Resolution></trt:VideoSources>"
        "</trt:GetVideoSourcesResponse>" ENV_CLOSE);
    venc_vars(c, t);
    onvif_send(c);
}

static void op_get_vsrc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetVideoSourceConfigurationsResponse><trt:Configurations token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY
                "</trt:Configurations></trt:GetVideoSourceConfigurationsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_vsrc_config(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "ConfigurationToken", tok, sizeof tok) != 0 || strcmp(tok, TOK_VSRCCFG) != 0) {
        onvif_fault_noconfig(c);
        return;
    }
    onvif_reply(c, ENV_OPEN "<trt:GetVideoSourceConfigurationResponse><trt:Configuration token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY
                "</trt:Configuration></trt:GetVideoSourceConfigurationResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_vsrc_options(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourceConfigurationOptionsResponse><trt:Options>"
        "<tt:BoundsRange><tt:XRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:XRange><tt:YRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:YRange>"
        "<tt:WidthRange><tt:Min>" SRC_W "</tt:Min><tt:Max>" SRC_W "</tt:Max></tt:WidthRange>"
        "<tt:HeightRange><tt:Min>" SRC_H "</tt:Min><tt:Max>" SRC_H "</tt:Max></tt:HeightRange></tt:BoundsRange>"
        "<tt:VideoSourceTokensAvailable>" TOK_VSRC "</tt:VideoSourceTokensAvailable>"
        "</trt:Options></trt:GetVideoSourceConfigurationOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_venc_configs(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoEncoderConfigurationsResponse>"
        "<trt:Configurations token=\"venccfg0\">" VENCCFG_BODY(0, "2304", "1296", "Main") "</trt:Configurations>"
        "<trt:Configurations token=\"venccfg1\">" VENCCFG_BODY(1, "640", "360", "Main") "</trt:Configurations>"
        "</trt:GetVideoEncoderConfigurationsResponse>" ENV_CLOSE);
    venc_vars(c, t);
    onvif_send(c);
}

/* "venccfg0" / "venccfg1" -> 0 / 1, else the fault is sent and -1 */
static int need_venc_config(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "ConfigurationToken", tok, sizeof tok) != 0 || strncmp(tok, "venccfg", 7) != 0 || (tok[7] != '0' && tok[7] != '1') || tok[8]) {
        onvif_fault_noconfig(c);
        return -1;
    }
    return tok[7] - '0';
}

static void op_get_venc_config(onvif_call_t *c)
{
    soap_tpl_t *t;
    int i = need_venc_config(c);
    if (i < 0)
        return;
    if (i == 0)
        t = onvif_reply(c, ENV_OPEN "<trt:GetVideoEncoderConfigurationResponse><trt:Configuration token=\"venccfg0\">"
                        VENCCFG_BODY(0, "2304", "1296", "Main") "</trt:Configuration></trt:GetVideoEncoderConfigurationResponse>" ENV_CLOSE);
    else
        t = onvif_reply(c, ENV_OPEN "<trt:GetVideoEncoderConfigurationResponse><trt:Configuration token=\"venccfg1\">"
                        VENCCFG_BODY(1, "640", "360", "Main") "</trt:Configuration></trt:GetVideoEncoderConfigurationResponse>" ENV_CLOSE);
    venc_vars(c, t);
    onvif_send(c);
}

#define VENC_OPTIONS(N, W, H, PROF) \
    "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options><tt:QualityRange><tt:Min>5</tt:Min><tt:Max>5</tt:Max></tt:QualityRange>" \
    "<tt:H264><tt:ResolutionsAvailable><tt:Width>" W "</tt:Width><tt:Height>" H "</tt:Height></tt:ResolutionsAvailable>" \
    "<tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>300</tt:Max></tt:GovLengthRange>" \
    "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>15</tt:Max></tt:FrameRateRange>" \
    "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>" \
    "<tt:H264ProfilesSupported>" PROF "</tt:H264ProfilesSupported></tt:H264></trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>"

static void op_get_venc_options(onvif_call_t *c)
{
    char tok[32];
    int i = 0;
    /* the options of the configuration named; without a token (allowed by the schema) those of the main stream */
    if (arg_text(c, "ConfigurationToken", tok, sizeof tok) == 0) {
        if (strcmp(tok, "venccfg0") == 0) i = 0;
        else if (strcmp(tok, "venccfg1") == 0) i = 1;
        else { onvif_fault_noconfig(c); return; }
    }
    if (i == 0)
        onvif_reply(c, ENV_OPEN VENC_OPTIONS(0, "2304", "1296", "Main") ENV_CLOSE);
    else
        onvif_reply(c, ENV_OPEN VENC_OPTIONS(1, "640", "360", "Main") ENV_CLOSE);
    onvif_send(c);
}

static void op_get_audio_sources(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetAudioSourcesResponse/>" ENV_CLOSE); /* the microphone track is L16, which ONVIF Media 1 cannot describe: it is in the RTSP SDP only */
    onvif_send(c);
}

static void op_get_stream_uri(onvif_call_t *c)
{
    char v[24], ip[16];
    soap_elem_t ss, e;
    soap_tpl_t *t;
    int i = need_profile(c);
    ip[0] = '\0';
    if (i < 0)
        return;
    if (soap_find(c->op.inner, "StreamSetup", &ss) == SOAP_OK) {
        soap_span_t in = ss.inner;
        if (soap_find(in, "Stream", &e) == SOAP_OK && soap_text(&e, v, sizeof v, SOAP_TRIM) >= 0 && strcmp(v, "RTP-Unicast") != 0) {
            onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidStreamSetup", "Only RTP-Unicast is supported");
            return;
        }
        if (soap_find(in, "Protocol", &e) == SOAP_OK && soap_text(&e, v, sizeof v, SOAP_TRIM) >= 0 && strcmp(v, "RTSP") != 0 && strcmp(v, "TCP") != 0 &&
            strcmp(v, "UDP") != 0) {
            onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidStreamSetup", "Protocol not supported");
            return;
        }
    }
    if (c->req->local.sin_family == AF_INET && c->req->local.sin_addr.s_addr != 0)
        inet_ntop(AF_INET, &c->req->local.sin_addr, ip, sizeof ip);
    if (!ip[0]) {
        int p, m;
        if (onvif_if_ipv4(ip, &p, &m) != 0)
            snprintf(ip, sizeof ip, "0.0.0.0");
    }
    snprintf(c->out->scratch, ONVIF_SCRATCH, "rtsp://%s:%d/stream%d", ip, g_rtsp_port, i);
    t = onvif_reply(c, ENV_OPEN
        "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>{{uri}}</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "uri", c->out->scratch);
    onvif_send(c);
}

static void op_get_snapshot_uri(onvif_call_t *c)
{
    if (need_profile(c) < 0)
        return;
    onvif_fault(c, 1, "ter:Action", "ter:ActionNotSupported", "Snapshots are not supported");
}

static void op_media_caps(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetServiceCapabilitiesResponse>" MEDIA_CAPS "</trt:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    onvif_send(c);
}

/* the device is read only: configuration changes are refused after the access gate (write class) */
static void op_refuse_conf(onvif_call_t *c)
{
    onvif_fault(c, 1, "ter:Action", "ter:ConfigurationConflict", "The profiles are fixed and cannot be changed");
}

static const onvif_op_t k_media_ops[] = {
    { "GetServiceCapabilities", ONVIF_PRE, op_media_caps },
    { "GetProfiles", ONVIF_READ, op_get_profiles },
    { "GetProfile", ONVIF_READ, op_get_profile },
    { "GetVideoSources", ONVIF_READ, op_get_video_sources },
    { "GetVideoSourceConfigurations", ONVIF_READ, op_get_vsrc_configs },
    { "GetVideoSourceConfiguration", ONVIF_READ, op_get_vsrc_config },
    { "GetVideoSourceConfigurationOptions", ONVIF_READ, op_get_vsrc_options },
    { "GetVideoEncoderConfigurations", ONVIF_READ, op_get_venc_configs },
    { "GetVideoEncoderConfiguration", ONVIF_READ, op_get_venc_config },
    { "GetVideoEncoderConfigurationOptions", ONVIF_READ, op_get_venc_options },
    { "GetAudioSources", ONVIF_READ, op_get_audio_sources },
    { "GetAudioSourceConfigurations", ONVIF_READ, onvif_op_get_asrc_configs },
    { "GetAudioEncoderConfigurations", ONVIF_READ, onvif_op_get_aenc_configs },
    { "GetStreamUri", ONVIF_READ, op_get_stream_uri },
    { "GetSnapshotUri", ONVIF_READ, op_get_snapshot_uri },
    { "SetVideoSourceConfiguration", ONVIF_WRITE, onvif_op_refuse_set },
    { "SetVideoEncoderConfiguration", ONVIF_WRITE, onvif_op_refuse_set },
    { "AddVideoSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddVideoEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddPTZConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveVideoSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveVideoEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemovePTZConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "CreateProfile", ONVIF_WRITE, onvif_op_refuse_create },
    { "DeleteProfile", ONVIF_WRITE, onvif_op_refuse_delete },
};

int a2k_onvif_media_register(void)
{
    return onvif_add_service("/onvif/media_service", k_media_ops, sizeof k_media_ops / sizeof k_media_ops[0]);
}
