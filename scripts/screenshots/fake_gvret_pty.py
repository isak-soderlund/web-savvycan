#!/usr/bin/env python3
"""A fake GVRET serial device on a pseudo-terminal, for screenshots and dry runs of the desktop build
without hardware: answers SavvyCAN's GVRET queries (one bus, CAN FD, as resolve-can-1's firmware) and
streams a fixed mix of classic and CAN FD frames (8 to 64 bytes, BRS on and off, standard and extended IDs).

  python3 scripts/screenshots/fake_gvret_pty.py --link /tmp/wsc-gvret [--seconds 60] [--rate 20]

Then point a "Serial connection (GVRET)" at /tmp/wsc-gvret. The protocol answers follow resolve-can's
test/hil/gvret_fake.py (the TCP version bridged to SocketCAN).
"""

import argparse
import os
import struct
import sys
import termios
import time
import tty
import select

# (id, extended, fd, brs, length)
FRAMES = [
    (0x100, False, False, False, 8),
    (0x1A0, False, False, False, 2),
    (0x18FF50E5, True, False, False, 8),
    (0x200, False, True, False, 12),
    (0x300, False, True, True, 64),
    (0x310, False, True, True, 32),
    (0x1ABCDE0, True, True, True, 48),
    (0x7DF, False, False, False, 3),
]


class Fake:
    def __init__(self):
        self.t0 = time.monotonic()
        self.enabled, self.listen_only, self.bitrate = True, False, 500_000
        self.fd, self.data_bitrate = True, 2_000_000
        self.buf = bytearray()
        self.counter = 0

    def us(self):
        return int((time.monotonic() - self.t0) * 1e6) & 0xFFFFFFFF

    def reply(self, op):
        body = {
            1: struct.pack("<I", self.us()),
            2: bytes(2), 3: bytes(15),
            6: bytes([int(self.enabled) | int(self.listen_only) << 4]) + struct.pack("<I", self.bitrate) + bytes(5),
            7: struct.pack("<H", 400) + bytes([0x20, 0, 0, 0]),
            9: b"\xde\xad", 12: b"\x01", 13: bytes(15),
            22: bytes([1, 0x01 | int(self.fd) << 1]) + struct.pack("<I", self.data_bitrate),
        }.get(op)
        return bytes([0xF1, op]) + body if body is not None else b""

    def parse(self, data):
        self.buf += data
        out = b""
        need = {0: 6, 11: 6, 20: 6, 5: 8, 21: 6, 14: 12, 4: 1, 8: 1, 10: 1}
        buf = self.buf
        while True:
            s = buf.find(0xF1)
            if s < 0:
                buf.clear()
                break
            del buf[:s]
            if len(buf) < 2:
                break
            op = buf[1]
            if op == 0xF1:
                del buf[:1]
                continue
            n = need.get(op)
            if n is None:
                out += self.reply(op)
                del buf[:2]
                continue
            if op in (0, 11, 20):
                if len(buf) < 8:
                    break
                n = 6 + (buf[7] & 0x7F)
            if len(buf) < 2 + n:
                break
            a = bytes(buf[2:2 + n])
            del buf[:2 + n]
            if op == 5:
                w = struct.unpack_from("<I", a)[0]
                if w & 0x80000000:
                    self.enabled, self.listen_only = bool(w & 0x40000000), bool(w & 0x20000000)
                    self.bitrate = w & 0x0FFFFFFF
            elif op == 21:
                self.fd = bool(a[1] & 0x02)
        return out

    def frame(self, ident, ext, fd, brs, length):
        self.counter += 1
        c = self.counter
        data = bytes(((c + i * 17) if i % 4 else (c >> (i // 4)) ) & 0xFF for i in range(length))
        raw = ident | (0x80000000 if ext else 0)
        if fd:
            return bytes([0xF1, 20]) + struct.pack("<II", self.us(), raw) + bytes([length | (0x80 if brs else 0), 0]) + data + b"\0"
        return bytes([0xF1, 0]) + struct.pack("<II", self.us(), raw) + bytes([length]) + data + b"\0"


def write(fd, data):
    try:
        os.write(fd, data)
    except (BlockingIOError, OSError):
        pass  # nobody reading: drop, as a device would


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--link", default="/tmp/wsc-gvret", help="symlink to the pty's device")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--tcp", type=int, default=0,
                    help="serve on 127.0.0.1:PORT instead of a pty (SavvyCAN's Network connection; see port23_redirect.c)")
    ap.add_argument("--rate", type=float, default=10, help="rounds of the frame mix per second")
    args = ap.parse_args()

    if args.tcp:
        import socket
        srv = socket.create_server(("127.0.0.1", args.tcp))
        print(f"fake GVRET on 127.0.0.1:{args.tcp}", flush=True)
        srv.settimeout(args.seconds)
        conn, _ = srv.accept()
        master = conn.detach()
    else:
        master, slave = os.openpty()
        tty.setraw(slave)
        name = os.ttyname(slave)
        try:
            os.unlink(args.link)
        except FileNotFoundError:
            pass
        os.symlink(name, args.link)
        print(f"fake GVRET on {name} ({args.link})", flush=True)

    os.set_blocking(master, False)
    dev = Fake()
    connected = False
    end = time.monotonic() + args.seconds
    next_round = time.monotonic()
    while time.monotonic() < end:
        r, _, _ = select.select([master], [], [], 0.02)
        if r:
            try:
                data = os.read(master, 4096)
            except OSError:
                data = b""
            if data:
                connected = True
                out = dev.parse(data)
                if out:
                    write(master, out)
        if time.monotonic() >= next_round:
            next_round += 1.0 / args.rate
            if connected and dev.enabled:
                write(master, b"".join(dev.frame(*f) for f in FRAMES))
    if not args.tcp:
        os.unlink(args.link)


if __name__ == "__main__":
    sys.exit(main())
