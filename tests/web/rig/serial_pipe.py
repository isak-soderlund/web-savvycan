#!/usr/bin/env python3
"""The serial side of the rig's Web Serial bridge (tests/web/rig/bridge.mjs): one process per page, talking to the
real rc1's serial port with pyserial, so the app's Web Serial code runs against rc1's firmware, DTR/RTS included.
From resolve-hil's tools/web/serial_pipe.py (the display's bridge); reads are batched here (rc1 sends up to
~200 KB/s): at most every 2 ms, up to 64 KB.

Not for use by hand: bridge.mjs starts it. Frames on stdin/stdout: 1 byte type, 4 bytes length
(little-endian), payload.
  in   O open (payload: the port path)   C close   W write (bytes)   S signals (2 bytes: DTR,
       RTS; 0 = low, 1 = high, 2 = as it is)   Q quit
  out  o opened / c closed / s signals set (payload: "" or the error)   R read (bytes)
       E the port was lost (payload: the error)
Opening raises DTR and RTS as Chrome's open does (Linux, HUPCL); rc1's session follows DTR. Opening retries for
10 s: after a replug the port can be away for a moment.
"""
import struct
import sys
import threading
import time

import serial

out_lock = threading.Lock()
port = None
port_lock = threading.Lock()


def emit(kind, payload=b""):
    if isinstance(payload, str):
        payload = payload.encode()
    with out_lock:
        sys.stdout.buffer.write(kind.encode() + struct.pack("<I", len(payload)) + payload)
        sys.stdout.buffer.flush()


def reader(s):
    while True:
        with port_lock:
            current = port
        if current is not s:
            return
        try:
            data = s.read(s.in_waiting or 1)
            if data:
                time.sleep(0.002)
                if s.in_waiting:
                    data += s.read(min(s.in_waiting, 65536))
        except (serial.SerialException, OSError, TypeError, AttributeError) as error:
            with port_lock:
                lost = port is s
            if lost:
                emit("E", str(error))
                close()
            return
        if data:
            emit("R", data)


def open_port(path):
    global port
    end = time.time() + 10
    last = None
    while time.time() < end:
        try:
            s = serial.Serial()
            s.port = path
            s.timeout = 0.05
            s.open()
            with port_lock:
                port = s
            threading.Thread(target=reader, args=(s,), daemon=True).start()
            return ""
        except (serial.SerialException, OSError) as error:
            last = error
            time.sleep(0.25)
    return f"could not open {path}: {last}"


def close():
    global port
    with port_lock:
        s, port = port, None
    if s is not None:
        try:
            s.close()
        except (serial.SerialException, OSError):
            pass


def main():
    stdin = sys.stdin.buffer
    while True:
        head = stdin.read(5)
        if len(head) < 5:
            break
        kind = chr(head[0])
        payload = stdin.read(struct.unpack("<I", head[1:])[0])
        if kind == "O":
            close()
            emit("o", open_port(payload.decode()))
        elif kind == "C":
            close()
            emit("c")
        elif kind == "W":
            with port_lock:
                s = port
            try:
                if s is None:
                    raise serial.SerialException("port not open")
                s.write(payload)
            except (serial.SerialException, OSError) as error:
                emit("E", str(error))
                close()
        elif kind == "S":
            with port_lock:
                s = port
            error = ""
            try:
                if s is None:
                    raise serial.SerialException("port not open")
                dtr, rts = payload[0], payload[1]
                if dtr != 2:
                    s.dtr = bool(dtr)
                if rts != 2:
                    s.rts = bool(rts)
            except (serial.SerialException, OSError) as e:
                error = str(e)
            emit("s", error)
        elif kind == "Q":
            break
    close()


if __name__ == "__main__":
    main()
