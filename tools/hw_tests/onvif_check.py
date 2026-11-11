#!/usr/bin/env python3
"""ONVIF check for an Akam camera: replays the calls ONVIF Device Manager makes on first contact.

Standard library only. Run it with `python3 -I tools/hw_tests/onvif_check.py <camera-ip[:port]> [user]`.
The password is asked without echo (or read from the environment variable named by --password-env) and is never
written anywhere. Options:
  --rtsp          also RTSP DESCRIBE (HTTP Digest) the URI GetStreamUri returned: expects 200 and an H264 m=video line
  --yes-imaging   run the brightness change/restore test without asking (not saved to flash)
  --no-imaging    skip that test without asking
  --no-discovery  skip the WS-Discovery probe (UDP 3702)
Without --yes-imaging/--no-imaging the question is asked on a terminal and skipped when there is none.
Exit status: 0 all checks passed, 1 at least one failed.
"""
import argparse, base64, calendar, getpass, hashlib, os, re, socket, sys, time, urllib.request, urllib.error, uuid
from xml.sax.saxutils import escape as xesc
import xml.etree.ElementTree as ET

NS = {"tds": "http://www.onvif.org/ver10/device/wsdl", "trt": "http://www.onvif.org/ver10/media/wsdl",
      "timg": "http://www.onvif.org/ver20/imaging/wsdl", "tt": "http://www.onvif.org/ver10/schema",
      "env": "http://www.w3.org/2003/05/soap-envelope",
      "d": "http://schemas.xmlsoap.org/ws/2005/04/discovery"}
CLOCK_NOTE_S = 300  # beyond this many seconds of clock offset the camera clock is considered not set


class Check:
    def __init__(self, host, user, pw):
        self.host, self.user, self.pw = host, user, pw
        self.ip = host.rsplit(":", 1)[0] if re.search(r":\d+$", host) else host
        self.dev = f"http://{host}/onvif/device_service"
        self.ok = self.bad = 0
        self.offset = 0.0  # camera clock minus PC clock, like ODM does

    def good(self, msg):
        print(f"  ok   {msg}"); self.ok += 1

    def fail(self, msg):
        print(f"  FAIL {msg}"); self.bad += 1

    def wsse(self):
        nonce = os.urandom(16)
        created = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + self.offset))
        digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + self.pw.encode()).digest()).decode()
        return ('<s:Header><Security s:mustUnderstand="1" xmlns="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken><Username>%s</Username>'
                '<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0'
                '#PasswordDigest">%s</Password><Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/'
                'oasis-200401-wss-soap-message-security-1.0#Base64Binary">%s</Nonce><Created xmlns="http://docs.'
                'oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created>'
                '</UsernameToken></Security></s:Header>' % (xesc(self.user), digest, base64.b64encode(nonce).decode(), created))

    def call(self, url, body, auth=True, expect=None, label=None):
        env = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
               'xmlns:tds="%s" xmlns:trt="%s" xmlns:timg="%s" xmlns:tt="%s">%s<s:Body>%s</s:Body></s:Envelope>'
               % (NS["tds"], NS["trt"], NS["timg"], NS["tt"], self.wsse() if auth else "", body))
        req = urllib.request.Request(url, env.encode(), {"Content-Type": "application/soap+xml; charset=utf-8"})
        label = label or re.match(r"<(\w+:)?(\w+)", body).group(2)
        t0 = time.time()
        www = None
        try:
            r = urllib.request.urlopen(req, timeout=8); code, data = r.status, r.read()
        except urllib.error.HTTPError as e:
            code, data, www = e.code, e.read(), e.headers.get("WWW-Authenticate")
        except Exception as e:
            self.fail(f"{label}: {e}"); return None
        ms = int((time.time() - t0) * 1000)
        if code == 401:
            # HTTP Digest challenge, usually with an empty body: not SOAP, nothing to parse.
            if expect == "fault" and www:
                self.good(f"{label}: refused as expected (HTTP 401 with a WWW-Authenticate challenge) {ms} ms")
            elif expect == "fault":
                self.fail(f"{label}: HTTP 401 without a WWW-Authenticate header")
            else:
                self.fail(f"{label}: HTTP 401 (wrong password, or the camera clock is off) {ms} ms")
            return None
        try:
            root = ET.fromstring(data)
        except ET.ParseError as e:
            self.fail(f"{label}: HTTP {code}, invalid XML ({e})"); return None
        fault = root.find(".//env:Fault", NS)
        if fault is not None or code != 200:
            sub = " / ".join(x.text or "" for x in root.iterfind(".//env:Value", NS))
            if expect == "fault":
                self.good(f"{label}: refused as expected (HTTP {code} {sub}) {ms} ms")
            else:
                self.fail(f"{label}: HTTP {code} {sub} {ms} ms")
            return None
        if expect == "fault":
            self.fail(f"{label}: accepted, a refusal was expected"); return root
        self.good(f"{label} ({ms} ms)")
        return root

    @staticmethod
    def txt(root, path):
        e = root.find(path, NS) if root is not None else None
        return e.text if e is not None else None

    def discovery(self):
        print(f"== WS-Discovery probe to {self.ip}:3702")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3); s.bind(("", 0))
        s.sendto(('<?xml version="1.0" encoding="UTF-8"?><e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" '
                  'xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/'
                  'discovery" xmlns:dn="http://www.onvif.org/ver10/network/wsdl"><e:Header><w:MessageID>uuid:%s</w:MessageID>'
                  '<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To><w:Action>http://schemas.xmlsoap.org/ws/2005/04/'
                  'discovery/Probe</w:Action></e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types>'
                  '</d:Probe></e:Body></e:Envelope>' % uuid.uuid4()).encode(), (self.ip, 3702))
        try:
            d, _ = s.recvfrom(8192)
            root = ET.fromstring(d)
            xa = [a for x in root.iterfind(".//d:XAddrs", NS) for a in (x.text or "").split()]
            if xa: self.good(f"ProbeMatches, XAddrs={xa}")
            else: self.fail("ProbeMatches without XAddrs")
        except Exception as e:
            self.fail(f"no valid ProbeMatches ({e})")
        finally:
            s.close()

    def rtsp_describe(self, uri):
        """RTSP DESCRIBE with Digest auth: 200 and an H264 m=video line."""
        print("== RTSP")
        m = re.match(r"rtsp://(?:[^@/]*@)?([^/:]+)(?::(\d+))?(/.*)?$", uri or "")
        if not m:
            self.fail(f"RTSP: no usable stream URI ({uri!r})"); return
        host, port, path = m.group(1), int(m.group(2) or 554), m.group(3) or "/"
        url = f"rtsp://{host}:{port}{path}" if m.group(2) else f"rtsp://{host}{path}"
        cseq = [1]

        def talk(extra):
            req = f"DESCRIBE {url} RTSP/1.0\r\nCSeq: {cseq[0]}\r\nAccept: application/sdp\r\n{extra}\r\n"
            cseq[0] += 1
            with socket.create_connection((host, port), timeout=8) as s:
                s.settimeout(8)
                s.sendall(req.encode())
                buf = b""
                while b"\r\n\r\n" not in buf:
                    c = s.recv(4096)
                    if not c: break
                    buf += c
                head, _, body = buf.partition(b"\r\n\r\n")
                hl = head.decode(errors="replace")
                n = re.search(r"(?im)^content-length:\s*(\d+)", hl)
                want = int(n.group(1)) if n else 0
                while len(body) < want:
                    c = s.recv(4096)
                    if not c: break
                    body += c
            return hl, body.decode(errors="replace")
        try:
            head, _ = talk("")
            code = int(head.split(None, 2)[1])
            if code == 200:
                self.fail("RTSP DESCRIBE accepted without a password"); return
            ch = re.search(r"(?im)^www-authenticate:\s*Digest\s+(.*)$", head)
            if code != 401 or not ch:
                self.fail(f"RTSP DESCRIBE without a password: {head.splitlines()[0]} (401 + Digest expected)"); return
            self.good("RTSP DESCRIBE without a password: 401 with a Digest challenge")
            p = {k.lower(): (q if q or not v else v)
                 for k, q, v in re.findall(r'(\w+)\s*=\s*(?:"([^"]*)"|([^,\s]*))', ch.group(1))}
            realm, nonce = p.get("realm", ""), p.get("nonce", "")
            qop = [x.strip() for x in p.get("qop", "").split(",")]
            if p.get("algorithm", "MD5").upper() != "MD5" or not nonce:
                self.fail(f"RTSP: unsupported Digest challenge ({ch.group(1)[:80]})"); return
            md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
            ha1, ha2 = md5(f"{self.user}:{realm}:{self.pw}"), md5(f"DESCRIBE:{url}")
            if "auth" in qop:
                cn, nc = os.urandom(8).hex(), "00000001"
                resp = md5(f"{ha1}:{nonce}:{nc}:{cn}:auth:{ha2}")
                tail = f', qop=auth, nc={nc}, cnonce="{cn}"'
            else:
                resp, tail = md5(f"{ha1}:{nonce}:{ha2}"), ""
            auth = (f'Authorization: Digest username="{self.user}", realm="{realm}", nonce="{nonce}", uri="{url}", '
                    f'response="{resp}"{tail}\r\n')
            head, body = talk(auth)
            code = int(head.split(None, 2)[1])
            if code != 200:
                self.fail(f"RTSP DESCRIBE with the password: {head.splitlines()[0]}"); return
            if re.search(r"(?im)^m=video\b", body) and re.search(r"(?i)a=rtpmap:\d+\s+H264/", body):
                self.good("RTSP DESCRIBE with the password: 200, H264 video in the SDP")
            else:
                self.fail("RTSP DESCRIBE: 200 but no H264 m=video line in the SDP")
        except Exception as e:
            self.fail(f"RTSP DESCRIBE: {e}")

    @staticmethod
    def want_imaging(a):
        if a.no_imaging: return False
        if a.yes_imaging: return True
        if not sys.stdin.isatty(): return False
        return input("\nChange brightness for 5 s then restore it (not saved to flash)? [y/N] ").strip().lower() == "y"

    def run(self, a):
        T, call = self.txt, self.call
        if not a.no_discovery:
            self.discovery()
        print("== Device service")
        r = call(self.dev, "<tds:GetSystemDateAndTime/>", auth=False)
        if r is not None:
            g = lambda k: int(T(r, ".//tt:UTCDateTime//tt:" + k) or 0)
            cam = calendar.timegm((g("Year"), g("Month"), g("Day"), g("Hour"), g("Minute"), g("Second")))
            self.offset = cam - time.time()
            print(f"       camera clock offset vs PC: {self.offset:+.0f} s")
            if abs(self.offset) > CLOCK_NOTE_S:
                print("       NOTE: the camera clock is not set (it has no battery and boots in the past). Clients that\n"
                      "             check WS-Security timestamps may refuse the login and RTCP dates are wrong.\n"
                      "             Fix: ./akam ntp set   (then ./akam ntp status)")
        call(self.dev, "<tds:GetDeviceInformation/>", auth=False, expect="fault",
             label="GetDeviceInformation without password")
        r = call(self.dev, "<tds:GetDeviceInformation/>")
        if r is not None:
            print("       ", {k: T(r, ".//tds:" + k) for k in ("Manufacturer", "Model", "FirmwareVersion", "SerialNumber")})
        call(self.dev, '<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>')
        r = call(self.dev, "<tds:GetServices><tds:IncludeCapability>true</tds:IncludeCapability></tds:GetServices>")
        media = imaging = None
        if r is not None:
            for svc in r.iterfind(".//tds:Service", NS):
                nsv, xa = T(svc, "tds:Namespace"), T(svc, "tds:XAddr")
                if nsv == NS["trt"]: media = xa
                if nsv == NS["timg"]: imaging = xa
            print(f"        media={media} imaging={imaging}")
        for op in ("GetScopes", "GetHostname", "GetDNS", "GetNTP", "GetNetworkInterfaces", "GetNetworkProtocols",
                   "GetNetworkDefaultGateway", "GetDiscoveryMode", "GetZeroConfiguration", "GetUsers"):
            call(self.dev, f"<tds:{op}/>")
        call(self.dev, "<tds:GetRelayOutputs/>", expect="fault", label="unknown action (GetRelayOutputs)")

        print("== Media service")
        media = media or f"http://{self.host}/onvif/media_service"
        r = call(media, "<trt:GetProfiles/>")
        tok = vsrc = uri = None
        if r is not None:
            p = r.find(".//trt:Profiles", NS)
            tok = p.get("token") if p is not None else None
            vs = r.find(".//tt:VideoSourceConfiguration/tt:SourceToken", NS)
            vsrc = vs.text if vs is not None else None
            enc = r.find(".//tt:VideoEncoderConfiguration", NS)
            if enc is not None:
                print("        profile", tok, "encoding", T(enc, "tt:Encoding"), T(enc, "tt:Resolution/tt:Width"), "x",
                      T(enc, "tt:Resolution/tt:Height"), "fps", T(enc, ".//tt:FrameRateLimit"), "kbps", T(enc, ".//tt:BitrateLimit"))
        for op in ("GetVideoSources", "GetVideoSourceConfigurations", "GetVideoEncoderConfigurations", "GetAudioSources",
                   "GetAudioEncoderConfigurations", "GetAudioOutputs"):
            call(media, f"<trt:{op}/>")
        if tok:
            call(media, f"<trt:GetProfile><trt:ProfileToken>{tok}</trt:ProfileToken></trt:GetProfile>")
            r = call(media, '<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>'
                            f'RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>{tok}</trt:ProfileToken></trt:GetStreamUri>')
            uri = T(r, ".//tt:Uri")
            print("        stream uri:", uri)
            call(media, f"<trt:GetSnapshotUri><trt:ProfileToken>{tok}</trt:ProfileToken></trt:GetSnapshotUri>",
                 expect="fault", label="GetSnapshotUri (no snapshot on this camera)")
            call(media, f"<trt:GetVideoEncoderConfigurationOptions><trt:ProfileToken>{tok}</trt:ProfileToken>"
                        "</trt:GetVideoEncoderConfigurationOptions>")

        print("== Imaging service")
        imaging = imaging or f"http://{self.host}/onvif/imaging_service"
        if vsrc:
            gs = f"<timg:GetImagingSettings><timg:VideoSourceToken>{vsrc}</timg:VideoSourceToken></timg:GetImagingSettings>"
            r = call(imaging, gs)
            bright = T(r, ".//tt:Brightness")
            print("        brightness", bright, "contrast", T(r, ".//tt:Contrast"), "saturation", T(r, ".//tt:ColorSaturation"),
                  "sharpness", T(r, ".//tt:Sharpness"), "ircut", T(r, ".//tt:IrCutFilter"))
            for op in ("GetOptions", "GetMoveOptions", "GetStatus"):
                call(imaging, f"<timg:{op}><timg:VideoSourceToken>{vsrc}</timg:VideoSourceToken></timg:{op}>")
            if bright is not None and self.want_imaging(a):
                b = float(bright); nb = b + 10 if b < 60 else b - 10
                st = lambda v: (f"<timg:SetImagingSettings><timg:VideoSourceToken>{vsrc}</timg:VideoSourceToken><timg:ImagingSettings>"
                                f"<tt:Brightness>{v}</tt:Brightness></timg:ImagingSettings><timg:ForcePersistence>false"
                                "</timg:ForcePersistence></timg:SetImagingSettings>")
                try:
                    call(imaging, st(nb), label=f"SetImagingSettings brightness {b} -> {nb}")
                    r = call(imaging, gs, label="GetImagingSettings after set")
                    print("        brightness now", T(r, ".//tt:Brightness"), "(look at the live image)")
                    time.sleep(a.imaging_hold)
                finally:   # also on Ctrl-C or an error: never leave the changed value behind
                    call(imaging, st(b), label=f"SetImagingSettings brightness restored to {b}")
        if a.rtsp:
            if uri: self.rtsp_describe(uri)
            else: self.fail("RTSP: GetStreamUri gave no URI to test")
        print(f"\nRESULT: {self.ok} ok, {self.bad} failed")
        return 1 if self.bad else 0


def main():
    ap = argparse.ArgumentParser(description="ONVIF check for an Akam camera (replays what ONVIF Device Manager does).")
    ap.add_argument("host", help="camera IP address, optionally :port of the ONVIF HTTP server")
    ap.add_argument("user", nargs="?", default="viewer")
    ap.add_argument("--rtsp", action="store_true", help="RTSP DESCRIBE (Digest) on the GetStreamUri URI")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--yes-imaging", action="store_true", help="change brightness and restore it, without asking")
    g.add_argument("--no-imaging", action="store_true", help="skip the brightness test, without asking")
    ap.add_argument("--no-discovery", action="store_true", help="skip the WS-Discovery probe")
    ap.add_argument("--password-env", metavar="VAR", help="read the password from this environment variable")
    ap.add_argument("--imaging-hold", type=float, default=5.0, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.password_env and a.password_env not in os.environ:
        ap.error(f"environment variable {a.password_env} is not set")
    pw = os.environ[a.password_env] if a.password_env else getpass.getpass(f"RTSP/ONVIF password for {a.user}@{a.host}: ")
    return Check(a.host, a.user, pw).run(a)


if __name__ == "__main__":
    sys.exit(main())
