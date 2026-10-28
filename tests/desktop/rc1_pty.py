#!/usr/bin/env python3
"""The fake rc1 for the desktop build: a pseudo-terminal that behaves like rc1's serial port (GVRET and LAWICEL/slcan
exactly as the firmware does), so web-savvycan's desktop build (and resolve-can's test/hil/serial_proto.py) can
connect to it without hardware. The device itself is the browser tests' fake (tests/web/fake/rc1.js, run by node via
rc1-stdio.js): one implementation for both.

  tests/desktop/rc1_pty.py --link /tmp/rc1-fake [--traffic leaf|classic-full|fd64-full|fd-mix|errors] [--stats F]
  tests/desktop/rc1_pty.py --tcp 2323 ...        # also/instead: SavvyCAN's "Network connection (GVRET)" on TCP

A pty has no modem lines: the session ends (rc1 back to slcan, CAN channel closed) when the host closes the port,
seen as the master's EIO (no slave open), the same as rc1 seeing DTR fall. DTR/RTS ioctls on a pty fail (EINVAL):
SavvyCAN's GVRET "ESP32 serial mode" sets them and would report an unsupported operation; its standard mode doesn't.

The desktop app finds serial ports with QSerialPortInfo::availablePorts(), which does not list ptys: its serial
connection must accept a path typed in (or QSerialPort::setPortName(path)) to open /tmp/rc1-fake (README).
Stats (JSON lines: frames on the bus, sent to the host, lost, the device log) to --stats or stderr. Ctrl-C ends.
"""

import argparse
import errno
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
STDIO = os.path.join(HERE, "..", "web", "fake", "rc1-stdio.js")


def start_device(args) -> tuple[subprocess.Popen, int]:
    r, w = os.pipe()
    cmd = ["node", STDIO, "--buffer", str(args.buffer)]
    if args.traffic:
        cmd += ["--traffic", args.traffic]
    if args.refuse_open:
        cmd.append("--refuse-open")
    stats = open(args.stats, "a") if args.stats else sys.stderr
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stats, pass_fds=(r,),
                         preexec_fn=lambda: os.dup2(r, 3))
    os.close(r)
    os.set_blocking(p.stdout.fileno(), False)
    return p, w


def control(w: int, **c) -> None:
    os.write(w, (json.dumps(c) + "\n").encode())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--link", default="/tmp/rc1-fake", help="symlink to the pty's slave (default /tmp/rc1-fake)")
    ap.add_argument("--tcp", type=int, help="serve GVRET on this TCP port too (one client at a time)")
    ap.add_argument("--traffic", help="traffic mode, started when the CAN channel opens (rc1.js makeTraffic)")
    ap.add_argument("--buffer", type=int, default=65536, help="bytes rc1 + the host hold before frames are lost")
    ap.add_argument("--refuse-open", action="store_true", help="the CAN channel is owned by gs_usb")
    ap.add_argument("--stats", help="append the stats JSON lines here")
    args = ap.parse_args()

    dev, ctl = start_device(args)
    master, slave = os.openpty()
    tty.setraw(slave)
    name = os.ttyname(slave)
    os.close(slave)
    os.set_blocking(master, False)
    if os.path.islink(args.link) or os.path.exists(args.link):
        os.unlink(args.link)
    os.symlink(name, args.link)
    srv = None
    if args.tcp:
        srv = socket.create_server(("127.0.0.1", args.tcp))
        srv.setblocking(False)
    print(f"fake rc1 on {args.link} -> {name}" + (f", GVRET on 127.0.0.1:{args.tcp}" if args.tcp else ""), flush=True)

    stop = False

    def end(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, end)
    signal.signal(signal.SIGTERM, end)
    host_open = False  # the pty's slave is open (a host holds the port)
    client = None
    try:
        while not stop and dev.poll() is None:
            rl = [dev.stdout] + ([srv] if srv else []) + ([client] if client else [])
            if host_open:
                rl.append(master)
            ready, _, _ = select.select(rl, [], [], 0.05)
            if not host_open and client is None:
                # Probe: EIO while no slave is open; anything else (EAGAIN, data) means a host opened it.
                try:
                    b = os.read(master, 4096)
                    host_open = True
                    control(ctl, cmd="open")
                    dev.stdin.write(b)
                    dev.stdin.flush()
                except OSError as e:
                    if e.errno == errno.EAGAIN:
                        host_open = True
                        control(ctl, cmd="open")
            if master in ready:
                try:
                    b = os.read(master, 65536)
                    dev.stdin.write(b)
                    dev.stdin.flush()
                except OSError as e:
                    if e.errno == errno.EIO:
                        host_open = False
                        control(ctl, cmd="closed")
            if srv and srv in ready and client is None:
                client, _ = srv.accept()
                client.setblocking(False)
                control(ctl, cmd="open")
            if client and client in ready:
                b = client.recv(65536)
                if b:
                    dev.stdin.write(b)
                    dev.stdin.flush()
                else:
                    client.close()
                    client = None
                    control(ctl, cmd="closed")
            if dev.stdout in ready:
                b = dev.stdout.read(65536) or b""
                if host_open:
                    try:
                        os.write(master, b)
                    except OSError:
                        pass
                if client:
                    try:
                        client.sendall(b)
                    except OSError:
                        pass
    finally:
        control(ctl, cmd="stats")
        time.sleep(0.2)
        dev.stdin.close()
        dev.wait(5)
        os.unlink(args.link)
    return 0


if __name__ == "__main__":
    sys.exit(main())
