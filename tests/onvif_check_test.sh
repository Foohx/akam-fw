#!/usr/bin/env bash
# Test of tools/hw_tests/onvif_check.py against a small fake ONVIF + RTSP server on 127.0.0.1 (python stdlib):
# HTTP 200 answers, an unauthenticated request answered with HTTP 401 and an EMPTY body (Digest challenge) which is the
# expected refusal, a SOAP fault, the RTSP Digest DESCRIBE, the "clock not set" note, and the failure exit when the
# fake accepts a request without a password. No camera, no network beyond 127.0.0.1. Skipped without python3.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TOOL="$REPO/tools/hw_tests/onvif_check.py"
command -v python3 >/dev/null 2>&1 || { echo "skip: python3 not installed"; exit 0; }
WORK=$(mktemp -d)
SRV_PID=""
trap '[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

cat >"$WORK/fake.py" <<'PY'
import hashlib, os, re, socketserver, sys, threading, time
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE = sys.argv[2]                       # normal | acceptall | oldclock
PASSWORD = "pw-for-test"
SOAP = '<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" xmlns:timg="http://www.onvif.org/ver20/imaging/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema"><s:Body>%s</s:Body></s:Envelope>'
rtsp_port = [0]
http_port = [0]


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, body, extra=None):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/soap+xml")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        op = re.search(r"<s:Body><(?:\w+:)?(\w+)", body).group(1)
        has_sec = "UsernameToken" in body
        base = "http://127.0.0.1:%d/onvif" % http_port[0]
        if op != "GetSystemDateAndTime" and not has_sec and MODE != "acceptall":
            return self.reply(401, "", {"WWW-Authenticate": 'Digest realm="fake", qop="auth", nonce="abc"'})
        if op == "GetRelayOutputs" or op == "GetSnapshotUri":
            return self.reply(400, SOAP % '<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:ActionNotSupported</s:Value></s:Subcode></s:Code><s:Reason><s:Text>no</s:Text></s:Reason></s:Fault>')
        if op == "GetSystemDateAndTime":
            y = 2001 if MODE == "oldclock" else time.gmtime().tm_year
            t = time.gmtime()
            return self.reply(200, SOAP % ('<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>' % (t.tm_hour, t.tm_min, t.tm_sec, y, t.tm_mon, t.tm_mday)))
        if op == "GetServices":
            return self.reply(200, SOAP % ('<tds:GetServicesResponse><tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace><tds:XAddr>%s/media_service</tds:XAddr></tds:Service><tds:Service><tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace><tds:XAddr>%s/imaging_service</tds:XAddr></tds:Service></tds:GetServicesResponse>' % (base, base)))
        if op == "GetProfiles":
            return self.reply(200, SOAP % '<trt:GetProfilesResponse><trt:Profiles token="p1"><tt:VideoSourceConfiguration><tt:SourceToken>vs1</tt:SourceToken></tt:VideoSourceConfiguration></trt:Profiles></trt:GetProfilesResponse>')
        if op == "GetStreamUri":
            return self.reply(200, SOAP % ('<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>rtsp://127.0.0.1:%d/stream</tt:Uri></trt:MediaUri></trt:GetStreamUriResponse>' % rtsp_port[0]))
        if op == "GetImagingSettings":
            return self.reply(200, SOAP % '<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>50</tt:Brightness></timg:ImagingSettings></timg:GetImagingSettingsResponse>')
        if op == "GetDeviceInformation":
            return self.reply(200, SOAP % '<tds:GetDeviceInformationResponse><tds:FirmwareVersion>b1-test</tds:FirmwareVersion></tds:GetDeviceInformationResponse>')
        return self.reply(200, SOAP % ('<tds:%sResponse/>' % op))


class R(socketserver.StreamRequestHandler):
    # Like the camera: the nonce belongs to this TCP connection (a nonce from
    # another connection is refused), and the challenge has no qop.
    def handle(self):
        nonce = os.urandom(8).hex()
        buf = b""
        while True:
            while b"\r\n\r\n" not in buf:
                c = self.request.recv(4096)
                if not c: return
                buf += c
            head, _, buf = buf.partition(b"\r\n\r\n")
            t = head.decode()
            uri = re.search(r"DESCRIBE (\S+)", t).group(1)
            a = re.search(r'Authorization: Digest (.*)', t)
            if a:
                p = dict(re.findall(r'(\w+)="?([^",\r]*)"?', a.group(1)))
                md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
                ha1 = md5("viewer:fake:" + PASSWORD)
                exp = md5("%s:%s:%s" % (ha1, nonce, md5("DESCRIBE:" + uri)))
                if p.get("nonce") == nonce and p.get("response") == exp:
                    sdp = "v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                    self.request.sendall(("RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: %d\r\n\r\n%s" % (len(sdp), sdp)).encode())
                    return
            self.request.sendall(('RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Digest realm="fake", nonce="%s", algorithm=MD5\r\n\r\n' % nonce).encode())


class TS(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


h = HTTPServer(("127.0.0.1", 0), H); h.daemon_threads = True
r = TS(("127.0.0.1", 0), R)
http_port[0], rtsp_port[0] = h.server_address[1], r.server_address[1]
threading.Thread(target=r.serve_forever, daemon=True).start()
open(sys.argv[1], "w").write(str(http_port[0]))
h.serve_forever()
PY

fail=0
check() { # description, condition command...
    local desc=$1
    shift
    if "$@"; then echo "  ok   - $desc"; else echo "FAIL: $desc"; fail=1; fi
}

failed_with() { [ "$RC" -eq 1 ] && has "$1"; }
has() { grep -q -- "$1" "$WORK/out"; }
hasnt() { ! grep -q -- "$1" "$WORK/out"; }
start() { # mode -> sets PORT
    if [ -n "$SRV_PID" ]; then kill "$SRV_PID" 2>/dev/null || true; fi
    rm -f "$WORK/port"
    python3 -I "$WORK/fake.py" "$WORK/port" "$1" &
    SRV_PID=$!
    for _ in $(seq 1 50); do [ -s "$WORK/port" ] && break; sleep 0.1; done
    PORT=$(cat "$WORK/port")
}
run() { # extra args; output in $WORK/out, status in $RC
    RC=0
    OCT_PW=pw-for-test python3 -I "$TOOL" "127.0.0.1:$PORT" viewer --password-env OCT_PW --no-discovery "$@" >"$WORK/out" 2>&1 || RC=$?
}

start normal
run --rtsp --yes-imaging --imaging-hold 0
cat "$WORK/out"
check "normal server: exit 0" test "$RC" -eq 0
check "401 with an empty body is the expected refusal" grep -q "GetDeviceInformation without password: refused as expected (HTTP 401" "$WORK/out"
check "SOAP fault is the expected refusal" grep -q "unknown action (GetRelayOutputs): refused as expected (HTTP 400" "$WORK/out"
check "authenticated call answered 200" grep -q "ok   GetNetworkInterfaces" "$WORK/out"
check "RTSP Digest DESCRIBE with H264 passes" grep -q "RTSP DESCRIBE with the password: 200, H264" "$WORK/out"
check "imaging set and restore ran" grep -q "restored to 50.0" "$WORK/out"
check "no clock note when the clock is right" hasnt "NOTE: the camera clock"

run --no-imaging
check "--no-imaging skips the brightness change" hasnt "SetImagingSettings"

RC=0
OCT_PW=wrong python3 -I "$TOOL" "127.0.0.1:$PORT" viewer --password-env OCT_PW --no-discovery --rtsp --no-imaging >"$WORK/out" 2>&1 || RC=$?
check "wrong password: RTSP DESCRIBE fails, exit 1" failed_with "FAIL RTSP DESCRIBE with the password: RTSP/1.0 401"

start oldclock
run --no-imaging
check "clock far off: the note suggests ./akam ntp set" grep -q "akam ntp set" "$WORK/out"

start acceptall
run --no-imaging
check "a request accepted without password fails the run" failed_with "accepted, a refusal was expected"

[ "$fail" -eq 0 ] || exit 1
echo "onvif_check_test: all passed"
