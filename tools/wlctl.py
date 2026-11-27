#!/usr/bin/env python3
"""lodos-wl control socket client (T042). Run with python3 -I. Protocol: native/lodos-wl/src/ctl.rs.

usage:
  wlctl.py <sock> tap x y [ms]                 touch down/up at x,y (hold ms, default 60)
  wlctl.py <sock> swipe x0 y0 x1 y1 ms         touch drag in ~16ms steps
  wlctl.py <sock> click x y                    pointer move + BTN_LEFT
  wlctl.py <sock> key code [code...]           press+release evdev keys in order
  wlctl.py <sock> type 'text'                  type ASCII text via evdev keys (US layout, \n = Enter)
  wlctl.py <sock> dump out.ppm                 framebuffer dump (absolute path recommended)
  wlctl.py <sock> raw 'line' ['line'...]       send raw protocol lines
"""
import os
import socket
import sys
import time


# US layout: char -> (evdev code, shift)
_ROWS = [("1234567890-=", 2), ("qwertyuiop[]", 16), ("asdfghjkl;'", 30), ("zxcvbnm,./", 44)]
_SHIFT = dict(zip('!@#$%^&*()_+{}:"<>?~|', "1234567890-=[];',./`\\"))
KEYMAP = {" ": (57, False), "\n": (28, False), "\t": (15, False), "`": (41, False), "\\": (43, False)}
for row, base in _ROWS:
    for i, ch in enumerate(row):
        KEYMAP[ch] = (base + i, False)
for ch in "abcdefghijklmnopqrstuvwxyz":
    KEYMAP[ch.upper()] = (KEYMAP[ch][0], True)
for ch, base in _SHIFT.items():
    KEYMAP[ch] = (KEYMAP[base][0], True)


class Ctl:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(10)
        self.s.connect(path)
        self.buf = b""

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                raise SystemExit("wlctl: server closed")
            self.buf += d
        rep, self.buf = self.buf.split(b"\n", 1)
        rep = rep.decode()
        if rep != "ok":
            raise SystemExit(f"wlctl: {line!r} -> {rep}")

    def tap(self, x, y, ms=60):
        self.cmd(f"touch down 0 {x} {y}")
        time.sleep(ms / 1000)
        self.cmd("touch up 0")

    def swipe(self, x0, y0, x1, y1, ms):
        n = max(2, int(ms / 16))
        self.cmd(f"touch down 0 {x0} {y0}")
        for i in range(1, n + 1):
            time.sleep(ms / 1000 / n)
            t = i / n
            self.cmd(f"touch motion 0 {x0 + (x1 - x0) * t:.1f} {y0 + (y1 - y0) * t:.1f}")
        self.cmd("touch up 0")

    def click(self, x, y):
        self.cmd(f"ptr move {x} {y}")
        time.sleep(0.03)
        self.cmd("btn 272 1")
        time.sleep(0.06)
        self.cmd("btn 272 0")

    def key(self, code):
        self.cmd(f"key {code} 1")
        time.sleep(0.03)
        self.cmd(f"key {code} 0")

    def type(self, text):
        for ch in text:
            if ch not in KEYMAP:
                raise SystemExit(f"wlctl: no key for {ch!r}")
            code, shift = KEYMAP[ch]
            if shift:
                self.cmd("key 42 1")
            self.key(code)
            if shift:
                self.cmd("key 42 0")


def main(a):
    if len(a) < 2:
        sys.exit(__doc__)
    c = Ctl(a[0])
    op, args = a[1], a[2:]
    num = [float(v) for v in args] if op in ("tap", "swipe", "click") else []
    if op == "tap" and len(num) in (2, 3):
        c.tap(*num)
    elif op == "swipe" and len(num) == 5:
        c.swipe(*num)
    elif op == "click" and len(num) == 2:
        c.click(*num)
    elif op == "key" and args:
        for k in args:
            c.key(int(k))
    elif op == "type" and len(args) == 1:
        c.type(args[0].replace("\\n", "\n"))
    elif op == "dump" and len(args) == 1:
        c.cmd("dump " + os.path.abspath(args[0]))
    elif op == "raw" and args:
        for line in args:
            c.cmd(line)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
