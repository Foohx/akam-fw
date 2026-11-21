#!/usr/bin/env python3
"""Test of firmware/lsc-rotatable-2k-ak3918av130/tools/mqtt_sink.py on loopback (nothing leaves the host).
Run: python3 -I this_file."""

import importlib.util
import io
import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PATH = os.path.join(HERE, "..", "firmware", "lsc-rotatable-2k-ak3918av130", "tools", "mqtt_sink.py")
spec = importlib.util.spec_from_file_location("mqtt_sink", PATH)
sink = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sink)

failed = 0


def check(what, ok):
    global failed
    print(("  ok   - " if ok else "  FAIL - ") + what)
    if not ok:
        failed += 1


def s(txt):
    b = txt.encode()
    return len(b).to_bytes(2, "big") + b


def pkt(first, body):
    return bytes([first]) + sink.enc_len(len(body)) + body


def connect(client, user=None, pw=None, will=None):
    flags = 0x02
    pay = s(client)
    if will:
        flags |= 0x04 | 0x20
        pay += s(will[0]) + s(will[1])
    if user is not None:
        flags |= 0x80
        pay += s(user)
    if pw is not None:
        flags |= 0x40
        pay += s(pw)
    return pkt(0x10, s("MQTT") + bytes([4, flags]) + (60).to_bytes(2, "big") + pay)


def publish(topic, payload, retain=False, qos=0, pid=1):
    body = s(topic) + (pid.to_bytes(2, "big") if qos else b"") + payload.encode()
    return pkt(0x30 | (qos << 1) | int(retain), body)


def recv_n(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            break
        buf += c
    return buf


import tempfile

savedir = tempfile.mkdtemp(prefix="rot2k_sink_")
out = io.StringIO()
srv = sink.Sink(("127.0.0.1", 0), idle_s=5, out=out, save_dir=savedir)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]


def client():
    c = socket.create_connection(("127.0.0.1", port), timeout=5)
    return c


def wait_for(text, secs=3):
    end = time.time() + secs
    while time.time() < end:
        if text in out.getvalue():
            return True
        time.sleep(0.02)
    return False


# a normal session: CONNECT with credentials and will, retained + QoS 1 publish, subscribe, ping, clean disconnect
c = client()
c.sendall(connect("cam1", "bob", "s3cr3t-pass", ("akam/x/status", "offline")))
check("CONNACK accepted", recv_n(c, 4) == b"\x20\x02\x00\x00")
c.sendall(publish("akam/x/status", "online", retain=True))
c.sendall(publish("akam/x/person/state", "ON", qos=1, pid=0x1234))
check("QoS 1 PUBACK carries the packet id", recv_n(c, 4) == b"\x40\x02\x12\x34")
c.sendall(pkt(0x82, (7).to_bytes(2, "big") + s("akam/x/+/set") + b"\x00"))
check("SUBACK grants QoS 0", recv_n(c, 5) == b"\x90\x03\x00\x07\x00")
c.sendall(b"\xc0\x00")
check("PINGRESP", recv_n(c, 2) == b"\xd0\x00")
c.sendall(b"\xe0\x00")
c.close()
check("publishes printed", wait_for("DISCONNECT"))
t = out.getvalue()
check("retained publish printed with topic and payload", "PUBLISH akam/x/status retain online" in t)
check("QoS 1 publish printed", "PUBLISH akam/x/person/state qos 1 ON" in t)
check("SUBSCRIBE printed and nothing delivered", "SUBSCRIBE akam/x/+/set (nothing is ever delivered)" in t)
check("the password is never printed", "s3cr3t-pass" not in t and "password: set" in t)
check("clean DISCONNECT: no will", "<- will" not in t)

# a binary payload (the JPEG snapshot) is summarised, never dumped as hex, and saved with --save-dir
c = client()
c.sendall(connect("cam1b"))
recv_n(c, 4)
jpg = b"\xff\xd8\xff\xe0" + bytes(range(256)) * 400 + b"\xff\xd9"
c.sendall(pkt(0x30, s("akam/x/snapshot") + jpg))
c.sendall(publish("akam/x/after", "ok"))
check("a snapshot is summarised", wait_for("PUBLISH akam/x/snapshot <%d bytes binary, jpeg, sha256 " % len(jpg)))
check("the packet after it is still read", wait_for("PUBLISH akam/x/after ok"))
check("no hex dump of the JPEG", "0xffd8" not in out.getvalue())
check("--save-dir keeps the JPEG whole", os.path.exists(os.path.join(savedir, "binary_0.jpg")) and open(os.path.join(savedir, "binary_0.jpg"), "rb").read() == jpg)
c.sendall(pkt(0x30, s("akam/x/small") + b"\xff\xfe\x00"))
check("a short binary payload stays a hex line", wait_for("PUBLISH akam/x/small 0xfffe00"))
for _ in range(sink.SAVE_SLOTS + 3):
    c.sendall(pkt(0x30, s("akam/x/snapshot") + jpg))
time.sleep(0.5)
check("at most 20 files are ever kept", len(os.listdir(savedir)) <= sink.SAVE_SLOTS)
c.close()

# a dropped connection publishes the will
c = client()
c.sendall(connect("cam2", will=("akam/y/status", "offline")))
recv_n(c, 4)
c.close()
check("a dropped client's will is printed", wait_for("PUBLISH akam/y/status retain offline   <- will"))

# garbage and hostile input never kill the sink
for junk in (b"\xff\xff\xff\xff\xff\xff", b"\x10\x05MQTT", pkt(0x10, s("MQTX") + b"\x04\x02\x00\x3c" + s("a")),
             pkt(0x10, s("MQTT") + bytes([4, 2]) + b"\x00\x3c" + s("a")) + pkt(0x34, s("t") + b"\x00\x01x"),
             b"\x30\xff\xff\xff\xff\x01"):
    j = client()
    try:
        j.sendall(junk)
        j.settimeout(1)
        try:
            j.recv(64)
        except socket.timeout:
            pass
    except OSError:
        pass
    j.close()
c = client()
c.sendall(connect("cam3"))
check("the sink still answers after junk", recv_n(c, 4) == b"\x20\x02\x00\x00")
c.sendall(publish("t/q2", "x", qos=2))
time.sleep(0.2)
check("QoS 2 is refused by closing the connection", wait_for("PUBLISH qos 2 refused"))
c.close()

srv.shutdown()
import shutil

shutil.rmtree(savedir, ignore_errors=True)
print(f"rot2k mqtt sink test: {failed} failed")
sys.exit(1 if failed else 0)
