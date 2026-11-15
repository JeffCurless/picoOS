"""
Stack measurement: run every app that can run unattended, then report the
deepest stack use per thread and a suggested app_table[] stack_size.

Short-lived threads (pi workers, hwtest modes) exit before `threads` could
catch them, so the measurement is taken on the device: task_free_thread()
records each thread's high-water mark as it frees it, and the shell
`stacks` command shows those merged with the live threads.

A reading is the deepest path that actually ran.  Error paths that were not
taken add nothing, so treat the numbers as lower bounds; the suggested
sizes add a margin for that.  scantest runs only with --slow (~75 s).

The workload tests below only drive apps; test_stack_report reads the
result.  Run the whole file (-k test_30) for a complete report.
"""

import os
import time

from harness import check, requires

HERE = os.path.dirname(os.path.abspath(__file__))

MAX_USE_PCT = 80     # fail if any thread used more than this much of its stack
DEFAULT_STACK = 2048


def suggested_size(peak: int) -> int:
    """Peak plus a margin for paths the run did not take — a quarter of the
    peak, at least 256 B — rounded up to 256 B."""
    want = peak + max(256, peak // 4)
    return (want + 255) // 256 * 256


def parse_stacks(p) -> dict:
    """`stacks` as {name: {peak, size, pct, exited, live}}."""
    rows = {}
    for ln in p.lines(p.cmd("stacks")):
        f = ln.split()
        if len(f) == 6 and f[1].isdigit() and f[3].endswith("%"):
            rows[f[0]] = {"peak": int(f[1]), "size": int(f[2]),
                          "pct": int(f[3].rstrip("%")),
                          "exited": int(f[4]), "live": int(f[5])}
    return rows


def _run_and_kill(p, apps, seconds):
    """Start apps, let them run, then kill their processes."""
    pids = []
    for app in apps:
        out = p.cmd(f"run {app}")
        check(f"started '{app}'" in out, f"run {app}: {out!r}")
        pids.append(int(out.split("PID ")[1].split()[0]))
    time.sleep(seconds)
    for pid in pids:
        p.cmd(f"killproc {pid}")


# ---------------------------------------------------------------------------
# Workloads
# ---------------------------------------------------------------------------
def test_stacks_reset(ctx):
    out = ctx.pico.cmd("stacks reset")
    check("cleared" in out, f"stacks reset: {out!r}")


def test_load_demo_apps(ctx):
    """producer + consumer exchange messages; sensor prints readings."""
    _run_and_kill(ctx.pico, ["producer", "consumer", "sensor"], 3.0)


def test_load_pi(ctx):
    p = ctx.pico
    for arg in ("", " single"):
        since = p.mark()
        p.cmd(f"run pi{arg}")
        p.expect(r"\[pi\] elapsed", timeout=30.0, since=since)


def test_load_hwtest_modes(ctx):
    p = ctx.pico
    try:
        for mode in ("kill-mutex", "kill-file"):
            since = p.mark()
            p.cmd(f"run hwtest {mode}")
            p.expect(rf"{mode}: RESULT: PASS", timeout=20.0, since=since)
        since = p.mark()
        p.cmd("run hwtest holdmutex")
        p.expect(r"released \(not killed\)", timeout=6.0, since=since)
        since = p.mark()
        p.cmd("run hwtest leakfile")
        p.expect(r"leakfile: pid \d+ exiting", timeout=5.0, since=since)
    finally:
        for f in ("hwt_kill.txt", "hwt_leak.txt"):
            p.cmd(f"rm {f}")


def test_load_shell_commands(ctx):
    """The shell thread's deepest paths: listings, the filesystem, trace."""
    p = ctx.pico
    for c in ("info", "help", "ps", "threads", "mem", "ls", "run",
              "trace on", "trace off", "trace show 20"):
        p.cmd(c)
    p.cmd("fs write hwt_stk.txt stack measurement")
    p.cmd("cat hwt_stk.txt")
    p.cmd("rm hwt_stk.txt")


@requires("wifi")
def test_load_radio_shell_commands(ctx):
    p = ctx.pico
    p.cmd("wifi status")
    p.cmd("wifi scan", timeout=30.0)
    p.cmd("bt status")
    p.cmd("bt scan", timeout=30.0)


@requires("wifi", "slow")
def test_load_scantest(ctx):
    p = ctx.pico
    since = p.mark()
    p.cmd("run scantest")
    p.expect(r"\[scantest\] RESULT: (PASS|FAIL)", timeout=180.0, since=since)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
def test_stack_report(ctx):
    """Print every thread's peak with a suggested size, save the table to
    hwtest/logs/stacks-<time>.txt, and fail if any thread is past
    MAX_USE_PCT of its stack."""
    p = ctx.pico
    rows = parse_stacks(p)
    check(rows, "stacks printed no rows")

    apps = set()
    for ln in p.lines(p.cmd("run")):
        if ln.startswith("  "):
            apps.add(ln.strip())

    lines = [f"{'thread':<16} {'peak':>5} {'size':>5} {'use':>4}  {'seen':>4}  "
             f"{'suggest':>7}  note"]
    for name, r in sorted(rows.items(), key=lambda kv: -kv[1]["pct"]):
        seen = r["exited"] + r["live"]
        note = "app_table[]" if name in apps else ""
        if r["pct"] > MAX_USE_PCT:
            note = (note + " OVER LIMIT").strip()
        lines.append(f"{name:<16} {r['peak']:>5} {r['size']:>5} {r['pct']:>3}%  "
                     f"{seen:>4}  {suggested_size(r['peak']):>7}  {note}")
    unmeasured = sorted(a for a in apps if a not in rows)
    if unmeasured:
        lines.append(f"not measured: {', '.join(unmeasured)}")
    report = "\n".join(lines)
    ctx.info(report)

    os.makedirs(os.path.join(HERE, "logs"), exist_ok=True)
    path = os.path.join(HERE, "logs", time.strftime("stacks-%Y%m%d-%H%M%S.txt"))
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"kit: {ctx.kit or '(not flashed)'}\n{report}\n")
    ctx.info(f"saved {os.path.relpath(path, os.path.dirname(HERE))}")

    over = [n for n, r in rows.items() if r["pct"] > MAX_USE_PCT]
    check(not over, f"threads past {MAX_USE_PCT}% of their stack: {over}")
