#!/usr/bin/env python3
"""The rig's CAN peer for the real-rc1 browser tests (tests/web/rig): another node on rc1's bus (the Kvaser's
channel wired to rc1's DB9, resolve-hil rig.yaml `resolve_can.can_channel`, default hilcan1) that sends known
frames, through SocketCAN with nothing but the standard library. Used by peer.mjs; usable by hand:

  can_peer.py setup  [--channel C] --bitrate 500000 [--dbitrate 2000000]   (sudo ip link: classic, or CAN FD)
  can_peer.py restore [--channel C]                                         (the rig's own: 500 kbit/s classic)
  can_peer.py send   [--channel C] --pattern P [--count N] [--rate R] [--seconds S]
                     prints one JSON line: {"sent": n, "seconds": s, "frames": [...] (unless --no-list)}
  can_peer.py state  [--channel C]                                          (JSON: state, error counters, tx/rx)
  can_peer.py release                rc1's own SocketCAN interface (gs_usb) down: while it is up the firmware gives
                                     the CAN channel to gs_usb and refuses it on the serial port; prints the names
                                     it took down (JSON list)
  can_peer.py reclaim NAME...        those interfaces up again

Patterns: classic (8-byte frames, sequence number in data[0..3], IDs 0x100.. in turn), fd-mix (every FD length with
BRS on and off, 11/29-bit, classic and remote frames: the fake rc1's fd-mix), fd64 (64-byte BRS frames, sequence
number), leaf (the IDs and lengths of a Leaf EV-CAN, sequence number in data[0..3]).
Every frame must be ACKed (the interface's tx counter): a frame nobody ACKs is retried by the controller; `send`
waits up to 3 s for the last ACK, then drops the queue and exits 1 ("only n of m frames ACKed"): rc1's channel isn't
open.
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CANFD_BRS = 0x01
LENS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]
LEAF = [(0x1DA, 8), (0x1DB, 8), (0x1DC, 8), (0x1F2, 8), (0x50A, 6), (0x50C, 5), (0x55A, 8), (0x55B, 8),
        (0x59E, 8), (0x5A9, 8), (0x5BC, 8), (0x5C0, 8), (0x11A, 8), (0x180, 8), (0x182, 8), (0x284, 8)]


def frames(pattern, count):
    """Yields {id, ext, fd, brs, rtr, data} dicts."""
    for i in range(count):
        seq = list(struct.pack("<I", i))
        if pattern == "classic":
            yield dict(id=0x100 + i % 16, ext=False, fd=False, brs=False, rtr=False, data=seq + [i & 0xFF, 0x55, 0xAA, 0x0F])
        elif pattern == "fd64":
            yield dict(id=0x200 + i % 4, ext=False, fd=True, brs=True, rtr=False,
                       data=seq + [(i + k) & 0xFF for k in range(60)])
        elif pattern == "leaf":
            fid, n = LEAF[i % len(LEAF)]
            yield dict(id=fid, ext=False, fd=False, brs=False, rtr=False, data=(seq + [0xC0 | k for k in range(4)])[:n])
        elif pattern == "fd-mix":
            k = i % 40
            ext = bool(k & 1)
            fid = (0x18DAF100 + i) & 0x1FFFFFFF if ext else 0x300 + k
            if k < 32:  # every FD length, BRS on and off
                n = LENS[k % 16]
                yield dict(id=fid, ext=ext, fd=True, brs=k >= 16, rtr=False, data=[(i * 7 + j) & 0xFF for j in range(n)])
            elif k < 38:  # classic 0-8 bytes
                n = (k - 32) + 3
                yield dict(id=fid, ext=ext, fd=False, brs=False, rtr=False, data=seq + [k] * (n - 4) if n >= 4 else seq[:n])
            else:  # remote
                yield dict(id=fid, ext=ext, fd=False, brs=False, rtr=True, data=[])
        else:
            raise SystemExit(f"unknown pattern {pattern}")


def pack(f):
    can_id = f["id"] | (CAN_EFF_FLAG if f["ext"] else 0) | (CAN_RTR_FLAG if f["rtr"] else 0)
    data = bytes(f["data"])
    if f["fd"]:
        n = next(l for l in LENS if l >= len(data))
        return struct.pack("=IBBBB", can_id, n, CANFD_BRS if f["brs"] else 0, 0, 0) + data.ljust(64, b"\0")
    dlc = 0 if f["rtr"] else len(data)
    return struct.pack("=IBBBB", can_id, dlc, 0, 0, 0) + data.ljust(8, b"\0")


def ip(*args):
    subprocess.run(["sudo", "ip", "link", *args], check=True)


def cmd_setup(a):
    ip("set", a.channel, "down")
    if a.dbitrate:
        ip("set", a.channel, "type", "can", "bitrate", str(a.bitrate), "dbitrate", str(a.dbitrate), "fd", "on",
           "restart-ms", "100")
    else:
        ip("set", a.channel, "type", "can", "bitrate", str(a.bitrate), "fd", "off", "restart-ms", "100")
    ip("set", a.channel, "up")
    print(json.dumps(state(a.channel)))


def cmd_restore(a):
    ip("set", a.channel, "down")
    ip("set", a.channel, "type", "can", "bitrate", "500000", "fd", "off", "restart-ms", "100", "berr-reporting", "on")
    ip("set", a.channel, "up")
    print(json.dumps(state(a.channel)))


def state(channel):
    out = subprocess.run(["ip", "-j", "-d", "-s", "link", "show", channel], capture_output=True, text=True, check=True)
    link = json.loads(out.stdout)[0]
    info = link.get("linkinfo", {}).get("info_data", {})
    st = link.get("stats64", link.get("stats", {}))
    return {"channel": channel, "up": "UP" in link.get("flags", []), "state": info.get("state"),
            "bitrate": info.get("bittiming", {}).get("bitrate"), "dbitrate": info.get("data_bittiming", {}).get("bitrate"),
            "fd": link.get("mtu") == 72, "berr": info.get("berr_counter"),
            "tx": st.get("tx", {}).get("packets"), "rx": st.get("rx", {}).get("packets")}


def rc1_netdevs():
    """SocketCAN interfaces of rc1 (USB 1209:0001), up or not: {name: up}."""
    found = {}
    for name in os.listdir("/sys/class/net"):
        dev = os.path.realpath(f"/sys/class/net/{name}/device")
        usb = os.path.dirname(dev)
        try:
            ids = [open(f"{usb}/{f}").read().strip() for f in ("idVendor", "idProduct")]
        except OSError:
            continue
        if ids == ["1209", "0001"]:
            found[name] = "UP" in open(f"/sys/class/net/{name}/operstate").read().upper() or \
                bool(int(open(f"/sys/class/net/{name}/flags").read(), 16) & 1)
    return found


def cmd_release(a):
    down = [n for n, up in sorted(rc1_netdevs().items()) if up]
    for n in down:
        ip("set", n, "down")
    print(json.dumps(down))


def cmd_reclaim(a):
    for n in a.names:
        ip("set", n, "up")
    print(json.dumps(a.names))


def cmd_state(a):
    print(json.dumps(state(a.channel)))


def cmd_send(a):
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 16)
    s.bind((a.channel,))
    s.setblocking(False)
    count = a.count if a.count else int((a.rate or 10000) * a.seconds)
    sent = []
    tx0 = state(a.channel)["tx"]
    period = 1.0 / a.rate if a.rate > 0 else 0  # --rate 0: as fast as the bus takes them
    start = time.monotonic()
    for i, f in enumerate(frames(a.pattern, count)):
        if period:
            wait = start + i * period - time.monotonic()
            if wait > 0:
                time.sleep(wait)
        raw = pack(f)
        stuck = time.monotonic()
        while True:
            try:
                s.send(raw)
                break
            except OSError as e:  # ENOBUFS/EAGAIN: the tx queue is full (no ACK, or faster than the bus)
                if e.errno not in (105, 11) or time.monotonic() - stuck > 2:
                    s.close()
                    ip("set", a.channel, "down")  # the queued frames would go out later, into the next test
                    ip("set", a.channel, "up")
                    print(json.dumps({"sent": len(sent), "acked": state(a.channel)["tx"] - tx0,
                                      "error": f"no progress for 2 s ({e}): rc1's channel not open?; queue dropped"}))
                    return 1
                time.sleep(0.0002)
        sent.append(f)
    # Every frame ACKed: the interface's tx counter (counts frames sent with an ACK) up by the count, within 3 s.
    # Otherwise the rest would still go out later (into the next test): the queue is dropped (down/up).
    end = time.monotonic() + 3
    while (acked := state(a.channel)["tx"] - tx0) < len(sent) and time.monotonic() < end:
        time.sleep(0.05)
    result = {"sent": len(sent), "acked": acked, "seconds": round(time.monotonic() - start, 3)}
    if acked < len(sent):
        s.close()
        ip("set", a.channel, "down")
        ip("set", a.channel, "up")
        result["error"] = f"only {acked} of {len(sent)} frames ACKed (rc1's channel not open?); the rest dropped"
        print(json.dumps(result))
        return 1
    if not a.no_list:
        result["frames"] = sent
    print(json.dumps(result))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["setup", "restore", "send", "state", "release", "reclaim"])
    p.add_argument("names", nargs="*")
    p.add_argument("--channel", default=os.environ.get("WSC_RC1_PEER", "hilcan1"))
    p.add_argument("--bitrate", type=int, default=500000)
    p.add_argument("--dbitrate", type=int, default=0)
    p.add_argument("--pattern", default="classic")
    p.add_argument("--count", type=int, default=0)
    p.add_argument("--rate", type=float, default=1000)
    p.add_argument("--seconds", type=float, default=1)
    p.add_argument("--no-list", action="store_true")
    a = p.parse_args()
    return {"setup": cmd_setup, "restore": cmd_restore, "send": cmd_send, "state": cmd_state,
            "release": cmd_release, "reclaim": cmd_reclaim}[a.command](a) or 0


if __name__ == "__main__":
    sys.exit(main())
