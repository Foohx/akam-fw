#define _GNU_SOURCE
/* Host test of src/ak_rtsp/onvif_media.c and onvif_imaging.c over real HTTP on a localhost port (with the real
 * onvif.c gate, cred.c, rtsp_auth.c), the camera side stubbed by onvif_stubs.c: every Media and Imaging operation
 * against golden responses (tests/fixtures/onvif_media/), the access classes (reads open and writes refused with
 * the password OFF, token needed with it ON), the stream URI taken from getsockname, the SetImagingSettings
 * mapping, validation and the persistence debounce with an injected clock.
 * Set ONVIF_UPDATE_GOLDEN=1 to rewrite the goldens (then review the diff).
 * Run: onvif_media_test TEMPDIR FIXTURE_DIR */
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "b64.h"
#include "cred.h"
#include "md5.h"
#include "onvif.h"
#include "onvif_device.h"
#include "onvif_imaging.h"
#include "rtsp_auth.h"
#include "sha1.h"
#include "video_cfg.h"

const char *firmware_version(void) { return "btest"; }

/* from onvif_stubs.c */
extern int g_stub_talk, g_stub_volume, g_stub_isp[4], g_stub_fail_key, g_stub_nlog, g_stub_persist_calls;
extern char g_stub_night[8], g_stub_log[64][48];

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static char dir[256], fixdir[300], cpath[300], apath[300];
static int g_port;
static rtsp_auth_db_t limiter;
static int64_t g_mono = 5000000000LL;
static int64_t fake_mono(void) { return __atomic_add_fetch(&g_mono, 1000, __ATOMIC_SEQ_CST); }
static int64_t g_clock_ms = 1000000;
static int64_t fake_ms(void) { return g_clock_ms; }

#define USER "viewer"
#define PASS "Correct-Horse-9"

static void put(const char *path, const char *content)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, content, strlen(content)) < 0) { perror("put"); exit(2); }
    close(fd);
    rename(tmp, path);
}
static void auth_on(void)
{
    char ha1[33], line[128], cr[128];
    md5_hex_cat(ha1, USER, ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "%s:akam:%s\n", USER, ha1);
    snprintf(cr, sizeof(cr), "user=%s\npass=%s\n", USER, PASS);
    put(apath, line);
    put(cpath, cr);
    cred_refresh(true);
}
static void auth_off(void)
{
    unlink(apath);
    unlink(cpath);
    cred_refresh(true);
}

/* ---------------------------------------------------------------- client */

typedef struct { int status; char hdr[2048]; char body[32768]; } hresp_t;

static int request(const char *host, const char *target, const char *body, hresp_t *r)
{
    memset(r, 0, sizeof(*r));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    char head[512];
    size_t bl = strlen(body);
    int n = snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                     "Content-Type: application/soap+xml\r\nContent-Length: %zu\r\n\r\n", target, bl);
    send(fd, head, (size_t)n, MSG_NOSIGNAL);
    send(fd, body, bl, MSG_NOSIGNAL);
    static char buf[40000];
    size_t have = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 8000) <= 0) break;
        ssize_t k = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
        if (k <= 0) break;
        have += (size_t)k;
    }
    close(fd);
    buf[have] = 0;
    char *he = strstr(buf, "\r\n\r\n");
    if (!he || sscanf(buf, "HTTP/1.1 %d", &r->status) != 1) return -2;
    snprintf(r->hdr, sizeof(r->hdr), "%.*s", (int)(he - buf), buf);
    snprintf(r->body, sizeof(r->body), "%s", he + 4);
    return 0;
}

static void wsse_header(char *out, size_t cap, const char *pass, int ctr)
{
    uint8_t raw[16] = { 0x55, (uint8_t)ctr, (uint8_t)(ctr >> 8) }, d[20];
    char nonce[32], created[40], dg[64];
    b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &t);
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, raw, sizeof(raw));
    sha1_update(&c, created, strlen(created));
    sha1_update(&c, pass, strlen(pass));
    sha1_final(&c, d);
    b64_encode(d, 20, dg, sizeof(dg));
    snprintf(out, cap,
        "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<wsse:UsernameToken><wsse:Username>" USER "</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</wsse:Password>"
        "<wsse:Nonce>%s</wsse:Nonce><wsu:Created>%s</wsu:Created></wsse:UsernameToken></wsse:Security>", dg, nonce, created);
}

#define MEDIA "/onvif/media_service"
#define IMG "/onvif/imaging_service"
#define TRT "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
#define TIMG "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\""
#define TT "xmlns:tt=\"http://www.onvif.org/ver10/schema\""
#define OPT(name) "<trt:" name " " TRT "/>"
#define OPI(name) "<timg:" name " " TIMG "/>"

static int g_ctr;
/* One request; `login` = send a fresh valid WS-Security token. The limiter is reset first (not what is tested). */
static int call(const char *host, const char *path, int login, const char *op, hresp_t *r)
{
    char h[1500] = "", body[4096];
    rtsp_auth_note_success(&limiter);
    if (login) wsse_header(h, sizeof(h), PASS, g_ctr++);
    snprintf(body, sizeof(body), "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
             "<s:Header>%s</s:Header><s:Body>%s</s:Body></s:Envelope>", h, op);
    return request(host, path, body, r);
}
static int has(const hresp_t *r, const char *s) { return strstr(r->body, s) != NULL; }

static void write_body(const char *name, const hresp_t *r)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/resp_%s.xml", dir, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(r->body, f); fclose(f); }
}

static void golden(const char *name, const char *path, const char *op, int expect_status)
{
    hresp_t r;
    char p[400], msg[160], want[32768], got[32768], portstr[16];
    if (call("127.0.0.1", path, 1, op, &r) != 0 || r.status != expect_status) {
        snprintf(msg, sizeof(msg), "golden %s: status %d (want %d)", name, r.status, expect_status);
        CHECK(0, msg);
        return;
    }
    write_body(name, &r);
    snprintf(portstr, sizeof(portstr), ":%d", g_port);
    char *o = got;
    for (const char *s = r.body; *s;) {
        if (!strncmp(s, portstr, strlen(portstr))) { o += sprintf(o, ":@PORT@"); s += strlen(portstr); }
        else *o++ = *s++;
    }
    *o = 0;
    snprintf(p, sizeof(p), "%s/%s.xml", fixdir, name);
    if (getenv("ONVIF_UPDATE_GOLDEN")) {
        FILE *f = fopen(p, "w");
        if (f) { fputs(got, f); fputc('\n', f); fclose(f); }
    }
    FILE *f = fopen(p, "r");
    size_t n = 0;
    if (f) { n = fread(want, 1, sizeof(want) - 1, f); fclose(f); }
    want[n] = 0;
    while (n && (want[n - 1] == '\n' || want[n - 1] == '\r')) want[--n] = 0;
    snprintf(msg, sizeof(msg), "golden %s (HTTP %d)", name, expect_status);
    CHECK(f && !strcmp(want, got), msg);
    if (f && strcmp(want, got)) printf("      want: %s\n      got:  %s\n", want, got);
}

/* ---------------------------------------------------------------- tests */

static int g_reboots;
static void fake_reboot(void) { g_reboots++; }

static void reset_stub(void)
{
    g_stub_talk = 0;
    g_stub_volume = 50;
    memset(g_stub_isp, 0, sizeof(g_stub_isp));
    strcpy(g_stub_night, "auto");
    g_stub_fail_key = 0;
    g_stub_nlog = 0;
    g_stub_persist_calls = 0;
}

static void test_classes(void)
{
    printf("Access classes\n");
    hresp_t r;
    reset_stub();
    g_stub_talk = 1;
    auth_off();
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetProfiles"), &r) == 0 && r.status == 200 && has(&r, "GetProfilesResponse"),
          "password OFF: GetProfiles is open");
    CHECK(has(&r, "token=\"venccfg\"") && !has(&r, "aoutcfg"), "password OFF: no audio output in the profile (backchannel needs the password)");
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetAudioOutputs"), &r) == 0 && r.status == 200 && !has(&r, "AudioOutputs token"),
          "password OFF: GetAudioOutputs empty although talk is enabled");
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetStreamUri") , &r) == 0 && r.status == 400, "GetStreamUri without ProfileToken: fault");
    CHECK(call("127.0.0.1", IMG, 0, "<timg:GetImagingSettings " TIMG "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken></timg:GetImagingSettings>", &r) == 0 &&
          r.status == 200, "password OFF: GetImagingSettings is open");
    const char *w[] = {
        "<trt:SetVideoEncoderConfiguration " TRT "/>", "<trt:CreateProfile " TRT "/>", "<trt:DeleteProfile " TRT "/>",
        "<trt:AddVideoEncoderConfiguration " TRT "/>", "<trt:RemoveVideoSourceConfiguration " TRT "/>", NULL };
    for (int i = 0; w[i]; i++) {
        char msg[96];
        int rc = call("127.0.0.1", MEDIA, 1, w[i], &r);
        snprintf(msg, sizeof(msg), "password OFF: %.30s refused with ter:NotAuthorized", w[i] + 1);
        CHECK(rc == 0 && r.status == 400 && has(&r, "ter:NotAuthorized"), msg);
    }
    CHECK(call("127.0.0.1", IMG, 1, "<timg:SetImagingSettings " TIMG " " TT "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken>"
              "<timg:ImagingSettings><tt:Brightness>10</tt:Brightness></timg:ImagingSettings></timg:SetImagingSettings>", &r) == 0 &&
          r.status == 400 && has(&r, "ter:NotAuthorized") && g_stub_nlog == 0 && g_stub_isp[0] == 0,
          "password OFF: SetImagingSettings refused, nothing applied");

    auth_on();
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetServiceCapabilities"), &r) == 0 && r.status == 200, "password ON: media GetServiceCapabilities open");
    CHECK(call("127.0.0.1", IMG, 0, OPI("GetServiceCapabilities"), &r) == 0 && r.status == 200, "password ON: imaging GetServiceCapabilities open");
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetProfiles"), &r) == 0 && r.status == 401, "password ON: GetProfiles needs a login (401)");
    CHECK(call("127.0.0.1", MEDIA, 0, OPT("GetStreamUri"), &r) == 0 && r.status == 401, "password ON: GetStreamUri needs a login");
    CHECK(call("127.0.0.1", IMG, 0, OPI("GetOptions"), &r) == 0 && r.status == 401, "password ON: imaging GetOptions needs a login");
    CHECK(call("127.0.0.1", IMG, 0, OPI("SetImagingSettings"), &r) == 0 && r.status == 401 && g_stub_nlog == 0,
          "password ON: SetImagingSettings without login: 401, nothing applied");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetProfiles"), &r) == 0 && r.status == 200, "password ON: valid token accepted");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("SetVideoEncoderConfiguration"), &r) == 0 && r.status == 400 && has(&r, "ter:ConfigModify"),
          "authorised Set*Configuration: ter:ConfigModify");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("CreateProfile"), &r) == 0 && r.status == 500 && has(&r, "ter:MaxNVTProfiles"), "CreateProfile: ter:MaxNVTProfiles");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("DeleteProfile"), &r) == 0 && r.status == 400 && has(&r, "ter:DeletionOfFixedProfile"), "DeleteProfile: fixed profile");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("AddAudioEncoderConfiguration"), &r) == 0 && r.status == 500 && has(&r, "ter:ConfigurationConflict"),
          "Add*Configuration refused");
}

#define GETPROF "<trt:GetProfile " TRT "><trt:ProfileToken>main</trt:ProfileToken></trt:GetProfile>"
#define CFGTOK(op, tok) "<trt:" op " " TRT "><trt:ConfigurationToken>" tok "</trt:ConfigurationToken></trt:" op ">"
#define COMPAT(op) "<trt:" op " " TRT "><trt:ProfileToken>main</trt:ProfileToken></trt:" op ">"
#define STREAM(stream, proto) "<trt:GetStreamUri " TRT " " TT "><trt:StreamSetup><tt:Stream>" stream "</tt:Stream>" \
    "<tt:Transport><tt:Protocol>" proto "</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>main</trt:ProfileToken></trt:GetStreamUri>"

static void test_media_golden(void)
{
    printf("Media: golden responses (backchannel off)\n");
    auth_on();
    reset_stub();
    golden("media_GetServiceCapabilities", MEDIA, OPT("GetServiceCapabilities"), 200);
    golden("media_GetProfiles", MEDIA, OPT("GetProfiles"), 200);
    golden("media_GetProfile", MEDIA, GETPROF, 200);
    golden("media_GetProfile_unknown", MEDIA, "<trt:GetProfile " TRT "><trt:ProfileToken>sub</trt:ProfileToken></trt:GetProfile>", 400);
    golden("media_GetVideoSources", MEDIA, OPT("GetVideoSources"), 200);
    golden("media_GetVideoSourceConfigurations", MEDIA, OPT("GetVideoSourceConfigurations"), 200);
    golden("media_GetVideoSourceConfiguration", MEDIA, CFGTOK("GetVideoSourceConfiguration", "vsrccfg"), 200);
    golden("media_GetVideoSourceConfiguration_unknown", MEDIA, CFGTOK("GetVideoSourceConfiguration", "nope"), 400);
    golden("media_GetVideoSourceConfigurationOptions", MEDIA, OPT("GetVideoSourceConfigurationOptions"), 200);
    golden("media_GetVideoEncoderConfigurations", MEDIA, OPT("GetVideoEncoderConfigurations"), 200);
    golden("media_GetVideoEncoderConfiguration", MEDIA, CFGTOK("GetVideoEncoderConfiguration", "venccfg"), 200);
    golden("media_GetVideoEncoderConfigurationOptions", MEDIA, OPT("GetVideoEncoderConfigurationOptions"), 200);
    golden("media_GetGuaranteedNumberOfVideoEncoderInstances", MEDIA, OPT("GetGuaranteedNumberOfVideoEncoderInstances"), 200);
    golden("media_GetAudioSources", MEDIA, OPT("GetAudioSources"), 200);
    golden("media_GetAudioSourceConfigurations", MEDIA, OPT("GetAudioSourceConfigurations"), 200);
    golden("media_GetAudioEncoderConfigurations", MEDIA, OPT("GetAudioEncoderConfigurations"), 200);
    golden("media_GetAudioOutputs_off", MEDIA, OPT("GetAudioOutputs"), 200);
    golden("media_GetAudioOutputConfigurations_off", MEDIA, OPT("GetAudioOutputConfigurations"), 200);
    golden("media_GetAudioDecoderConfigurations_off", MEDIA, OPT("GetAudioDecoderConfigurations"), 200);
    golden("media_GetAudioOutputConfiguration_off", MEDIA, CFGTOK("GetAudioOutputConfiguration", "aoutcfg"), 400);
    golden("media_GetCompatibleVideoSourceConfigurations", MEDIA, COMPAT("GetCompatibleVideoSourceConfigurations"), 200);
    golden("media_GetCompatibleVideoEncoderConfigurations", MEDIA, COMPAT("GetCompatibleVideoEncoderConfigurations"), 200);
    golden("media_GetCompatibleAudioOutputConfigurations_off", MEDIA, COMPAT("GetCompatibleAudioOutputConfigurations"), 200);
    golden("media_GetSnapshotUri", MEDIA, COMPAT("GetSnapshotUri"), 500);

    printf("Media: golden responses (backchannel on)\n");
    g_stub_talk = 1;
    g_stub_volume = 70;
    golden("media_GetProfiles_bc", MEDIA, OPT("GetProfiles"), 200);
    golden("media_GetProfile_bc", MEDIA, GETPROF, 200);
    golden("media_GetAudioOutputs_bc", MEDIA, OPT("GetAudioOutputs"), 200);
    golden("media_GetAudioOutputConfigurations_bc", MEDIA, OPT("GetAudioOutputConfigurations"), 200);
    golden("media_GetAudioOutputConfiguration_bc", MEDIA, CFGTOK("GetAudioOutputConfiguration", "aoutcfg"), 200);
    golden("media_GetAudioOutputConfigurationOptions_bc", MEDIA, CFGTOK("GetAudioOutputConfigurationOptions", "aoutcfg"), 200);
    golden("media_GetAudioDecoderConfigurations_bc", MEDIA, OPT("GetAudioDecoderConfigurations"), 200);
    golden("media_GetAudioDecoderConfiguration_bc", MEDIA, CFGTOK("GetAudioDecoderConfiguration", "adec"), 200);
    golden("media_GetAudioDecoderConfigurationOptions_bc", MEDIA, CFGTOK("GetAudioDecoderConfigurationOptions", "adec"), 200);
    golden("media_GetCompatibleAudioOutputConfigurations_bc", MEDIA, COMPAT("GetCompatibleAudioOutputConfigurations"), 200);
    golden("media_GetCompatibleAudioDecoderConfigurations_bc", MEDIA, COMPAT("GetCompatibleAudioDecoderConfigurations"), 200);
    hresp_t r;
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetAudioOutputConfigurations"), &r) == 0 && has(&r, "<tt:OutputLevel>70</tt:OutputLevel>"),
          "OutputLevel follows audio.out_volume");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetProfiles"), &r) == 0 && !has(&r, "AudioSourceConfiguration") && !has(&r, "AudioEncoderConfiguration"),
          "profile never carries an audio source or encoder (the microphone is L16, not describable)");
    g_stub_talk = 0;
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetAudioDecoderConfigurations"), &r) == 0 && !has(&r, "adec"),
          "talk switched off: the decoder configuration disappears at once");
    golden("media_GetAudioEncoderConfiguration_none", MEDIA, CFGTOK("GetAudioEncoderConfiguration", "x"), 400);
}

/* The reported bitrate, frame rate and GOP follow the saved video settings; the defaults give the goldens above. */
static void test_video_cfg(void)
{
    printf("Media: video settings reported from /etc/config/akam/video\n");
    hresp_t r;
    char p[400];
    snprintf(p, sizeof(p), "%s/video", dir);
    put(p, "bitrate=900\nfps=10\ngop=3\n");
    video_cfg_reset_for_test();
    video_cfg_load();
    CHECK(call("127.0.0.1", MEDIA, 1, CFGTOK("GetVideoEncoderConfiguration", "venccfg"), &r) == 0 &&
          has(&r, "<tt:FrameRateLimit>10</tt:FrameRateLimit>") && has(&r, "<tt:BitrateLimit>900</tt:BitrateLimit>") &&
          has(&r, "<tt:GovLength>30</tt:GovLength>"), "GetVideoEncoderConfiguration: configured fps, bitrate, GOP (3 s x 10 fps)");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetVideoEncoderConfigurations"), &r) == 0 && has(&r, "<tt:BitrateLimit>900</tt:BitrateLimit>"),
          "GetVideoEncoderConfigurations: configured bitrate");
    CHECK(call("127.0.0.1", MEDIA, 1, GETPROF, &r) == 0 && has(&r, "<tt:FrameRateLimit>10</tt:FrameRateLimit>") &&
          has(&r, "<tt:GovLength>30</tt:GovLength>"), "GetProfile carries the configured values");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetVideoEncoderConfigurationOptions"), &r) == 0 &&
          has(&r, "<tt:FrameRateRange><tt:Min>10</tt:Min><tt:Max>10</tt:Max></tt:FrameRateRange>") &&
          has(&r, "<tt:GovLengthRange><tt:Min>30</tt:Min><tt:Max>30</tt:Max></tt:GovLengthRange>"), "Options: configured fps and GOP");
    CHECK(call("127.0.0.1", MEDIA, 1, OPT("GetVideoSources"), &r) == 0 && has(&r, "<tt:Framerate>10</tt:Framerate>"),
          "GetVideoSources: configured frame rate");
    /* an invalid file leaves the defaults */
    put(p, "bitrate=99999\nfps=7\ngop=0\n");
    video_cfg_reset_for_test();
    video_cfg_load();
    golden("media_GetVideoEncoderConfiguration", MEDIA, CFGTOK("GetVideoEncoderConfiguration", "venccfg"), 200);
    unlink(p);
    video_cfg_reset_for_test();
}

static void test_stream_uri(void)
{
    printf("GetStreamUri\n");
    auth_on();
    hresp_t r;
    CHECK(call("127.0.0.1", MEDIA, 1, STREAM("RTP-Unicast", "RTSP"), &r) == 0 && r.status == 200 &&
          has(&r, "<tt:Uri>rtsp://127.0.0.1:554/</tt:Uri>"), "RTP-Unicast + RTSP: rtsp://127.0.0.1:554/ (address of the connection)");
    CHECK(call("127.0.0.2", MEDIA, 1, STREAM("RTP-Unicast", "TCP"), &r) == 0 && r.status == 200 &&
          has(&r, "<tt:Uri>rtsp://127.0.0.2:554/</tt:Uri>"), "TCP, reached through 127.0.0.2: the URI follows getsockname");
    CHECK(call("127.0.0.1", MEDIA, 1, STREAM("RTP-Unicast", "UDP"), &r) == 0 && r.status == 200, "UDP accepted");
    CHECK(call("127.0.0.1", MEDIA, 1, STREAM("RTP-Multicast", "UDP"), &r) == 0 && r.status == 400 && has(&r, "ter:InvalidStreamSetup"),
          "multicast: fault ter:InvalidStreamSetup");
    CHECK(call("127.0.0.1", MEDIA, 1, STREAM("RTP-Unicast", "HTTP"), &r) == 0 && r.status == 400 && has(&r, "ter:InvalidStreamSetup"),
          "HTTP transport: fault");
    CHECK(call("127.0.0.1", MEDIA, 1, "<trt:GetStreamUri " TRT "><trt:ProfileToken>other</trt:ProfileToken></trt:GetStreamUri>", &r) == 0 &&
          r.status == 400 && has(&r, "ter:NoProfile"), "unknown profile: ter:NoProfile");
    CHECK(call("127.0.0.1", MEDIA, 1, COMPAT("GetSnapshotUri"), &r) == 0 && r.status == 500 && has(&r, "ter:ActionNotSupported"),
          "GetSnapshotUri: ter:ActionNotSupported");
}

#define SETIMG(body) "<timg:SetImagingSettings " TIMG " " TT "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken>" \
    "<timg:ImagingSettings>" body "</timg:ImagingSettings></timg:SetImagingSettings>"
#define SETIMG_FP(body, fp) "<timg:SetImagingSettings " TIMG " " TT "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken>" \
    "<timg:ImagingSettings>" body "</timg:ImagingSettings><timg:ForcePersistence>" fp "</timg:ForcePersistence></timg:SetImagingSettings>"
#define GETIMG(op) "<timg:" op " " TIMG "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken></timg:" op ">"

static int logged(const char *s)
{
    for (int i = 0; i < g_stub_nlog; i++)
        if (!strcmp(g_stub_log[i], s)) return 1;
    return 0;
}

static void test_imaging(void)
{
    printf("Imaging\n");
    auth_on();
    reset_stub();
    int64_t (*orig_clock)(void) = onvif_imaging_now_ms;
    onvif_imaging_now_ms = fake_ms;
    onvif_imaging_thread = 0;
    hresp_t r;
    golden("img_GetServiceCapabilities", IMG, OPI("GetServiceCapabilities"), 200);
    g_stub_isp[0] = 10; g_stub_isp[1] = -20; g_stub_isp[2] = 0; g_stub_isp[3] = 50;
    strcpy(g_stub_night, "day");
    golden("img_GetImagingSettings", IMG, GETIMG("GetImagingSettings"), 200);
    golden("img_GetOptions", IMG, GETIMG("GetOptions"), 200);
    golden("img_GetImagingSettings_nosource", IMG, "<timg:GetImagingSettings " TIMG "><timg:VideoSourceToken>x</timg:VideoSourceToken></timg:GetImagingSettings>", 400);
    strcpy(g_stub_night, "night");
    CHECK(call("127.0.0.1", IMG, 1, GETIMG("GetImagingSettings"), &r) == 0 && has(&r, "<tt:IrCutFilter>OFF</tt:IrCutFilter>"), "night.mode night -> IrCutFilter OFF");
    strcpy(g_stub_night, "auto");
    CHECK(call("127.0.0.1", IMG, 1, GETIMG("GetImagingSettings"), &r) == 0 && has(&r, "<tt:IrCutFilter>AUTO</tt:IrCutFilter>"), "night.mode auto -> IrCutFilter AUTO");
    golden("img_SetImagingSettings", IMG, SETIMG("<tt:IrCutFilter>ON</tt:IrCutFilter>"), 200);
    CHECK(!strcmp(g_stub_night, "day") && logged("night.mode=day/p1"), "IrCutFilter ON -> night.mode day, persisted at once (persist=1)");
    reset_stub();
    onvif_imaging_poll();

    /* mapping of the four levels, RAM only */
    unsigned f0 = onvif_imaging_flushes();
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>10</tt:Brightness><tt:ColorSaturation>-5</tt:ColorSaturation>"
              "<tt:Contrast>20</tt:Contrast><tt:Sharpness>-50</tt:Sharpness>"), &r) == 0 && r.status == 200, "SetImagingSettings with the four levels");
    CHECK(g_stub_isp[0] == 10 && g_stub_isp[1] == 20 && g_stub_isp[2] == -5 && g_stub_isp[3] == -50, "values reach isp.brightness/contrast/saturation/sharpness");
    CHECK(logged("isp.brightness=10/p0") && logged("isp.sharpness=-50/p0") && !logged("isp.brightness=10/p1"), "applied with persist=0 (RAM)");
    CHECK(g_stub_persist_calls == 0 && onvif_imaging_flushes() == f0, "nothing written to flash yet");

    /* debounce */
    g_clock_ms += 4999;
    CHECK(onvif_imaging_poll() == 0 && g_stub_persist_calls == 0, "4.999 s after the change: still not written");
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>11</tt:Brightness>"), &r) == 0 && r.status == 200, "slider moves again");
    g_clock_ms += 4999;
    CHECK(onvif_imaging_poll() == 0 && g_stub_persist_calls == 0, "the quiet time restarts with each change");
    g_clock_ms += 1;
    CHECK(onvif_imaging_poll() == 1 && g_stub_persist_calls == 1, "5 s after the last change: one flash write");
    CHECK(onvif_imaging_poll() == 0 && g_stub_persist_calls == 1, "and no second write");
    CHECK(onvif_imaging_flushes() == f0 + 1, "flush counter");
    int n = 0;
    for (int i = 0; i < 20; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", i - 10);
        char op[512];
        snprintf(op, sizeof(op), SETIMG("<tt:Contrast>%s</tt:Contrast>"), v);
        call("127.0.0.1", IMG, 1, op, &r);
        g_clock_ms += 100;
        n += (int)onvif_imaging_poll();
    }
    g_clock_ms += 5000;
    n += (int)onvif_imaging_poll();
    CHECK(n == 1 && g_stub_persist_calls == 2, "20 slider steps 100 ms apart: exactly one more flash write");

    /* unchanged values do not touch the hardware or the timer */
    g_stub_nlog = 0;
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>11</tt:Brightness><tt:Contrast>9</tt:Contrast>"), &r) == 0 && r.status == 200 && g_stub_nlog == 0,
          "a client echoing the current values: no setter call");
    g_clock_ms += 10000;
    CHECK(onvif_imaging_poll() == 0, "... and nothing pending");

    /* ForcePersistence */
    int pc = g_stub_persist_calls;
    CHECK(call("127.0.0.1", IMG, 1, SETIMG_FP("<tt:Brightness>-7</tt:Brightness>", "true"), &r) == 0 && r.status == 200 &&
          g_stub_persist_calls == pc + 1, "ForcePersistence=true writes at once");
    CHECK(onvif_imaging_poll() == 0, "... and leaves nothing pending");

    /* SystemReboot flushes a pending debounced picture write first */
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>-3</tt:Brightness>"), &r) == 0 && r.status == 200, "a change is pending");
    pc = g_stub_persist_calls;
    int rb = g_reboots;
    CHECK(call("127.0.0.1", "/onvif/device_service", 1, "<tds:SystemReboot xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>", &r) == 0 &&
          r.status == 200 && g_stub_persist_calls == pc + 1 && g_reboots == rb + 1, "SystemReboot writes the pending picture values, then reboots");
    CHECK(onvif_imaging_poll() == 0, "... nothing left pending");

    /* decimals */
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>12.5</tt:Brightness><tt:Sharpness>-0.4</tt:Sharpness>"), &r) == 0 && r.status == 200 &&
          g_stub_isp[0] == 13 && g_stub_isp[3] == 0, "decimals round to the nearest integer (12.5 -> 13, -0.4 -> 0)");

    /* IrCutFilter all modes, persist at once */
    g_stub_nlog = 0;
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:IrCutFilter>OFF</tt:IrCutFilter>"), &r) == 0 && !strcmp(g_stub_night, "night") && logged("night.mode=night/p1"), "OFF -> night");
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:IrCutFilter>AUTO</tt:IrCutFilter>"), &r) == 0 && !strcmp(g_stub_night, "auto") && logged("night.mode=auto/p1"), "AUTO -> auto");
    g_stub_nlog = 0;
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:IrCutFilter>AUTO</tt:IrCutFilter>"), &r) == 0 && r.status == 200 && g_stub_nlog == 0, "same mode again: no write");

    /* validation: nothing applied */
    g_stub_nlog = 0;
    int b = g_stub_isp[0];
    const char *bad[] = {
        SETIMG("<tt:Brightness>51</tt:Brightness>"),
        SETIMG("<tt:Brightness>-50.6</tt:Brightness>"),
        SETIMG("<tt:Contrast>abc</tt:Contrast>"),
        SETIMG("<tt:Contrast>1e1</tt:Contrast>"),
        SETIMG("<tt:Contrast></tt:Contrast>"),
        SETIMG("<tt:Brightness>5</tt:Brightness><tt:Sharpness>99</tt:Sharpness>"),
        SETIMG("<tt:Brightness>5</tt:Brightness><tt:IrCutFilter>MAYBE</tt:IrCutFilter>"),
        SETIMG("<tt:WhiteBalance><tt:Mode>AUTO</tt:Mode></tt:WhiteBalance>"),
        SETIMG("<tt:Exposure><tt:Mode>AUTO</tt:Mode></tt:Exposure>"),
        SETIMG("<tt:Focus><tt:AutoFocusMode>AUTO</tt:AutoFocusMode></tt:Focus>"),
        SETIMG("<tt:WideDynamicRange><tt:Mode>OFF</tt:Mode></tt:WideDynamicRange>"),
        SETIMG("<tt:BacklightCompensation><tt:Mode>OFF</tt:Mode></tt:BacklightCompensation>"),
        NULL };
    int ok = 1;
    for (int i = 0; bad[i]; i++) {
        int rc = call("127.0.0.1", IMG, 1, bad[i], &r);
        if (!(rc == 0 && r.status == 400 && has(&r, "ter:SettingsInvalid"))) { ok = 0; printf("      case %d: status %d\n", i, r.status); }
    }
    CHECK(ok, "out of range / not a number / unsupported element: Sender fault ter:SettingsInvalid");
    CHECK(g_stub_nlog == 0 && g_stub_isp[0] == b, "... and not a single setter call (validated first)");
    CHECK(call("127.0.0.1", IMG, 1, "<timg:SetImagingSettings " TIMG "><timg:VideoSourceToken>nope</timg:VideoSourceToken>"
              "<timg:ImagingSettings/></timg:SetImagingSettings>", &r) == 0 && r.status == 400 && has(&r, "ter:NoSource"), "unknown video source: ter:NoSource");
    CHECK(call("127.0.0.1", IMG, 1, "<timg:SetImagingSettings " TIMG "><timg:VideoSourceToken>vsrc</timg:VideoSourceToken></timg:SetImagingSettings>", &r) == 0 &&
          r.status == 400, "no ImagingSettings: fault");
    CHECK(call("127.0.0.1", IMG, 1, SETIMG(""), &r) == 0 && r.status == 200 && g_stub_nlog == 0, "empty ImagingSettings: accepted, nothing to do");

    /* a refusing camera */
    g_stub_fail_key = 1;
    CHECK(call("127.0.0.1", IMG, 1, SETIMG("<tt:Brightness>3</tt:Brightness>"), &r) == 0 && r.status == 500 && has(&r, "env:Receiver"), "setter refuses: Receiver fault");
    g_stub_fail_key = 0;
    onvif_imaging_now_ms = orig_clock;
    CHECK(onvif_imaging_now_ms() > 0, "the default clock (CLOCK_MONOTONIC) answers");
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: onvif_media_test TEMPDIR FIXTURE_DIR\n"); return 2; }
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(fixdir, sizeof(fixdir), "%s", argv[2]);
    snprintf(cpath, sizeof(cpath), "%s/credentials", dir);
    snprintf(apath, sizeof(apath), "%s/rtsp_auth", dir);
    setenv("TZ", "UTC", 1);
    tzset();
    cred_mono_us = fake_mono;
    rtsp_auth_db_init(&limiter, apath);
    cred_init(cpath, &limiter);
    onvif_ifname = "lo";
    onvif_reboot_fn = fake_reboot;
    CHECK(onvif_start(0) == 0, "onvif_start on a free port");
    g_port = http_srv_port();
    test_classes();
    test_media_golden();
    test_video_cfg();
    test_stream_uri();
    test_imaging();
    onvif_stop();
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
