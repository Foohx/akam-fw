#!/usr/bin/env python3
"""onvif_events_probe.py -- host-side, read-only probe of the ONVIF event service of the stock app (yy_cam).

Runs on the PC, never on the camera. Python standard library only. It answers, one command at a time:

  caps        GetEventServiceCapabilities: does the camera claim PullPoint support?
  properties  GetEventProperties: which topics does it announce?
  pullpoint   CreatePullPointSubscription: expected to be refused (offline analysis: no PullPoint in this build)
  listen      Subscribe (WS-BaseNotification push): starts a small HTTP listener on this PC, subscribes the camera
              to it and prints every Notify received until --duration ends, then unsubscribes.
              The camera must be able to reach --listen-host:--listen-port (same LAN, no firewall in between).

Nothing is hard-coded: host, port, credentials and listener address come from the command line / environment.
The password is read from the environment variable ONVIF_PASSWORD (or asked with --ask-password), never from argv.
Exit codes: 0 ok, 1 the camera answered with a SOAP fault or an HTTP error, 2 usage / network error.
See docs/porting/lsc-rotatable-2k/npu-findings.md for what the results mean.
"""

import argparse
import base64
import datetime
import getpass
import hashlib
import http.server
import os
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET

NS_ENV = "http://www.w3.org/2003/05/soap-envelope"
NS_WSA = "http://www.w3.org/2005/08/addressing"
NS_WSNT = "http://docs.oasis-open.org/wsn/b-2"
NS_TEV = "http://www.onvif.org/ver10/events/wsdl"
NS_TT = "http://www.onvif.org/ver10/schema"
NS_WSSE = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
NS_WSU = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
PW_DIGEST = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest"

ACT_SUBSCRIBE = "http://docs.oasis-open.org/wsn/bw-2/NotificationProducer/SubscribeRequest"
ACT_UNSUBSCRIBE = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest"
ACT_PROPS = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetEventPropertiesRequest"
ACT_CAPS = "http://www.onvif.org/ver10/events/wsdl/EventPortType/GetServiceCapabilitiesRequest"
ACT_PULL = "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest"


def ws_security(user, password, nonce=None, created=None):
    """WS-Security UsernameToken (PasswordDigest) header block; empty string without a user."""
    if not user:
        return ""
    nonce = nonce if nonce is not None else os.urandom(16)
    created = created or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + (password or "").encode()).digest()).decode()
    return (
        f'<wsse:Security xmlns:wsse="{NS_WSSE}" xmlns:wsu="{NS_WSU}"><wsse:UsernameToken>'
        f"<wsse:Username>{_esc(user)}</wsse:Username>"
        f'<wsse:Password Type="{PW_DIGEST}">{digest}</wsse:Password>'
        f'<wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">'
        f"{base64.b64encode(nonce).decode()}</wsse:Nonce><wsu:Created>{created}</wsu:Created>"
        f"</wsse:UsernameToken></wsse:Security>"
    )


def _esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def envelope(action, to_url, body, user=None, password=None):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        f'<s:Envelope xmlns:s="{NS_ENV}" xmlns:wsa="{NS_WSA}" xmlns:wsnt="{NS_WSNT}" xmlns:tev="{NS_TEV}">'
        f"<s:Header><wsa:Action>{action}</wsa:Action><wsa:To>{_esc(to_url)}</wsa:To>"
        f"<wsa:MessageID>urn:uuid:{uuid.uuid4()}</wsa:MessageID>{ws_security(user, password)}</s:Header>"
        f"<s:Body>{body}</s:Body></s:Envelope>"
    )


def post(url, action, body, user=None, password=None, timeout=10):
    """POST one SOAP request. Returns (http_status, xml_root_or_None, raw_text)."""
    data = envelope(action, url, body, user, password).encode()
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": f'application/soap+xml; charset=utf-8; action="{action}"'}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status, text = resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as err:  # a SOAP fault usually comes with HTTP 400/500
        status, text = err.code, err.read().decode("utf-8", "replace")
    try:
        root = parse_xml(text)
    except ET.ParseError:
        root = None
    return status, root, text


def parse_xml(text):
    """Parse untrusted XML. A DOCTYPE (hence any entity declaration) is refused: no entity expansion at all."""
    if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
        raise ET.ParseError("DOCTYPE refused")
    return ET.fromstring(text)


def local(tag):
    return tag.rsplit("}", 1)[-1]


def fault_reason(root):
    """The text of a SOAP fault, or None when the answer is not a fault."""
    if root is None:
        return None
    for el in root.iter():
        if local(el.tag) == "Fault":
            texts = [t.text.strip() for t in el.iter() if local(t.tag) in ("Text", "faultstring") and t.text]
            return " / ".join(texts) or "fault without text"
    return None


def topics_of(root):
    """Topic paths announced by GetEventProperties (wstop:TopicSet children carrying wstop:topic=true)."""
    out = []

    def walk(el, path):
        for ch in el:
            name = local(ch.tag)
            here = path + [name]
            if any(local(k) == "topic" and v == "true" for k, v in ch.attrib.items()):
                out.append("/".join(here))
            walk(ch, here)

    for el in root.iter():
        if local(el.tag) == "TopicSet":
            walk(el, [])
    return out


def parse_notify(text):
    """(topic, {SimpleItem name: value}) of every NotificationMessage in a Notify body."""
    try:
        root = parse_xml(text)
    except ET.ParseError:
        return []
    out = []
    for msg in root.iter():
        if local(msg.tag) != "NotificationMessage":
            continue
        topic = ""
        items = {}
        for el in msg.iter():
            if local(el.tag) == "Topic" and el.text:
                topic = el.text.strip()
            elif local(el.tag) == "SimpleItem" and "Name" in el.attrib:
                items[el.attrib["Name"]] = el.attrib.get("Value", "")
        out.append((topic, items))
    return out


def subscription_address(root):
    for el in root.iter():
        if local(el.tag) == "SubscriptionReference":
            for a in el.iter():
                if local(a.tag) == "Address" and a.text:
                    return a.text.strip()
    return None


MAX_BODY = 1 << 20  # largest Notify body accepted (1 MiB)
MAX_SINK = 10000  # most Notify messages kept


class Collector(http.server.BaseHTTPRequestHandler):
    """Receives the Notify posts of the camera. Bounded: oversized bodies are refused, the sink is capped."""

    def do_POST(self):  # noqa: N802 (http.server API)
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            self.send_response(413)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        text = self.rfile.read(length).decode("utf-8", "replace")
        events = parse_notify(text)
        if len(self.server.sink) < MAX_SINK:
            self.server.sink.append((time.time(), self.client_address[0], text, events))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *args):  # silence the default stderr access log
        pass


def cmd_simple(args, action, body, label):
    url = args.event_url
    status, root, text = post(url, action, body, args.user, args.password, args.timeout)
    reason = fault_reason(root)
    if reason:
        print(f"{label}: FAULT (http {status}): {reason}")
        return 1
    if root is None or status >= 400:
        print(f"{label}: http {status}, answer is not SOAP: {text[:200]!r}")
        return 1
    return root


def run(args):
    if args.cmd == "caps":
        root = cmd_simple(args, ACT_CAPS, "<tev:GetServiceCapabilities/>", "caps")
        if isinstance(root, int):
            return root
        for el in root.iter():
            if local(el.tag) == "Capabilities":
                for k, v in sorted(el.attrib.items()):
                    print(f"caps: {local(k)}={v}")
        return 0
    if args.cmd == "properties":
        root = cmd_simple(args, ACT_PROPS, "<tev:GetEventProperties/>", "properties")
        if isinstance(root, int):
            return root
        topics = topics_of(root)
        for t in topics:
            print(f"properties: topic {t}")
        print(f"properties: {len(topics)} topic(s) announced")
        return 0
    if args.cmd == "pullpoint":
        body = '<tev:CreatePullPointSubscription><tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime></tev:CreatePullPointSubscription>'
        root = cmd_simple(args, ACT_PULL, body, "pullpoint")
        if isinstance(root, int):
            return 0 if root == 1 else root  # a refusal is the expected, informative answer
        print(f"pullpoint: ACCEPTED, reference {subscription_address(root)}")
        return 0
    return listen(args)


def same_host(url, other):
    return urllib.parse.urlsplit(url).hostname == urllib.parse.urlsplit(other).hostname


def listen(args):
    # Binds the address given (default: --listen-host, never all interfaces), only for --duration.
    server = http.server.ThreadingHTTPServer((args.listen_bind or args.listen_host, args.listen_port), Collector)
    server.sink = []
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        return listen_loop(args, server, port)
    finally:
        server.shutdown()
        server.server_close()


def listen_loop(args, server, port):
    consumer = f"http://{args.listen_host}:{port}/"
    body = (
        f"<wsnt:Subscribe><wsnt:ConsumerReference><wsa:Address>{_esc(consumer)}</wsa:Address></wsnt:ConsumerReference>"
        f"<wsnt:InitialTerminationTime>PT{int(args.duration) + 30}S</wsnt:InitialTerminationTime></wsnt:Subscribe>"
    )
    status, root, text = post(args.event_url, ACT_SUBSCRIBE, body, args.user, args.password, args.timeout)
    reason = fault_reason(root)
    if reason or root is None or status >= 400:
        print(f"listen: Subscribe refused (http {status}): {reason or text[:200]!r}")
        return 1
    ref = subscription_address(root)
    print(f"listen: subscribed, consumer {consumer}, reference {ref}")
    print(f"listen: waiting {args.duration:g} s for Notify messages (walk in front of the camera now)")
    start = time.time()
    shown = 0
    while time.time() - start < args.duration:
        time.sleep(0.2)
        while shown < len(server.sink):
            ts, src, raw, events = server.sink[shown]
            shown += 1
            stamp = time.strftime("%H:%M:%S", time.localtime(ts))
            if not events:
                print(f"{stamp} notify from {src}: unparsed body {raw[:120]!r}")
            for topic, items in events:
                kv = " ".join(f"{k}={v}" for k, v in sorted(items.items()))
                print(f"{stamp} notify from {src}: topic={topic} {kv}")
    # The reference comes from the camera: only call it back when it points at the camera itself.
    if ref and same_host(ref, args.event_url):
        try:
            post(ref, ACT_UNSUBSCRIBE, "<wsnt:Unsubscribe/>", args.user, args.password, args.timeout)
        except (urllib.error.URLError, OSError) as err:
            print(f"listen: unsubscribe failed ({err}); the subscription expires by itself")
    elif ref:
        print(f"listen: reference {ref} is not on the camera host, not called")
    print(f"listen: done, {len(server.sink)} Notify message(s) received")
    return 0


def parse_args(argv):
    ap = argparse.ArgumentParser(description="Read-only probe of the ONVIF event service (see the module docstring).")
    ap.add_argument("--host", required=True, help="camera address (no default: never hard-coded)")
    ap.add_argument("--port", type=int, default=5000, help="ONVIF HTTP port of the camera (default 5000, UNVERIFIED)")
    ap.add_argument("--event-url", help="full event service URL (default http://HOST:PORT/onvif/event_services)")
    ap.add_argument("--user", help="ONVIF user, if the camera asks for authentication")
    ap.add_argument("--ask-password", action="store_true", help="ask the password (otherwise env ONVIF_PASSWORD)")
    ap.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout in seconds")
    ap.add_argument("cmd", choices=["caps", "properties", "pullpoint", "listen"])
    ap.add_argument("--listen-host", help="address of THIS PC as the camera sees it (required for listen)")
    ap.add_argument("--listen-bind", default="", help="local bind address of the listener (default: --listen-host)")
    ap.add_argument("--listen-port", type=int, default=0, help="listener port (0: pick a free one)")
    ap.add_argument("--duration", type=float, default=60.0, help="seconds to wait for events (listen)")
    args = ap.parse_args(argv)
    if args.cmd == "listen" and not args.listen_host:
        ap.error("listen needs --listen-host (the address of this PC that the camera can reach)")
    args.password = getpass.getpass("ONVIF password: ") if args.ask_password else os.environ.get("ONVIF_PASSWORD")
    if args.event_url and urllib.parse.urlsplit(args.event_url).scheme not in ("http", "https"):
        ap.error("--event-url must be an http(s) URL")
    if not args.event_url:
        if not re.fullmatch(r"[A-Za-z0-9.\-:\[\]]+", args.host):
            ap.error("--host contains characters that are not valid in a host name")
        args.event_url = f"http://{args.host}:{args.port}/onvif/event_services"
    return args


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return run(args)
    except (urllib.error.URLError, OSError) as err:
        print(f"{args.cmd}: network error: {err}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
