#!/usr/bin/env python3
"""
run.py — picoOS hardware test runner
====================================
Flashes a kit onto an attached board with `picotool load <kit> --force`,
then runs the hardware tests (hwtest/test_*.py) through the USB shell.

Usage
-----
  python3 hwtest/run.py                     # flash newest picowos_D kit, run all
  python3 hwtest/run.py --no-flash          # test whatever the board runs now
  python3 hwtest/run.py -k kill             # only tests whose name contains "kill"
  python3 hwtest/run.py --kit kits/picowos_D-v0.4.8.uf2
  python3 hwtest/run.py --board pico2w --variant D2
  python3 hwtest/run.py --list              # list tests and exit
  python3 hwtest/run.py -v                  # show device output live
  python3 hwtest/run.py --slow              # include slow tests (scantest, ~75 s)

Build the kits first (./build, or ./build wifi).  Close any console.py on
the port: the runner opens it exclusively.  The full device log goes to
hwtest/logs/<time>.log.  Exit status: 0 if every test passed (or was
skipped), 1 otherwise.
"""

from __future__ import annotations

import argparse
import glob
import importlib
import inspect
import os
import re
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from harness import CheckFailed, Context  # noqa: E402
from pico import Pico, PicoError          # noqa: E402

KIT_PREFIX = {"ND": "{b}os-v", "D": "{b}os_D-v", "D2": "{b}os_D2-v"}


def kit_features(board: str, variant: str) -> set:
    f = set()
    if board.endswith("w"):
        f |= {"wifi", "bt"}
    if variant in ("D", "D2"):
        f |= {"display", "led"}
    return f


def parse_kit_name(path: str):
    """kits/picowos_D-v0.4.8.uf2 -> ("picow", "D", "0.4.8")."""
    m = re.match(r"(pico2?w?)os(?:_(D2?))?-v(\d+\.\d+\.\d+)(?:_INJ)?\.uf2$",
                 os.path.basename(path))
    if not m:
        return None
    return m.group(1), m.group(2) or "ND", m.group(3)


def newest_kit(board: str, variant: str) -> str | None:
    prefix = KIT_PREFIX[variant].format(b=board)
    found = []
    for p in glob.glob(os.path.join(ROOT, "kits", prefix + "*.uf2")):
        parsed = parse_kit_name(p)
        if parsed and parsed[0] == board and parsed[1] == variant:
            found.append((tuple(int(x) for x in parsed[2].split(".")), p))
    return max(found)[1] if found else None


def discover(pattern: str | None):
    """Yield (module_name, test_name, fn) in file order, then source order."""
    for path in sorted(glob.glob(os.path.join(HERE, "test_*.py"))):
        mod = importlib.import_module(os.path.splitext(os.path.basename(path))[0])
        fns = [(n, f) for n, f in inspect.getmembers(mod, inspect.isfunction)
               if n.startswith("test_") and f.__module__ == mod.__name__]
        fns.sort(key=lambda nf: inspect.getsourcelines(nf[1])[1])
        for name, fn in fns:
            full = f"{mod.__name__}.{name}"
            if pattern is None or pattern in full:
                yield mod.__name__, name, fn


def main() -> int:
    sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="picow", choices=["pico", "picow", "pico2", "pico2w"])
    ap.add_argument("--variant", default="D", choices=["ND", "D", "D2"])
    ap.add_argument("--kit", help="UF2 to flash (default: newest matching kit in kits/)")
    ap.add_argument("--no-flash", action="store_true", help="skip picotool load")
    ap.add_argument("--port", "-p", help="serial port (auto-detected)")
    ap.add_argument("-k", dest="pattern", help="run tests whose name contains this")
    ap.add_argument("--list", action="store_true", help="list tests and exit")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo device output")
    ap.add_argument("-x", "--exitfirst", action="store_true", help="stop at first failure")
    ap.add_argument("--slow", action="store_true",
                    help="also run tests marked @requires(\"slow\") (minutes each)")
    args = ap.parse_args()

    tests = list(discover(args.pattern))
    if args.list:
        for mod, name, fn in tests:
            req = ",".join(sorted(getattr(fn, "requires", ()))) or "-"
            print(f"{mod}.{name:<45} requires: {req}")
        return 0
    if not tests:
        print("no tests match")
        return 1

    board, variant, version, kit = args.board, args.variant, None, None
    if not args.no_flash:
        kit = args.kit or newest_kit(board, variant)
        if kit is None:
            print(f"no {KIT_PREFIX[variant].format(b=board)}*.uf2 in kits/ — run ./build first")
            return 1
        parsed = parse_kit_name(kit)
        if parsed:
            board, variant, version = parsed

    os.makedirs(os.path.join(HERE, "logs"), exist_ok=True)
    log_path = os.path.join(HERE, "logs", time.strftime("%Y%m%d-%H%M%S") + ".log")
    pico = Pico(port=args.port, log_path=log_path, echo=args.verbose)
    ctx = Context(pico=pico, kit=kit, version=version, board=board,
                  variant=variant, features=kit_features(board, variant))
    if args.slow:
        ctx.features.add("slow")

    try:
        if kit:
            print(f"flashing {os.path.relpath(kit, ROOT)} ...")
            pico.flash(kit)
        print("connecting ...")
        pico.connect(timeout=40.0)
    except PicoError as e:
        print(f"SETUP FAILED: {e}")
        pico.shutdown()
        return 1

    print(f"board {board}, variant {variant}, features "
          f"{','.join(sorted(ctx.features)) or '-'}; log {os.path.relpath(log_path, ROOT)}\n")

    results = []   # (name, status, seconds, message)
    for mod, name, fn in tests:
        full = f"{mod}.{name}"
        missing = getattr(fn, "requires", set()) - ctx.features
        if missing:
            results.append((full, "SKIP", 0.0, "needs " + ",".join(sorted(missing))))
            print(f"SKIP  {full}  (needs {','.join(sorted(missing))})")
            continue

        pico.note(f"TEST {full}")
        t0 = time.monotonic()
        status, msg = "PASS", ""
        try:
            pico.sync()           # every test starts at an idle prompt
            fn(ctx)
        except CheckFailed as e:
            status, msg = "FAIL", str(e)
        except PicoError as e:
            status, msg = "FAIL", f"board: {e}"
        except Exception:
            status, msg = "ERROR", traceback.format_exc(limit=3)
        dt = time.monotonic() - t0
        results.append((full, status, dt, msg))
        print(f"{status:<5} {full}  ({dt:.1f} s)")
        for note in ctx.notes:
            for ln in note.rstrip().splitlines():
                print(f"        {ln}")
        ctx.notes.clear()
        if msg:
            for ln in msg.rstrip().splitlines():
                print(f"        {ln}")
        pico.note(f"{status} {full} {msg}")

        if status != "PASS":
            # Get the shell back for the next test; reconnect if it is gone.
            try:
                pico.sync(timeout=3.0)
            except PicoError:
                try:
                    pico.close()
                    pico.connect(timeout=30.0)
                except PicoError as e:
                    print(f"board lost ({e}); stopping")
                    break
            if args.exitfirst:
                break

    pico.shutdown()
    n = {s: sum(1 for r in results if r[1] == s) for s in ("PASS", "FAIL", "ERROR", "SKIP")}
    print(f"\n{n['PASS']} passed, {n['FAIL']} failed, {n['ERROR']} errors, "
          f"{n['SKIP']} skipped, {len(tests) - len(results)} not run")
    ok = n["FAIL"] == 0 and n["ERROR"] == 0 and len(results) == len(tests)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
