#!/usr/bin/env python3
"""Drive a program on a real pseudo-terminal: send keys, wait for text, resize, interrupt.

Usage: python3 -I pty_drive.py [--cols N] [--rows N] [--out FILE] [--timeout S] STEP... -- CMD ARG...

STEP is one of
  send:TEXT      write TEXT to the terminal (Python escapes: \\n \\x1b[B \\x03 ...)
  expect:TEXT    wait until TEXT appears in the output received since the last expect/send
  expectcode:TXT same, but TXT may contain Python escapes (matched against the raw bytes)
  resize:CxR     change the window size and send SIGWINCH to the child
  sigint         send SIGINT to the child's process group (like Ctrl-C without a tty driver)
  wait:SECONDS   sleep
The raw terminal output is written to --out. Exit: prints "EXIT=<code>" or "SIGNAL=<n>" then
"TTY=sane|raw" (are echo and canonical mode back on once the child is gone) and "TIMEOUT=<step>"
if an expect never matched (exit status 3). Standard library only.
"""
import codecs
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def main(argv):
    cols, rows, out, timeout = 80, 24, None, 15.0
    steps = []
    i = 0
    while i < len(argv) and argv[i] != "--":
        a = argv[i]
        if a == "--cols":
            cols = int(argv[i + 1]); i += 1
        elif a == "--rows":
            rows = int(argv[i + 1]); i += 1
        elif a == "--out":
            out = argv[i + 1]; i += 1
        elif a == "--timeout":
            timeout = float(argv[i + 1]); i += 1
        else:
            steps.append(a)
        i += 1
    cmd = argv[i + 1:]
    if not cmd:
        print("pty_drive: no command", file=sys.stderr)
        return 2

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(cmd[0], cmd)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    buf = b""
    mark = 0
    alive = True

    def pump(seconds):
        nonlocal buf, alive
        end = time.time() + seconds
        while time.time() < end and alive:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    alive = False
                    return
                if not d:
                    alive = False
                    return
                buf += d

    def decode(s):
        return codecs.decode(s, "unicode_escape").encode("latin-1")

    failed = None
    for st in steps:
        kind, _, arg = st.partition(":")
        if kind == "send":
            os.write(fd, decode(arg))
            pump(0.3)
        elif kind in ("expect", "expectcode"):
            want = arg.encode() if kind == "expect" else decode(arg)
            end = time.time() + timeout
            while want not in buf[mark:] and time.time() < end and alive:
                pump(0.1)
            if want not in buf[mark:]:
                failed = st
                break
            mark = len(buf)
        elif kind == "resize":
            c, _, r = arg.partition("x")
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", int(r), int(c), 0, 0))
            os.kill(pid, signal.SIGWINCH)
            pump(1.2)
        elif kind == "sigint":
            try:
                os.killpg(os.getpgid(pid), signal.SIGINT)
            except ProcessLookupError:
                pass
            pump(1.0)
        elif kind == "wait":
            pump(float(arg))
    pump(0.5)

    # give the child time to exit, then reap it
    code = None
    end = time.time() + 5
    while time.time() < end:
        try:
            p, status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            code = "EXIT=0"
            break
        if p:
            if os.WIFEXITED(status):
                code = "EXIT=%d" % os.WEXITSTATUS(status)
            else:
                code = "SIGNAL=%d" % os.WTERMSIG(status)
            break
        pump(0.1)
    if code is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        code = "HUNG"
    pump(0.1)
    try:
        lflag = termios.tcgetattr(fd)[3]
        tty = "sane" if (lflag & termios.ECHO and lflag & termios.ICANON) else "raw"
    except termios.error:
        tty = "gone"
    if out:
        with open(out, "wb") as f:
            f.write(buf)
    print(code)
    print("TTY=" + tty)
    if failed:
        print("TIMEOUT=" + failed)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
