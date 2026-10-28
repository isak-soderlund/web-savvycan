#!/usr/bin/env python3
"""Checks the desktop fake rc1 (rc1_pty.py) with resolve-can's own host-side protocol tool, test/hil/serial_proto.py
(the one the firmware's M8 HIL tests use against the real rc1): GVRET as SavvyCAN drives it (classic and FD,
frames both ways) and slcan, on the pty. serial_proto.py lives on resolve-can's `firmware` branch; it is taken from
there (git show) unless --serial-proto names a file. pty ioctls for DTR/RTS fail, so they are made no-ops here.

  python3 tests/desktop/check_pty.py [--resolve-can ~/src/resolve-can] [--serial-proto FILE]
Run with a Python that has pyserial (resolve-can's .venv, or Ubuntu's python3-serial). Exit 1 on a failure.
"""

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load_serial_proto(args):
    path = args.serial_proto
    if not path:
        src = subprocess.run(["git", "-C", os.path.expanduser(args.resolve_can), "show",
                              "origin/firmware:test/hil/serial_proto.py"], check=True, capture_output=True).stdout
        d = tempfile.mkdtemp(prefix="rc1-check-")
        path = os.path.join(d, "serial_proto.py")
        with open(path, "wb") as f:
            f.write(src)
    import serial.serialposix as sp
    sp.Serial._update_dtr_state = lambda self: None
    sp.Serial._update_rts_state = lambda self: None
    spec = importlib.util.spec_from_file_location("serial_proto", path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def run(m, argv):
    sys.argv = ["serial_proto.py"] + argv
    print("$ serial_proto.py " + " ".join(argv), flush=True)
    return m.main()


def last_stats(path):
    with open(path) as f:
        lines = [json.loads(x) for x in f if x.startswith("{")]
    return lines[-1] if lines else {}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resolve-can", default="~/src/resolve-can")
    ap.add_argument("--serial-proto")
    args = ap.parse_args()
    m = load_serial_proto(args)
    failures = []
    for traffic, cases in [
        ("leaf", [["gvret", "--bitrate", "500000", "--send", "123#1122", "--send", "1234567A#11", "--seconds", "2"],
                  ["slcan", "--bitrate", "500000", "--send", "123#1122", "--seconds", "2"]]),
        ("fd-mix", [["gvret", "--bitrate", "500000", "--data-bitrate", "2000000", "--send", "123##1" + "AB" * 64,
                     "--seconds", "2"]]),
    ]:
        for case in cases:
            link = tempfile.mktemp(prefix="rc1-fake-")
            stats = link + ".jsonl"
            p = subprocess.Popen([sys.executable, os.path.join(HERE, "rc1_pty.py"), "--link", link, "--traffic", traffic,
                                  "--stats", stats], stdout=subprocess.PIPE, text=True)
            p.stdout.readline()
            try:
                rc = run(m, [case[0], link] + case[1:])
            except SystemExit as e:
                rc = e.code
            time.sleep(1.2)
            p.terminate()
            p.wait(10)
            s = last_stats(stats)
            print(f"  -> exit {rc}; fake: bus {s.get('bus')} sent {s.get('encoded')} lost {s.get('lost')} host frames "
                  f"{s.get('hostFrames')} log {s.get('log')}", flush=True)
            ok = rc == 0 and s.get("encoded", 0) > 0 and s.get("lost") == 0 and s.get("hostFrames", 0) == case.count("--send")
            if not ok:
                failures.append(" ".join(case))
            os.unlink(stats)
    print("FAIL: " + "; ".join(failures) if failures else "OK: GVRET and slcan on the pty")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
