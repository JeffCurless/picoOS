#!/usr/bin/env python3
"""
scantest.py — run the on-device WiFi/BT scan-buffer test and report the result
================================================================================
Sends "run scantest [raw]" to a pico_w / pico2_w running picoOS, echoes the
app's output, and exits 0 on "RESULT: PASS", 1 on "RESULT: FAIL" or timeout.

Usage
-----
  python3 tools/scantest.py                       # auto-detect the Pico
  python3 tools/scantest.py --port /dev/ttyACM0
  python3 tools/scantest.py --raw                 # test the deprecated getters
  python3 tools/scantest.py --log scantest.log

Build the firmware with "./build wifi" and flash a picowos*/pico2wos* image
from kits/ first.  Close any other console session on the port.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from console import DEFAULT_BAUD, open_connection  # noqa: E402

TIMEOUT_S = 150.0   # 5 WiFi + 3 BT scans + continuous mode take about 75 s


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", "-p", default=None, help="serial port (auto-detected)")
    ap.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--raw", action="store_true",
                    help="test the deprecated *_get_scan_results pointer API")
    ap.add_argument("--log", "-l", default=None, help="also write output to this file")
    ap.add_argument("--timeout", type=float, default=TIMEOUT_S,
                    help=f"seconds to wait for a result (default {TIMEOUT_S:.0f})")
    args = ap.parse_args()

    ser = open_connection(args.port, args.baud)
    log = open(args.log, "w") if args.log else None

    # Drain anything already buffered, then start the test.
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(("run scantest raw\r" if args.raw else "run scantest\r").encode())
    ser.flush()

    result = None
    line = b""
    deadline = time.monotonic() + args.timeout
    while result is None and time.monotonic() < deadline:
        chunk = ser.read(256)
        if not chunk:
            continue
        line += chunk
        while b"\n" in line:
            raw, line = line.split(b"\n", 1)
            text = raw.decode("utf-8", errors="replace").rstrip("\r")
            print(text)
            if log:
                log.write(text + "\n")
            if "[scantest] RESULT: PASS" in text:
                result = 0
            elif "[scantest] RESULT: FAIL" in text:
                result = 1
            elif "run: app 'scantest' not found" in text:
                print("scantest is not in this firmware — flash a ./build wifi image")
                result = 1

    ser.close()
    if log:
        log.close()
    if result is None:
        print(f"TIMEOUT: no result after {args.timeout:.0f} s")
        return 1
    return result


if __name__ == "__main__":
    sys.exit(main())
