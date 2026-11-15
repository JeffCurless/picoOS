"""Filesystem round trips through the shell."""

from harness import check

F = "hwt_fs.txt"


def _rm(p, name):
    p.cmd(f"rm {name}")   # ignore "not found"


def test_write_cat_rm(ctx):
    p = ctx.pico
    try:
        out = p.cmd(f"fs write {F} hello hardware test")
        check(f"wrote 20 bytes to '{F}'" in out, f"fs write reports 20 bytes: {out!r}")
        check("hello hardware test" in p.cmd(f"cat {F}"), "cat shows the text")
        check(F in p.cmd("ls"), "ls lists the file")
        check(f"deleted '{F}'" in p.cmd(f"rm {F}"), "rm deletes it")
        check("cannot open" in p.cmd(f"cat {F}"), "cat fails after rm")
    finally:
        _rm(p, F)


def test_append(ctx):
    p = ctx.pico
    try:
        p.cmd(f"fs write {F} line one")
        out = p.cmd(f"fs append {F} line two")
        check("appended 9 bytes" in out, f"append reports 9 bytes: {out!r}")
        text = p.cmd(f"cat {F}")
        check("line one\nline two" in text, f"both lines, in order: {text!r}")
    finally:
        _rm(p, F)


def test_multiline_write(ctx):
    """`fs write <f>` reads its own lines.  Each one is sent only after its
    "> " prompt; sent early, a line would be read by the wrong reader."""
    p = ctx.pico
    try:
        body = [f"row {i} of the multi-line test" for i in range(8)]
        since = p.start(f"fs write {F}")   # prompts "> ", not "pico> "
        p.expect(r"\n> ", since=since)
        out = p.type_lines(body + ["."])
        check("wrote" in out, f"fs write finished: {out[-200:]!r}")
        text = p.cmd(f"cat {F}")
        check(text.strip().split("\n") == body,
              f"file holds exactly the lines sent: {text!r}")
    finally:
        _rm(p, F)


def test_survives_reboot(ctx):
    p = ctx.pico
    try:
        p.cmd(f"fs write {F} persists across reboot")
        p.reboot()
        check("persists across reboot" in p.cmd(f"cat {F}"), "file survives a reboot")
    finally:
        _rm(p, F)
