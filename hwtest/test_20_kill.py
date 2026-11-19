"""
Issue #26: a kill must not leak a held kmutex or the files a process had open.

Two device-side tests run the timing-critical sequences on the board (kill a
thread while it holds a mutex, and check the instant after).  The rest are
host-driven: an `hwtest` holder mode keeps a state for seconds, and the host
kills it at its own pace and checks the result through the shell.
"""

import re

from harness import check


def _run_device_test(p, mode, timeout=20.0):
    since = p.mark()
    p.cmd(f"run hwtest {mode}")
    m = p.expect(rf"\[hwtest\] {mode}: RESULT: (PASS|FAIL)", timeout=timeout, since=since)
    fails = [ln for ln in p.text_since(since).split("\n") if "FAIL " in ln]
    check(m.group(1) == "PASS", f"device test {mode} failed: {fails}")


def _start_holder(p, mode, pattern):
    """Run a holder mode and wait for its ready line; returns the match."""
    since = p.mark()
    p.cmd(f"run hwtest {mode}")
    return p.expect(pattern, timeout=5.0, since=since)


def _rm(p, *names):
    for n in names:
        p.cmd(f"rm {n}")


def test_device_kill_mutex(ctx):
    _run_device_test(ctx.pico, "kill-mutex")


def test_device_kill_file(ctx):
    try:
        _run_device_test(ctx.pico, "kill-file")
    finally:
        _rm(ctx.pico, "hwt_kill.txt")


def test_kill_waits_for_mutex_release(ctx):
    """kill of a thread holding a mutex: it stays (kill pending) and dies in
    its unlock, never reaching the code after it."""
    p = ctx.pico
    m = _start_holder(p, "holdmutex",
                      r"holdmutex: pid (\d+) tid (\d+) holding for (\d+) ms")
    tid = int(m.group(2))
    since = p.mark()
    check("killed" in p.cmd(f"kill {tid}"), "kill accepts the holder")
    check(tid in p.threads(), "holder still exists while it holds the mutex")
    p.expect(r"holdmutex: releasing", timeout=5.0, since=since)
    p.wait_until(lambda: tid not in p.threads(), timeout=2.0,
                 what=f"thread {tid} to go once it unlocked")
    check(p.absent(r"released \(not killed\)", 0.5, since=since),
          "holder did not run past kmutex_unlock()")


def test_unkilled_holder_runs_to_completion(ctx):
    """Control for the test above: without a kill, the holder finishes."""
    p = ctx.pico
    since = p.mark()
    _start_holder(p, "holdmutex", r"holdmutex: pid \d+ tid \d+ holding")
    p.expect(r"released \(not killed\)", timeout=6.0, since=since)


def test_killproc_closes_open_file(ctx):
    """A process killed with a file open for writing: its data is committed
    and the single write buffer is free for the next writer."""
    p = ctx.pico
    try:
        m = _start_holder(p, "holdfile", r"holdfile: pid (\d+) tid (\d+) holding fd")
        pid = int(m.group(1))
        out = p.cmd("fs write hwt_b.txt blocked")
        check("cannot open" in out, f"write buffer is held while the holder lives: {out!r}")
        out = p.cmd(f"killproc {pid}")
        check("closed 1 open file(s)" in out, f"killproc closes the holder's file: {out!r}")
        check(pid not in p.processes(), "holder process is gone")
        out = p.cmd("fs write hwt_b.txt free again")
        check("wrote" in out, f"a new writer gets the buffer: {out!r}")
        check("held-by-holdfile" in p.cmd("cat hwt_hold.txt"),
              "the killed holder's data was committed")
    finally:
        _rm(p, "hwt_b.txt", "hwt_hold.txt")


def test_kill_last_thread_closes_open_file(ctx):
    """Same, through `kill <tid>` of the process's only thread."""
    p = ctx.pico
    try:
        m = _start_holder(p, "holdfile", r"holdfile: pid (\d+) tid (\d+) holding fd")
        pid, tid = int(m.group(1)), int(m.group(2))
        p.cmd(f"kill {tid}")
        check(pid not in p.processes(), "holder process is gone")
        check("wrote" in p.cmd("fs write hwt_b.txt after kill"),
              "write buffer is free after kill")
    finally:
        _rm(p, "hwt_b.txt", "hwt_hold.txt")


def test_exit_without_close_is_cleaned_up(ctx):
    """An app that returns with a file still open does not keep the buffer."""
    p = ctx.pico
    try:
        m = _start_holder(p, "leakfile", r"leakfile: pid (\d+) exiting")
        pid = int(m.group(1))
        p.wait_until(lambda: pid not in p.processes(), timeout=3.0,
                     what=f"process {pid} to exit")
        check("wrote" in p.cmd("fs write hwt_b.txt after leak"),
              "the next writer reaps the leaked fd and gets the buffer")
        check("leaked-by-leakfile" in p.cmd("cat hwt_leak.txt"),
              "the leaked file's data was committed")
    finally:
        _rm(p, "hwt_b.txt", "hwt_leak.txt")


def test_no_threads_left_behind(ctx):
    """After the tests above, only the boot-time threads remain."""
    p = ctx.pico
    names = sorted(t["name"] for t in p.threads().values())
    leftovers = [n for n in names if re.match(r"hwt|hwtest", n)]
    check(not leftovers, f"test threads still alive: {leftovers}")


def test_device_kill_blocked(ctx):
    """#5: a thread killed while it waits on a semaphore, event flags, a
    message queue (either side) or a mutex that is never signalled is freed
    at once: gone from the thread table, off the waiter list, stack back on
    the heap.  Then MAX_THREADS + 8 kills in a row, which used to run the
    thread pool dry."""
    _run_device_test(ctx.pico, "kill-blocked", timeout=60.0)
