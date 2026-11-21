#!/usr/bin/env python3
"""mqtt_sink.py - a minimal MQTT 3.1.1 broker SINK for testing akam2k_rtsp's MQTT output without a real broker.

Standard library only. It accepts CONNECT (always CONNACK 0, any credentials), SUBSCRIBE (SUBACK, granted QoS as
requested but nothing is ever delivered), PUBLISH (prints topic and payload; QoS 1 gets a PUBACK, QoS 2 is refused by
closing), PINGREQ (PINGRESP) and DISCONNECT. It NEVER forwards a message to anyone and keeps nothing (no retained
store, no sessions). When a client drops without DISCONNECT its will message is printed like a PUBLISH, tagged "will".
The password of a CONNECT is never printed (only "password: set").

    python3 mqtt_sink.py --bind 0.0.0.0 --port 1883

Output, one line per event, flushed:  HH:MM:SS <client> PUBLISH <topic> [retain] [qos N] <payload>
A binary payload (the JPEG snapshot) is summarised as "<N bytes binary, jpeg, sha256 xxxxxxxx>"; with --save-dir DIR it is
also written to DIR/binary_<n>.jpg|bin (n cycles 0..19: at most 20 files are ever kept).
Intended for the PC of the test bench on the LAN: bind a LAN address, never expose it elsewhere.
"""
import argparse
import hashlib
import os
import socket
import socketserver
import sys
import threading
import time

MAX_REMAINING = 268435455
MAX_CLIENTS = 16  # simultaneous connections; one more is closed at once (bounded memory)
MAX_PACKET = 1 << 20  # a sink never needs more than 1 MiB per packet: bigger is refused (closes the connection)


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def read_packet(sock):
    """Returns (first_byte, body) or raises EOFError / ValueError."""
    first = read_exact(sock, 1)[0]
    mult, value = 1, 0
    for i in range(4):
        b = read_exact(sock, 1)[0]
        value += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
    else:
        raise ValueError("remaining length too long")
    if value > MAX_PACKET:
        raise ValueError("packet too large")
    return first, read_exact(sock, value) if value else b""


def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def take_str(body, pos):
    if pos + 2 > len(body):
        raise ValueError("truncated string")
    n = int.from_bytes(body[pos:pos + 2], "big")
    if pos + 2 + n > len(body):
        raise ValueError("truncated string")
    return body[pos + 2:pos + 2 + n].decode("utf-8", "replace"), pos + 2 + n


def parse_connect(body):
    """Returns dict(client, user, has_pass, will_topic, will_payload, will_retain, keepalive)."""
    proto, pos = take_str(body, 0)
    if proto != "MQTT" or pos + 4 > len(body) or body[pos] != 4:
        raise ValueError("not MQTT 3.1.1")
    flags = body[pos + 1]
    keepalive = int.from_bytes(body[pos + 2:pos + 4], "big")
    pos += 4
    client, pos = take_str(body, pos)
    c = {"client": client, "user": None, "has_pass": False, "will_topic": None, "will_payload": b"",
         "will_retain": False, "keepalive": keepalive}
    if flags & 0x04:
        c["will_topic"], pos = take_str(body, pos)
        n = int.from_bytes(body[pos:pos + 2], "big")
        c["will_payload"] = body[pos + 2:pos + 2 + n]
        pos += 2 + n
        c["will_retain"] = bool(flags & 0x20)
    if flags & 0x80:
        c["user"], pos = take_str(body, pos)
    if flags & 0x40:
        _, pos = take_str(body, pos)  # the password is read and dropped, never stored or printed
        c["has_pass"] = True
    return c


def parse_publish(first, body):
    qos = (first >> 1) & 3
    topic, pos = take_str(body, 0)
    pid = None
    if qos:
        if pos + 2 > len(body):
            raise ValueError("truncated packet id")
        pid = int.from_bytes(body[pos:pos + 2], "big")
        pos += 2
    return {"topic": topic, "qos": qos, "retain": bool(first & 1), "pid": pid, "payload": body[pos:]}


SAVE_SLOTS = 20


def fmt_payload(p):
    try:
        return p.decode("utf-8")
    except UnicodeDecodeError:
        return "0x" + p.hex()


def summarize_binary(p):
    """None for text; for a binary payload of more than 64 bytes the one-line summary (and its extension)."""
    try:
        p.decode("utf-8")
        return None
    except UnicodeDecodeError:
        pass
    if len(p) <= 64:
        return None
    jpeg = p[:2] == b"\xff\xd8"
    return "<%d bytes binary%s, sha256 %s>" % (len(p), ", jpeg" if jpeg else "", hashlib.sha256(p).hexdigest()[:8]), \
        ".jpg" if jpeg else ".bin"


class Handler(socketserver.BaseRequestHandler):
    def log(self, msg):
        self.server.emit("%s %s %s" % (time.strftime("%H:%M:%S"), self.client, msg))

    def handle(self):
        self.client = "%s:%d" % self.client_address[:2]
        sock = self.request
        if not self.server.slots.acquire(blocking=False):
            return
        try:
            self.serve(sock)
        finally:
            self.server.slots.release()

    def serve(self, sock):
        sock.settimeout(self.server.idle_s)
        will = None
        clean_exit = False
        try:
            first, body = read_packet(sock)
            if first >> 4 != 1:
                return
            c = parse_connect(body)
            self.client = c["client"] or self.client
            self.log("CONNECT keepalive %d user %s password: %s will: %s" % (
                c["keepalive"], c["user"] if c["user"] is not None else "-", "set" if c["has_pass"] else "none",
                c["will_topic"] or "-"))
            if c["will_topic"]:
                will = c
            sock.sendall(b"\x20\x02\x00\x00")
            while True:
                first, body = read_packet(sock)
                t = first >> 4
                if t == 3:
                    m = parse_publish(first, body)
                    if m["qos"] == 2:
                        self.log("PUBLISH qos 2 refused, closing")
                        return
                    summ = summarize_binary(m["payload"])
                    self.log("PUBLISH %s%s%s %s" % (m["topic"], " retain" if m["retain"] else "",
                                                  " qos 1" if m["qos"] else "",
                                                  summ[0] if summ else fmt_payload(m["payload"])))
                    if summ and self.server.save_dir:
                        self.server.save(m["payload"], summ[1])
                    if m["qos"] == 1:
                        sock.sendall(b"\x40\x02" + m["pid"].to_bytes(2, "big"))
                elif t == 8:
                    pid = int.from_bytes(body[:2], "big")
                    pos, grants = 2, bytearray()
                    while pos < len(body):
                        flt, pos = take_str(body, pos)
                        q = min(body[pos], 1) if pos < len(body) else 0
                        pos += 1
                        grants.append(q)
                        self.log("SUBSCRIBE %s (nothing is ever delivered)" % flt)
                    sock.sendall(b"\x90" + enc_len(2 + len(grants)) + pid.to_bytes(2, "big") + bytes(grants))
                elif t == 12:
                    sock.sendall(b"\xd0\x00")
                elif t == 14:
                    clean_exit = True
                    self.log("DISCONNECT")
                    return
                else:
                    self.log("packet type %d ignored" % t)
        except (EOFError, ConnectionError, socket.timeout, ValueError, IndexError, OSError) as e:
            self.log("connection ended (%s)" % (type(e).__name__,))
        finally:
            if will and not clean_exit:
                self.log("PUBLISH %s%s %s   <- will" % (will["will_topic"], " retain" if will["will_retain"] else "",
                                                       fmt_payload(will["will_payload"])))


class Sink(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, idle_s=300, out=None, save_dir=None):
        super().__init__(addr, Handler)
        self.idle_s = idle_s
        self.save_dir = save_dir
        self.saved = 0
        self.out = out or sys.stdout
        self.lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(MAX_CLIENTS)

    def save(self, payload, ext):
        """Writes a binary payload to save_dir/binary_<n><ext>, n cycling over SAVE_SLOTS (bounded disk use)."""
        with self.lock:
            n = self.saved % SAVE_SLOTS
            self.saved += 1
        try:
            with open(os.path.join(self.save_dir, "binary_%d%s" % (n, ext)), "wb") as f:
                f.write(payload)
        except OSError:
            pass

    def emit(self, line):
        with self.lock:
            print(line, file=self.out, flush=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Minimal MQTT 3.1.1 broker sink (prints publishes, forwards nothing).")
    ap.add_argument("--bind", default="127.0.0.1", help="address to listen on (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--idle", type=int, default=300, help="drop a silent client after this many seconds")
    ap.add_argument("--save-dir", default=None, help="write binary payloads (JPEG snapshots) here, at most 20 files")
    a = ap.parse_args(argv)
    if a.save_dir:
        os.makedirs(a.save_dir, exist_ok=True)
    srv = Sink((a.bind, a.port), idle_s=a.idle, save_dir=a.save_dir)
    print("mqtt_sink listening on %s:%d (Ctrl-C to stop)" % srv.server_address[:2], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
