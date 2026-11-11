/* ONVIF Media service, see onvif_media.h. Responses are static templates (soap.h); the few runtime facts (stream
 * address, backchannel availability, speaker level) are substituted. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ak_rtsp.h"
#include "audio_out.h"
#include "cred.h"
#include "onvif.h"
#include "onvif_media.h"
#include "video_cfg.h"

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE

#define STR_(x) #x
#define STR(x) STR_(x)
#define W STR(ENC_WIDTH)
#define H STR(ENC_HEIGHT)
/* Frame rate, GOP and bitrate are the saved settings (video_cfg.h, the values the encoder was created with):
 * placeholders in the templates, filled by venc_vars(). */
#define FPS "{{fps}}"
#define GOP "{{gop}}"
#define KBPS "{{kbps}}"

#define TOK_PROFILE "main"
#define TOK_VSRC "vsrc"
#define TOK_VSRCCFG "vsrccfg"
#define TOK_VENCCFG "venccfg"
#define TOK_ASRC "asrc"
#define TOK_AOUT "aout"
#define TOK_AOUTCFG "aoutcfg"
#define TOK_ADEC "adec"

/* ---- fragments (string literals, concatenated by the templates) ---- */
#define VSRCCFG_BODY \
    "<tt:Name>" TOK_VSRCCFG "</tt:Name><tt:UseCount>1</tt:UseCount><tt:SourceToken>" TOK_VSRC "</tt:SourceToken>" \
    "<tt:Bounds x=\"0\" y=\"0\" width=\"" W "\" height=\"" H "\"/>"
#define VENCCFG_BODY \
    "<tt:Name>" TOK_VENCCFG "</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>" \
    "<tt:Resolution><tt:Width>" W "</tt:Width><tt:Height>" H "</tt:Height></tt:Resolution><tt:Quality>5</tt:Quality>" \
    "<tt:RateControl><tt:FrameRateLimit>" FPS "</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval>" \
    "<tt:BitrateLimit>" KBPS "</tt:BitrateLimit></tt:RateControl>" \
    "<tt:H264><tt:GovLength>" GOP "</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>" \
    "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>" \
    "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>" \
    "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
#define AOUTCFG_BODY(level) \
    "<tt:Name>" TOK_AOUTCFG "</tt:Name><tt:UseCount>1</tt:UseCount><tt:OutputToken>" TOK_AOUT "</tt:OutputToken>" \
    "<tt:SendPrimacy>www.onvif.org/ver20/HalfDuplex/Auto</tt:SendPrimacy><tt:OutputLevel>" level "</tt:OutputLevel>"
#define ADEC_BODY "<tt:Name>" TOK_ADEC "</tt:Name><tt:UseCount>1</tt:UseCount>"

/* ---- runtime facts ---- */

/* Fill the {{fps}}, {{gop}} and {{kbps}} placeholders from the active video settings. */
static void venc_vars(soap_tpl_t *t)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", video_fps());
    soap_tpl_set(t, "fps", v);
    snprintf(v, sizeof(v), "%d", video_gop_frames());
    soap_tpl_set(t, "gop", v);
    snprintf(v, sizeof(v), "%d", video_kbps());
    soap_tpl_set(t, "kbps", v);
}

/* The G.711 backchannel can be served right now: same rule as rtsp.c (password ON, audio out and talk enabled). */
static int backchannel_on(void)
{
    return cred_auth_mode() == RTSP_AUTH_ON && audio_out_talk_available();
}

/* Speaker volume 0..100 as text (audio.out_volume), 50 when it cannot be read. */
static void out_level(char *out, size_t cap)
{
    char line[64];
    int v = 50;
    const char *eq;
    if (audio_out_ctl_get("audio.out_volume", line, sizeof(line)) == 0 && (eq = strchr(line, '=')) != NULL) {
        char *end;
        long x = strtol(eq + 1, &end, 10);
        if (end != eq + 1 && x >= 0 && x <= 100) v = (int)x;
    }
    snprintf(out, cap, "%d", v);
}

static int arg_text(onvif_call_t *c, const char *local, char *out, size_t cap)
{
    soap_elem_t e;
    if (soap_find(c->op.inner, local, &e) != SOAP_OK) return -1;
    return soap_text(&e, out, cap, SOAP_TRIM) >= 0 ? 0 : -1;
}

static void fault_noprofile(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoProfile", "The requested profile token does not exist");
}
static void fault_noconfig(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoConfig", "The requested configuration token does not exist");
}

/* The request names the profile (ProfileToken) and it is ours; else the fault is sent. */
static int need_profile(onvif_call_t *c)
{
    char tok[32];
    if (arg_text(c, "ProfileToken", tok, sizeof(tok)) != 0 || strcmp(tok, TOK_PROFILE) != 0) {
        fault_noprofile(c);
        return 0;
    }
    return 1;
}

/* A ConfigurationToken argument that must equal `want`; else the fault is sent. */
static int need_config(onvif_call_t *c, const char *want)
{
    char tok[32];
    if (arg_text(c, "ConfigurationToken", tok, sizeof(tok)) != 0 || strcmp(tok, want) != 0) {
        fault_noconfig(c);
        return 0;
    }
    return 1;
}

/* Same, for the configurations that exist only while the backchannel is on. */
static int need_bc_config(onvif_call_t *c, const char *want)
{
    if (!backchannel_on()) {
        fault_noconfig(c);
        return 0;
    }
    return need_config(c, want);
}

/* ---- profile ---- */

/* Everything of the profile after the name; the audio output and decoder configurations (Extension) only while
 * the backchannel is on. */
#define PROFILE_INNER \
    "<tt:Name>" TOK_PROFILE "</tt:Name>" \
    "<tt:VideoSourceConfiguration token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY "</tt:VideoSourceConfiguration>" \
    "<tt:VideoEncoderConfiguration token=\"" TOK_VENCCFG "\">" VENCCFG_BODY "</tt:VideoEncoderConfiguration>" \
    "{{ext}}"

static void profile_ext(char *s, size_t cap)
{
    s[0] = '\0';
    if (!backchannel_on()) return;
    char lvl[8];
    out_level(lvl, sizeof(lvl));
    snprintf(s, cap,
        "<tt:Extension><tt:AudioOutputConfiguration token=\"" TOK_AOUTCFG "\">" AOUTCFG_BODY("%s")
        "</tt:AudioOutputConfiguration><tt:AudioDecoderConfiguration token=\"" TOK_ADEC "\">" ADEC_BODY
        "</tt:AudioDecoderConfiguration></tt:Extension>", lvl);
}

static void op_get_profiles(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetProfilesResponse><trt:Profiles fixed=\"true\" token=\"" TOK_PROFILE "\">" PROFILE_INNER
        "</trt:Profiles></trt:GetProfilesResponse>" ENV_CLOSE);
    profile_ext(c->out->scratch, ONVIF_SCRATCH);
    soap_tpl_set_raw(t, "ext", c->out->scratch);
    venc_vars(t);
    onvif_send(c);
}

static void op_get_profile(onvif_call_t *c)
{
    if (!need_profile(c)) return;
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetProfileResponse><trt:Profile fixed=\"true\" token=\"" TOK_PROFILE "\">" PROFILE_INNER
        "</trt:Profile></trt:GetProfileResponse>" ENV_CLOSE);
    profile_ext(c->out->scratch, ONVIF_SCRATCH);
    soap_tpl_set_raw(t, "ext", c->out->scratch);
    venc_vars(t);
    onvif_send(c);
}

/* ---- video ---- */

static void op_get_video_sources(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"" TOK_VSRC "\"><tt:Framerate>" FPS "</tt:Framerate>"
        "<tt:Resolution><tt:Width>" W "</tt:Width><tt:Height>" H "</tt:Height></tt:Resolution></trt:VideoSources>"
        "</trt:GetVideoSourcesResponse>" ENV_CLOSE);
    venc_vars(t);
    onvif_send(c);
}

static void op_get_vsrc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourceConfigurationsResponse><trt:Configurations token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY
        "</trt:Configurations></trt:GetVideoSourceConfigurationsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_vsrc_config(onvif_call_t *c)
{
    if (!need_config(c, TOK_VSRCCFG)) return;
    onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourceConfigurationResponse><trt:Configuration token=\"" TOK_VSRCCFG "\">" VSRCCFG_BODY
        "</trt:Configuration></trt:GetVideoSourceConfigurationResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_vsrc_options(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetVideoSourceConfigurationOptionsResponse><trt:Options>"
        "<tt:BoundsRange><tt:XRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:XRange>"
        "<tt:YRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:YRange>"
        "<tt:WidthRange><tt:Min>" W "</tt:Min><tt:Max>" W "</tt:Max></tt:WidthRange>"
        "<tt:HeightRange><tt:Min>" H "</tt:Min><tt:Max>" H "</tt:Max></tt:HeightRange></tt:BoundsRange>"
        "<tt:VideoSourceTokensAvailable>" TOK_VSRC "</tt:VideoSourceTokensAvailable>"
        "</trt:Options></trt:GetVideoSourceConfigurationOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_venc_configs(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoEncoderConfigurationsResponse><trt:Configurations token=\"" TOK_VENCCFG "\">" VENCCFG_BODY
        "</trt:Configurations></trt:GetVideoEncoderConfigurationsResponse>" ENV_CLOSE);
    venc_vars(t);
    onvif_send(c);
}

static void op_get_venc_config(onvif_call_t *c)
{
    if (!need_config(c, TOK_VENCCFG)) return;
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoEncoderConfigurationResponse><trt:Configuration token=\"" TOK_VENCCFG "\">" VENCCFG_BODY
        "</trt:Configuration></trt:GetVideoEncoderConfigurationResponse>" ENV_CLOSE);
    venc_vars(t);
    onvif_send(c);
}

static void op_get_venc_options(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
        "<tt:QualityRange><tt:Min>5</tt:Min><tt:Max>5</tt:Max></tt:QualityRange>"
        "<tt:H264><tt:ResolutionsAvailable><tt:Width>" W "</tt:Width><tt:Height>" H "</tt:Height></tt:ResolutionsAvailable>"
        "<tt:GovLengthRange><tt:Min>" GOP "</tt:Min><tt:Max>" GOP "</tt:Max></tt:GovLengthRange>"
        "<tt:FrameRateRange><tt:Min>" FPS "</tt:Min><tt:Max>" FPS "</tt:Max></tt:FrameRateRange>"
        "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>"
        "<tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported></tt:H264>"
        "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>" ENV_CLOSE);
    venc_vars(t);
    onvif_send(c);
}

static void op_get_guaranteed(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse><trt:TotalNumber>1</trt:TotalNumber>"
        "<trt:H264>1</trt:H264></trt:GetGuaranteedNumberOfVideoEncoderInstancesResponse>" ENV_CLOSE);
    onvif_send(c);
}

/* ---- audio ---- */

static void op_get_audio_sources(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetAudioSourcesResponse><trt:AudioSources token=\"" TOK_ASRC "\"><tt:Channels>1</tt:Channels>"
        "</trt:AudioSources></trt:GetAudioSourcesResponse>" ENV_CLOSE);
    onvif_send(c);
}

/* Empty lists: the microphone is L16, which ONVIF Media 1 cannot name (see onvif_media.h). */
static void op_get_asrc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetAudioSourceConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}
static void op_get_aenc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetAudioEncoderConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}
static void op_get_compat_asrc(onvif_call_t *c)
{
    if (!need_profile(c)) return;
    onvif_reply(c, ENV_OPEN "<trt:GetCompatibleAudioSourceConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}
static void op_get_compat_aenc(onvif_call_t *c)
{
    if (!need_profile(c)) return;
    onvif_reply(c, ENV_OPEN "<trt:GetCompatibleAudioEncoderConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}
static void op_no_config(onvif_call_t *c) { fault_noconfig(c); }

static void op_get_audio_outputs(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN "<trt:GetAudioOutputsResponse>{{o}}</trt:GetAudioOutputsResponse>" ENV_CLOSE);
    soap_tpl_set_raw(t, "o", backchannel_on() ? "<trt:AudioOutputs token=\"" TOK_AOUT "\"/>" : "");
    onvif_send(c);
}

static void aout_fragment(char *s, size_t cap, const char *open, const char *close)
{
    char lvl[8];
    out_level(lvl, sizeof(lvl));
    snprintf(s, cap, "%s" AOUTCFG_BODY("%s") "%s", open, lvl, close);
}

static void op_get_aout_configs(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetAudioOutputConfigurationsResponse>{{o}}</trt:GetAudioOutputConfigurationsResponse>" ENV_CLOSE);
    c->out->scratch[0] = '\0';
    if (backchannel_on())
        aout_fragment(c->out->scratch, ONVIF_SCRATCH, "<trt:Configurations token=\"" TOK_AOUTCFG "\">",
                      "</trt:Configurations>");
    soap_tpl_set_raw(t, "o", c->out->scratch);
    onvif_send(c);
}

static void op_get_aout_config(onvif_call_t *c)
{
    if (!need_bc_config(c, TOK_AOUTCFG)) return;
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetAudioOutputConfigurationResponse>{{o}}</trt:GetAudioOutputConfigurationResponse>" ENV_CLOSE);
    aout_fragment(c->out->scratch, ONVIF_SCRATCH, "<trt:Configuration token=\"" TOK_AOUTCFG "\">",
                  "</trt:Configuration>");
    soap_tpl_set_raw(t, "o", c->out->scratch);
    onvif_send(c);
}

static void op_get_aout_options(onvif_call_t *c)
{
    if (!need_bc_config(c, TOK_AOUTCFG)) return;
    onvif_reply(c, ENV_OPEN
        "<trt:GetAudioOutputConfigurationOptionsResponse><trt:Options>"
        "<tt:OutputTokensAvailable>" TOK_AOUT "</tt:OutputTokensAvailable>"
        "<tt:SendPrimacyOptions>www.onvif.org/ver20/HalfDuplex/Auto</tt:SendPrimacyOptions>"
        "<tt:OutputLevelRange><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:OutputLevelRange>"
        "</trt:Options></trt:GetAudioOutputConfigurationOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_adec_configs(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetAudioDecoderConfigurationsResponse>{{o}}</trt:GetAudioDecoderConfigurationsResponse>" ENV_CLOSE);
    soap_tpl_set_raw(t, "o", backchannel_on()
        ? "<trt:Configurations token=\"" TOK_ADEC "\">" ADEC_BODY "</trt:Configurations>" : "");
    onvif_send(c);
}

static void op_get_adec_config(onvif_call_t *c)
{
    if (!need_bc_config(c, TOK_ADEC)) return;
    onvif_reply(c, ENV_OPEN
        "<trt:GetAudioDecoderConfigurationResponse><trt:Configuration token=\"" TOK_ADEC "\">" ADEC_BODY
        "</trt:Configuration></trt:GetAudioDecoderConfigurationResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_adec_options(onvif_call_t *c)
{
    if (!need_bc_config(c, TOK_ADEC)) return;
    onvif_reply(c, ENV_OPEN
        "<trt:GetAudioDecoderConfigurationOptionsResponse><trt:Options>"
        "<tt:G711DecOptions><tt:Bitrate><tt:Items>64</tt:Items></tt:Bitrate>"
        "<tt:SampleRateRange><tt:Items>8</tt:Items></tt:SampleRateRange></tt:G711DecOptions>"
        "</trt:Options></trt:GetAudioDecoderConfigurationOptionsResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_compat_aout(onvif_call_t *c)
{
    if (need_profile(c)) op_get_aout_configs(c);
}
static void op_get_compat_adec(onvif_call_t *c)
{
    if (need_profile(c)) op_get_adec_configs(c);
}
static void op_get_compat_vsrc(onvif_call_t *c)
{
    if (need_profile(c)) op_get_vsrc_configs(c);
}
static void op_get_compat_venc(onvif_call_t *c)
{
    if (need_profile(c)) op_get_venc_configs(c);
}

/* ---- stream ---- */

static void op_get_stream_uri(onvif_call_t *c)
{
    if (!need_profile(c)) return;
    char v[24];
    soap_elem_t ss, e;
    if (soap_find(c->op.inner, "StreamSetup", &ss) == SOAP_OK) {
        soap_span_t in = ss.inner;
        if (soap_find(in, "Stream", &e) == SOAP_OK && soap_text(&e, v, sizeof(v), SOAP_TRIM) >= 0 &&
            strcmp(v, "RTP-Unicast") != 0) {
            onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidStreamSetup", "Only RTP-Unicast is supported");
            return;
        }
        if (soap_find(in, "Protocol", &e) == SOAP_OK && soap_text(&e, v, sizeof(v), SOAP_TRIM) >= 0 &&
            strcmp(v, "RTSP") != 0 && strcmp(v, "TCP") != 0 && strcmp(v, "UDP") != 0) {
            onvif_fault(c, 0, "ter:InvalidArgVal", "ter:InvalidStreamSetup", "Protocol not supported");
            return;
        }
    }
    char ip[16] = "";
    if (c->req->local.sin_family == AF_INET && c->req->local.sin_addr.s_addr != 0)
        inet_ntop(AF_INET, &c->req->local.sin_addr, ip, sizeof(ip));
    if (!ip[0]) {
        int p, m;
        if (onvif_if_ipv4(ip, &p, &m) != 0) snprintf(ip, sizeof(ip), "0.0.0.0");
    }
    char *uri = c->out->scratch;
    snprintf(uri, ONVIF_SCRATCH, "rtsp://%s:%d/", ip, RTSP_SERVER_PORT);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>{{uri}}</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "uri", uri);
    onvif_send(c);
}

static void op_get_snapshot_uri(onvif_call_t *c)
{
    if (!need_profile(c)) return;
    onvif_fault(c, 0, "ter:ActionNotSupported", NULL, "Snapshots are not supported");
}

static void op_service_caps(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"false\" Rotation=\"false\""
        " VideoSourceMode=\"false\" OSD=\"false\" TemporaryOSDText=\"false\" EXICompression=\"false\">"
        "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
        "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\""
        " NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/></trt:Capabilities>"
        "</trt:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    onvif_send(c);
}

/* ---- refused changes (read only device) ---- */

static void op_refuse_set(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:ConfigModify", "The configuration cannot be changed (read-only device)");
}
static void op_refuse_conf(onvif_call_t *c)
{
    onvif_fault(c, 1, "ter:Action", "ter:ConfigurationConflict", "The profile is fixed and cannot be changed");
}
static void op_refuse_create(onvif_call_t *c)
{
    onvif_fault(c, 1, "ter:Action", "ter:MaxNVTProfiles", "The maximum number of profiles is already reached");
}
static void op_refuse_delete(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:DeletionOfFixedProfile", "The profile is fixed");
}

static const onvif_op_t k_ops[] = {
    { "GetServiceCapabilities", ONVIF_PRE, op_service_caps },
    { "GetProfiles", ONVIF_READ, op_get_profiles },
    { "GetProfile", ONVIF_READ, op_get_profile },
    { "GetVideoSources", ONVIF_READ, op_get_video_sources },
    { "GetVideoSourceConfigurations", ONVIF_READ, op_get_vsrc_configs },
    { "GetVideoSourceConfiguration", ONVIF_READ, op_get_vsrc_config },
    { "GetVideoSourceConfigurationOptions", ONVIF_READ, op_get_vsrc_options },
    { "GetVideoEncoderConfigurations", ONVIF_READ, op_get_venc_configs },
    { "GetVideoEncoderConfiguration", ONVIF_READ, op_get_venc_config },
    { "GetVideoEncoderConfigurationOptions", ONVIF_READ, op_get_venc_options },
    { "GetGuaranteedNumberOfVideoEncoderInstances", ONVIF_READ, op_get_guaranteed },
    { "GetAudioSources", ONVIF_READ, op_get_audio_sources },
    { "GetAudioSourceConfigurations", ONVIF_READ, op_get_asrc_configs },
    { "GetAudioSourceConfiguration", ONVIF_READ, op_no_config },
    { "GetAudioSourceConfigurationOptions", ONVIF_READ, op_no_config },
    { "GetAudioEncoderConfigurations", ONVIF_READ, op_get_aenc_configs },
    { "GetAudioEncoderConfiguration", ONVIF_READ, op_no_config },
    { "GetAudioEncoderConfigurationOptions", ONVIF_READ, op_no_config },
    { "GetAudioOutputs", ONVIF_READ, op_get_audio_outputs },
    { "GetAudioOutputConfigurations", ONVIF_READ, op_get_aout_configs },
    { "GetAudioOutputConfiguration", ONVIF_READ, op_get_aout_config },
    { "GetAudioOutputConfigurationOptions", ONVIF_READ, op_get_aout_options },
    { "GetAudioDecoderConfigurations", ONVIF_READ, op_get_adec_configs },
    { "GetAudioDecoderConfiguration", ONVIF_READ, op_get_adec_config },
    { "GetAudioDecoderConfigurationOptions", ONVIF_READ, op_get_adec_options },
    { "GetCompatibleVideoSourceConfigurations", ONVIF_READ, op_get_compat_vsrc },
    { "GetCompatibleVideoEncoderConfigurations", ONVIF_READ, op_get_compat_venc },
    { "GetCompatibleAudioSourceConfigurations", ONVIF_READ, op_get_compat_asrc },
    { "GetCompatibleAudioEncoderConfigurations", ONVIF_READ, op_get_compat_aenc },
    { "GetCompatibleAudioOutputConfigurations", ONVIF_READ, op_get_compat_aout },
    { "GetCompatibleAudioDecoderConfigurations", ONVIF_READ, op_get_compat_adec },
    { "GetStreamUri", ONVIF_READ, op_get_stream_uri },
    { "GetSnapshotUri", ONVIF_READ, op_get_snapshot_uri },
    { "SetVideoSourceConfiguration", ONVIF_WRITE, op_refuse_set },
    { "SetVideoEncoderConfiguration", ONVIF_WRITE, op_refuse_set },
    { "SetAudioSourceConfiguration", ONVIF_WRITE, op_refuse_set },
    { "SetAudioEncoderConfiguration", ONVIF_WRITE, op_refuse_set },
    { "SetAudioOutputConfiguration", ONVIF_WRITE, op_refuse_set },
    { "SetAudioDecoderConfiguration", ONVIF_WRITE, op_refuse_set },
    { "AddVideoSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddVideoEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddAudioSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddAudioEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddAudioOutputConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "AddAudioDecoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveVideoSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveVideoEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveAudioSourceConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveAudioEncoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveAudioOutputConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "RemoveAudioDecoderConfiguration", ONVIF_WRITE, op_refuse_conf },
    { "CreateProfile", ONVIF_WRITE, op_refuse_create },
    { "DeleteProfile", ONVIF_WRITE, op_refuse_delete },
};

int onvif_media_register(void)
{
    return onvif_add_service("/onvif/media_service", k_ops, sizeof(k_ops) / sizeof(k_ops[0]));
}
