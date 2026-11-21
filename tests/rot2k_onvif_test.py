#!/usr/bin/env python3
"""ONVIF conformance smoke of the Rotatable 2K stack on loopback (python standard library only).

Starts the host harness tests/rot2k_onvif_server.c (the REAL http_srv / soap / onvif / wsd / cred / rtsp_auth code and the real 2K device, media and PTZ services, over a FAKE
motor) and drives it like an ONVIF client would: HTTP Digest with the stream credentials, one request per operation compared with a golden XML file
(tests/fixtures/rot2k_onvif/*.xml, canonical form, volatile fields normalised), the ContinuousMove -> motor mapping table, the lease (Timeout) clamp, Stop, GotoHomePosition,
GetPresets / GotoPreset, every refusal state, WS-Security refusal, the pre-authentication list, WS-Discovery Probe on loopback. Nothing leaves the host.

Run: python3 -I tests/rot2k_onvif_test.py SERVER_BINARY [--update]     (--update rewrites the golden files; review the diff)
"""

import base64
import hashlib
import http.client
import os
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
GOLD = os.path.join(HERE, "fixtures", "rot2k_onvif")
USER, PASSWORD, REALM = "admin", "s3cret-pw", "akam"

fails = 0
UPDATE = "--update" in sys.argv


def check(cond, msg):
    global fails
    if cond:
        print("  ok   - " + msg)
    else:
        print("  FAIL - " + msg)
        fails += 1


NS = (
    'xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
    'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" '
    'xmlns:tt="http://www.onvif.org/ver10/schema"'
)


def envelope(body, header=""):
    return '<?xml version="1.0" encoding="utf-8"?><s:Envelope %s><s:Header>%s</s:Header><s:Body>%s</s:Body></s:Envelope>' % (NS, header, body)


class Client:
    """HTTP Digest (RFC 2617, MD5, qop=auth) the way ONVIF clients do it, over one keep-alive connection."""

    def __init__(self, port, user=USER, password=PASSWORD):
        self.port, self.user, self.password = port, user, password
        self.conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        self.chal = None
        self.nc = 0

    def _auth(self, uri, method="POST"):
        c = self.chal
        self.nc += 1
        nc = "%08x" % self.nc
        cnonce = os.urandom(8).hex()
        ha1 = hashlib.md5(("%s:%s:%s" % (self.user, c["realm"], self.password)).encode()).hexdigest()
        ha2 = hashlib.md5(("%s:%s" % (method, uri)).encode()).hexdigest()
        resp = hashlib.md5(("%s:%s:%s:%s:auth:%s" % (ha1, c["nonce"], nc, cnonce, ha2)).encode()).hexdigest()
        return ('Digest username="%s", realm="%s", nonce="%s", uri="%s", qop=auth, nc=%s, cnonce="%s", response="%s", algorithm=MD5'
                % (self.user, c["realm"], c["nonce"], uri, nc, cnonce, resp))

    @staticmethod
    def parse_challenge(h):
        return dict(re.findall(r'(\w+)="?([^",]*)"?', h))

    def post(self, path, body, auth=True, headers=None):
        hdr = {"Content-Type": "application/soap+xml; charset=utf-8"}
        hdr.update(headers or {})
        for attempt in range(3):
            if auth and self.chal:
                hdr["Authorization"] = self._auth(path)
            self.conn.request("POST", path, body.encode(), hdr)
            r = self.conn.getresponse()
            data = r.read().decode()
            if r.status == 401 and auth and r.getheader("WWW-Authenticate", "").startswith("Digest"):
                if attempt == 0 or "stale=true" in r.getheader("WWW-Authenticate").lower():
                    self.chal = self.parse_challenge(r.getheader("WWW-Authenticate"))
                    self.nc = 0
                    hdr.pop("Authorization", None)
                    continue
            return r.status, data, r
        return r.status, data, r


def canon(xml):
    return ET.canonicalize(xml, strip_text=True)


def normalise(name, xml, port):
    xml = xml.replace("127.0.0.1:%d" % port, "HOST:PORT")
    xml = re.sub(r"(rtsp://)127\.0\.0\.1", r"\1HOST", xml)
    host = re.sub(r"[^A-Za-z0-9-]", "-", socket.gethostname())
    xml = xml.replace("/name/" + host, "/name/HOSTNAME").replace("<tt:Name>%s</tt:Name>" % host, "<tt:Name>HOSTNAME</tt:Name>")
    xml = re.sub(r"(<tds:FirmwareVersion>)[^<]*", r"\1FW", xml)
    xml = re.sub(r"(<tt:Name>HOSTNAME</tt:Name>)", r"\1", xml)
    if "GetNetworkProtocols" in name:
        xml = re.sub(r"(<tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>)\d+", r"\1PORT", xml)
    return xml


def golden(name, xml, port):
    """The response must be well formed XML and equal the golden file (canonical form)."""
    try:
        got = canon(normalise(name, xml, port))
    except ET.ParseError as e:
        check(False, "%s: well-formed XML (%s)" % (name, e))
        return
    path = os.path.join(GOLD, name + ".xml")
    if UPDATE:
        os.makedirs(GOLD, exist_ok=True)
        with open(path, "w") as f:
            f.write(got + "\n")
    try:
        want = open(path).read().rstrip("\n")
    except OSError:
        check(False, "%s: golden file missing (run with --update)" % name)
        return
    check(got == want, "%s: response equals the golden XML" % name)
    if got != want:
        print("    got : " + got[:600])
        print("    want: " + want[:600])


class Server:
    def __init__(self, binary):
        self.dir = tempfile.mkdtemp(prefix="a2konvif")
        self.state_file = os.path.join(self.dir, "state")
        self.auth_file = os.path.join(self.dir, "rtsp.conf")
        self.set_state("ok")
        ha1 = hashlib.md5(("%s:%s:%s" % (USER, REALM, PASSWORD)).encode()).hexdigest()
        with open(self.auth_file, "w") as f:
            f.write("%s:%s:%s\n" % (USER, REALM, ha1))
        os.chmod(self.auth_file, 0o600)
        self.p = subprocess.Popen([binary, self.auth_file, self.state_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        ready = self.line(10)
        m = re.match(r"READY http=(\d+) wsd=(\d+) state=(\S+)", ready or "")
        if not m:
            raise SystemExit("server did not start: %r" % ready)
        self.port, self.wsd, self.state = int(m.group(1)), int(m.group(2)), m.group(3)

    def _pump(self):
        for ln in self.p.stdout:  # the cores print their own log lines ("[http] ..."): only the harness lines matter
            if re.match(r"(READY|STATE|VEL|STOP|HOME|GOTO)\b", ln):
                self.lines.put(ln.rstrip("\n"))

    def line(self, timeout=2.0):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.lines.get_nowait())
            except queue.Empty:
                return out

    def set_state(self, s):
        with open(self.state_file, "w") as f:
            f.write(s + "\n")

    def onvif_state(self):
        self.p.stdin.write("state\n")
        self.p.stdin.flush()
        for _ in range(20):
            ln = self.line(2)
            if ln and ln.startswith("STATE "):
                return ln[6:]
        return None

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def fault_of(xml):
    """(code, subcode1, subcode2) of a SOAP 1.2 fault."""
    ns = {"s": "http://www.w3.org/2003/05/soap-envelope"}
    root = ET.fromstring(xml)
    f = root.find(".//s:Fault", ns)
    if f is None:
        return None
    code = f.find("s:Code/s:Value", ns)
    sub = f.find("s:Code/s:Subcode/s:Value", ns)
    sub2 = f.find("s:Code/s:Subcode/s:Subcode/s:Value", ns)
    t = lambda e: e.text if e is not None else None
    return (t(code), t(sub), t(sub2))


def move(velx, vely, timeout=None, profile="stream0", extra="", pt_attr=""):
    to = "<tptz:Timeout>%s</tptz:Timeout>" % timeout if timeout else ""
    return envelope(
        '<tptz:ContinuousMove><tptz:ProfileToken>%s</tptz:ProfileToken><tptz:Velocity>'
        '<tt:PanTilt x="%s" y="%s"%s/>%s</tptz:Velocity>%s</tptz:ContinuousMove>' % (profile, velx, vely, pt_attr, extra, to)
    )


def stop(profile="stream0", pantilt=None):
    pt = "<tptz:PanTilt>%s</tptz:PanTilt>" % pantilt if pantilt else ""
    return envelope("<tptz:Stop><tptz:ProfileToken>%s</tptz:ProfileToken>%s</tptz:Stop>" % (profile, pt))


def main():
    binary = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    srv = Server(binary)
    try:
        run(srv)
    finally:
        srv.close()
    print("rot2k onvif test: %d failed" % fails)
    return 1 if fails else 0


def run(srv):
    port = srv.port
    DEV, MED, PTZ = "/onvif/device_service", "/onvif/media_service", "/onvif/ptz_service"
    c = Client(port)
    print("== service state and discovery")
    check(srv.state == "on:%d" % port and srv.onvif_state() == "on:%d" % port, "state is on:<port> (%s)" % srv.state)

    print("== authentication: pre-authentication list, Digest with the stream credentials, refusals")
    st, body, _ = c.post(DEV, envelope("<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>"), auth=False)
    check(st == 200 and "/onvif/ptz_service" in body and "/onvif/media_service" in body, "GetCapabilities needs no login and lists device, media and PTZ")
    for dev_pre in ("GetSystemDateAndTime", "GetServices", "GetServiceCapabilities", "GetScopes", "GetHostname"):
        st, _, _ = c.post(DEV, envelope("<tds:%s/>" % dev_pre), auth=False)
        check(st == 200, "%s is open (pre-authentication)" % dev_pre)
    st, _, _ = c.post(PTZ, envelope("<tptz:GetServiceCapabilities/>"), auth=False)
    check(st == 200, "PTZ GetServiceCapabilities is open")
    st, _, r = c.post(PTZ, envelope("<tptz:GetNodes/>"), auth=False)
    ch = r.getheader("WWW-Authenticate", "")
    check(st == 401 and ch.startswith("Digest") and 'realm="akam"' in ch and "qop" in ch and "nonce=" in ch, "GetNodes without credentials: 401 with a Digest challenge (realm akam, qop auth)")
    st, _, _ = c.post(PTZ, move("1", "0"), auth=False)
    check(st == 401, "ContinuousMove without credentials: 401")
    good = Client(port)
    st, body, _ = good.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 200 and "PTZNode" in body, "the stream credentials log in (HTTP Digest)")
    bad = Client(port, password="wrong")
    st, _, _ = bad.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 401, "a wrong password is refused (401)")
    st, _, _ = good.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 200, "... and a success clears the failure count (every client here shares 127.0.0.1)")
    bad2 = Client(port, user="mallory")
    st, _, _ = bad2.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 401, "an unknown user is refused (401)")
    st, _, _ = good.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 200, "... and the right login still works")
    for _ in range(3):
        st, _, _ = good.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 200, "repeated requests with an increasing nc keep working")
    old = good.chal
    replay = Client(port)
    replay.chal = dict(old)
    replay.nc = 0  # the same nonce and nc 1 again: a replay of an authenticated request
    st, _, r = replay.post(PTZ, envelope("<tptz:GetNodes/>"), auth=False, headers={"Authorization": replay._auth(PTZ)})
    check(st == 401, "a replayed nonce count is refused with a fresh challenge")
    wsse = (
        '<wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">'
        '<wsse:UsernameToken><wsse:Username>%s</wsse:Username><wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">AAAA</wsse:Password>'
        "<wsse:Nonce>AAAAAAAAAAAAAAAAAAAAAA==</wsse:Nonce><wsu:Created xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">2026-01-01T00:00:00Z</wsu:Created></wsse:UsernameToken></wsse:Security>"
        % USER
    )
    st, body, _ = Client(port).post(PTZ, envelope('<tptz:Stop><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:Stop>', header=wsse), auth=False)
    check(st in (400, 401) and (st == 401 or (fault_of(body) or ("",))[1] == "ter:NotAuthorized"), "WS-Security PasswordDigest is refused (the 2K stores no clear-text password): login with HTTP Digest")
    st, _, _ = good.post(PTZ, envelope("<tptz:GetNodes/>"))
    check(st == 200, "... and the right login still works after it")
    srv.drain()
    c = good

    print("== golden XML of every operation")
    ops = [
        ("dev_GetCapabilities", DEV, "<tds:GetCapabilities/>"),
        ("dev_GetServices", DEV, "<tds:GetServices><tds:IncludeCapability>true</tds:IncludeCapability></tds:GetServices>"),
        ("dev_GetServicesNoCaps", DEV, "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>"),
        ("dev_GetServiceCapabilities", DEV, "<tds:GetServiceCapabilities/>"),
        ("dev_GetSystemDateAndTime", DEV, "<tds:GetSystemDateAndTime/>"),
        ("dev_GetScopes", DEV, "<tds:GetScopes/>"),
        ("dev_GetHostname", DEV, "<tds:GetHostname/>"),
        ("dev_GetDeviceInformation", DEV, "<tds:GetDeviceInformation/>"),
        ("dev_GetDiscoveryMode", DEV, "<tds:GetDiscoveryMode/>"),
        ("dev_GetNetworkProtocols", DEV, "<tds:GetNetworkProtocols/>"),
        ("media_GetServiceCapabilities", MED, "<trt:GetServiceCapabilities/>"),
        ("media_GetProfiles", MED, "<trt:GetProfiles/>"),
        ("media_GetProfile_stream0", MED, "<trt:GetProfile><trt:ProfileToken>stream0</trt:ProfileToken></trt:GetProfile>"),
        ("media_GetProfile_stream1", MED, "<trt:GetProfile><trt:ProfileToken>stream1</trt:ProfileToken></trt:GetProfile>"),
        ("media_GetVideoSources", MED, "<trt:GetVideoSources/>"),
        ("media_GetVideoSourceConfigurations", MED, "<trt:GetVideoSourceConfigurations/>"),
        ("media_GetVideoSourceConfiguration", MED, "<trt:GetVideoSourceConfiguration><trt:ConfigurationToken>vsrccfg</trt:ConfigurationToken></trt:GetVideoSourceConfiguration>"),
        ("media_GetVideoSourceConfigurationOptions", MED, "<trt:GetVideoSourceConfigurationOptions/>"),
        ("media_GetVideoEncoderConfigurations", MED, "<trt:GetVideoEncoderConfigurations/>"),
        ("media_GetVideoEncoderConfiguration_1", MED, "<trt:GetVideoEncoderConfiguration><trt:ConfigurationToken>venccfg1</trt:ConfigurationToken></trt:GetVideoEncoderConfiguration>"),
        ("media_GetVideoEncoderConfigurationOptions_0", MED, "<trt:GetVideoEncoderConfigurationOptions><trt:ConfigurationToken>venccfg0</trt:ConfigurationToken></trt:GetVideoEncoderConfigurationOptions>"),
        ("media_GetVideoEncoderConfigurationOptions_1", MED, "<trt:GetVideoEncoderConfigurationOptions><trt:ConfigurationToken>venccfg1</trt:ConfigurationToken></trt:GetVideoEncoderConfigurationOptions>"),
        ("media_GetStreamUri_stream0", MED, "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>stream0</trt:ProfileToken></trt:GetStreamUri>"),
        ("media_GetStreamUri_stream1", MED, "<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>stream1</trt:ProfileToken></trt:GetStreamUri>"),
        ("media_GetAudioSources", MED, "<trt:GetAudioSources/>"),
        ("media_GetSnapshotUri_fault", MED, "<trt:GetSnapshotUri><trt:ProfileToken>stream0</trt:ProfileToken></trt:GetSnapshotUri>"),
        ("media_CreateProfile_fault", MED, "<trt:CreateProfile><trt:Name>x</trt:Name></trt:CreateProfile>"),
        ("ptz_GetServiceCapabilities", PTZ, "<tptz:GetServiceCapabilities/>"),
        ("ptz_GetNodes", PTZ, "<tptz:GetNodes/>"),
        ("ptz_GetNode", PTZ, "<tptz:GetNode><tptz:NodeToken>ptznode</tptz:NodeToken></tptz:GetNode>"),
        ("ptz_GetConfigurations", PTZ, "<tptz:GetConfigurations/>"),
        ("ptz_GetConfiguration", PTZ, "<tptz:GetConfiguration><tptz:PTZConfigurationToken>ptzcfg</tptz:PTZConfigurationToken></tptz:GetConfiguration>"),
        ("ptz_GetConfigurationOptions", PTZ, "<tptz:GetConfigurationOptions><tptz:ConfigurationToken>ptzcfg</tptz:ConfigurationToken></tptz:GetConfigurationOptions>"),
        ("ptz_GetStatus_idle", PTZ, "<tptz:GetStatus><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetStatus>"),
        ("ptz_GetPresets", PTZ, "<tptz:GetPresets><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetPresets>"),
        ("ptz_ContinuousMove", PTZ, None),
        ("ptz_Stop", PTZ, None),
        ("ptz_GotoHomePosition", PTZ, "<tptz:GotoHomePosition><tptz:ProfileToken>stream1</tptz:ProfileToken></tptz:GotoHomePosition>"),
        ("ptz_GotoPreset", PTZ, "<tptz:GotoPreset><tptz:ProfileToken>stream0</tptz:ProfileToken><tptz:PresetToken>window</tptz:PresetToken></tptz:GotoPreset>"),
        ("ptz_AbsoluteMove_unsupported", PTZ, "<tptz:AbsoluteMove><tptz:ProfileToken>stream0</tptz:ProfileToken><tptz:Position><tt:PanTilt x=\"0\" y=\"0\"/></tptz:Position></tptz:AbsoluteMove>"),
    ]
    for name, path, body in ops:
        if name == "ptz_ContinuousMove":
            st, xml, _ = c.post(path, move("0.5", "-0.25", "PT1S"))
        elif name == "ptz_Stop":
            st, xml, _ = c.post(path, stop())
        else:
            st, xml, _ = c.post(path, envelope(body))
        want_fault = name.endswith("fault") or name.endswith("unsupported")
        check((st >= 400) == want_fault, "%s: HTTP %d" % (name, st))
        golden(name, xml, port)
    srv.drain()

    print("== PTZ: ContinuousMove -> motor command mapping")
    table = [  # (x, y, timeout, expected motor line)
        ("1", "0", None, "VEL 1 0 2 0 450"),
        ("-1", "0", None, "VEL -1 0 2 0 450"),
        ("0", "1", None, "VEL 0 1 0 2 450"),
        ("0", "-1", None, "VEL 0 -1 0 2 450"),
        ("0.5", "0", None, "VEL 1 0 4 0 450"),
        ("0.1", "0", None, "VEL 1 0 14 0 450"),
        ("0.06", "0", None, "VEL 1 0 18 0 450"),
        ("-0.25", "0", None, "VEL -1 0 7 0 450"),
        ("1", "1", None, "VEL 1 1 3 3 450"),
        ("-1", "1", None, "VEL -1 1 3 3 450"),
        ("1", "-1", None, "VEL 1 -1 3 3 450"),
        ("-1", "-1", None, "VEL -1 -1 3 3 450"),
        ("0.5", "0.5", None, "VEL 1 1 4 4 450"),
        ("1", "0.2", None, "VEL 1 1 3 8 450"),
        ("0.7", "0.02", None, "VEL 1 0 3 0 450"),
        ("1", "0", "PT1S", "VEL 1 0 2 0 1000"),
        ("1", "0", "PT0.5S", "VEL 1 0 2 0 500"),
        ("1", "0", "PT2S", "VEL 1 0 2 0 2000"),
        ("1", "0", "PT5S", "VEL 1 0 2 0 2000"),
        ("1", "0", "PT1M", "VEL 1 0 2 0 2000"),
        ("1", "0", "PT0.05S", "VEL 1 0 2 0 150"),
        ("1", "0", "PT0S", "VEL 1 0 2 0 150"),
        ("0.04", "-0.03", None, "STOP"),
        ("0", "0", None, "STOP"),
    ]
    for x, y, to, want in table:
        srv.drain()
        st, xml, _ = c.post(PTZ, move(x, y, to))
        got = srv.line(2)
        check(st == 200 and got == want, "ContinuousMove x=%s y=%s Timeout=%s -> %s (got %s, HTTP %d)" % (x, y, to, want, got, st))
    # the same command repeated is forwarded every time (the controller makes it a lease refresh without a driver command)
    srv.drain()
    for _ in range(3):
        c.post(PTZ, move("0.5", "0", "PT1S"))
    lines = srv.drain()
    check(lines == ["VEL 1 0 4 0 1000"] * 3, "a repeated ContinuousMove reaches the motor layer every time with its lease (refresh)")
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetStatus><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetStatus>"))
    check("<tt:PanTilt>MOVING</tt:PanTilt>" in xml, "GetStatus reports MOVING while the lease runs")
    srv.drain()
    st, _, _ = c.post(PTZ, stop())
    check(st == 200 and srv.line(2) == "STOP", "Stop reaches the motor layer at once")
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetStatus><tptz:ProfileToken>stream1</tptz:ProfileToken></tptz:GetStatus>"))
    check("<tt:PanTilt>IDLE</tt:PanTilt>" in xml, "GetStatus reports IDLE after the Stop")
    st, _, _ = c.post(PTZ, stop(pantilt="false"))
    check(st == 200 and srv.line(0.5) is None, "Stop with PanTilt=false does nothing")
    st, _, _ = c.post(PTZ, stop(pantilt="true"))
    check(st == 200 and srv.line(2) == "STOP", "Stop with PanTilt=true stops")
    st, _, _ = c.post(PTZ, stop(profile="stream1"))
    check(st == 200 and srv.line(2) == "STOP", "Stop on the sub profile too")
    srv.drain()
    c.post(PTZ, move("1", "0", "PT0.15S"))
    time.sleep(0.45)
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetStatus><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetStatus>"))
    check("<tt:PanTilt>IDLE</tt:PanTilt>" in xml, "a 150 ms lease expires on its own: GetStatus IDLE without any Stop")

    print("== PTZ: argument faults")
    cases = [
        ("unknown profile", move("1", "0", profile="main"), 400, "ter:InvalidArgVal", "ter:NoProfile"),
        ("velocity out of range", move("1.5", "0"), 400, "ter:InvalidArgVal", "ter:InvalidVelocity"),
        ("velocity not a number", move("fast", "0"), 400, "ter:InvalidArgVal", "ter:InvalidVelocity"),
        ("velocity with an exponent", move("1e0", "0"), 400, "ter:InvalidArgVal", "ter:InvalidVelocity"),
        ("a position space instead of velocity", move("1", "0", pt_attr=' space="http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace"'), 400, "ter:InvalidArgVal", "ter:SpaceNotSupported"),
        ("a non-zero zoom", move("1", "0", extra='<tt:Zoom x="0.5"/>'), 400, "ter:InvalidArgVal", "ter:SpaceNotSupported"),
        ("bad timeout", move("1", "0", timeout="soon"), 400, "ter:InvalidArgVal", "ter:InvalidTimeout"),
        ("negative timeout", move("1", "0", timeout="-PT1S"), 400, "ter:InvalidArgVal", "ter:InvalidTimeout"),
    ]
    for label, body, status, sub1, sub2 in cases:
        srv.drain()
        st, xml, _ = c.post(PTZ, body)
        f = fault_of(xml)
        check(st == status and f and f[1] == sub1 and f[2] == sub2 and srv.line(0.2) is None, "%s: HTTP %d, %s / %s, the motor is not touched" % (label, status, sub1, sub2))
    st, xml, _ = c.post(PTZ, move("0", "0", extra='<tt:Zoom x="0"/>'))
    check(st == 200 and srv.line(2) == "STOP", "a zero zoom (sent by many clients) is accepted")
    st, xml, _ = c.post(PTZ, envelope("<tptz:ContinuousMove><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:ContinuousMove>"))
    check(st == 400 and (fault_of(xml) or ("",))[1] == "ter:InvalidArgs", "ContinuousMove without Velocity: Sender fault")
    st, xml, _ = c.post(PTZ, envelope("<tptz:Stop><tptz:ProfileToken>nope</tptz:ProfileToken></tptz:Stop>"))
    check(st == 400 and (fault_of(xml) or ("", "", ""))[2] == "ter:NoProfile", "Stop with an unknown profile: ter:NoProfile")
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetNode><tptz:NodeToken>zzz</tptz:NodeToken></tptz:GetNode>"))
    check(st == 400, "GetNode with an unknown node token is a fault")
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetConfigurationOptions><tptz:ConfigurationToken>zzz</tptz:ConfigurationToken></tptz:GetConfigurationOptions>"))
    check(st == 400 and (fault_of(xml) or ("", "", ""))[2] == "ter:NoConfig", "GetConfigurationOptions with an unknown token: ter:NoConfig")
    for op in ("RelativeMove", "SetPreset", "RemovePreset", "SetHomePosition", "GetPresetTours"):
        st, xml, _ = c.post(PTZ, envelope('<tptz:%s><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:%s>' % (op, op)))
        f = fault_of(xml)
        check(st == 500 and f and f[2] == "ter:ActionNotSupported", "%s is not supported: ter:ActionNotSupported" % op)
    st, xml, _ = c.post(PTZ, "<not-xml")
    check(st in (400, 401), "malformed SOAP is a fault, not a crash")

    print("== PTZ: home and presets")
    srv.drain()
    st, xml, _ = c.post(PTZ, envelope("<tptz:GotoHomePosition><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GotoHomePosition>"))
    check(st == 200 and srv.line(2) == "HOME", "GotoHomePosition -> ptz.home")
    st, xml, _ = c.post(PTZ, envelope("<tptz:GetPresets><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetPresets>"))
    toks = re.findall(r'<tptz:Preset token="([^"]+)"', xml)
    check(toks == ["door", "window", "desk_2"], "GetPresets lists the stored presets in order (%s)" % toks)
    for t in toks:
        srv.drain()
        st, xml, _ = c.post(PTZ, envelope("<tptz:GotoPreset><tptz:ProfileToken>stream1</tptz:ProfileToken><tptz:PresetToken>%s</tptz:PresetToken></tptz:GotoPreset>" % t))
        check(st == 200 and srv.line(2) == "GOTO " + t, "GotoPreset %s" % t)
    for bad in ("nope", "door2", "a" * 40):
        st, xml, _ = c.post(PTZ, envelope("<tptz:GotoPreset><tptz:ProfileToken>stream0</tptz:ProfileToken><tptz:PresetToken>%s</tptz:PresetToken></tptz:GotoPreset>" % bad))
        f = fault_of(xml)
        check(st == 400 and f and f[2] == "ter:NoToken", "GotoPreset with an unknown token (%s): ter:NoToken" % bad[:8])
    st, xml, _ = c.post(PTZ, envelope("<tptz:GotoPreset><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GotoPreset>"))
    check(st == 400, "GotoPreset without a token is a Sender fault")

    print("== PTZ: refusal states (parked / privacy, calibrating, not ready, controller down); Stop always works")
    for state, why in (("parked", "parked"), ("calibrating", "not ready"), ("notready", "not ready"), ("down", "not available")):
        srv.set_state(state)
        srv.drain()
        st, xml, _ = c.post(PTZ, move("1", "0"))
        f = fault_of(xml)
        check(st == 500 and f and f[0] == "env:Receiver" and f[1] == "ter:Action" and f[2] == "ter:PTZUnavailable" and why in xml and srv.line(0.2) is None,
              "%s: ContinuousMove is a SOAP fault (HTTP 500, ter:PTZUnavailable) and nothing moves" % state)
        st, xml, _ = c.post(PTZ, envelope("<tptz:GotoHomePosition><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GotoHomePosition>"))
        check(st == 500 and (fault_of(xml) or ("", "", ""))[2] == "ter:PTZUnavailable" and srv.line(0.2) is None, "%s: GotoHomePosition refused" % state)
        st, xml, _ = c.post(PTZ, envelope("<tptz:GotoPreset><tptz:ProfileToken>stream0</tptz:ProfileToken><tptz:PresetToken>door</tptz:PresetToken></tptz:GotoPreset>"))
        check(st == 500 and (fault_of(xml) or ("", "", ""))[2] == "ter:PTZUnavailable" and srv.line(0.2) is None, "%s: GotoPreset refused" % state)
        st, xml, _ = c.post(PTZ, stop())
        check(st == 200 and srv.line(2) == "STOP", "%s: Stop is still accepted (it only ever makes the head safer)" % state)
        st, xml, _ = c.post(PTZ, move("0", "0"))
        check(st == 200, "%s: a velocity inside the dead zone is a stop and is accepted" % state)
        st, xml, _ = c.post(PTZ, envelope("<tptz:GetPresets><tptz:ProfileToken>stream0</tptz:ProfileToken></tptz:GetPresets>"))
        check(st == 200, "%s: reads still work" % state)
    srv.set_state("ok")
    srv.drain()
    st, xml, _ = c.post(PTZ, move("1", "0"))
    check(st == 200 and srv.line(2) == "VEL 1 0 2 0 450", "back to ok: motion is accepted again")
    c.post(PTZ, stop())

    print("== media: streams")
    st, xml, _ = c.post(MED, envelope("<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Multicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>stream0</trt:ProfileToken></trt:GetStreamUri>"))
    check(st == 400, "multicast is refused")
    st, xml, _ = c.post(MED, envelope("<trt:GetStreamUri><trt:ProfileToken>nope</trt:ProfileToken></trt:GetStreamUri>"))
    check(st == 400 and (fault_of(xml) or ("", "", ""))[2] == "ter:NoProfile", "unknown profile: ter:NoProfile")
    st, xml, _ = c.post(MED, envelope("<trt:GetStreamUri><trt:ProfileToken>stream1</trt:ProfileToken></trt:GetStreamUri>"))
    check("rtsp://127.0.0.1:554/stream1" in xml, "the stream URI of the sub profile is rtsp://<the address the client used>:554/stream1")
    st, xml, _ = c.post(MED, envelope("<trt:SetVideoEncoderConfiguration/>"))
    check(st == 400 and (fault_of(xml) or ("", "", ""))[2] == "ter:ConfigModify", "configuration changes are refused (read-only device)")
    st, xml, _ = c.post(DEV, envelope("<tds:SystemReboot/>"))
    check(st == 500 and (fault_of(xml) or ("", "", ""))[2] == "ter:ActionNotSupported", "SystemReboot and every other device operation are not offered")

    print("== WS-Discovery (loopback)")
    probe = (
        '<?xml version="1.0" encoding="utf-8"?><e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
        'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" xmlns:dn="http://www.onvif.org/ver10/network/wsdl"><e:Header>'
        "<w:MessageID>uuid:11111111-2222-3333-4444-555555555555</w:MessageID><w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>"
    )
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.sendto(probe.encode(), ("127.0.0.1", srv.wsd))
    try:
        data, _ = s.recvfrom(65535)
        txt = data.decode()
    except socket.timeout:
        txt = ""
    check("ProbeMatches" in txt and "http://127.0.0.1:%d/onvif/device_service" % port in txt and "onvif://www.onvif.org/type/ptz" in txt and "Profile/S" in txt,
          "a Probe is answered with a ProbeMatch (XAddr = device_service on the ONVIF port, scopes Profile/S and type/ptz)")
    s.close()


if __name__ == "__main__":
    sys.exit(main())
