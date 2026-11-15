"""Smoke tests: the board runs the flashed kit and the shell answers."""

import re

from harness import check, requires


def test_info_matches_kit(ctx):
    out = ctx.pico.cmd("info")
    m = re.search(r"picoOS v(\d+\.\d+\.\d+)", out)
    check(m is not None, "info prints the picoOS version")
    if ctx.version:
        check(m.group(1) == ctx.version,
              f"running v{m.group(1)}, flashed kit is v{ctx.version}")
    if "wifi" in ctx.features:
        check("WiFi      : CYW43" in out, "info reports WiFi")
        check("Bluetooth : CYW43" in out, "info reports Bluetooth")
    if "display" in ctx.features:
        check("Display   :" in out, "info reports the display")


def test_help_lists_commands(ctx):
    out = ctx.pico.cmd("help")
    names = {ln.split()[0] for ln in ctx.pico.lines(out) if ln.startswith("  ")}
    want = {"info", "help", "ps", "threads", "kill", "killproc", "mem", "ls",
            "cat", "rm", "fs", "reboot", "update", "trace", "run"}
    if "wifi" in ctx.features:
        want |= {"wifi", "bt"}
    if "display" in ctx.features:
        want |= {"display", "led"}
    check(want <= names, f"help is missing {sorted(want - names)}")


def test_kernel_processes_running(ctx):
    procs = ctx.pico.processes()
    check(1 in procs and procs[1]["alive"], "kernel process (PID 1) is alive")
    check(2 in procs and procs[2]["alive"], "shell process (PID 2) is alive")
    names = {t["name"] for t in ctx.pico.threads().values()}
    check({"idle", "idle1", "shell"} <= names, f"core threads present, got {sorted(names)}")


def test_hwtest_app_present(ctx):
    out = ctx.pico.cmd("run")
    check(re.search(r"^\s+hwtest$", out, re.M) is not None,
          "this kit has the hwtest app (built with demo apps)")


@requires("wifi")
def test_wifi_status(ctx):
    out = ctx.pico.cmd("wifi status")
    check("WiFi state:" in out, "wifi status answers")
