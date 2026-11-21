#!/usr/bin/env python3
"""Test of firmware/lsc-rotatable-2k-ak3918av130/tools/onvif_events_probe.py against a fake camera on loopback.

Nothing leaves the host: the fake camera and the probe both use 127.0.0.1. Run: python3 -I this_file.
"""

import contextlib
import http.server
import importlib.util
import io
import os
import re
import sys
import threading
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "firmware", "lsc-rotatable-2k-ak3918av130", "tools", "onvif_events_probe.py")
spec = importlib.util.spec_from_file_location("onvif_events_probe", PROBE)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

PROPS = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tev="http://www.onvif.org/ver10/events/wsdl" '
    'xmlns:wstop="http://docs.oasis-open.org/wsn/t-1" xmlns:tns1="http://www.onvif.org/ver10/topics">'
    "<s:Body><tev:GetEventPropertiesResponse><wstop:TopicSet><tns1:RuleEngine><CellMotionDetector>"
    '<Motion wstop:topic="true"/></CellMotionDetector></tns1:RuleEngine></wstop:TopicSet>'
    "</tev:GetEventPropertiesResponse></s:Body></s:Envelope>"
)
CAPS = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tev="http://www.onvif.org/ver10/events/wsdl">'
    '<s:Body><tev:GetServiceCapabilitiesResponse><tev:Capabilities WSPullPointSupport="false" MaxPullPoints="10"/>'
    "</tev:GetServiceCapabilitiesResponse></s:Body></s:Envelope>"
)
FAULT = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><s:Fault><s:Code><s:Value>s:Receiver'
    "</s:Value></s:Code><s:Reason><s:Text>Action not supported</s:Text></s:Reason></s:Fault></s:Body></s:Envelope>"
)
NOTIFY = (
    '<wsnt:Notify xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" xmlns:tt="http://www.onvif.org/ver10/schema">'
    "<wsnt:NotificationMessage><wsnt:Topic>tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic><wsnt:Message>"
    '<tt:Message><tt:Data><tt:SimpleItem Name="IsMotion" Value="true"/></tt:Data></tt:Message></wsnt:Message>'
    "</wsnt:NotificationMessage></wsnt:Notify>"
)


class FakeCamera(http.server.BaseHTTPRequestHandler):
    seen = []

    def do_POST(self):  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode()
        FakeCamera.seen.append(body)
        status, out = 200, FAULT
        if "GetEventProperties" in body:
            out = PROPS
        elif "GetServiceCapabilities" in body:
            out = CAPS
        elif "CreatePullPointSubscription" in body:
            status = 500
        elif "<wsnt:Subscribe>" in body:
            consumer = re.search(r"<wsa:Address>([^<]+)</wsa:Address>", body).group(1)
            ref = f"http://127.0.0.1:{self.server.server_address[1]}/event_service/1"
            out = (
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" xmlns:wsa="http://www.w3.org/2005/08/addressing">'
                f"<s:Body><wsnt:SubscribeResponse><wsnt:SubscriptionReference><wsa:Address>{ref}</wsa:Address>"
                "</wsnt:SubscriptionReference></wsnt:SubscribeResponse></s:Body></s:Envelope>"
            )
            threading.Timer(0.3, lambda: urllib.request.urlopen(consumer, NOTIFY.encode(), timeout=5)).start()
        elif "Unsubscribe" in body:
            out = '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body/></s:Envelope>'
        data = out.encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


failed = 0


def check(name, ok):
    global failed
    print(("  ok   - " if ok else "  FAIL - ") + name)
    if not ok:
        failed += 1


def call(*argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = probe.main(list(argv))
    return rc, buf.getvalue()


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeCamera)
threading.Thread(target=server.serve_forever, daemon=True).start()
port = str(server.server_address[1])
base = ["--host", "127.0.0.1", "--port", port, "--timeout", "5"]

rc, out = call(*base, "caps")
check("caps reports WSPullPointSupport=false", rc == 0 and "WSPullPointSupport=false" in out)
rc, out = call(*base, "properties")
check("properties lists the announced topic", rc == 0 and "RuleEngine/CellMotionDetector/Motion" in out)
rc, out = call(*base, "pullpoint")
check("pullpoint refusal is reported, not an error", rc == 0 and "pullpoint: FAULT" in out and "Action not supported" in out)
rc, out = call(*base, "listen", "--listen-host", "127.0.0.1", "--listen-bind", "127.0.0.1", "--duration", "1.5")
check("listen subscribes", "listen: subscribed" in out)
check("listen prints the Notify topic and IsMotion", "CellMotionDetector/Motion IsMotion=true" in out)
check("listen ends cleanly with the count", rc == 0 and "1 Notify message(s) received" in out)
check("listen unsubscribed", any("Unsubscribe" in b for b in FakeCamera.seen))

# Authentication header: digest present with a user, absent without; the password never appears in clear.
os.environ["ONVIF_PASSWORD"] = "s3cret-pw"
FakeCamera.seen.clear()
call(*base, "--user", "someone", "caps")
check("user: UsernameToken digest sent", "PasswordDigest" in FakeCamera.seen[-1] and "someone" in FakeCamera.seen[-1])
check("password is not sent in clear", "s3cret-pw" not in FakeCamera.seen[-1])
FakeCamera.seen.clear()
call(*base, "caps")
check("no user: no security header", "UsernameToken" not in FakeCamera.seen[-1])
check("digest is deterministic for a fixed nonce", probe.ws_security("u", "p", b"n" * 16, "2000-01-01T00:00:00Z") ==
      probe.ws_security("u", "p", b"n" * 16, "2000-01-01T00:00:00Z"))

# Usage and network errors.
for argv, name in ((["--host", "127.0.0.1", "listen"], "listen without --listen-host"),):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            probe.main(argv)
        check(name + " is a usage error", False)
    except SystemExit as err:
        check(name + " is a usage error (exit 2)", err.code == 2)
with contextlib.redirect_stderr(io.StringIO()):
    rc, _ = call("--host", "127.0.0.1", "--port", "1", "--timeout", "1", "caps")
check("connection refused exits 2", rc == 2)
try:
    with contextlib.redirect_stderr(io.StringIO()):
        probe.main(["caps"])
    check("--host is required", False)
except SystemExit as err:
    check("--host is required (exit 2)", err.code == 2)

# Hardening: no DOCTYPE/entities, bounded listener body, http(s) only.
check("DOCTYPE is refused", probe.parse_notify('<!DOCTYPE x [<!ENTITY e "boom">]><a>&e;</a>') == [])
try:
    probe.parse_xml('<!DOCTYPE x><a/>')
    check("parse_xml refuses a DOCTYPE", False)
except probe.ET.ParseError:
    check("parse_xml refuses a DOCTYPE", True)
try:
    with contextlib.redirect_stderr(io.StringIO()):
        probe.main(["--host", "127.0.0.1", "--event-url", "file:///etc/passwd", "caps"])
    check("--event-url file:// refused", False)
except SystemExit as err:
    check("--event-url file:// refused (exit 2)", err.code == 2)
coll = http.server.ThreadingHTTPServer(("127.0.0.1", 0), probe.Collector)
coll.sink = []
threading.Thread(target=coll.serve_forever, daemon=True).start()
try:
    urllib.request.urlopen(
        urllib.request.Request(f"http://127.0.0.1:{coll.server_address[1]}/", b"x" * (probe.MAX_BODY + 1)), timeout=5
    )
    check("oversized Notify refused", False)
except urllib.error.HTTPError as err:
    check("oversized Notify refused (413, nothing kept)", err.code == 413 and not coll.sink)
except urllib.error.URLError as err:
    # The collector answers 413 without reading the whole body: the client may see the reset first (a race).
    check("oversized Notify refused (connection cut, nothing kept)",
          isinstance(err.reason, (BrokenPipeError, ConnectionResetError)) and not coll.sink)
coll.shutdown()

server.shutdown()
print(f"rot2k onvif probe test: {failed} failed")
sys.exit(1 if failed else 0)
