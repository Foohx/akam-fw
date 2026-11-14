/* ONVIF Device service, see onvif_device.h. Responses are static templates (soap.h); arguments are read with the
 * bounded scanner. No XML library, no allocation beyond the one per-response block of onvif.c. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cred.h"
#include "onvif.h"
#include "onvif_device.h"
#include "onvif_imaging.h"
#include "version.h"

const char *onvif_hostname_path = ONVIF_HOSTNAME_FILE;
const char *onvif_resolv_path = ONVIF_RESOLV_FILE;
const char *onvif_ntp_path = ONVIF_NTP_FILE;
const char *onvif_route_path = ONVIF_ROUTE_FILE;

static int64_t default_time(void) { return (int64_t)time(NULL); }
static int default_settime(int64_t sec)
{
    struct timeval tv = { (time_t)sec, 0 };
    return settimeofday(&tv, NULL);
}
int64_t (*onvif_time_fn)(void) = default_time;
int (*onvif_settime_fn)(int64_t) = default_settime;

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE

/* ------------------------------------------------------------------------ hostname */

int onvif_hostname_valid(const char *n)
{
    size_t l = strlen(n);
    if (l < 1 || l > 32 || n[0] == '-' || n[l - 1] == '-') return 0;
    int alldigits = 1;
    for (size_t i = 0; i < l; i++) {
        char ch = n[i];
        int dig = ch >= '0' && ch <= '9';
        if (!dig && !(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && ch != '-') return 0;
        if (!dig) alldigits = 0;
    }
    return !alldigits;
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

void onvif_hostname_get(char *out)
{
    char line[64] = "";
    FILE *f = fopen(onvif_hostname_path, "re");
    if (f) {
        if (fgets(line, sizeof(line), f)) line[strcspn(line, "\r\n")] = '\0';
        fclose(f);
        if (onvif_hostname_valid(line)) {
            lower(line);
            memcpy(out, line, strlen(line) + 1);
            return;
        }
    }
    char mac[18];
    if (onvif_mac(mac) == 0) snprintf(out, 33, "akam-%c%c%c%c", mac[12], mac[13], mac[15], mac[16]);
    else snprintf(out, 33, "akam");
    lower(out);
}

/* Persist NAME in the file the helper uses (same path, one line, mode 0644, atomic rename). The kernel hostname
 * and the DHCP request pick it up at the next power cycle, exactly like `akam_hostname.sh set` which does not
 * restart udhcpc either; GetHostname reads the file, so it answers with the new name at once. */
static int hostname_store(const char *name)
{
    char tmp[300], dir[300];
    snprintf(tmp, sizeof(tmp), "%s.new.%d", onvif_hostname_path, (int)getpid());
    snprintf(dir, sizeof(dir), "%s", onvif_hostname_path);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; (void)mkdir(dir, 0755); }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "%s\n", name);
    int ok = write(fd, buf, (size_t)n) == n && fchmod(fd, 0644) == 0 && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, onvif_hostname_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ small helpers */

static int arg_text(onvif_call_t *c, const char *local, char *out, size_t cap)
{
    soap_elem_t e;
    if (soap_find(c->op.inner, local, &e) != SOAP_OK) return -1;
    return soap_text(&e, out, cap, SOAP_TRIM) >= 0 ? 0 : -1;
}

static int arg_int(soap_span_t range, const char *local, int *v)
{
    soap_elem_t e;
    char b[16];
    if (soap_find(range, local, &e) != SOAP_OK || soap_text(&e, b, sizeof(b), SOAP_TRIM) < 1) return -1;
    char *end;
    long x = strtol(b, &end, 10);
    if (*end != '\0') return -1;
    *v = (int)x;
    return 0;
}

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* ------------------------------------------------------------------------ operations */

static void op_device_information(onvif_call_t *c)
{
    char mac[18], serial[13];
    onvif_mac(mac);
    for (int i = 0, j = 0; i < 17; i++)
        if (mac[i] != ':') serial[j++] = mac[i];
    serial[12] = '\0';
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetDeviceInformationResponse><tds:Manufacturer>Akam</tds:Manufacturer><tds:Model>{{model}}</tds:Model>"
        "<tds:FirmwareVersion>{{fw}}</tds:FirmwareVersion><tds:SerialNumber>{{serial}}</tds:SerialNumber>"
        "<tds:HardwareId>{{hw}}</tds:HardwareId></tds:GetDeviceInformationResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "model", ONVIF_MODEL);
    soap_tpl_ref(t, "fw", firmware_version());
    soap_tpl_set(t, "serial", serial);
    soap_tpl_ref(t, "hw", ONVIF_HARDWARE_ID);
    onvif_send(c);
}

static void op_capabilities(onvif_call_t *c)
{
    char base[64];
    onvif_base_url(c->req, base, sizeof(base));
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
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
        "<tt:Imaging><tt:XAddr>{{base}}/onvif/imaging_service</tt:XAddr></tt:Imaging>"
        "<tt:Media><tt:XAddr>{{base}}/onvif/media_service</tt:XAddr><tt:StreamingCapabilities>"
        "<tt:RTPMulticast>false</tt:RTPMulticast><tt:RTP_TCP>false</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
        "</tt:StreamingCapabilities></tt:Media>"
        "</tds:Capabilities></tds:GetCapabilitiesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "base", base);
    onvif_send(c);
}

/* The servers akam_ntp.sh was given (file of `server=NAME` lines, at most 3; 0 when it is off). With out, the
 * <tt:NTPManual> elements for them are written there (cap >= ONVIF_SCRATCH). Names are re-validated here: the file is
 * root-owned but this goes into XML and nothing but a host name or an IPv4 address may. */
static int ntp_servers(char *out, size_t cap)
{
    int n = 0;
    size_t used = 0;
    FILE *f = fopen(onvif_ntp_path, "re");
    if (out && cap) out[0] = '\0';
    if (!f) return 0;
    char line[96];
    while (n < 3 && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "server=", 7) != 0) continue;
        char *v = line + 7;
        v[strcspn(v, "\r\n")] = '\0';
        size_t l = strlen(v);
        int ok = l >= 1 && l <= 63 && v[0] != '-';
        for (size_t i = 0; ok && i < l; i++) {
            char ch = v[i];
            ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        }
        if (!ok) continue;
        struct in_addr a;
        int ip4 = inet_pton(AF_INET, v, &a) == 1;
        if (out) {
            int w = snprintf(out + used, cap - used, "<tt:NTPManual><tt:Type>%s</tt:Type><tt:%s>%s</tt:%s></tt:NTPManual>",
                             ip4 ? "IPv4" : "DNS", ip4 ? "IPv4Address" : "DNSname", v, ip4 ? "IPv4Address" : "DNSname");
            if (w < 0 || (size_t)w >= cap - used) { out[used] = '\0'; break; }
            used += (size_t)w;
        }
        n++;
    }
    fclose(f);
    return n;
}

#define DEV_CAPS_HEAD \
    "<tds:Capabilities>" \
    "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"" \
    " Dot11Configuration=\"false\" Dot1XConfigurations=\"0\" HostnameFromDHCP=\"false\" NTP=\""
#define DEV_CAPS_TAIL \
    "\" DHCPv6=\"false\"/>" \
    "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\"" \
    " AccessPolicyConfig=\"false\" DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\"" \
    " X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" UsernameToken=\"true\" HttpDigest=\"true\"" \
    " RELToken=\"false\" MaxUsers=\"1\" MaxUserNameLength=\"32\" MaxPasswordLength=\"128\"/>" \
    "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" RemoteDiscovery=\"false\" SystemBackup=\"false\"" \
    " SystemLogging=\"false\" FirmwareUpgrade=\"false\" HttpFirmwareUpgrade=\"false\" HttpSystemBackup=\"false\"" \
    " HttpSystemLogging=\"false\" HttpSupportInformation=\"false\" StorageConfiguration=\"false\"/>" \
    "</tds:Capabilities>"

static void op_services(onvif_call_t *c)
{
    char base[64], inc[8] = "";
    onvif_base_url(c->req, base, sizeof(base));
    soap_elem_t e;
    if (soap_find(c->op.inner, "IncludeCapability", &e) == SOAP_OK) (void)soap_text(&e, inc, sizeof(inc), SOAP_TRIM);
    int with = !strcmp(inc, "true") || !strcmp(inc, "1");
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetServicesResponse>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/device_service</tds:XAddr>{{cd}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/media_service</tds:XAddr>{{cm}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace>"
        "<tds:XAddr>{{base}}/onvif/imaging_service</tds:XAddr>{{ci}}"
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
        "</tds:GetServicesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "base", base);
    /* Service/Capabilities wraps the very element GetServiceCapabilities of that service returns */
    static const char dev_off[] = "<tds:Capabilities>" DEV_CAPS_HEAD "0" DEV_CAPS_TAIL "</tds:Capabilities>";
    static const char dev_ntp[] = "<tds:Capabilities>" DEV_CAPS_HEAD "3" DEV_CAPS_TAIL "</tds:Capabilities>";
    static const char med[] = "<tds:Capabilities>" ONVIF_MEDIA_CAPS "</tds:Capabilities>";
    static const char img[] = "<tds:Capabilities>" ONVIF_IMAGING_CAPS "</tds:Capabilities>";
    soap_tpl_set_raw(t, "cd", with ? (ntp_servers(NULL, 0) > 0 ? dev_ntp : dev_off) : "");
    soap_tpl_set_raw(t, "cm", with ? med : "");
    soap_tpl_set_raw(t, "ci", with ? img : "");
    onvif_send(c);
}

static void op_service_capabilities(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN "<tds:GetServiceCapabilitiesResponse>" DEV_CAPS_HEAD "{{ntp}}" DEV_CAPS_TAIL
                                "</tds:GetServiceCapabilitiesResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "ntp", ntp_servers(NULL, 0) > 0 ? "3" : "0");
    onvif_send(c);
}

/* POSIX TZ text ("UTC0", "UTC-1": the sign is inverted, west is positive) for the TZ element. */
static void tz_text(const struct tm *lt, char *out, size_t cap)
{
    long off = -lt->tm_gmtoff;
    if (off == 0) { snprintf(out, cap, "UTC0"); return; }
    long a = off < 0 ? -off : off;
    if (a % 3600 == 0) snprintf(out, cap, "UTC%c%ld", off < 0 ? '-' : '+', a / 3600);
    else snprintf(out, cap, "UTC%c%ld:%02ld", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
}

static int dt_block(char *s, size_t cap, const char *tag, const struct tm *t)
{
    return snprintf(s, cap,
        "<tt:%s><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
        "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:%s>",
        tag, t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, tag);
}

static void op_get_time(onvif_call_t *c)
{
    time_t now = (time_t)onvif_time_fn();
    struct tm u, l;
    gmtime_r(&now, &u);
    localtime_r(&now, &l);
    char tz[24];
    tz_text(&l, tz, sizeof(tz));
    char *s = c->out->scratch;
    int n = dt_block(s, ONVIF_SCRATCH, "UTCDateTime", &u);
    dt_block(s + n, (size_t)ONVIF_SCRATCH - (size_t)n, "LocalDateTime", &l);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>{{type}}</tt:DateTimeType>"
        "<tt:DaylightSavings>{{dst}}</tt:DaylightSavings><tt:TimeZone><tt:TZ>{{tz}}</tt:TZ></tt:TimeZone>"
        "{{times}}</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>" ENV_CLOSE);
    soap_tpl_ref(t, "type", ntp_servers(NULL, 0) > 0 ? "NTP" : "Manual");
    soap_tpl_ref(t, "dst", l.tm_isdst > 0 ? "true" : "false");
    soap_tpl_set(t, "tz", tz);
    soap_tpl_set_raw(t, "times", s);
    onvif_send(c);
}

static void op_set_time(onvif_call_t *c)
{
    char type[16];
    if (arg_text(c, "DateTimeType", type, sizeof(type)) != 0) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidArgVal", "DateTimeType missing");
        return;
    }
    if (strcmp(type, "Manual") != 0) {
        /* No NTP client in the image (specs/onvif.md): only Manual. */
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidArgVal", "Only Manual date and time is supported (no NTP client)");
        return;
    }
    soap_elem_t ut;
    int h, mi, s, y, mo, d;
    if (soap_find(c->op.inner, "UTCDateTime", &ut) != SOAP_OK || arg_int(ut.inner, "Hour", &h) ||
        arg_int(ut.inner, "Minute", &mi) || arg_int(ut.inner, "Second", &s) || arg_int(ut.inner, "Year", &y) ||
        arg_int(ut.inner, "Month", &mo) || arg_int(ut.inner, "Day", &d) || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidDateTime", "Invalid UTCDateTime");
        return;
    }
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > dm[mo - 1] + (mo == 2 && leap)) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidDateTime", "Invalid UTCDateTime");
        return;
    }
    /* The WS-Security Created window is checked against this clock: a clock set far back would reopen it. */
    if (y < 2024 || y > 2037) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidArgVal", "Year must be 2024 to 2037");
        return;
    }
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    if (onvif_settime_fn(t) != 0) {
        onvif_fault(c, 1, "ter:Action", NULL, "Cannot set the clock");
        return;
    }
    onvif_reply(c, ENV_OPEN "<tds:SetSystemDateAndTimeResponse/>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_hostname(onvif_call_t *c)
{
    char name[33];
    onvif_hostname_get(name);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>{{name}}</tt:Name>"
        "</tds:HostnameInformation></tds:GetHostnameResponse>" ENV_CLOSE);
    soap_tpl_set(t, "name", name);
    onvif_send(c);
}

static void op_set_hostname(onvif_call_t *c)
{
    char name[48];
    if (arg_text(c, "Name", name, sizeof(name)) != 0 || !onvif_hostname_valid(name)) {
        onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidHostname",
                    "Invalid hostname (RFC 1123 label, 1 to 32 of A-Z a-z 0-9 -)");
        return;
    }
    lower(name);
    char cur[64] = "";
    FILE *hf = fopen(onvif_hostname_path, "re");
    if (hf) {
        if (fgets(cur, sizeof(cur), hf)) cur[strcspn(cur, "\r\n")] = '\0';
        fclose(hf);
    }
    if (strcmp(cur, name) != 0 && hostname_store(name) != 0) {     /* unchanged: no flash write */
        onvif_fault(c, 1, "ter:Action", NULL, "Cannot store the hostname");
        return;
    }
    onvif_reply(c, ENV_OPEN "<tds:SetHostnameResponse/>" ENV_CLOSE);
    onvif_send(c);
}

static void op_network_interfaces(onvif_call_t *c)
{
    char mac[18], ip[16] = "", pfx[8], mtu[8];
    int p = 24, m = 1500;
    onvif_mac(mac);
    int have_ip = onvif_if_ipv4(ip, &p, &m) == 0;
    snprintf(pfx, sizeof(pfx), "%d", p);
    snprintf(mtu, sizeof(mtu), "%d", m);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"{{ifn}}\"><tt:Enabled>true</tt:Enabled>"
        "<tt:Info><tt:Name>{{ifn}}</tt:Name><tt:HwAddress>{{mac}}</tt:HwAddress><tt:MTU>{{mtu}}</tt:MTU></tt:Info>"
        "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>{{addr}}<tt:DHCP>true</tt:DHCP></tt:Config></tt:IPv4>"
        "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "ifn", onvif_if_name());
    soap_tpl_set(t, "mac", mac);
    soap_tpl_set(t, "mtu", mtu);
    if (have_ip) {
        snprintf(c->out->scratch, ONVIF_SCRATCH,
                 "<tt:FromDHCP><tt:Address>%s</tt:Address><tt:PrefixLength>%s</tt:PrefixLength></tt:FromDHCP>", ip, pfx);
        soap_tpl_set_raw(t, "addr", c->out->scratch);
    }
    onvif_send(c);
}

static void op_scopes(onvif_call_t *c)
{
    char name[33];
    onvif_hostname_get(name);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetScopesResponse>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/S</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/" ONVIF_HARDWARE_ID
        "</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/{{name}}</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/location/</tt:ScopeItem></tds:Scopes>"
        "</tds:GetScopesResponse>" ENV_CLOSE);
    soap_tpl_set(t, "name", name);
    onvif_send(c);
}

static void op_get_ntp(onvif_call_t *c)
{
    ntp_servers(c->out->scratch, ONVIF_SCRATCH);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP>{{ntp}}</tds:NTPInformation></tds:GetNTPResponse>"
        ENV_CLOSE);
    soap_tpl_set_raw(t, "ntp", c->out->scratch);
    onvif_send(c);
}

static void op_set_ntp(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgs", "ter:InvalidArgVal", "NTP servers are set with akam_ntp.sh over SSH");
}

static void op_get_dns(onvif_call_t *c)
{
    char *s = c->out->scratch;
    size_t used = 0;
    int n = 0;
    FILE *f = fopen(onvif_resolv_path, "re");
    if (f) {
        char line[160];
        while (n < 3 && fgets(line, sizeof(line), f)) {
            char addr[48];
            struct in_addr a;
            if (sscanf(line, " nameserver %47s", addr) != 1 || inet_pton(AF_INET, addr, &a) != 1) continue;
            char canon[16];
            inet_ntop(AF_INET, &a, canon, sizeof(canon));
            used += (size_t)snprintf(s + used, ONVIF_SCRATCH - used,
                "<tt:DNSFromDHCP><tt:Type>IPv4</tt:Type><tt:IPv4Address>%s</tt:IPv4Address></tt:DNSFromDHCP>", canon);
            n++;
        }
        fclose(f);
    }
    s[used] = '\0';
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetDNSResponse><tds:DNSInformation><tt:FromDHCP>true</tt:FromDHCP>{{dns}}</tds:DNSInformation></tds:GetDNSResponse>"
        ENV_CLOSE);
    soap_tpl_set_raw(t, "dns", s);
    onvif_send(c);
}

/* The default gateway: the first /proc/net/route line with destination 0.0.0.0 and the gateway flag. */
static int default_gateway(char *out, size_t cap)
{
    FILE *f = fopen(onvif_route_path, "re");
    if (!f) return -1;
    char line[160];
    int found = -1;
    while (found < 0 && fgets(line, sizeof(line), f)) {
        unsigned dest, gw, flags;
        char ifn[24];
        if (sscanf(line, "%23s %x %x %x", ifn, &dest, &gw, &flags) != 4 || dest != 0 || !(flags & 2) || gw == 0) continue;
        struct in_addr a;
        a.s_addr = gw;                  /* /proc prints the raw network-order word in host order */
        if (inet_ntop(AF_INET, &a, out, (socklen_t)cap)) found = 0;
    }
    fclose(f);
    return found;
}

static void op_get_gateway(onvif_call_t *c)
{
    char gw[16];
    if (default_gateway(gw, sizeof(gw)) != 0) {
        onvif_reply(c, ENV_OPEN "<tds:GetNetworkDefaultGatewayResponse><tds:NetworkGateway/></tds:GetNetworkDefaultGatewayResponse>" ENV_CLOSE);
    } else {
        soap_tpl_t *t = onvif_reply(c, ENV_OPEN
            "<tds:GetNetworkDefaultGatewayResponse><tds:NetworkGateway><tt:IPv4Address>{{gw}}</tt:IPv4Address></tds:NetworkGateway>"
            "</tds:GetNetworkDefaultGatewayResponse>" ENV_CLOSE);
        soap_tpl_set(t, "gw", gw);
    }
    onvif_send(c);
}

static void op_get_protocols(onvif_call_t *c)
{
    char hp[8], rp[8];
    snprintf(hp, sizeof(hp), "%d", http_srv_port() > 0 ? http_srv_port() : 80);
    snprintf(rp, sizeof(rp), "%d", ONVIF_RTSP_PORT);
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetNetworkProtocolsResponse>"
        "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{{hp}}</tt:Port></tds:NetworkProtocols>"
        "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>{{rp}}</tt:Port></tds:NetworkProtocols>"
        "</tds:GetNetworkProtocolsResponse>" ENV_CLOSE);
    soap_tpl_set(t, "hp", hp);
    soap_tpl_set(t, "rp", rp);
    onvif_send(c);
}

static void op_get_discovery_mode(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>" ENV_CLOSE);
    onvif_send(c);
}

static void op_get_zeroconf(onvif_call_t *c)
{
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN
        "<tds:GetZeroConfigurationResponse><tds:ZeroConfiguration><tt:InterfaceToken>{{ifn}}</tt:InterfaceToken>"
        "<tt:Enabled>false</tt:Enabled></tds:ZeroConfiguration></tds:GetZeroConfigurationResponse>" ENV_CLOSE);
    soap_tpl_set(t, "ifn", onvif_if_name());
    onvif_send(c);
}

static void op_get_users(onvif_call_t *c)
{
    char user[CRED_USER_MAX + 1];
    if (!cred_get_user(user, sizeof(user))) user[0] = '\0';
    soap_tpl_t *t = onvif_reply(c, ENV_OPEN "<tds:GetUsersResponse>{{user}}</tds:GetUsersResponse>" ENV_CLOSE);
    if (user[0]) {
        /* user names are A-Za-z0-9._@+- (cred.h): safe inside an element */
        snprintf(c->out->scratch, ONVIF_SCRATCH,
                 "<tds:User><tt:Username>%s</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User>", user);
        soap_tpl_set_raw(t, "user", c->out->scratch);
    }
    onvif_send(c);
}

/* ------------------------------------------------------------------------ reboot */

/* The camera has a hardware watchdog with nowayout, fed by /usr/bin/watchdog, which must never be stopped; this
 * does not touch it: the kernel restart path takes the machine down and the hardware timer is the safety net if
 * it hangs. The docs (akam_v1_flash_and_restore.md) report the software reboot as unreliable on this hardware, so
 * the hardware test H6 decides if SystemReboot is kept as is. */
static void *reboot_thread(void *arg)
{
    (void)arg;
    usleep(1000000);        /* the reply has been written long before */
    sync();
    reboot(RB_AUTOBOOT);
    return NULL;
}

static void default_reboot(void)
{
    static int once;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int already = once;
    once = 1;
    pthread_mutex_unlock(&mu);
    if (already) return;
    printf("[onvif] SystemReboot requested, rebooting in 1 s\n");
    fflush(stdout);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 32 * 1024);
    if (pthread_create(&th, &at, reboot_thread, NULL) != 0) {
        pthread_mutex_lock(&mu);
        once = 0;
        pthread_mutex_unlock(&mu);
    }
    pthread_attr_destroy(&at);
}
void (*onvif_reboot_fn)(void) = default_reboot;

static void op_reboot(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN
        "<tds:SystemRebootResponse><tds:Message>Rebooting in 1 second</tds:Message></tds:SystemRebootResponse>" ENV_CLOSE);
    onvif_send(c);
    onvif_imaging_flush_pending();   /* a debounced picture write must not be lost to the reboot */
    onvif_reboot_fn();
}

/* ------------------------------------------------------------------------ registration */

static const onvif_op_t k_ops[] = {
    { "GetSystemDateAndTime", ONVIF_PRE, op_get_time },
    { "GetCapabilities", ONVIF_PRE, op_capabilities },
    { "GetServices", ONVIF_PRE, op_services },
    { "GetServiceCapabilities", ONVIF_PRE, op_service_capabilities },
    { "GetScopes", ONVIF_PRE, op_scopes },
    { "GetHostname", ONVIF_PRE, op_get_hostname },
    { "GetDeviceInformation", ONVIF_READ, op_device_information },
    { "GetNetworkInterfaces", ONVIF_READ, op_network_interfaces },
    { "GetNTP", ONVIF_READ, op_get_ntp },
    { "GetDNS", ONVIF_READ, op_get_dns },
    { "GetUsers", ONVIF_READ, op_get_users },
    { "GetNetworkProtocols", ONVIF_READ, op_get_protocols },
    { "GetNetworkDefaultGateway", ONVIF_READ, op_get_gateway },
    { "GetDiscoveryMode", ONVIF_READ, op_get_discovery_mode },
    { "GetZeroConfiguration", ONVIF_READ, op_get_zeroconf },
    { "SetSystemDateAndTime", ONVIF_WRITE, op_set_time },
    { "SetHostname", ONVIF_WRITE, op_set_hostname },
    { "SetNTP", ONVIF_WRITE, op_set_ntp },
    { "SystemReboot", ONVIF_WRITE, op_reboot },
};

int onvif_device_register(void)
{
    return onvif_add_service("/onvif/device_service", k_ops, sizeof(k_ops) / sizeof(k_ops[0]));
}
